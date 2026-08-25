/*
 * SSH session channel (RFC 4254), and the bridge from it to an espix session.
 *
 * One channel, of type "session", carrying an interactive shell. The client
 * requests a pty, which means it sends raw keystrokes and expects the *server*
 * to echo and to do the line editing. That work is esp_linenoise's, driven
 * through the read/write callbacks below — the same editor the serial console
 * runs, with its own instance and its own history.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <sys/select.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_linenoise.h"

#include "espix_auth.h"
#include "espix_kernel.h"
#include "espix_proc.h"
#include "espix_shell.h"
#include "ssh_priv.h"

#define TAG "sshchan"

/*
 * Our receive window. The client only ever sends keystrokes, so this is never
 * under pressure; it is replenished at half consumption to keep the arithmetic
 * honest rather than because it matters.
 */
#define LOCAL_WINDOW  32768

/*
 * Maximum payload we let the client put in one CHANNEL_DATA. Deliberately
 * small: it bounds the buffer keystrokes are copied into, and a channel
 * carrying typing never needs more. A paste simply arrives as several packets.
 */
#define MAX_PACKET    256

/* Bound on packets read while waiting for the peer to open its window. A client
 * that never adjusts is broken or hostile; either way we should not spin. */
#define WINDOW_WAIT_PACKETS 64

/* How long a writing task waits for the read side before giving up on it. */
#define RX_WAIT_MS 250

/*
 * Fallback terminal size, and the floor under whatever a client asks for.
 *
 * A client may legitimately request a pty of 0x0 when it has no terminal of its
 * own, and the editor divides by the width to work out how many rows a line
 * occupies — so a zero reaches it as a divide-by-zero exception and takes the
 * connection task down. Clamping is not defensive padding: no terminal is zero
 * columns wide, and the editor has no way to say so.
 */
#define TERM_COLS_MIN 20
#define TERM_ROWS_MIN 2
#define TERM_COLS_DEF 80
#define TERM_ROWS_DEF 24

/* Concurrent sessions the fd -> channel map has room for. ssh_server.c admits
 * one connection at a time today; the map is sized to outlive that. */
#define SSH_MAX_SESSIONS 4

typedef struct {
    ssh_conn_t *conn;
    uint32_t    peer_chan;
    uint32_t    peer_window;
    uint32_t    peer_max_packet;
    uint32_t    local_window;
    bool        want_shell;
    bool        want_sftp;
    bool        closed;

    uint16_t    cols;
    uint16_t    rows;

    /* Line editing, one instance per session — which is the whole reason espix
     * uses esp_linenoise rather than IDF's, whose history and callbacks are
     * file-scope statics. */
    esp_linenoise_handle_t editor;
    espix_history_t       *history;   /* owned by the user, not by us */
    int64_t                pace_us;

    /*
     * Bytes received but not yet consumed. One packet can hold more than one
     * line — a paste does — and the surplus has to survive until the shell asks
     * for the next line, or those commands are silently swallowed.
     */
    uint8_t     pending[MAX_PACKET];
    size_t      pending_len;
    size_t      pending_pos;
    bool        last_was_cr;    /* for collapsing CR LF into one newline */

    /* Answers to terminal queries we handle ourselves rather than forwarding
     * to the client — see ssh_edit_write(). Read back before real input. */
    uint8_t     injected[24];
    size_t      injected_len;
    size_t      injected_pos;

    /*
     * Once an app's stdout points here, two tasks can write to one connection.
     * Packets must not interleave — they share out_buf and a sequence number
     * the MAC covers — so tx serialises them. rx exists because a blocked write
     * needs to read a window adjustment, and two readers on one socket would
     * swallow each other's packets.
     */
    SemaphoreHandle_t tx_lock;      /* recursive: chan_write() nests send_data() */
    SemaphoreHandle_t rx_lock;
} ssh_chan_t;

/* ------------------------------------------------------------------ */
/* Sending                                                             */
/* ------------------------------------------------------------------ */

/*
 * Block until the peer reopens its window, servicing the adjustment ourselves.
 * Only ever needed after a large burst of output.
 */
