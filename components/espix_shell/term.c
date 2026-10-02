/*
 * A terminal: grid, parser, editor, session. Where the pixels go is the caller's
 * problem, and that is the whole point of the file.
 *
 * Moved here from canvas_console.c, which now supplies a view and owns the
 * screen. The code below is that code with the canvas taken out of it: the same
 * CSI handling, the same UTF-8 decoding one cell per code point, the same
 * answers to the Device Status and Device Attributes queries -- which are not
 * optional, because esp_linenoise probes for a terminal and turns line editing
 * off if nobody answers.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"

#include "esp_linenoise.h"

#include "espix_display.h"
#include "espix_kernel.h"
#include "espix_shell.h"
#include "espix_term.h"

#define TAG "term"

/*
 * The editor's callbacks are handed an int fd and nothing else, so the fd is
 * used as a *key* rather than a descriptor -- both read and write callbacks are
 * supplied, so nothing ever opens, closes or fcntl()s it. It used to be minus
 * one, which worked while there was exactly one terminal on the board; there are
 * two now, and the number is how the editor says which one it means.
 */
#define TERM_EDIT_SLOTS 4

static espix_term_t *s_edit_slot[TERM_EDIT_SLOTS];

static int term_slot_take(espix_term_t *t)
{
    for (int i = 1; i < TERM_EDIT_SLOTS; i++) {
        if (s_edit_slot[i] == NULL) {
            s_edit_slot[i] = t;
            return i;
        }
    }
    return -1;
}

static espix_term_t *term_slot_get(int fd)
{
    return (fd > 0 && fd < TERM_EDIT_SLOTS) ? s_edit_slot[fd] : NULL;
}

struct espix_term {
    espix_term_view_t view;

    int    cols, rows;
    char  *grid;                /* rows * cols, PSRAM */
    int    row, col;
    QueueHandle_t keys;         /* chars from the input callback */
    volatile bool quit;
    int    esc;                 /* 0 outside an escape, 1 after ESC, 2 inside CSI */

    uint16_t params[4];
    int      nparam;
    int      param;

    uint32_t utf;               /* partial UTF-8 code point */
    int      utf_need;

    bool ctrl;                  /* RFB KeyEvent has no modifier field */
    int  edit_fd;               /* the editor's key for this terminal */

    esp_linenoise_handle_t editor;
    espix_history_t       *history;
    espix_session_t        session;
};

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

static void term_cell(espix_term_t *t, int row, int col, char ch)
{
    if (t->view.cell != NULL) {
        t->view.cell(t->view.ctx, row, col, ch);
    }
}

static void term_repaint(espix_term_t *t);

static void term_scroll(espix_term_t *t)
{
    memmove(t->grid, t->grid + t->cols, (size_t)(t->rows - 1) * (size_t)t->cols);
    memset(t->grid + (size_t)(t->rows - 1) * (size_t)t->cols, ' ',
           (size_t)t->cols);
    t->row = t->rows - 1;

    /*
     * The model moved, so the screen has to follow. Without this the screen
     * never scrolls: everything above the newest line stays frozen and only the
     * bottom row changes, which is exactly what it looked like.
     *
     * A full repaint per scrolled line is not cheap. It is correct, and the
     * accelerator path -- PPA on this chip -- is where it becomes cheap.
     */
    term_repaint(t);
}

static void term_newline(espix_term_t *t)
{
    if (++t->row >= t->rows) {
        term_scroll(t);
    }
}

static void term_putc(espix_term_t *t, char c);

/* One row, from the model. */
static void term_draw_row(espix_term_t *t, int row)
{
    if (t->view.row == NULL || row < 0 || row >= t->rows) {
        return;
    }
    t->view.row(t->view.ctx, row, t->grid + (size_t)row * (size_t)t->cols,
                t->cols);
}

/* 0 = cursor to end, 1 = start to cursor, 2 = the whole line. */
static void term_erase_line(espix_term_t *t, int mode)
{
    int from = 0;
    int to   = t->cols;
    if (mode == 0) {
        from = t->col;
    } else if (mode == 1) {
        to = t->col + 1;
    }

    memset(t->grid + (size_t)t->row * (size_t)t->cols + from, ' ',
           (size_t)(to - from));
    term_draw_row(t, t->row);
}

