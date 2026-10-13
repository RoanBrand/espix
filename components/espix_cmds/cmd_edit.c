/*
 * edit -- a small full-screen text editor, built into the kernel.
 *
 * nano's shape, not vi's: there are no modes, typing inserts, and the two keys
 * that matter are Ctrl-O (write) and Ctrl-X (exit). That is the smaller editor,
 * the one somebody editing /etc/units over a serial console wants, and the one
 * this much code can get right. A vi subset would sit on the same rendering and
 * input path and is a later thing.
 *
 * It is a builtin because it belongs on every board with no /bin to populate,
 * and because a config file is exactly what a rescue shell has to be able to
 * fix. It reads keys the way an ELF app does: espix_proc_stdin_open() hands it
 * the same terminal stream the loader would have made stdin, so an escape
 * sequence arrives whole and no keystroke is dropped. That primitive is what
 * makes "compiled in" and "loaded from /bin" one program model rather than two;
 * read_line() (the shell's line editor) is the other way, and it cannot drive a
 * full-screen editor because it hands back a line, not a key.
 *
 * The file is one malloc'd string per line, and the limits are named rather
 * than discovered: /etc/units is a few hundred bytes, and a file bigger than
 * MAX_TOTAL is refused with a reason instead of being silently truncated.
 *
 * Compared with nano, what is missing, and why nothing pretends otherwise:
 * wrapping (long lines scroll sideways), undo, search, and multiple buffers.
 * What is here is every key needed to edit a config file and save it.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "espix_cmds_priv.h"
#include "espix_proc.h"

#define MAX_LINES   4096
#define MAX_LINE    1024
#define MAX_TOTAL   (256 * 1024)
#define MAX_COLS    200
#define MAX_ROWS    60
#define EDIT_PATH_MAX 256
#define OUT_MAX     (MAX_ROWS * (MAX_COLS + 40) + 512)

/* Keys that are not a byte. Values above 0xff so they cannot collide. */
enum {
    KEY_UP = 0x100, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
    KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN, KEY_DEL, KEY_NONE
};

typedef struct {
    char **line;                /* malloc'd, NUL-terminated */
    int   *len;
    int    count;
    int    cap;
} doc_t;

static doc_t            doc;
static espix_session_t *g_s;
static FILE            *g_in;
static char            *out;        /* the screen being drawn, heap-owned */
static char             path[EDIT_PATH_MAX];
static bool             modified;
static int              cx, cy;     /* cursor: byte column in line cy, row cy */
static int              top, left;  /* first visible row and column */
static int              rows = 24, cols = 80;
static char             msg[192];
static bool             quit;
static char            *cut_buf;    /* one cut line, or NULL */
static int              cut_len;
static bool             asking;     /* "save before exit?" prompt is up */

/* --------------------------------------------------------------- output --- */

static int out_len;

static void o_reset(void)
{
    out_len = 0;
}

static void o_add(const char *s, int n)
{
    if (n <= 0) {
        return;
    }
    if (out_len + n > OUT_MAX) {
        n = OUT_MAX - out_len;
    }
    if (n > 0) {
        memcpy(out + out_len, s, (size_t)n);
        out_len += n;
    }
}

static void o_str(const char *s)
{
    o_add(s, (int)strlen(s));
}

static void o_fmt(const char *fmt, ...)
{
    char    b[256];
    va_list ap;

    va_start(ap, fmt);
    const int n = vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);

    if (n > 0) {
        o_add(b, (n < (int)sizeof(b)) ? n : (int)sizeof(b) - 1);
    }
}

static void o_flush(void)
{
    if (out_len > 0) {
        (void)g_s->write(g_s, out, (size_t)out_len);
    }
}

/* ------------------------------------------------------------- the file --- */

static long doc_total(void)
{
    long n = 0;

    for (int i = 0; i < doc.count; i++) {
        n += doc.len[i];
    }
    return n;
}

static bool doc_init(void)
{
    doc.cap   = 64;
    doc.line  = malloc((size_t)doc.cap * sizeof(*doc.line));
    doc.len   = malloc((size_t)doc.cap * sizeof(*doc.len));
    doc.count = 0;

    return doc.line != NULL && doc.len != NULL;
}