static esp_err_t wait_for_window(ssh_chan_t *ch)
{
    ssh_conn_t *c = ch->conn;

    for (unsigned waited = 0;
         ch->peer_window == 0 && waited < WINDOW_WAIT_PACKETS; waited++) {

        if (xSemaphoreTake(ch->rx_lock, pdMS_TO_TICKS(RX_WAIT_MS)) != pdTRUE) {
            /*
             * The shell task is parked in a read waiting for a keystroke, so
             * only a backgrounded app reaches this. Racing it for the socket
             * would cost the user their input; losing this output is the
             * lesser harm, and it is at least said out loud.
             */
            espix_klog(ESPIX_KLOG_WARN, TAG,
                       "window closed while the read side is busy; output lost");
            return ESP_ERR_TIMEOUT;
        }

        const esp_err_t err = ssh_packet_read(c);
        if (err == ESP_OK &&
            c->in_payload[0] == SSH_MSG_CHANNEL_WINDOW_ADJUST) {
            ssh_buf_t in;
            ssh_buf_read_from(&in, c->in_payload, c->in_len);
            ssh_skip(&in, 1);
            ssh_get_u32(&in);               /* recipient channel */
            ch->peer_window += ssh_get_u32(&in);
        }
        /* Anything else read here is dropped, type-ahead included: the caller
         * is mid-write and cannot hand it to the line editor. */
        xSemaphoreGive(ch->rx_lock);

        if (err != ESP_OK) {
            ch->closed = true;
            return ESP_FAIL;
        }
    }

    if (ch->peer_window == 0) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "peer window never opened");
        ch->closed = true;
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* Write one packet under the transmit lock. */
static esp_err_t send_packet(ssh_chan_t *ch, ssh_buf_t *b)
{
    xSemaphoreTakeRecursive(ch->tx_lock, portMAX_DELAY);
    const esp_err_t err = ssh_packet_write(ch->conn, b);
    xSemaphoreGiveRecursive(ch->tx_lock);
    return err;
}

static esp_err_t send_data(ssh_chan_t *ch, const char *data, size_t len)
{
    ssh_conn_t *c = ch->conn;

    /* Held across the whole write, not per packet: a long line split over
     * several packets must not have another task's output spliced into it. */
    xSemaphoreTakeRecursive(ch->tx_lock, portMAX_DELAY);

    esp_err_t err = ESP_OK;

    while (len > 0 && !ch->closed) {
        /* Wait for window rather than truncating: dropping output silently is
         * far worse to debug than a stalled write. */
        if (ch->peer_window == 0) {
            err = wait_for_window(ch);
            if (err != ESP_OK) {
                break;
            }
        }

        size_t chunk = len;
        if (chunk > ch->peer_window) {
            chunk = ch->peer_window;
        }
        if (chunk > ch->peer_max_packet) {
            chunk = ch->peer_max_packet;
        }
        if (chunk > SSH_MAX_PACKET - 64) {
            chunk = SSH_MAX_PACKET - 64;
        }

        ssh_buf_t b;
        ssh_buf_init(&b, c->out_buf, sizeof(c->out_buf));
        ssh_put_u8(&b, SSH_MSG_CHANNEL_DATA);
        ssh_put_u32(&b, ch->peer_chan);
        ssh_put_string(&b, data, chunk);

        if (ssh_packet_write(c, &b) != ESP_OK) {
            ch->closed = true;
            err = ESP_FAIL;
            break;
        }

        ch->peer_window -= chunk;
        data += chunk;
        len  -= chunk;
    }

    xSemaphoreGiveRecursive(ch->tx_lock);
    return (ch->closed && err == ESP_OK) ? ESP_FAIL : err;
}

/*
 * espix commands end lines with "\n". A pty expects "\r\n" — without the
 * carriage return every line starts where the previous one ended and the output
 * staircases down the screen. Translating here keeps every command unaware of
 * which transport it is writing to.
 */
/*
 * Write with LF turned into CR LF — the output half of a line discipline's job
 * (ONLCR), matching the CR to LF we do on input. Without it a bare "\n" drops
 * the cursor a row without returning it to column one, so every line starts
 * indented under wherever the last one ended.
 *
 * The serial console never needs this: IDF's UART VFS is configured with
 * ESP_LINE_ENDINGS_CRLF and does the same translation on write. Over SSH
 * nothing sits between the editor and the client except us.
 */
static int send_cooked(ssh_chan_t *ch, const char *data, size_t len)
{
    size_t start = 0;
    int    result = (int)len;

    /* One write stays one burst even though it is emitted line by line, so an
     * app's output and the shell's cannot end up interleaved mid-message. */
    xSemaphoreTakeRecursive(ch->tx_lock, portMAX_DELAY);

    for (size_t i = 0; i < len && result >= 0; i++) {
        if (data[i] != '\n') {
            continue;
        }
        if ((i > start && send_data(ch, data + start, i - start) != ESP_OK) ||
            send_data(ch, "\r\n", 2) != ESP_OK) {
            result = -1;
            break;
        }
        start = i + 1;
    }

    if (result >= 0 && start < len &&
        send_data(ch, data + start, len - start) != ESP_OK) {
        result = -1;
    }

    xSemaphoreGiveRecursive(ch->tx_lock);
    return result;
}