static void term_erase_screen(espix_term_t *t, int mode)
{
    if (mode == 2 || mode == 3) {
        memset(t->grid, ' ', (size_t)t->rows * (size_t)t->cols);
        term_repaint(t);
        return;
    }
    if (mode == 0) {
        term_erase_line(t, 0);
        for (int r = t->row + 1; r < t->rows; r++) {
            memset(t->grid + (size_t)r * (size_t)t->cols, ' ', (size_t)t->cols);
        }
        term_repaint(t);
    }
}

/*
 * One code point, one cell -- which is the whole reason this exists. The motd's
 * antenna is drawn with box-drawing characters, and each is three bytes in
 * UTF-8; drawing them byte by byte made every glyph three columns wide and
 * wrapped the line, shifting everything under it. The 8x8 font has no such
 * glyphs, so they are approximated in ASCII: the drawing still does not look
 * right, but the columns hold, which is what alignment means.
 */
static void term_glyph(espix_term_t *t, uint32_t cp)
{
    char ch;

    switch (cp) {
    case 0x2500: case 0x2501: case 0x2504: case 0x2505:
        ch = '-';
        break;
    case 0x2502: case 0x2503: case 0x2506: case 0x2507:
        ch = '|';
        break;
    case 0x250C: case 0x2510: case 0x2514: case 0x2518:
    case 0x251C: case 0x2524: case 0x252C: case 0x2534:
    case 0x253C:
    case 0x250F: case 0x2513: case 0x2517: case 0x251B:
    case 0x2523: case 0x252B: case 0x2533: case 0x253B:
    case 0x254B:
        ch = '+';
        break;
    default:
        ch = (cp < 0x80) ? (char)cp : ' ';
        break;
    }

    term_putc(t, ch);
}

/*
 * The bit that makes this a terminal rather than a printer: a program that
 * wants the cursor somewhere says so with an escape sequence, and this moves
 * it. Only the handful that line editors and full-screen programs actually use
 * are implemented; everything else is parsed and dropped, which is the
 * property that keeps this robust rather than exhaustive.
 */
static void term_csi(espix_term_t *t, char final)
{
    if (t->nparam < 4) {
        t->params[t->nparam++] = (uint16_t)t->param;
    }

    const int a = t->nparam > 0 ? t->params[0] : 0;
    const int b = t->nparam > 1 ? t->params[1] : 0;

    switch (final) {
    case 'A': t->row -= (a ? a : 1); break;
    case 'B': t->row += (a ? a : 1); break;
    case 'C': t->col += (a ? a : 1); break;
    case 'D': t->col -= (a ? a : 1); break;
    case 'G': t->col = (a ? a : 1) - 1; break;
    case 'H':
    case 'f':
        t->row = (a ? a : 1) - 1;
        t->col = (b ? b : 1) - 1;
        break;
    case 'K': term_erase_line(t, a); break;
    case 'J': term_erase_screen(t, a); break;

    case 'n':
        /*
         * Device Status Report -- "where is your cursor?". This is ours to
         * answer, because we *are* the terminal on this side: there is nothing
         * else between the shell and the screen.
         *
         * It is not optional. esp_linenoise finds the terminal width by sending
         * this query and reading the reply, so with nobody answering it waits
         * for ever inside create_instance -- before the session task exists and
         * before a frame is ever sent, which is why the viewer was simply
         * black. Over SSH the client's terminal emulator answers; a VNC viewer
         * is a framebuffer and never will.
         */
        if (a == 5) {
            /*
             * Status report, which is what esp_linenoise_probe() actually
             * sends -- not the cursor-position request it looks like. It
             * requires exactly "ESC[0n" (four bytes) within 500 ms, and if it
             * does not get it it concludes the terminal is dumb and turns line
             * editing and history OFF. So this reply is what makes arrows,
             * history and completion work at all.
             */
            for (const char *p = "\x1b[0n"; *p != '\0'; p++) {
                (void)xQueueSend(t->keys, p, 0);
            }
        } else if (a == 6) {
            /* Cursor position, the other thing a program may ask for. */
            char reply[16];
            const int n = snprintf(reply, sizeof(reply), "\x1b[%d;%dR",
                                   t->row + 1, t->col + 1);
            for (int i = 0; i < n; i++) {
                (void)xQueueSend(t->keys, &reply[i], 0);
            }
        }
        break;

    case 'c':
        /*
         * Device Attributes. Not used for anything here, but a terminal that
         * does not answer is a terminal something will wait on -- and every
         * query answered is one fewer way to hang.
         */
        for (const char *p = "\x1b[?1;2c"; *p != '\0'; p++) {
            (void)xQueueSend(t->keys, p, 0);
        }
        break;

    default:  break;    /* SGR and the rest: recognised, nothing to render */
    }

    if (t->row < 0)          { t->row = 0; }
    if (t->row >= t->rows)   { t->row = t->rows - 1; }
    if (t->col < 0)          { t->col = 0; }
    if (t->col >= t->cols)   { t->col = t->cols - 1; }
}

