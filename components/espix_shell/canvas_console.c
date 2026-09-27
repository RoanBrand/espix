/*
 * The on-screen console: an espix shell rendered into the display canvas.
 *
 * It is the third transport for the same shell, after the UART console and the
 * SSH channel, and it exists so that a viewer always has something useful to
 * look at -- a board with no desktop program still gives you a prompt, which is
 * the point.
 *
 * On demand. It is started when a viewer attaches and nothing else owns the
 * screen, and stopped when the viewer goes, so a headless board allocates none
 * of it. That is also why it lives in espix_shell rather than in the display
 * service: it is a shell transport that happens to draw to a canvas, and a
 * display should not need a shell to exist.
 *
 * The line reader below is still deliberately plain -- no history, no arrows,
 * no completion -- but the *output* side is a terminal: CSI sequences that move
 * the cursor or erase are acted on, so a full-screen program redraws in place,
 * and UTF-8 is decoded one cell per code point. Switching the input side to the
 * real editor is the next step, and is now unblocked rather than blocked.
 *
 * Colour is parsed and dropped: the font is 1-bit and the grid holds one byte
 * per cell, so rendering it means a colour attribute per cell.
 */

#include <stdio.h>
#include <string.h>

#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"

#include "espix_display.h"
#include "espix_kernel.h"
#include "espix_shell.h"

#define TAG "vnc0"

/*
 * An inset, because a grid flush to the edge reads as a bug rather than as a
 * terminal -- the first glyph's left edge sits on the bezel and the last row
 * has nowhere to breathe. Sixteen pixels is two cells: enough to look
 * deliberate without wasting screen.
 */
#define CON_MARGIN 16
#define CON_COLS ((ESPIX_DISPLAY_W - 2 * CON_MARGIN) / 8)
#define CON_ROWS ((ESPIX_DISPLAY_H - 2 * CON_MARGIN) / 8)

#define CON_FG 0xC618   /* light grey on black, the usual terminal look */
#define CON_BG 0x0000

typedef struct {
    char         *grid;     /* CON_ROWS * CON_COLS, PSRAM */
    int           row, col;
    QueueHandle_t keys;     /* chars from the input callback */
    TaskHandle_t  task;
    volatile bool quit;
    bool          up;
    /* Set while the console is shutting down, so that a release -- which asks
     * the display for its fallback -- does not start this console again. */
    bool          leaving;
    /* 0 outside an escape sequence, 1 after ESC, 2 inside CSI. */
    int           esc;
    uint32_t      gen;

    /* CSI parameter accumulator: up to four numeric parameters, as every
     * terminal has. Only a handful are used below; the rest are parsed so an
     * unknown sequence is consumed rather than drawn. */
    uint16_t      params[4];
    int           nparam;
    int           param;

    /* Partial UTF-8 code point, and how many continuation bytes are still to
     * come. One code point must occupy exactly one cell. */
    uint32_t      utf;
    int           utf_need;

    /* RFB KeyEvent has no modifier field: Control arrives as its own key. */
    bool          ctrl;
    espix_session_t session;
} canvas_console_t;

static canvas_console_t s_con;

/* Bumped for each console, so a task that outlives its own can tell. */
static uint32_t s_gen;

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

static void con_cell(int row, int col, char ch)
{
    espix_canvas_t *cv = espix_display_canvas();
    if (cv == NULL || row < 0 || row >= CON_ROWS || col < 0 || col >= CON_COLS) {
        return;
    }
    const char cell[2] = { ch, '\0' };
    espix_canvas_lock(cv);
    espix_canvas_text(cv, CON_MARGIN + col * 8, CON_MARGIN + row * 8,
                      cell, CON_FG, CON_BG);
    espix_canvas_unlock(cv);
}

static void con_repaint(void *ctx);