static int chan_write(espix_session_t *s, const char *data, size_t len)
{
    return send_cooked(s->transport, data, len);
}

/*
 * stdio adapter for apps. A raw descriptor cannot be used — channel output has
 * to be wrapped in CHANNEL_DATA and encrypted — so an app's stdout is a
 * funopen() stream landing back in chan_write().
 */
static int chan_stream_write(void *cookie, const char *data, int len)
{
    if (len <= 0) {
        return 0;
    }
    return (chan_write(cookie, data, (size_t)len) < 0) ? -1 : len;
}

/*
 * One stream per caller. The task that receives it owns it: FreeRTOS teardown
 * runs esp_cleanup_r(), which fcloses a task's stdout and stderr when they are
 * not the global ones, so these must not be shared or closed here.
 */
static FILE *chan_open_stream(espix_session_t *s)
{
    FILE *f = funopen(s, NULL, chan_stream_write, NULL, NULL);
    if (f != NULL) {
        /* Line buffered: output should appear as it is produced, but a packet
         * per character would be absurd. */
        setvbuf(f, NULL, _IOLBF, 128);
    }
    return f;
}

/* Record a size a client asked for, refusing nonsense rather than passing it
 * to an editor that will divide by it. */
static void set_term_size(ssh_chan_t *ch, uint32_t cols, uint32_t rows)
{
    if (cols < TERM_COLS_MIN || cols > UINT16_MAX) {
        espix_klog(ESPIX_KLOG_DEBUG, TAG,
                   "client asked for %u columns; using %d", (unsigned)cols,
                   TERM_COLS_DEF);
        cols = TERM_COLS_DEF;
    }
    if (rows < TERM_ROWS_MIN || rows > UINT16_MAX) {
        rows = TERM_ROWS_DEF;
    }
    ch->cols = (uint16_t)cols;
    ch->rows = (uint16_t)rows;
}

static void adjust_local_window(ssh_chan_t *ch, uint32_t consumed)
{
    ch->local_window -= consumed;
    if (ch->local_window > LOCAL_WINDOW / 2) {
        return;
    }

    const uint32_t add = LOCAL_WINDOW - ch->local_window;
    ssh_buf_t      b;
    ssh_buf_init(&b, ch->conn->out_buf, sizeof(ch->conn->out_buf));
    ssh_put_u8(&b, SSH_MSG_CHANNEL_WINDOW_ADJUST);
    ssh_put_u32(&b, ch->peer_chan);
    ssh_put_u32(&b, add);

    if (send_packet(ch, &b) == ESP_OK) {
        ch->local_window += add;
    }
}

/* ------------------------------------------------------------------ */
/* Line editing                                                        */
/* ------------------------------------------------------------------ */

static esp_err_t handle_channel_request(ssh_chan_t *ch, ssh_buf_t *in);

/*
 * esp_linenoise hands its read/write callbacks an int fd and no context
 * pointer, so the fd has to identify the session. We pass the real socket —
 * which is never read or written directly here, both callbacks being supplied,
 * but is genuinely ours and already unique per connection.
 *
 * Only connection tasks touch this, and ssh_server.c admits one at a time.
 */
static struct {
    int         fd;
    ssh_chan_t *ch;
} s_editor_map[SSH_MAX_SESSIONS];

static void editor_map_add(int fd, ssh_chan_t *ch)
{
    for (size_t i = 0; i < sizeof(s_editor_map) / sizeof(s_editor_map[0]); i++) {
        if (s_editor_map[i].ch == NULL) {
            s_editor_map[i].fd = fd;
            s_editor_map[i].ch = ch;
            return;
        }
    }
}

static void editor_map_remove(int fd)
{
    for (size_t i = 0; i < sizeof(s_editor_map) / sizeof(s_editor_map[0]); i++) {
        if (s_editor_map[i].fd == fd) {
            s_editor_map[i].ch = NULL;
        }
    }
}

static ssh_chan_t *editor_map_get(int fd)
{
    for (size_t i = 0; i < sizeof(s_editor_map) / sizeof(s_editor_map[0]); i++) {
        if (s_editor_map[i].ch != NULL && s_editor_map[i].fd == fd) {
            return s_editor_map[i].ch;
        }
    }
    return NULL;
}

/*
 * Read one packet and act on it. Everything the client may send — window
 * adjustments, a resized terminal, a close — arrives interleaved with
 * keystrokes and has to be serviced, which is why the editor's input path is a
 * packet loop rather than a socket read.
 */