static void doc_free(void)
{
    for (int i = 0; i < doc.count; i++) {
        free(doc.line[i]);
    }
    free(doc.line);
    free(doc.len);
    doc.count = 0;
}

static bool doc_insert(int y, const char *s, int n)
{
    if (doc.count >= MAX_LINES) {
        return false;
    }
    if (doc.count >= doc.cap) {
        const int  nc = doc.cap * 2;
        char     **nl = realloc(doc.line, (size_t)nc * sizeof(*doc.line));
        int       *nn = realloc(doc.len, (size_t)nc * sizeof(*doc.len));

        if (nl == NULL || nn == NULL) {
            return false;
        }
        doc.line = nl;
        doc.len  = nn;
        doc.cap  = nc;
    }
    memmove(&doc.line[y + 1], &doc.line[y],
            (size_t)(doc.count - y) * sizeof(*doc.line));
    memmove(&doc.len[y + 1], &doc.len[y],
            (size_t)(doc.count - y) * sizeof(*doc.len));

    char *p = malloc((size_t)n + 1);

    if (p == NULL) {
        return false;
    }
    memcpy(p, s, (size_t)n);
    p[n] = '\0';
    doc.line[y] = p;
    doc.len[y]  = n;
    doc.count++;
    return true;
}

static void doc_delete(int y)
{
    free(doc.line[y]);
    memmove(&doc.line[y], &doc.line[y + 1],
            (size_t)(doc.count - y - 1) * sizeof(*doc.line));
    memmove(&doc.len[y], &doc.len[y + 1],
            (size_t)(doc.count - y - 1) * sizeof(*doc.len));
    doc.count--;
}

static bool doc_append(const char *s, int n)
{
    return doc_insert(doc.count, s, n);
}

static void say(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    (void)vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
}

static bool load_file(void)
{
    FILE *f = fopen(path, "rb");

    if (f == NULL) {
        /*
         * Absent is not the same as unreadable. A path that exists but is not
         * this account's to read -- root's /etc/units, say -- used to open as an
         * empty buffer and be announced as a new file, which is a lie that turns
         * into data loss the moment somebody types and saves. Only ENOENT starts
         * one; anything else is reported as the error it is.
         */
        if (errno != ENOENT) {
            say("%s: %s", path, strerror(errno));
            return false;
        }
        if (!doc_append("", 0)) {
            say("out of memory");
            return false;
        }
        say("new file");
        return true;
    }

    size_t cap = 8192, len = 0;
    char  *data = malloc(cap);

    if (data == NULL) {
        fclose(f);
        say("out of memory");
        return false;
    }

    for (;;) {
        if (len == cap) {
            if (cap >= MAX_TOTAL) {
                free(data);
                fclose(f);
                say("file too large (limit %d KB)", MAX_TOTAL / 1024);
                return false;
            }
            cap *= 2;
            if (cap > MAX_TOTAL) {
                cap = MAX_TOTAL;
            }
            char *nd = realloc(data, cap);

            if (nd == NULL) {
                free(data);
                fclose(f);
                say("out of memory");
                return false;
            }
            data = nd;
        }
        const size_t got = fread(data + len, 1, cap - len, f);

        if (got == 0) {
            break;
        }
        len += got;
    }
    fclose(f);

    int start = 0;

    for (size_t i = 0; i < len; i++) {
        if (data[i] != '\n') {
            continue;
        }
        int n = (int)(i - (size_t)start);

        if (n > 0 && data[i - 1] == '\r') {
            n--;
        }
        if (!doc_append(data + start, n)) {
            free(data);
            say("too many lines (limit %d)", MAX_LINES);
            return false;
        }
        start = (int)i + 1;
    }
    if (start < (int)len) {
        int n = (int)len - start;

        if (n > 0 && data[len - 1] == '\r') {
            n--;
        }
        if (!doc_append(data + start, n)) {
            free(data);
            say("too many lines (limit %d)", MAX_LINES);
            return false;
        }
    }
    free(data);

    if (doc.count == 0 && !doc_append("", 0)) {
        say("out of memory");
        return false;
    }

    say("read %d line%s", doc.count, (doc.count == 1) ? "" : "s");
    return true;
}