static void con_scroll(void)
{
    memmove(s_con.grid, s_con.grid + CON_COLS,
            (size_t)(CON_ROWS - 1) * CON_COLS);
    memset(s_con.grid + (size_t)(CON_ROWS - 1) * CON_COLS, ' ', CON_COLS);
    s_con.row = CON_ROWS - 1;

    /*
     * The model moved, so the canvas has to follow. Without this the screen
     * never scrolls: everything above the newest line stays frozen and only the
     * bottom row changes, which is exactly what it looked like.
     *
     * A full repaint per scrolled line is not cheap. It is correct, and the
     * accelerator path -- PPA on this chip -- is where it becomes cheap.
     */
    con_repaint(NULL);
}

static void con_newline(void)
{
    if (++s_con.row >= CON_ROWS) {
        con_scroll();
    }
}

static void con_putc(char c);

/* One row, from the model. */
static void con_draw_row(int row)
{
    espix_canvas_t *cv = espix_display_canvas();
    if (cv == NULL || row < 0 || row >= CON_ROWS) {
        return;
    }

    char line[CON_COLS + 1];
    memcpy(line, s_con.grid + (size_t)row * CON_COLS, CON_COLS);
    line[CON_COLS] = '\0';

    espix_canvas_lock(cv);
    espix_canvas_text(cv, CON_MARGIN, CON_MARGIN + row * 8, line, CON_FG, CON_BG);
    espix_canvas_unlock(cv);
}

/* 0 = cursor to end, 1 = start to cursor, 2 = the whole line. */
static void con_erase_line(int mode)
{
    if (s_con.grid == NULL) {
        return;
    }

    int from = 0;
    int to   = CON_COLS;
    if (mode == 0) {
        from = s_con.col;
    } else if (mode == 1) {
        to = s_con.col + 1;
    }

    memset(s_con.grid + (size_t)s_con.row * CON_COLS + from, ' ',
           (size_t)(to - from));
    con_draw_row(s_con.row);
}