static esp_err_t chan_pump(ssh_chan_t *ch)
{
    ssh_conn_t *c = ch->conn;

    xSemaphoreTake(ch->rx_lock, portMAX_DELAY);
    const esp_err_t rd = ssh_packet_read(c);
    xSemaphoreGive(ch->rx_lock);

    if (rd != ESP_OK) {
        ch->closed = true;
        return ESP_FAIL;
    }

    ssh_buf_t in;
    ssh_buf_read_from(&in, c->in_payload, c->in_len);
    const uint8_t msg = ssh_get_u8(&in);

    switch (msg) {
    case SSH_MSG_CHANNEL_DATA: {
        ssh_get_u32(&in);               /* recipient channel */
        size_t         n = 0;
        const uint8_t *data = ssh_get_string(&in, &n);
        if (in.bad || data == NULL) {
            return ESP_FAIL;
        }

        /* The client is bound by the maximum packet size we advertised, so
         * anything larger is a protocol violation rather than a resize. */
        if (n > sizeof(ch->pending)) {
            espix_klog(ESPIX_KLOG_WARN, TAG, "oversized channel data (%u)",
                       (unsigned)n);
            return ESP_FAIL;
        }

        /*
         * Copied out of the packet buffer before anything else can run: the
         * editor echoes as it consumes, echoing can block on the peer's window,
         * and blocking reads a packet — which decrypts straight over the buffer
         * these bytes would otherwise still be sitting in.
         */
        memcpy(ch->pending, data, n);
        ch->pending_len = n;
        ch->pending_pos = 0;
        adjust_local_window(ch, (uint32_t)n);
        break;
    }

    case SSH_MSG_CHANNEL_WINDOW_ADJUST:
        ssh_get_u32(&in);
        ch->peer_window += ssh_get_u32(&in);
        break;

    case SSH_MSG_CHANNEL_REQUEST:
        handle_channel_request(ch, &in);
        break;

    case SSH_MSG_CHANNEL_EOF:
    case SSH_MSG_CHANNEL_CLOSE:
    case SSH_MSG_DISCONNECT:
        ch->closed = true;
        return ESP_FAIL;

    case SSH_MSG_IGNORE:
    case SSH_MSG_DEBUG:
        break;

    default:
        /* Not fatal: an unknown request mid-session is better ignored than
         * treated as a reason to drop someone's shell. */
        espix_klog(ESPIX_KLOG_DEBUG, TAG, "ignoring message %u", msg);
        break;
    }

    return ESP_OK;
}

/*
 * Input side of the editor: hand it whatever has arrived, pumping packets until
 * something has. Returning 0 means end of input, which is how a dropped
 * connection reaches the shell as EOF.
 *
 * Enter is translated from CR to LF on the way through, because the editor
 * tests for LF and a pty client sends CR. The serial console never needed this:
 * IDF's UART VFS is configured with ESP_LINE_ENDINGS_CR and does the same
 * translation before the editor sees a byte. Here espix *is* the pty, so the
 * line discipline's job — ICRNL — is ours. A CR LF pair collapses to one
 * newline rather than submitting twice.
 */
static ssize_t ssh_edit_read(int fd, void *buf, size_t count)
{
    ssh_chan_t *ch = editor_map_get(fd);

    if (ch == NULL || count == 0) {
        return -1;
    }

    uint8_t *out = buf;
    size_t   got = 0;

    /* Synthetic replies jump the queue and bypass CR translation: they are ours
     * and already correct. */
    while (got < count && ch->injected_pos < ch->injected_len) {
        out[got++] = ch->injected[ch->injected_pos++];
    }
    if (got > 0) {
        return (ssize_t)got;
    }

    while (got < count) {
        /* Pace delivery so the editor takes its refreshing path rather than
         * its paste path — see ESPIX_PACE_MS. */
        if (got == 0 && ch->pending_pos < ch->pending_len) {
            espix_pace(&ch->pace_us);
        }

        if (ch->pending_pos >= ch->pending_len) {
            if (got > 0) {
                break;          /* deliver what we have rather than block */
            }
            if (ch->closed || chan_pump(ch) != ESP_OK) {
                return 0;
            }
            continue;
        }

        const uint8_t byte = ch->pending[ch->pending_pos++];

        if (byte == '\n' && ch->last_was_cr) {
            ch->last_was_cr = false;
            continue;           /* second half of a CR LF pair */
        }
        ch->last_was_cr = (byte == '\r');

        out[got++] = (byte == '\r') ? '\n' : byte;
    }

    return (ssize_t)got;
}

/*
 * Does this write consist of exactly `lit`? The library builds these with
 * sizeof() on a string literal, so the trailing NUL is usually included —
 * accept it either way.
 */