static bool save_file(void)
{
    FILE *f = fopen(path, "w");

    if (f == NULL) {
        say("cannot write %s", path);
        return false;
    }

    long bytes = 0;
    bool err   = false;

    for (int i = 0; i < doc.count; i++) {
        if (doc.len[i] > 0 &&
            fwrite(doc.line[i], 1, (size_t)doc.len[i], f) !=
                (size_t)doc.len[i]) {
            err = true;
        }
        if (fwrite("\n", 1, 1, f) != 1) {
            err = true;
        }
        bytes += doc.len[i] + 1;
    }
    if (fclose(f) != 0) {
        err = true;
    }
    if (err) {
        say("write failed");
        return false;
    }

    modified = false;
    say("wrote %ld bytes, %d line%s", bytes, doc.count,
        (doc.count == 1) ? "" : "s");
    return true;
}

/* ------------------------------------------------------------- the keys --- */

static int get_byte(void)
{
    const int c = fgetc(g_in);

    return (c == EOF) ? -1 : c;
}

/*
 * One key, with the escape sequences a terminal sends for the keys that are not
 * characters. Blocking: the terminal stream returns as soon as a byte is there,
 * and no other reader is competing for it.
 *
 * A bare ESC blocks for the next byte and then throws the ESC away if what
 * arrives is an ordinary key. There is no mode for ESC to leave, so the only
 * cost is that a lone ESC waits; the alternative, select(), would be a second
 * way to watch the terminal that would have to be taught to every transport.
 */
static int read_key(void)
{
    int c = get_byte();

    if (c < 0 || c != 0x1b) {
        return c;
    }

    const int a = get_byte();

    if (a < 0) {
        return KEY_NONE;
    }
    if (a != '[' && a != 'O') {
        return a;                       /* ESC then a key: that key */
    }

    const int b = get_byte();

    if (b < 0) {
        return KEY_NONE;
    }
    if (b >= '0' && b <= '9') {
        const int d = get_byte();

        if (d != '~') {
            return KEY_NONE;
        }
        switch (b) {
        case '1': case '7': return KEY_HOME;
        case '3':           return KEY_DEL;
        case '4': case '8': return KEY_END;
        case '5':           return KEY_PGUP;
        case '6':           return KEY_PGDN;
        default:            return KEY_NONE;
        }
    }
    switch (b) {
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'C': return KEY_RIGHT;
    case 'D': return KEY_LEFT;
    case 'H': return KEY_HOME;
    case 'F': return KEY_END;
    default:  return KEY_NONE;
    }
}

/* -------------------------------------------------------------- editing --- */

static int cur_len(void)
{
    return (doc.count > 0) ? doc.len[cy] : 0;
}

static void clamp_cursor(void)
{
    if (doc.count == 0) {
        cy = cx = 0;
        return;
    }
    if (cy < 0) {
        cy = 0;
    }
    if (cy > doc.count - 1) {
        cy = doc.count - 1;
    }
    if (cx < 0) {
        cx = 0;
    }
    if (cx > cur_len()) {
        cx = cur_len();
    }
}

static int text_rows(void)
{
    const int r = rows - 2;

    return (r < 1) ? 1 : r;
}

static void ins_char(int ch)
{
    clamp_cursor();
    if (doc.count == 0 && !doc_append("", 0)) {
        say("out of memory");
        return;
    }
    const int l = cur_len();

    if (l + 1 >= MAX_LINE) {
        say("line limit (%d bytes) reached", MAX_LINE - 1);
        return;
    }
    if (doc_total() + 1 > MAX_TOTAL) {
        say("file too large (limit %d KB)", MAX_TOTAL / 1024);
        return;
    }

    char *nl = malloc((size_t)l + 2);

    if (nl == NULL) {
        say("out of memory");
        return;
    }
    memcpy(nl, doc.line[cy], (size_t)cx);
    nl[cx] = (char)ch;
    /* The tail and its NUL: doc.line[cy] is l+1 bytes and cx <= l. */
    memcpy(nl + cx + 1, doc.line[cy] + cx, (size_t)(l - cx + 1));

    free(doc.line[cy]);
    doc.line[cy] = nl;
    doc.len[cy]  = l + 1;
    cx++;
    modified = true;
}