static void term_putc(espix_term_t *t, char c)
{
    /*
     * Escape sequences: parsed, and the ones that move the cursor acted on.
     * Dropping the ESC alone is not harmless -- its arguments are printable, so
     * a colour sequence draws as "[36m" -- and dropping the whole thing is not
     * enough either, because then a cursor-home does not go home and a
     * full-screen program appends for ever.
     *
     * Unknown sequences are consumed and ignored, which is what keeps this
     * robust rather than exhaustive.
     */
    if (c == 0x1B) {
        t->esc    = 1;
        t->nparam = 0;
        t->param  = 0;
        return;
    }
    if (t->esc == 1) {
        t->esc = (c == '[') ? 2 : 0;        /* only CSI is understood */
        return;
    }
    if (t->esc == 2) {
        if (c >= '0' && c <= '9') {
            t->param = t->param * 10 + (c - '0');
            if (t->param > 9999) {
                t->param = 9999;
            }
        } else if (c == ';') {
            if (t->nparam < 4) {
                t->params[t->nparam++] = (uint16_t)t->param;
            }
            t->param = 0;
        } else if (c >= 0x40 && c <= 0x7E) {
            t->esc = 0;
            term_csi(t, c);
        }
        return;
    }

    /*
     * UTF-8, one cell per code point. Without this the motd's antenna -- box
     * drawing, three bytes each -- drew three columns per glyph and wrapped,
     * shifting every line underneath it.
     */
    if (t->utf_need > 0) {
        if (((unsigned char)c & 0xC0) == 0x80) {
            t->utf = (t->utf << 6) | ((unsigned char)c & 0x3F);
            if (--t->utf_need == 0) {
                term_glyph(t, t->utf);
            }
        } else {
            t->utf_need = 0;                /* malformed: drop it */
        }
        return;
    }
    if ((unsigned char)c >= 0xC0) {
        const unsigned char u = (unsigned char)c;
        t->utf      = u & (u >= 0xF0 ? 0x07u : (u >= 0xE0 ? 0x0Fu : 0x1Fu));
        t->utf_need = u >= 0xF0 ? 3 : (u >= 0xE0 ? 2 : 1);
        return;
    }

    if (c == '\n') { t->col = 0; term_newline(t); return; }
    if (c == '\r') { t->col = 0; return; }

    if (c == '\b') {
        if (t->col > 0) {
            t->col--;
            t->grid[(size_t)t->row * (size_t)t->cols + t->col] = ' ';
            term_cell(t, t->row, t->col, ' ');
        }
        return;
    }

    if (c == '\t') {
        /* The motd aligns its columns with tabs, and a tab is not one character
         * of output -- dropping it pulls everything after it leftwards, which
         * is exactly how it looked. Next multiple of eight, as every terminal
         * does it. */
        do {
            term_putc(t, ' ');
        } while (t->col % 8 != 0);
        return;
    }

    if (c < 0x20 || c > 0x7E) {
        return;
    }

    t->grid[(size_t)t->row * (size_t)t->cols + t->col] = c;
    term_cell(t, t->row, t->col, c);

    if (++t->col >= t->cols) {
        t->col = 0;
        term_newline(t);
    }
}