static bool seq_is(const void *buf, size_t count, const char *lit)
{
    const size_t n = strlen(lit);
    const uint8_t *b = buf;

    if (count != n && count != n + 1) {
        return false;
    }
    if (count == n + 1 && b[n] != '\0') {
        return false;
    }
    return memcmp(b, lit, n) == 0;
}

static void inject(ssh_chan_t *ch, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf((char *)ch->injected, sizeof(ch->injected), fmt, ap);
    va_end(ap);

    ch->injected_len = (n > 0) ? (size_t)n : 0;
    ch->injected_pos = 0;
}

/*
 * Output side, sharing the session's LF to CR LF translation. The editor emits
 * bare newlines of its own — esp_linenoise_raw() ends every line with one —
 * and they need cooking just as command output does. Its escape sequences are
 * unaffected: they carry bare CR, never LF.
 *
 * Terminal geometry queries are answered here rather than forwarded. A real pty
 * does not ask the terminal how wide it is — the size is state the driver holds,
 * and espix already has it from pty-req and window-change. Round-tripping the
 * question to the client instead means racing the user's own keystrokes: an
 * arrow key arriving while the reply is awaited gets parsed as the reply, which
 * both corrupts the width and swallows the keypress. Answering locally also
 * spares two round-trips before every prompt.
 */
static ssize_t ssh_edit_write(int fd, const void *buf, size_t count)
{
    ssh_chan_t *ch = editor_map_get(fd);

    if (ch == NULL) {
        return -1;
    }
    if (count == 0) {
        return 0;
    }

    /* Cursor position: answer with the far corner, so the caller reads the same
     * value whether it asks before or after moving to the right margin. It
     * therefore measures the true width and skips its restore sequence. */
    if (seq_is(buf, count, "\x1b[6n")) {
        inject(ch, "\x1b[%u;%uR", (unsigned)ch->rows, (unsigned)ch->cols);
        return (ssize_t)count;
    }

    /* Device status: the client asked for a pty, so it is a terminal. */
    if (seq_is(buf, count, "\x1b[5n")) {
        inject(ch, "\x1b[0n");
        return (ssize_t)count;
    }

    /* Only ever sent to find the right margin, which we already know. */
    if (seq_is(buf, count, "\x1b[999C")) {
        return (ssize_t)count;
    }

    if (send_cooked(ch, buf, count) < 0) {
        return -1;
    }
    return (ssize_t)count;
}

/*
 * Apply anything the client has already sent before the editor asks how wide
 * the terminal is.
 *
 * window-change keeps ch->cols current, but packets are only read when the
 * editor blocks for input. A resize that lands while a command is running
 * therefore sits unprocessed, and the next get_line() measures before its first
 * read — so the prompt after a resize would be drawn at the old width for no
 * reason. The line that was already part-typed when the resize happened cannot
 * be rescued (the editor samples the width once and offers no way to update
 * it), but the one after it can.
 */
static void chan_drain_pending(ssh_chan_t *ch)
{
    while (!ch->closed && ch->pending_pos >= ch->pending_len) {
        fd_set         rfds;
        struct timeval tv = { .tv_sec = 0, .tv_usec = 0 };

        FD_ZERO(&rfds);
        FD_SET(ch->conn->fd, &rfds);

        if (select(ch->conn->fd + 1, &rfds, NULL, NULL, &tv) <= 0) {
            return;             /* nothing waiting */
        }
        if (chan_pump(ch) != ESP_OK) {
            return;
        }
        /* A keystroke that arrived early belongs to the editor, and pumping
         * again would overwrite the buffer holding it. */
    }
}

/*
 * Raw channel I/O for a subsystem. Deliberately not chan_write()/ssh_edit_read():
 * those cook line endings for a terminal, which would corrupt binary data.
 */
static ssh_chan_t *s_raw_chan;