static void ins_newline(void)
{
    clamp_cursor();
    if (doc.count == 0 && !doc_append("", 0)) {
        say("out of memory");
        return;
    }
    const int l    = cur_len();
    const int tail = l - cx;

    char *t = malloc((size_t)tail + 1);

    if (t == NULL) {
        say("out of memory");
        return;
    }
    memcpy(t, doc.line[cy] + cx, (size_t)tail);
    t[tail] = '\0';

    doc.line[cy][cx] = '\0';
    doc.len[cy]      = cx;

    if (!doc_insert(cy + 1, t, tail)) {
        free(t);
        say("out of memory");
        return;
    }
    free(t);
    cy++;
    cx = 0;
    modified = true;
}

static void do_backspace(void)
{
    clamp_cursor();
    if (doc.count == 0) {
        return;
    }
    if (cx > 0) {
        memmove(doc.line[cy] + cx - 1, doc.line[cy] + cx,
                (size_t)(doc.len[cy] - cx + 1));
        doc.len[cy]--;
        cx--;
        modified = true;
        return;
    }
    if (cy == 0) {
        return;
    }

    const int pl = doc.len[cy - 1];
    const int cl = doc.len[cy];

    if (pl + cl >= MAX_LINE) {
        say("line limit (%d bytes) reached", MAX_LINE - 1);
        return;
    }
    char *nl = malloc((size_t)(pl + cl) + 1);

    if (nl == NULL) {
        say("out of memory");
        return;
    }
    memcpy(nl, doc.line[cy - 1], (size_t)pl);
    memcpy(nl + pl, doc.line[cy], (size_t)cl);
    nl[pl + cl] = '\0';

    free(doc.line[cy - 1]);
    doc.line[cy - 1] = nl;
    doc.len[cy - 1]  = pl + cl;

    doc_delete(cy);
    cy--;
    cx = pl;
    modified = true;
}

static void do_delete(void)
{
    clamp_cursor();
    if (doc.count == 0) {
        return;
    }
    if (cx < cur_len()) {
        memmove(doc.line[cy] + cx, doc.line[cy] + cx + 1,
                (size_t)(doc.len[cy] - cx));
        doc.len[cy]--;
        modified = true;
        return;
    }
    if (cy >= doc.count - 1) {
        return;
    }

    const int nl = doc.len[cy + 1];
    const int pl = doc.len[cy];

    if (pl + nl >= MAX_LINE) {
        say("line limit (%d bytes) reached", MAX_LINE - 1);
        return;
    }
    char *p = realloc(doc.line[cy], (size_t)(pl + nl) + 1);

    if (p == NULL) {
        say("out of memory");
        return;
    }
    memcpy(p + pl, doc.line[cy + 1], (size_t)nl);
    p[pl + nl]   = '\0';
    doc.line[cy] = p;
    doc.len[cy]  = pl + nl;

    doc_delete(cy + 1);
    modified = true;
}

static void cut_line(void)
{
    clamp_cursor();
    if (doc.count == 0) {
        return;
    }
    free(cut_buf);
    cut_len = doc.len[cy];
    cut_buf = malloc((size_t)cut_len + 1);

    if (cut_buf == NULL) {
        cut_len = 0;
        say("out of memory");
        return;
    }
    memcpy(cut_buf, doc.line[cy], (size_t)cut_len);
    cut_buf[cut_len] = '\0';

    doc_delete(cy);
    if (doc.count == 0) {
        (void)doc_append("", 0);
    }
    clamp_cursor();
    cx       = 0;
    modified = true;
    say("cut 1 line");
}

static void paste_kill(void)
{
    clamp_cursor();
    if (cut_buf == NULL || cut_len == 0) {
        say("nothing to paste");
        return;
    }
    for (int i = 0; i < cut_len; i++) {
        ins_char(cut_buf[i]);
    }
}

/* ------------------------------------------------------------- the view --- */

static void ensure_visible(void)
{
    const int tr = text_rows();

    if (cy < top) {
        top = cy;
    }
    if (cy >= top + tr) {
        top = cy - tr + 1;
    }
    if (top < 0) {
        top = 0;
    }
    if (cx < left) {
        left = cx;
    }
    if (cx >= left + cols) {
        left = cx - cols + 1;
    }
    if (left < 0) {
        left = 0;
    }
}