static void con_erase_screen(int mode)
{
    if (s_con.grid == NULL) {
        return;
    }

    if (mode == 2 || mode == 3) {
        memset(s_con.grid, ' ', (size_t)CON_ROWS * CON_COLS);
        con_repaint(NULL);
        return;
    }
    if (mode == 0) {
        con_erase_line(0);
        for (int r = s_con.row + 1; r < CON_ROWS; r++) {
            memset(s_con.grid + (size_t)r * CON_COLS, ' ', CON_COLS);
        }
        con_repaint(NULL);
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
static void con_glyph(uint32_t cp)
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

    con_putc(ch);
}

/*
 * The bit that makes this a terminal rather than a printer: a program that
 * wants the cursor somewhere says so with an escape sequence, and this moves
 * it. Only the handful that line editors and full-screen programs actually use
 * are implemented; everything else is parsed and dropped, which is the
 * property that keeps this robust rather than exhaustive.
 */
static void con_csi(char final)
{
    if (s_con.nparam < 4) {
        s_con.params[s_con.nparam++] = (uint16_t)s_con.param;
    }

    const int a = s_con.nparam > 0 ? s_con.params[0] : 0;
    const int b = s_con.nparam > 1 ? s_con.params[1] : 0;

    switch (final) {
    case 'A': s_con.row -= (a ? a : 1); break;
    case 'B': s_con.row += (a ? a : 1); break;
    case 'C': s_con.col += (a ? a : 1); break;
    case 'D': s_con.col -= (a ? a : 1); break;
    case 'G': s_con.col = (a ? a : 1) - 1; break;
    case 'H':
    case 'f':
        s_con.row = (a ? a : 1) - 1;
        s_con.col = (b ? b : 1) - 1;
        break;
    case 'K': con_erase_line(a); break;
    case 'J': con_erase_screen(a); break;
    default:  break;    /* SGR and the rest: recognised, nothing to render */
    }

    if (s_con.row < 0)              { s_con.row = 0; }
    if (s_con.row >= CON_ROWS)      { s_con.row = CON_ROWS - 1; }
    if (s_con.col < 0)              { s_con.col = 0; }
    if (s_con.col >= CON_COLS)      { s_con.col = CON_COLS - 1; }
}

static void con_putc(char c)
{
    if (s_con.grid == NULL) {
        return;
    }

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
        s_con.esc    = 1;
        s_con.nparam = 0;
        s_con.param  = 0;
        return;
    }
    if (s_con.esc == 1) {
        s_con.esc = (c == '[') ? 2 : 0;     /* only CSI is understood */
        return;
    }
    if (s_con.esc == 2) {
        if (c >= '0' && c <= '9') {
            s_con.param = s_con.param * 10 + (c - '0');
            if (s_con.param > 9999) {
                s_con.param = 9999;
            }
        } else if (c == ';') {
            if (s_con.nparam < 4) {
                s_con.params[s_con.nparam++] = (uint16_t)s_con.param;
            }
            s_con.param = 0;
        } else if (c >= 0x40 && c <= 0x7E) {
            s_con.esc = 0;
            con_csi(c);
        }
        return;
    }

    /*
     * UTF-8, one cell per code point. Without this the motd's antenna -- box
     * drawing, three bytes each -- drew three columns per glyph and wrapped,
     * shifting every line underneath it.
     */
    if (s_con.utf_need > 0) {
        if (((unsigned char)c & 0xC0) == 0x80) {
            s_con.utf = (s_con.utf << 6) | ((unsigned char)c & 0x3F);
            if (--s_con.utf_need == 0) {
                con_glyph(s_con.utf);
            }
        } else {
            s_con.utf_need = 0;             /* malformed: drop it */
        }
        return;
    }
    if ((unsigned char)c >= 0xC0) {
        const unsigned char u = (unsigned char)c;
        s_con.utf      = u & (u >= 0xF0 ? 0x07u : (u >= 0xE0 ? 0x0Fu : 0x1Fu));
        s_con.utf_need = u >= 0xF0 ? 3 : (u >= 0xE0 ? 2 : 1);
        return;
    }

    if (c == '\n') { s_con.col = 0; con_newline(); return; }
    if (c == '\r') { s_con.col = 0; return; }

    if (c == '\b') {
        if (s_con.col > 0) {
            s_con.col--;
            s_con.grid[s_con.row * CON_COLS + s_con.col] = ' ';
            con_cell(s_con.row, s_con.col, ' ');
        }
        return;
    }

    if (c == '\t') {
        /* The motd aligns its columns with tabs, and a tab is not one character
         * of output -- dropping it pulls everything after it leftwards, which
         * is exactly how it looked. Next multiple of eight, as every terminal
         * does it. */
        do {
            con_putc(' ');
        } while (s_con.col % 8 != 0);
        return;
    }

    if (c < 0x20 || c > 0x7E) {
        return;                         /* no escape sequences yet */
    }

    s_con.grid[s_con.row * CON_COLS + s_con.col] = c;
    con_cell(s_con.row, s_con.col, c);

    if (++s_con.col >= CON_COLS) {
        s_con.col = 0;
        con_newline();
    }
}

/* Redraw from the model. This is what makes ownership a repaint rather than a
 * handover of state -- the console keeps its screen whether or not it is the
 * one on display. */
static void con_repaint(void *ctx)
{
    (void)ctx;

    espix_canvas_t *cv = espix_display_canvas();
    if (cv == NULL || s_con.grid == NULL) {
        return;
    }

    espix_canvas_lock(cv);

    /* The margin is outside the grid, so it is painted separately -- otherwise
     * whatever the previous owner left there shows through. */
    espix_canvas_fill(cv, (espix_rect_t){ 0, 0, ESPIX_DISPLAY_W, ESPIX_DISPLAY_H },
                      CON_BG);

    for (int r = 0; r < CON_ROWS; r++) {
        char line[CON_COLS + 1];
        memcpy(line, s_con.grid + (size_t)r * CON_COLS, CON_COLS);
        line[CON_COLS] = '\0';
        espix_canvas_text(cv, CON_MARGIN, CON_MARGIN + r * 8, line, CON_FG, CON_BG);
    }
    espix_canvas_unlock(cv);
}