esp_err_t ssh_channel_send_raw(ssh_conn_t *c, const void *data, size_t len)
{
    (void)c;
    if (s_raw_chan == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return send_data(s_raw_chan, data, len);
}

esp_err_t ssh_channel_recv_raw(ssh_conn_t *c, uint8_t **out, size_t *out_len)
{
    (void)c;
    ssh_chan_t *ch = s_raw_chan;

    if (ch == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    while (ch->pending_pos >= ch->pending_len) {
        if (ch->closed || chan_pump(ch) != ESP_OK) {
            return ESP_FAIL;
        }
    }

    *out     = ch->pending + ch->pending_pos;
    *out_len = ch->pending_len - ch->pending_pos;
    ch->pending_pos = ch->pending_len;
    return ESP_OK;
}

static int chan_read_line(espix_session_t *s, const char *prompt,
                          char *buf, size_t len)
{
    ssh_chan_t *ch = s->transport;

    if (ch->closed) {
        return -1;
    }

    chan_drain_pending(ch);

    esp_linenoise_set_prompt(ch->editor, prompt);

    /* get_line() returns ESP_OK for an empty line without writing the buffer,
     * so anything left here from last time would be run as a command. */
    buf[0] = '\0';

    if (esp_linenoise_get_line(ch->editor, buf, len) != ESP_OK) {
        /*
         * Two very different keys land here. The editor sets errno to EAGAIN
         * for Ctrl-C, which abandons the line and should leave the user at a
         * fresh prompt, and leaves it alone for Ctrl-D on an empty line, which
         * is end of input and ends the session. Treating both as the end
         * dropped the connection on Ctrl-C — not what any other shell does.
         */
        if (errno == EAGAIN) {
            return 0;
        }
        return -1;
    }

    if (buf[0] != '\0') {
        espix_history_push(ch->history, buf);
        espix_history_apply(ch->history, ch->editor);
    }

    return (int)strlen(buf);
}

/* ------------------------------------------------------------------ */
/* Channel requests                                                    */
/* ------------------------------------------------------------------ */

static esp_err_t reply_request(ssh_chan_t *ch, bool ok)
{
    ssh_buf_t b;
    ssh_buf_init(&b, ch->conn->out_buf, sizeof(ch->conn->out_buf));
    ssh_put_u8(&b, ok ? SSH_MSG_CHANNEL_SUCCESS : SSH_MSG_CHANNEL_FAILURE);
    ssh_put_u32(&b, ch->peer_chan);
    return send_packet(ch, &b);
}

static esp_err_t handle_channel_request(ssh_chan_t *ch, ssh_buf_t *in)
{
    ssh_get_u32(in);                    /* recipient channel */

    size_t         type_len = 0;
    const uint8_t *type = ssh_get_string(in, &type_len);
    if (in->bad || type == NULL) {
        return ESP_ERR_INVALID_SIZE;
    }
    const bool want_reply = ssh_get_bool(in);

#define REQ_IS(s) (type_len == strlen(s) && memcmp(type, (s), type_len) == 0)

    if (REQ_IS("pty-req")) {
        ssh_get_string(in, NULL);       /* TERM */
        {
            const uint32_t cols = ssh_get_u32(in);
            const uint32_t rows = ssh_get_u32(in);
            set_term_size(ch, cols, rows);
        }
        /* Pixel dimensions and the encoded terminal modes follow; neither
         * matters until there is something that draws. */
        espix_klog(ESPIX_KLOG_DEBUG, TAG, "pty %ux%u", ch->cols, ch->rows);
        if (want_reply) {
            reply_request(ch, true);
        }
        return ESP_OK;
    }

    if (REQ_IS("shell")) {
        ch->want_shell = true;
        if (want_reply) {
            reply_request(ch, true);
        }
        return ESP_OK;
    }

    if (REQ_IS("subsystem")) {
        size_t         name_len = 0;
        const uint8_t *name = ssh_get_string(in, &name_len);
        const bool     is_sftp = (!in->bad && name != NULL &&
                                  name_len == strlen("sftp") &&
                                  memcmp(name, "sftp", name_len) == 0);
        if (is_sftp) {
            ch->want_sftp = true;
        } else {
            espix_klog(ESPIX_KLOG_INFO, TAG, "refused subsystem '%.*s'",
                       (int)name_len, (const char *)(name ? name : (const uint8_t *)""));
        }
        if (want_reply) {
            reply_request(ch, is_sftp);
        }
        return ESP_OK;
    }

    if (REQ_IS("window-change")) {
        {
            const uint32_t cols = ssh_get_u32(in);
            const uint32_t rows = ssh_get_u32(in);
            set_term_size(ch, cols, rows);
        }
        /* Never carries want_reply per RFC 4254 §6.7. Load-bearing: this is
         * what ssh_edit_write() answers cursor-position queries with, so a
         * resize takes effect on the next prompt. */
        return ESP_OK;
    }

    if (REQ_IS("env")) {
        /* Accepted and discarded: espix has no environment yet, and refusing
         * makes clients that send LANG print a warning for no reason. */
        if (want_reply) {
            reply_request(ch, true);
        }
        return ESP_OK;
    }

    /* exec and subsystem are how scp and sftp would arrive. Refusing cleanly
     * means `scp` reports a sensible error instead of hanging. */
    espix_klog(ESPIX_KLOG_INFO, TAG, "refused channel request '%.*s'",
               (int)type_len, (const char *)type);
    if (want_reply) {
        reply_request(ch, false);
    }
    return ESP_OK;

#undef REQ_IS
}

/* ------------------------------------------------------------------ */
/* Session                                                             */
/* ------------------------------------------------------------------ */

static esp_err_t send_exit_status(ssh_chan_t *ch, uint32_t status)
{
    ssh_buf_t b;
    ssh_buf_init(&b, ch->conn->out_buf, sizeof(ch->conn->out_buf));
    ssh_put_u8(&b, SSH_MSG_CHANNEL_REQUEST);
    ssh_put_u32(&b, ch->peer_chan);
    ssh_put_cstr(&b, "exit-status");
    ssh_put_u8(&b, 0);                  /* never wants a reply */
    ssh_put_u32(&b, status);
    return send_packet(ch, &b);
}

static void close_channel(ssh_chan_t *ch)
{
    ssh_buf_t b;

    ssh_buf_init(&b, ch->conn->out_buf, sizeof(ch->conn->out_buf));
    ssh_put_u8(&b, SSH_MSG_CHANNEL_EOF);
    ssh_put_u32(&b, ch->peer_chan);
    send_packet(ch, &b);

    ssh_buf_init(&b, ch->conn->out_buf, sizeof(ch->conn->out_buf));
    ssh_put_u8(&b, SSH_MSG_CHANNEL_CLOSE);
    ssh_put_u32(&b, ch->peer_chan);
    send_packet(ch, &b);
}

esp_err_t ssh_channel_run(ssh_conn_t *c)
{
    esp_err_t err = ESP_OK;

    ssh_chan_t ch = {
        .conn         = c,
        .local_window = LOCAL_WINDOW,
        .cols         = TERM_COLS_DEF,
        .rows         = TERM_ROWS_DEF,
        .tx_lock      = xSemaphoreCreateRecursiveMutex(),
        .rx_lock      = xSemaphoreCreateMutex(),
    };

    if (ch.tx_lock == NULL || ch.rx_lock == NULL) {
        goto out;
    }

    /* CHANNEL_OPEN */
    if (ssh_packet_read(c) != ESP_OK) {
        err = ESP_FAIL;
        goto out;
    }
    if (c->in_payload[0] != SSH_MSG_CHANNEL_OPEN) {
        err = ESP_ERR_INVALID_RESPONSE;
        goto out;
    }

    ssh_buf_t in;
    ssh_buf_read_from(&in, c->in_payload, c->in_len);
    ssh_skip(&in, 1);

    size_t         type_len = 0;
    const uint8_t *type = ssh_get_string(&in, &type_len);
    ch.peer_chan        = ssh_get_u32(&in);
    ch.peer_window      = ssh_get_u32(&in);
    ch.peer_max_packet  = ssh_get_u32(&in);

    if (in.bad || type == NULL) {
        err = ESP_ERR_INVALID_SIZE;
        goto out;
    }
    if (type_len != strlen("session") ||
        memcmp(type, "session", type_len) != 0) {
        ssh_buf_t b;
        ssh_buf_init(&b, c->out_buf, sizeof(c->out_buf));
        ssh_put_u8(&b, SSH_MSG_CHANNEL_OPEN_FAILURE);
        ssh_put_u32(&b, ch.peer_chan);
        ssh_put_u32(&b, 3);             /* unknown channel type */
        ssh_put_cstr(&b, "only session channels are supported");
        ssh_put_cstr(&b, "");
        ssh_packet_write(c, &b);
        err = ESP_ERR_NOT_SUPPORTED;
        goto out;
    }

    {
        ssh_buf_t b;
        ssh_buf_init(&b, c->out_buf, sizeof(c->out_buf));
        ssh_put_u8(&b, SSH_MSG_CHANNEL_OPEN_CONFIRMATION);
        ssh_put_u32(&b, ch.peer_chan);
        ssh_put_u32(&b, 0);             /* our channel id; only ever one */
        ssh_put_u32(&b, LOCAL_WINDOW);
        ssh_put_u32(&b, MAX_PACKET);
        if (ssh_packet_write(c, &b) != ESP_OK) {
            err = ESP_FAIL;
            goto out;
        }
    }

    /* Requests until the client asks for a shell. */
    while (!ch.want_shell && !ch.want_sftp && !ch.closed) {
        if (ssh_packet_read(c) != ESP_OK) {
            err = ESP_FAIL;
            goto out;
        }

        ssh_buf_t req;
        ssh_buf_read_from(&req, c->in_payload, c->in_len);
        const uint8_t msg = ssh_get_u8(&req);

        if (msg == SSH_MSG_CHANNEL_REQUEST) {
            handle_channel_request(&ch, &req);
        } else if (msg == SSH_MSG_CHANNEL_CLOSE ||
                   msg == SSH_MSG_CHANNEL_EOF ||
                   msg == SSH_MSG_DISCONNECT) {
            goto out;
        }
        /* Anything else before a shell is noise. */
    }

    /*
     * A subsystem carries bytes, not a terminal: no editor, no session, no
     * greeting. Everything below that belongs to an interactive shell would
     * corrupt a transfer.
     */
    if (ch.want_sftp) {
        s_raw_chan = &ch;
        espix_sftp_run(c);
        s_raw_chan = NULL;

        send_exit_status(&ch, 0);
        close_channel(&ch);
        goto out;
    }

    espix_klog(ESPIX_KLOG_INFO, TAG, "shell for %s (%ux%u)", c->user,
               ch.cols, ch.rows);

    /*
     * The editor identifies this session by the socket fd, since its callbacks
     * carry no context pointer. Register before creating the instance: probing
     * and the first prompt both call straight back into them.
     */
    editor_map_add(c->fd, &ch);

    /* Belongs to the user and outlives this connection, so reconnecting finds
     * what was typed last time. Not freed at logout. */
    ch.history = espix_history_for(c->user);

    esp_linenoise_config_t ed_cfg;
    esp_linenoise_get_instance_config_default(&ed_cfg);

    ed_cfg.in_fd               = c->fd;
    ed_cfg.out_fd              = c->fd;
    ed_cfg.max_cmd_line_length = ESPIX_LINE_MAX;
    ed_cfg.history_max_length  = 32;
    ed_cfg.allow_multi_line    = true;
    ed_cfg.allow_empty_line    = true;
    ed_cfg.completion_cb       = espix_shell_completion;
    ed_cfg.hints_cb            = espix_shell_hint;
    ed_cfg.read_bytes_cb       = ssh_edit_read;
    ed_cfg.write_bytes_cb      = ssh_edit_write;

    if (esp_linenoise_create_instance(&ed_cfg, &ch.editor) != ESP_OK) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "cannot create the line editor");
        editor_map_remove(c->fd);
        err = ESP_ERR_NO_MEM;
        goto out;
    }

    /* Load the user's history into this instance up front; otherwise the first
     * arrow-up of a reconnected session finds nothing until a command runs. */
    espix_history_apply(ch.history, ch.editor);

    /*
     * No esp_linenoise_probe() here: it calls fcntl() on in_fd directly and
     * gives up when that fails, and our fd is a socket the library never
     * actually reads. The client asked for a pty, so escape sequences are a
     * given. Terminal width still auto-detects, because the cursor-position
     * query goes out through the write callback and its reply comes back
     * through the read one.
     */

    espix_session_t session = {
        .name      = "ssh",
        .cwd       = "/",
        .read_line = chan_read_line,
        .write     = chan_write,
        .transport = &ch,
        .fg_pid    = ESPIX_PID_NONE,
        .ansi      = true,          /* the client asked for a pty */
        .open_stream = chan_open_stream,
    };
    strlcpy(session.user, c->user, sizeof(session.user));

    /* Identical to what the serial console prints, by construction: both go
     * through the same command. */
    espix_shell_exec(&session, "motd");

    espix_shell_session_run(&session);

    /*
     * Hang up: a backgrounded app writes through a stream whose cookie is this
     * session, which lives on this stack, so nothing may outlive it.
     */
    esp_linenoise_delete_instance(ch.editor);
    editor_map_remove(c->fd);

    const size_t orphans = espix_proc_hangup(&session);
    if (orphans > 0) {
        espix_klog(ESPIX_KLOG_INFO, TAG, "%s: killed %u process%s on logout",
                   c->user, (unsigned)orphans, orphans == 1 ? "" : "es");
    }

    /*
     * A process killed above may have died inside a write, still holding the
     * transmit lock — espix_proc_kill() deletes the task outright and cannot
     * unwind what it held. Probe the lock rather than block on it: closing the
     * connection abruptly costs the client a warning, whereas waiting forever
     * would strand this task and its buffers for the life of the system.
     */
    if (xSemaphoreTakeRecursive(ch.tx_lock, pdMS_TO_TICKS(RX_WAIT_MS)) == pdTRUE) {
        xSemaphoreGiveRecursive(ch.tx_lock);
        send_exit_status(&ch, (uint32_t)session.last_status);
        close_channel(&ch);
    } else {
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "transmit lock held by a killed process; closing abruptly");
    }

    espix_klog(ESPIX_KLOG_INFO, TAG, "%s logged out", c->user);

out:
    if (ch.tx_lock != NULL) {
        vSemaphoreDelete(ch.tx_lock);
    }
    if (ch.rx_lock != NULL) {
        vSemaphoreDelete(ch.rx_lock);
    }
    return err;
}