static void render(void)
{
    clamp_cursor();
    ensure_visible();
    o_reset();
    o_str("\033[H");

    for (int r = 0; r < text_rows(); r++) {
        const int d = top + r;

        o_fmt("\033[%d;1H\033[K", r + 1);
        if (d < doc.count) {
            int s = left;

            if (s > doc.len[d]) {
                s = doc.len[d];
            }
            int n = doc.len[d] - s;

            if (n > cols) {
                n = cols;
            }
            if (n > 0) {
                o_add(doc.line[d] + s, n);
            }
        }
    }

    /* The status line, inverted and padded to the full width. */
    char st[320];
    int  n = snprintf(st, sizeof(st), " edit: %s%s   %d/%d  col %d",
                      path, modified ? " [modified]" : "",
                      cy + 1, doc.count, cx + 1);

    if (n < 0) {
        n = 0;
    }
    if (n > (int)sizeof(st) - 1) {
        n = (int)sizeof(st) - 1;
    }
    if (n > cols) {
        n = cols;
    }
    o_fmt("\033[%d;1H\033[7m", rows - 1);
    o_add(st, n);
    for (int i = n; i < cols; i++) {
        o_add(" ", 1);
    }
    o_str("\033[0m");

    /* The message line, and the cursor last so nothing overwrites it. */
    o_fmt("\033[%d;1H\033[K", rows);
    o_str((msg[0] != '\0') ? msg
                           : "^O write  ^X exit  ^K cut  ^U paste  ^L redraw");

    int sr = cy - top + 1;
    int sc = cx - left + 1;

    if (sr < 1) {
        sr = 1;
    }
    if (sr > text_rows()) {
        sr = text_rows();
    }
    if (sc < 1) {
        sc = 1;
    }
    if (sc > cols) {
        sc = cols;
    }
    o_fmt("\033[%d;%dH", sr, sc);
    o_flush();
}

static void show_help(void)
{
    o_reset();
    o_str("\033[H\033[2J");
    o_str("edit -- keys\r\n\r\n");
    o_str("  arrows, PgUp/PgDn, Home/End   move\r\n");
    o_str("  typing                        insert\r\n");
    o_str("  Enter Backspace Delete Tab    edit\r\n");
    o_str("  ^A ^E                         start / end of line\r\n");
    o_str("  ^K / ^U                       cut / paste a line\r\n");
    o_str("  ^O                            write the file\r\n");
    o_str("  ^X                            exit (asks if modified)\r\n");
    o_str("  ^L                            redraw, re-read the size\r\n");
    o_str("\r\n  no search, no undo, no wrapping -- yet\r\n");
    o_str("\r\n  press any key\r\n");
    o_flush();

    int c;
    do {
        c = get_byte();
    } while (c < 0);
    msg[0] = '\0';
}

static void query_size(void)
{
    int c = 0, r = 0;

    if (espix_shell_term_size(&c, &r) == 0 && c > 0 && r > 0) {
        cols = c;
        rows = r;
    }
    if (cols > MAX_COLS) {
        cols = MAX_COLS;
    }
    if (rows > MAX_ROWS) {
        rows = MAX_ROWS;
    }
    if (cols < 20) {
        cols = 20;
    }
    if (rows < 4) {
        rows = 4;
    }
}

static void move_home(void)  { cx = 0; }
static void move_end(void)   { cx = cur_len(); }

static void move_vertical(int dir)
{
    cy += dir;
    clamp_cursor();
}

static void move_horizontal(int dir)
{
    if (dir < 0) {
        if (cx > 0) {
            cx--;
        } else if (cy > 0) {
            cy--;
            cx = cur_len();
        }
    } else {
        if (cx < cur_len()) {
            cx++;
        } else if (cy < doc.count - 1) {
            cy++;
            cx = 0;
        }
    }
}

/* ------------------------------------------------------------------ main --- */