/* ------------------------------------------------------------------ */
/* Input and output                                                    */
/* ------------------------------------------------------------------ */

static void con_input(void *ctx, const espix_input_event_t *ev)
{
    (void)ctx;

    if (ev->kind != ESPIX_INPUT_KEY) {
        return;                         /* a text console has no pointer */
    }

    /*
     * Modifiers are key events of their own -- an RFB KeyEvent carries a keysym
     * and nothing else -- so Ctrl-C arrives as Control down, then 'c' down.
     * Without tracking that, the console saw the letter 'c' and no interrupt at
     * all, which is why Ctrl-C did nothing however well everything else worked.
     */
    if (ev->keysym == 0xFFE3 || ev->keysym == 0xFFE4) {    /* Control_L, _R */
        s_con.ctrl = ev->down;
        return;
    }
    if (!ev->down) {
        return;
    }

    char ch = espix_keysym_char(ev->keysym);
    if (ch == '\0') {
        return;
    }

    /* Ctrl-<key> is the key's low five bits, which is where ^C = 0x03 comes
     * from. Shift needs nothing: the client sends the shifted keysym. */
    if (s_con.ctrl) {
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

    (void)xQueueSend(s_con.keys, &ch, 0);
}

static int con_write(espix_session_t *s, const char *data, size_t len)
{
    (void)s;

    for (size_t i = 0; i < len; i++) {
        con_putc(data[i]);
    }
    return (int)len;
}

/*
 * Read one line off the key queue.
 *
 * The session's own read_line is what the shell blocks in, so this is where the
 * console's task spends its life -- and where the owner's input callback is the
 * only other party. Neither touches the other's state: the callback pushes a
 * char, this pulls one.
 */
static int con_read_line(espix_session_t *s, const char *prompt,
                         char *buf, size_t len)
{
    con_write(s, prompt, strlen(prompt));

    size_t n = 0;
    for (;;) {
        char ch;
        if (xQueueReceive(s_con.keys, &ch, pdMS_TO_TICKS(100)) != pdTRUE) {
            if (s_con.quit) {
                return -1;              /* the viewer went away */
            }
            continue;
        }

        if (ch == '\r' || ch == '\n') {
            con_write(s, "\n", 1);
            break;
        }
        if (ch == 0x08 || ch == 0x7F) {         /* Backspace, DEL */
            if (n > 0) {
                n--;
                con_write(s, "\b", 1);
            }
            continue;
        }
        if (ch == 0x03) {                       /* Ctrl-C abandons the line */
            con_write(s, "^C\n", 3);
            n = 0;
            break;
        }
        if (ch >= 0x20 && ch < 0x7F && n + 1 < len) {
            buf[n++] = ch;
            con_write(s, &ch, 1);
        }
    }

    buf[n] = '\0';
    return (int)n;
}

/*
 * Called by the shell between writes while a command runs. It is the only
 * reader of the key queue during a command, which is what makes Ctrl-C work --
 * and what stops typed keys from piling up and then being replayed as a line
 * the moment the command exits.
 */
static bool con_poll_interrupt(espix_session_t *s)
{
    (void)s;

    /*
     * The viewer is gone. A running command never sees s_con.quit -- this
     * callback is the only thing the shell consults while one runs -- so
     * without this, `top` sat there for ever and the console task could never
     * be joined. That is what left the screen owned by a console nobody could
     * see, and what made the next connection get nothing at all.
     */
    if (s_con.quit) {
        return true;
    }

    bool interrupted = false;
    char ch;

    while (xQueueReceive(s_con.keys, &ch, 0) == pdTRUE) {
        if (ch == 0x03) {
            interrupted = true;
        }
    }
    return interrupted;
}

static const espix_screen_t s_console_screen = {
    .name    = "vnc-console",
    .input   = con_input,
    .repaint = con_repaint,
    .ctx     = NULL,
};

/* ------------------------------------------------------------------ */
/* The session                                                         */
/* ------------------------------------------------------------------ */

static void con_task(void *arg)
{
    /* Which console this task belongs to. A task that would not stop keeps
     * running while a newer console exists, and its teardown must then free
     * nothing -- those buffers are the new console's. */
    const uint32_t gen = (uint32_t)(uintptr_t)arg;

    espix_session_t *s = &s_con.session;
    *s = (espix_session_t){
        .name      = "vnc0",
        .cwd       = "/",
        .read_line = con_read_line,
        .write     = con_write,
        .poll_interrupt = con_poll_interrupt,
        .transport = NULL,
        .fg_pid    = ESPIX_PID_NONE,
        /*
         * The esp account, not root -- the deliberate difference from the
         * serial console. That one is root because whoever is holding the
         * board has already won; a viewer over the network has not, so this
         * console starts unprivileged and escalates with sudo when it means
         * to. The protection against someone who does hold the cable, or the
         * VNC password, is the SSH tunnel rather than RFB's own authentication.
         *
         * 1000 and "esp" are written out rather than named because espix_auth
         * owns the names and this component cannot reach it: espix_auth is
         * above espix_fs, which is above this one. Same reason, and the same
         * shape, as the uid in tty_console.c.
         */
        .uid       = 1000,      /* esp */
        .gid       = 1000,
        .login     = false,
        /*
         * True now that there is a parser behind this. It is not cosmetic:
         * commands read it to decide whether they may use cursor addressing,
         * and `top` printed a fresh screenful per update while it was false --
         * which is the "scrolling for ever" that a terminal without escapes
         * genuinely should do.
         */
        .ansi      = true,
    };
    strlcpy(s->user, "esp", sizeof(s->user));

    /*
     * The account's home, or root if it is not there. A rootfs can be replaced
     * wholesale, and refusing to open a console because a directory went
     * missing would be a poor trade -- the same bargain apply_account() makes.
     */
    strlcpy(s->home, "/home/esp", sizeof(s->home));
    strlcpy(s->cwd,  "/home/esp", sizeof(s->cwd));

    struct stat st;
    if (stat(s->cwd, &st) != 0 || !S_ISDIR(st.st_mode)) {
        strlcpy(s->cwd, "/", sizeof(s->cwd));
    }

    /*
     * `exit` ends the session, not the console.
     *
     * A VT that dropped to a blank screen on logout would be a poor getty: what
     * follows an exit is a *fresh* session, which is what init respawning getty
     * gives you on Linux. The only thing that ends the console itself is the
     * viewer leaving -- and that is why this loop is bounded by s_con.quit and
     * not by the shell returning.
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
        strlcpy(s->cwd, s->home, sizeof(s->cwd));
    } while (!s_con.quit);

    /*
     * The task owns its own teardown, exactly as it owns its own deletion: it
     * is the only thing that knows it has stopped touching the queue and the
     * grid. Freeing them from stop() left a window in which a task still inside
     * read_line touched a queue that no longer existed -- which is the
     * xQueueReceive(NULL) assert that rebooted the board.
     */
    s_con.leaving = true;

    if (gen != s_gen) {
        espix_klog(ESPIX_KLOG_INFO, TAG, "stale console task exiting");
        vTaskDeleteWithCaps(NULL);
    }

    if (s_con.keys != NULL) {
        vQueueDelete(s_con.keys);
        s_con.keys = NULL;
    }
    if (s_con.grid != NULL) {
        heap_caps_free(s_con.grid);
        s_con.grid = NULL;
    }

    espix_display_release(&s_console_screen);

    s_con.up   = false;
    s_con.task = NULL;
    vTaskDeleteWithCaps(NULL);          /* frees the PSRAM stack it was given */
}

esp_err_t espix_console_canvas_start(void)
{
    /*
     * Idempotent, and it re-claims when it is already running: a desktop that
     * took the screen and then gave it back must find the console where it
     * left it, not a dead screen.
     */
    if (s_con.leaving) {
        return ESP_ERR_INVALID_STATE;   /* on its way out; do not restart it */
    }
    if (s_con.up) {
        return espix_display_claim(&s_console_screen);
    }

    memset(&s_con, 0, sizeof(s_con));

    s_con.grid = heap_caps_malloc((size_t)CON_ROWS * CON_COLS,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_con.grid == NULL) {
        s_con.grid = heap_caps_malloc((size_t)CON_ROWS * CON_COLS,
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (s_con.grid == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(s_con.grid, ' ', (size_t)CON_ROWS * CON_COLS);

    s_con.keys = xQueueCreate(64, sizeof(char));
    if (s_con.keys == NULL) {
        heap_caps_free(s_con.grid);
        s_con.grid = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* Claimed before the task runs, so the first frame is the console's and
     * nothing can slip in between. */
    if (espix_display_claim(&s_console_screen) != ESP_OK) {
        vQueueDelete(s_con.keys);
        s_con.keys = NULL;
        heap_caps_free(s_con.grid);
        s_con.grid = NULL;
        return ESP_ERR_INVALID_STATE;
    }

    s_con.up = true;

    void *const gen = (void *)(uintptr_t)++s_gen;

    /*
     * 8192, not 4096: this task runs commands *inline*, exactly as the SSH
     * connection task does, and that task's stack is sized for the same reason.
     * 4096 tripped the canary on `ps` -- a stack-protection panic inside
     * _svfprintf_r -- which is the same class of failure the SSH stack's own
     * comment records for `lsusb` at 6144.
     *
     * A command that needs more still declares its own stack in the command
     * table, where it costs one command rather than every console.
     */
    if (xTaskCreateWithCaps(con_task, "espix:vnc0", 8192, gen, 4, &s_con.task,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        (void)xTaskCreateWithCaps(con_task, "espix:vnc0", 8192, gen, 4,
                                  &s_con.task,
                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (s_con.task == NULL) {
        espix_display_release(&s_console_screen);
        vQueueDelete(s_con.keys);
        s_con.keys = NULL;
        heap_caps_free(s_con.grid);
        s_con.grid = NULL;
        s_con.up = false;
        return ESP_ERR_NO_MEM;
    }

    espix_klog(ESPIX_KLOG_INFO, TAG, "console up (%dx%d)", CON_COLS, CON_ROWS);
    return ESP_OK;
}

void espix_console_canvas_stop(void)
{
    if (!s_con.up) {
        return;
    }

    /*
     * Asked, not forced -- and nothing is freed here. The task releases the
     * screen and frees the buffers as its last act, because only it knows when
     * it has stopped using them. Waiting for it to be gone is the whole of this
     * function; anything freed here would be freed while the task might still
     * be in read_line.
     */
    s_con.leaving = true;
    s_con.quit    = true;

    for (int i = 0; i < 300 && s_con.task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (s_con.task != NULL) {
        /*
         * Better a leaked buffer than a freed one under a live task, so the
         * task keeps its own -- but the screen is released here regardless. A
         * console that is gone must not hold it: that is what left the next
         * connection staring at a frozen image with no console of its own.
         */
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "console task did not stop; releasing the screen anyway");
        espix_display_release(&s_console_screen);
    }

    s_con.up      = false;
    s_con.leaving = false;
    espix_klog(ESPIX_KLOG_INFO, TAG, "console down");
}