/*
 * Redraw from the model. This is what makes ownership a repaint rather than a
 * handover of state: a terminal keeps its screen whether or not it is the one
 * on display -- and, for the console, whether or not it is behind the desktop.
 */
static void term_repaint(espix_term_t *t)
{
    if (t->view.clear != NULL) {
        t->view.clear(t->view.ctx);
    }
    for (int r = 0; r < t->rows; r++) {
        term_draw_row(t, r);
    }
}

void espix_term_repaint(espix_term_t *t)
{
    if (t != NULL) {
        term_repaint(t);
    }
}

bool espix_term_resize(espix_term_t *t, int cols, int rows)
{
    if (t == NULL || cols <= 0 || cols > ESPIX_TERM_MAX_COLS ||
        rows <= 0 || rows > ESPIX_TERM_MAX_ROWS) {
        return false;
    }
    if (cols == t->cols && rows == t->rows) {
        return true;
    }

    const size_t cells = (size_t)cols * (size_t)rows;

    char *grid = heap_caps_malloc(cells, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (grid == NULL) {
        grid = heap_caps_malloc(cells, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (grid == NULL) {
        return false;                   /* the old size stays, and still works */
    }
    memset(grid, ' ', cells);

    /* Same row, same column, for as much of both grids as exists. */
    const int keep_rows = rows < t->rows ? rows : t->rows;
    const int keep_cols = cols < t->cols ? cols : t->cols;

    for (int r = 0; r < keep_rows; r++) {
        memcpy(grid + (size_t)r * (size_t)cols,
               t->grid + (size_t)r * (size_t)t->cols, (size_t)keep_cols);
    }

    heap_caps_free(t->grid);
    t->grid = grid;
    t->cols = cols;
    t->rows = rows;

    if (t->row >= rows) { t->row = rows - 1; }
    if (t->col >= cols) { t->col = cols - 1; }
    if (t->row < 0)     { t->row = 0; }
    if (t->col < 0)     { t->col = 0; }

    term_repaint(t);
    return true;
}

int espix_term_cols(const espix_term_t *t)
{
    return t != NULL ? t->cols : 0;
}

int espix_term_rows(const espix_term_t *t)
{
    return t != NULL ? t->rows : 0;
}

const char *espix_term_row_text(const espix_term_t *t, int row)
{
    if (t == NULL || row < 0 || row >= t->rows) {
        return NULL;
    }
    return t->grid + (size_t)row * (size_t)t->cols;
}

/* ------------------------------------------------------------------ */
/* Input and output                                                    */
/* ------------------------------------------------------------------ */

void espix_term_key(espix_term_t *t, uint32_t keysym, bool down)
{
    if (t == NULL) {
        return;
    }

    /*
     * Modifiers are key events of their own -- an RFB KeyEvent carries a keysym
     * and nothing else -- so Ctrl-C arrives as Control down, then 'c' down.
     * Without tracking that, a terminal saw the letter 'c' and no interrupt at
     * all, which is why Ctrl-C did nothing however well everything else worked.
     */
    if (keysym == 0xFFE3 || keysym == 0xFFE4) {         /* Control_L, _R */
        t->ctrl = down;
        return;
    }
    if (!down) {
        return;
    }

    char ch = espix_keysym_char(keysym);
    if (ch == '\0') {
        return;
    }

    /* Ctrl-<key> is the key's low five bits, which is where ^C = 0x03 comes
     * from. Shift needs nothing: the client sends the shifted keysym. */
    if (t->ctrl) {
        if (ch >= 'a' && ch <= 'z') {
            ch = (char)(ch - 'a' + 1);
        } else if (ch >= 'A' && ch <= 'Z') {
            ch = (char)(ch - 'A' + 1);
        } else if (ch == ' ') {
            ch = 0x00;
        } else if (ch >= '@' && ch <= '_') {
            ch = (char)(ch - '@');
        }
    }

    (void)xQueueSend(t->keys, &ch, 0);
}

void espix_term_write(espix_term_t *t, const char *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        term_putc(t, data[i]);
    }
}

/* ------------------------------------------------------------------ */
/* The session                                                         */
/* ------------------------------------------------------------------ */

static espix_term_t *term_of(espix_session_t *s)
{
    return (espix_term_t *)s->transport;
}

/* The canvas grid's size, for espix_term_size(). */
static void term_term_size(espix_session_t *s, int *cols, int *rows)
{
    const espix_term_t *t = term_of(s);

    *cols = t->cols;
    *rows = t->rows;
}

static int term_write(espix_session_t *s, const char *data, size_t len)
{
    espix_term_write(term_of(s), data, len);
    return (int)len;
}

/*
 * A spawned process's stdio.
 *
 * Without this an app's printf goes nowhere: the session's write() is the
 * shell's output path, and a program that calls libc does not go through it.
 * espix_proc points the task's streams at whatever this returns, which is how
 * an app's output reaches the screen at all.
 *
 * funopen over the same paths the shell uses, exactly as the SSH transport
 * does, so there is one rendering path for both.
 */
static int term_stream_write(void *cookie, const char *buf, int len)
{
    return term_write((espix_session_t *)cookie, buf, (size_t)len);
}

static int term_stream_read(void *cookie, char *buf, int len)
{
    espix_term_t *t = term_of((espix_session_t *)cookie);

    /*
     * Blocks only until there is something, then takes whatever else is already
     * queued: waiting for a full request would leave an interactive reader
     * stuck behind a buffer that will not fill.
     */
    int n = 0;
    while (n < len) {
        char ch;
        if (xQueueReceive(t->keys, &ch,
                          n == 0 ? pdMS_TO_TICKS(100) : 0) != pdTRUE) {
            break;
        }
        buf[n++] = ch;
    }
    return n;
}

static FILE *term_open_stream(espix_session_t *s, espix_stream_t which)
{
    if (which == ESPIX_STREAM_IN) {
        FILE *f = funopen(s, term_stream_read, NULL, NULL, NULL);
        if (f != NULL) {
            setvbuf(f, NULL, _IOFBF, 512);
        }
        return f;
    }

    /* Buffered, not unbuffered: a repaint is expensive and a byte at a time
     * would be absurd. */
    FILE *f = funopen(s, NULL, term_stream_write, NULL, NULL);
    if (f != NULL) {
        setvbuf(f, NULL, _IOLBF, 128);
    }
    return f;
}

/*
 * The editor's two ends. esp_linenoise hands its callbacks an int fd and no
 * context pointer, so the fd is only a key -- the same arrangement the SSH
 * transport uses, and the reason neither ever calls esp_linenoise_probe(),
 * which would fcntl() a descriptor that is not a terminal.
 */
static ssize_t term_edit_read(int fd, void *buf, size_t count)
{
    espix_term_t *t = term_slot_get(fd);
    if (t == NULL) {
        return 0;
    }

    char *p = buf;
    size_t n = 0;

    while (n < count) {
        char ch;
        if (xQueueReceive(t->keys, &ch,
                          n == 0 ? pdMS_TO_TICKS(100) : 0) != pdTRUE) {
            if (t->quit) {
                break;                      /* the viewer is gone: EOF */
            }
            if (n > 0) {
                break;
            }
            continue;
        }

        /*
         * ICRNL. A terminal sends CR for Enter and esp_linenoise tests for LF,
         * so without this Enter does nothing at all. The serial console gets it
         * from IDF's UART VFS (ESP_LINE_ENDINGS_CR) and SSH does it in its own
         * read callback -- "espix *is* the pty here, so the line discipline's
         * job is ours". Same job, same place.
         */
        if (ch == '\r') {
            ch = '\n';
        }
        p[n++] = ch;
    }
    return (ssize_t)n;
}

static ssize_t term_edit_write(int fd, const void *buf, size_t count)
{
    espix_term_t *t = term_slot_get(fd);
    if (t == NULL) {
        return 0;
    }
    espix_term_write(t, buf, count);
    return (ssize_t)count;
}

/*
 * Read one line off the key queue.
 *
 * The session's own read_line is what the shell blocks in, so this is where the
 * terminal's task spends its life -- and where the owner's input callback is
 * the only other party. Neither touches the other's state: the callback pushes
 * a char, this pulls one.
 */
static int term_read_line(espix_session_t *s, const char *prompt,
                          char *buf, size_t len)
{
    espix_term_t *t = term_of(s);

    if (t->quit || t->editor == NULL) {
        return -1;
    }

    esp_linenoise_set_prompt(t->editor, prompt);

    /* get_line() returns ESP_OK for an empty line without writing the buffer,
     * so anything left from last time would be run as a command. */
    buf[0] = '\0';

    if (esp_linenoise_get_line(t->editor, buf, len) != ESP_OK) {
        /*
         * Two different keys land here, exactly as they do over SSH: the editor
         * sets EAGAIN for Ctrl-C, which abandons the line and should leave a
         * fresh prompt, and leaves errno alone for Ctrl-D on an empty line,
         * which is end of input. Treating both as the end would drop the
         * session on Ctrl-C, which no other shell does.
         */
        if (errno == EAGAIN) {
            return 0;
        }
        return -1;
    }

    if (buf[0] != '\0') {
        espix_history_push(t->history, buf);
        espix_history_apply(t->history, t->editor);
    }

    return (int)strlen(buf);
}

/*
 * Called by the shell between writes while a command runs. It is the only
 * reader of the key queue during a command, which is what makes Ctrl-C work --
 * and what stops typed keys from piling up and then being replayed as a line
 * the moment the command exits.
 */
static bool term_poll_interrupt(espix_session_t *s)
{
    espix_term_t *t = term_of(s);

    /*
     * The viewer is gone. A running command never sees t->quit -- this callback
     * is the only thing the shell consults while one runs -- so without this,
     * `top` sat there for ever and the task could never be joined. That is what
     * left the screen owned by a console nobody could see, and what made the
     * next connection get nothing at all.
     */
    if (t->quit) {
        return true;
    }

    bool interrupted = false;
    char ch;

    while (xQueueReceive(t->keys, &ch, 0) == pdTRUE) {
        if (ch == 0x03) {
            interrupted = true;
        }
    }
    return interrupted;
}

espix_session_t *espix_term_session(espix_term_t *t)
{
    espix_session_t *s = &t->session;

    *s = (espix_session_t){
        .cwd            = "/",
        .read_line      = term_read_line,
        .write          = term_write,
        .poll_interrupt = term_poll_interrupt,
        .open_stream    = term_open_stream,
        .transport      = t,
        .term_size      = term_term_size,
        .fg_pid         = ESPIX_PID_NONE,
        .login          = false,
        /*
         * True now that there is a parser behind this. It is not cosmetic:
         * commands read it to decide whether they may use cursor addressing,
         * and `top` printed a fresh screenful per update while it was false --
         * which is the "scrolling for ever" that a terminal without escapes
         * genuinely should do.
         */
        .ansi           = true,
    };
    return s;
}

void espix_term_run(espix_term_t *t)
{
    espix_session_t *s = &t->session;

    /*
     * The account's home, or root if it is not there. A rootfs can be replaced
     * wholesale, and refusing to open a terminal because a directory went
     * missing would be a poor trade -- the same bargain apply_account() makes.
     */
    struct stat st;
    if (s->home[0] == '\0' || stat(s->home, &st) != 0 || !S_ISDIR(st.st_mode)) {
        strlcpy(s->cwd, "/", sizeof(s->cwd));
    } else {
        strlcpy(s->cwd, s->home, sizeof(s->cwd));
    }

    /*
     * `exit` ends the session, not the terminal.
     *
     * A VT that dropped to a blank screen on logout would be a poor getty: what
     * follows an exit is a *fresh* session, which is what init respawning getty
     * gives you on Linux. Only the terminal being stopped ends the loop.
     */
    do {
        /* Cleared before reuse, or the next session_run() would return
         * immediately and spin. */
        s->want_exit = false;

        /* The same greeting the serial console and an SSH login print, by
         * construction: both go through the same command. */
        espix_shell_exec(s, "motd");

        espix_shell_session_run(s);

        /* Per-session and heap-allocated; the shell hung the processes up as
         * it returned. */
        espix_env_free(s);
        strlcpy(s->cwd, s->home[0] != '\0' ? s->home : "/", sizeof(s->cwd));
    } while (!t->quit);
}

void espix_term_stop(espix_term_t *t)
{
    if (t != NULL) {
        t->quit = true;
    }
}

bool espix_term_stopping(const espix_term_t *t)
{
    return t != NULL && t->quit;
}

/* ------------------------------------------------------------------ */
/* Lifetime                                                            */
/* ------------------------------------------------------------------ */

espix_term_t *espix_term_new(const espix_term_view_t *view, int cols, int rows,
                             const char *user)
{
    if (view == NULL || cols <= 0 || cols > ESPIX_TERM_MAX_COLS ||
        rows <= 0 || rows > ESPIX_TERM_MAX_ROWS) {
        return NULL;
    }

    espix_term_t *t = calloc(1, sizeof(*t));
    if (t == NULL) {
        return NULL;
    }
    t->view = *view;
    t->cols = cols;
    t->rows = rows;

    const size_t cells = (size_t)cols * (size_t)rows;

    t->grid = heap_caps_malloc(cells, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (t->grid == NULL) {
        t->grid = heap_caps_malloc(cells, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (t->grid == NULL) {
        free(t);
        return NULL;
    }
    memset(t->grid, ' ', cells);

    /*
     * Before the editor, not after it. esp_linenoise issues a cursor-position
     * query when it starts and reads the reply back through term_edit_read(),
     * which pulls from this queue -- so creating the editor first was an
     * xQueueReceive(NULL) assert the moment a viewer connected, on the RFB
     * task, which is where a console is started from.
     */
    t->keys = xQueueCreate(64, sizeof(char));
    if (t->keys == NULL) {
        heap_caps_free(t->grid);
        free(t);
        return NULL;
    }

    /*
     * The same editor the UART and SSH consoles run, so history, arrows and
     * completion behave identically everywhere. The fd is a key, not a
     * descriptor -- both callbacks are supplied, so nothing reads or writes it.
     */
    t->history = espix_history_for(user != NULL ? user : "esp");

    const int slot = term_slot_take(t);
    if (slot < 0) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "no editor slot for another terminal");
        vQueueDelete(t->keys);
        heap_caps_free(t->grid);
        free(t);
        return NULL;
    }
    t->edit_fd = slot;

    esp_linenoise_config_t ed;
    esp_linenoise_get_instance_config_default(&ed);
    ed.in_fd               = slot;
    ed.out_fd              = slot;
    ed.max_cmd_line_length = ESPIX_LINE_MAX;
    ed.history_max_length  = 32;
    ed.allow_multi_line    = true;
    ed.allow_empty_line    = true;
    ed.completion_cb       = espix_shell_completion;
    ed.hints_cb            = espix_shell_hint;
    ed.read_bytes_cb       = term_edit_read;
    ed.write_bytes_cb      = term_edit_write;

    if (esp_linenoise_create_instance(&ed, &t->editor) != ESP_OK) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "cannot create the line editor");
        s_edit_slot[slot] = NULL;
        vQueueDelete(t->keys);
        heap_caps_free(t->grid);
        free(t);
        return NULL;
    }
    espix_history_apply(t->history, t->editor);

    return t;
}

void espix_term_free(espix_term_t *t)
{
    if (t == NULL) {
        return;
    }
    if (t->edit_fd > 0 && t->edit_fd < TERM_EDIT_SLOTS) {
        s_edit_slot[t->edit_fd] = NULL;
        t->edit_fd = 0;
    }
    if (t->editor != NULL) {
        esp_linenoise_delete_instance(t->editor);
        t->editor = NULL;
    }
    if (t->keys != NULL) {
        vQueueDelete(t->keys);
        t->keys = NULL;
    }
    if (t->grid != NULL) {
        heap_caps_free(t->grid);
        t->grid = NULL;
    }
    free(t);
}