static int cmd_edit(espix_session_t *s, int argc, char **argv)
{
    /*
     * Everything starts here, because this is a builtin: its variables are the
     * kernel's, not a process image mapped fresh on every load. A second
     * \`sudo nano\` inherits whatever the first one left -- a \`quit\` still set makes
     * it exit on the first key, and a stale cursor row points past the line
     * table and faults in memcpy. An app is handed a clean image and never had
     * to say this; a builtin does.
     */
    doc.line  = NULL;
    doc.len   = NULL;
    doc.count = 0;
    doc.cap   = 0;
    cx = cy = 0;
    top = left = 0;
    modified = false;
    quit     = false;
    asking   = false;
    msg[0]   = '\0';
    path[0]  = '\0';
    out      = NULL;
    out_len  = 0;
    cut_buf  = NULL;
    cut_len  = 0;
    rows = 24;
    cols = 80;

    if (argc < 2) {
        espix_eprintf(s, "usage: edit <file>\n");
        return 1;
    }
    if (!espix_cmd_path(s, argv[1], path, sizeof(path))) {
        return 1;
    }

    g_s  = s;
    out  = malloc(OUT_MAX);
    g_in = espix_proc_stdin_open(s);

    if (out == NULL) {
        espix_eprintf(s, "edit: out of memory\n");
        return 1;
    }
    if (g_in == NULL) {
        espix_eprintf(s, "edit: no terminal to read\n");
        free(out);
        return 1;
    }
    if (!doc_init()) {
        espix_eprintf(s, "edit: out of memory\n");
        fclose(g_in);
        free(out);
        return 1;
    }

    query_size();
    if (!load_file()) {
        espix_eprintf(s, "edit: %s\n", msg);
        doc_free();
        fclose(g_in);
        free(out);
        return 1;
    }

    for (;;) {
        render();
        const int k = read_key();

        if (k < 0 || espix_shell_stopping(s)) {
            break;
        }
        if (asking) {
            if (k == 'y' || k == 'Y') {
                if (save_file()) {
                    quit = true;
                } else {
                    asking = false;
                }
            } else if (k == 'n' || k == 'N') {
                quit = true;
            } else if (k == 0x1b || k == 0x03) {
                asking = false;
                msg[0] = '\0';
            }
            if (quit) {
                break;
            }
            continue;
        }

        switch (k) {
        case 0x0f: save_file(); break;                  /* ^O */
        case 0x18:                                      /* ^X */
            if (modified) {
                asking = true;
                say("save modified buffer? (y/n)");
            } else {
                quit = true;
            }
            break;
        case 0x0b: cut_line(); break;                   /* ^K */
        case 0x15: paste_kill(); break;                 /* ^U */
        case 0x07: show_help(); break;                  /* ^G */
        case 0x0c: msg[0] = '\0'; query_size(); break;  /* ^L */
        case 0x01: move_home(); break;                  /* ^A */
        case 0x05: move_end(); break;                   /* ^E */
        case '\r': case '\n': ins_newline(); break;
        case 0x7f: case 0x08: do_backspace(); break;
        case '\t': {
            int sp = 8 - (cx % 8);

            while (sp-- > 0) {
                ins_char(' ');
            }
            break;
        }
        case KEY_LEFT:  move_horizontal(-1); break;
        case KEY_RIGHT: move_horizontal(1); break;
        case KEY_UP:    move_vertical(-1); break;
        case KEY_DOWN:  move_vertical(1); break;
        case KEY_HOME:  move_home(); break;
        case KEY_END:   move_end(); break;
        case KEY_PGUP:  move_vertical(-text_rows()); break;
        case KEY_PGDN:  move_vertical(text_rows()); break;
        case KEY_DEL:   do_delete(); break;
        case KEY_NONE:  break;
        default:
            if (k >= 32 && k < 127) {
                ins_char(k);
            }
            break;
        }
        if (quit) {
            break;
        }
    }

    o_reset();
    o_str("\033[2J\033[H");     /* leave a clean screen behind */
    o_flush();

    doc_free();
    free(cut_buf);
    cut_buf = NULL;
    fclose(g_in);
    free(out);
    out = NULL;
    return 0;
}

static espix_cmd_t s_edit_cmds[] = {
    { .name = "edit", .fn = cmd_edit, .stack = 16384,
      .help = "full-screen text editor (^O write, ^X exit)",
      .usage = "edit <file>" },
    /* The name somebody will type first. Same editor, same keys. */
    { .name = "nano", .fn = cmd_edit, .stack = 16384,
      .help = "alias for edit", .usage = "nano <file>" },
};

void espix_cmds_register_edit(void)
{
    espix_cmds_register_table(s_edit_cmds,
                              sizeof(s_edit_cmds) / sizeof(s_edit_cmds[0]));
}
