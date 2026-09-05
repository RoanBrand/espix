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
#include <stdlib.h>
#include <string.h>

#include <sys/select.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_linenoise.h"

#include "sdkconfig.h"

#include "espix_auth.h"
#include "espix_kernel.h"
#include "espix_proc.h"
#include "espix_shell.h"
#include "ssh_priv.h"

#define TAG "sshchan"

/*
 * Our receive window: how much a peer may send before waiting for us to
 * acknowledge it. Purely a number we advertise — adjust_local_window() tops it
 * back up as bytes are consumed, and nothing buffers a window's worth — so a
 * large one is free.
 *
 * It was 32768, chosen when the channel only carried keystrokes. That silently
 * capped an upload at one window round-trip per 32KB, which an SFTP client
 * pipelining 64 requests will hit immediately.
 */
#define LOCAL_WINDOW  262144


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

/* Concurrent sessions the fd -> channel map has room for. Derived from the
 * connection limit rather than written out again: an undersized map does not
 * fail loudly, it leaves an editor unable to find its channel. */
#define SSH_MAX_SESSIONS CONFIG_ESPIX_SSH_MAX_SESSIONS

typedef struct {
    ssh_conn_t *conn;
    uint32_t    peer_chan;
    uint32_t    peer_window;
    uint32_t    peer_max_packet;
    uint32_t    local_window;
    bool        want_shell;
    bool        want_sftp;
    bool        closed;

    /*
     * `ssh host <cmd>`: the command the client asked to run, on the heap
     * because it arrives in the packet buffer that the next read overwrites.
     */
    bool        want_exec;
    char       *exec_cmd;

    /* Whether the client asked for a terminal. Decides whether output is
     * cooked and whether the session claims ANSI; see chan_write(). */
    bool        has_pty;
    bool        raw_out;

    /* The client has sent CHANNEL_EOF: no more input will arrive, but the
     * channel is still open for output. Distinct from `closed`, which means the
     * channel is gone in both directions. */
    bool        eof_seen;

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
    uint8_t     pending[SSH_CHANNEL_MAX_PACKET];
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
     *
     * Which is also why klog must never reach the console through a plain
     * printf(). It used to, and in a loaded app's task printf() is not the
     * console — proc_task() points stdout at the session, so a kernel log from
     * anywhere on this path landed back in chan_write() and re-entered the send
     * it was reporting on. tx_lock does not catch that: it is recursive by
     * design so chan_write() can nest send_data(), so the same task is admitted
     * rather than deadlocked, and the inner packet rebuilds out_buf and bumps
     * seq_out while the outer send is still using them.
     *
     * klog now writes to a console stream captured at boot (see s_console_out
     * in espix_kernel/klog.c), so the logs on this path can say what they mean
     * at the level they deserve. Do not reintroduce a task-relative printf
     * under here.
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

    /*
     * Held across the whole write, not per packet: a long line split over
     * several packets must not have another task's output spliced into it.
     *
     * And note what the lock is really guarding -- conn->out_buf, not just the
     * socket. Every writer on this connection builds its packet in that one
     * buffer, so the lock has to be held from the first ssh_put_* to the last
     * send. Control packets used to be filled outside it, which quietly
     * clobbered whatever was mid-flight.
     */
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

static esp_err_t chan_pump(ssh_chan_t *ch);

/*
 * Ctrl-C while a foreground process runs. Same shape as chan_drain_pending(),
 * but it inspects what it pumped rather than leaving it for the editor: the
 * editor is not running, the shell is blocked on the process, and bytes left in
 * the buffer would surface on the next prompt.
 */
static bool chan_poll_interrupt(espix_session_t *s)
{
    ssh_chan_t *ch  = s->transport;
    bool        hit = false;

    if (ch == NULL) {
        return false;
    }

    for (;;) {
        /* Anything already decrypted and waiting. */
        while (ch->pending_pos < ch->pending_len) {
            if (ch->pending[ch->pending_pos++] == 0x03) {
                hit = true;
            }
        }

        if (ch->closed) {
            return hit;
        }

        fd_set         rfds;
        struct timeval tv = { .tv_sec = 0, .tv_usec = 0 };

        FD_ZERO(&rfds);
        FD_SET(ch->conn->fd, &rfds);

        if (select(ch->conn->fd + 1, &rfds, NULL, NULL, &tv) <= 0) {
            return hit;                 /* nothing on the wire */
        }

        /*
         * No rx_lock here: chan_pump() takes it itself, and it is not
         * recursive, so wrapping the call deadlocks this task against itself.
         * That is not hypothetical -- it hung every foreground Ctrl-C over SSH
         * until it was removed.
         */
        if (chan_pump(ch) != ESP_OK) {
            return hit;
        }
    }
}

/*
 * Cooked for a terminal, raw otherwise.
 *
 * send_cooked() turns every \n into \r\n, which is right for a pty and wrong
 * for `ssh host 'cat /etc/motd' > file` -- that would put carriage returns in
 * the file. `raw_out` is set only when entering the exec path without a
 * pty-req, so an interactive shell is unaffected whatever it asked for.
 */
static int chan_write(espix_session_t *s, const char *data, size_t len)
{
    ssh_chan_t *ch = s->transport;

    if (ch->raw_out) {
        return (send_data(ch, data, len) == ESP_OK) ? (int)len : -1;
    }
    return send_cooked(ch, data, len);
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

    /*
     * The lock goes round the *building* of the packet as well as the send.
     * conn->out_buf is one buffer shared by every writer on this connection,
     * and send_data() holds this same lock while filling it -- so filling it
     * here unlocked would overwrite whatever an app's task is midway through
     * transmitting. That is not theoretical: it truncated `ssh host <cmd>`
     * output about a third of the time, because the app writes from its own
     * task while the connection task tears the session down.
     *
     * Recursive, so send_packet() taking it again is free.
     */
    xSemaphoreTakeRecursive(ch->tx_lock, portMAX_DELAY);

    ssh_buf_init(&b, ch->conn->out_buf, sizeof(ch->conn->out_buf));
    ssh_put_u8(&b, SSH_MSG_CHANNEL_WINDOW_ADJUST);
    ssh_put_u32(&b, ch->peer_chan);
    ssh_put_u32(&b, add);

    if (send_packet(ch, &b) == ESP_OK) {
        ch->local_window += add;
    }

    xSemaphoreGiveRecursive(ch->tx_lock);
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
 * Only connection tasks touch this, one entry per live connection.
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

    /* Cannot happen while the map is sized from the connection limit, but the
     * failure it would cause -- an editor whose callbacks find no channel --
     * looks like a dead terminal rather than a full table. */
    espix_klog(ESPIX_KLOG_ERROR, TAG, "editor map full; session will not work");
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

    /*
     * EOF is the client saying "I will send no more input", not "stop sending
     * output". Those are separate halves of a channel, and conflating them
     * truncated every command that outlived the message.
     *
     * `ssh host <cmd>` sends it immediately, having no stdin to forward. So a
     * foreground app's output was cut wherever chan_poll_interrupt() happened
     * to pump next -- at a 50ms poll, which is why the loss was a different
     * length every run and why a fast command never noticed. `eof_seen` marks
     * the input side done so a reader can stop waiting; writes carry on.
     */
    case SSH_MSG_CHANNEL_EOF:
        ch->eof_seen = true;
        return ESP_OK;

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
            /* eof_seen as well as closed: EOF no longer closes the channel,
             * and a reader that ignored it would spin forever waiting for
             * input the client has promised never to send. */
            if (ch->closed || ch->eof_seen || chan_pump(ch) != ESP_OK) {
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
        /* eof_seen ends the transfer as cleanly as a close would: the client
         * has said it will send nothing further, so there is no next request
         * to wait for. Without it this loop would never exit. */
        if (ch->closed || ch->eof_seen || chan_pump(ch) != ESP_OK) {
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

    /* Locked across the fill; see the note in adjust_local_window(). */
    xSemaphoreTakeRecursive(ch->tx_lock, portMAX_DELAY);

    ssh_buf_init(&b, ch->conn->out_buf, sizeof(ch->conn->out_buf));
    ssh_put_u8(&b, ok ? SSH_MSG_CHANNEL_SUCCESS : SSH_MSG_CHANNEL_FAILURE);
    ssh_put_u32(&b, ch->peer_chan);

    const esp_err_t err = send_packet(ch, &b);
    xSemaphoreGiveRecursive(ch->tx_lock);
    return err;
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
        ch->has_pty = true;
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

    /*
     * `ssh host <cmd>`. The command is one SSH string, and it points into the
     * connection's packet buffer -- which the next read overwrites -- so it is
     * copied before anything else can happen to it.
     *
     * Capped at the same length the line editor enforces on a typed line.
     * Refused rather than truncated over that: half a command is a different
     * command, and running one the client did not send is worse than failing.
     */
    if (REQ_IS("exec")) {
        size_t         cmd_len = 0;
        const uint8_t *cmd     = ssh_get_string(in, &cmd_len);
        bool           ok      = false;

        if (!in->bad && cmd != NULL && cmd_len > 0 && cmd_len < ESPIX_LINE_MAX) {
            ch->exec_cmd = malloc(cmd_len + 1);
            if (ch->exec_cmd != NULL) {
                memcpy(ch->exec_cmd, cmd, cmd_len);
                ch->exec_cmd[cmd_len] = '\0';
                ch->want_exec = true;
                ok = true;
            }
        } else if (cmd_len >= ESPIX_LINE_MAX) {
            espix_klog(ESPIX_KLOG_WARN, TAG,
                       "exec command of %u bytes exceeds the %d-byte limit",
                       (unsigned)cmd_len, ESPIX_LINE_MAX - 1);
        }

        if (want_reply) {
            reply_request(ch, ok);
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

    /* Refusing cleanly rather than ignoring: a client that asked for something
     * espix does not do gets an error instead of a hang. */
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

/*
 * Locked across the fill; see adjust_local_window(). This one is why the bug
 * was worth chasing: when the buffer got clobbered it was usually *this*
 * packet that lost, so the client received no exit status at all and reported
 * 255 for a command that had succeeded.
 */
static esp_err_t send_exit_status(ssh_chan_t *ch, uint32_t status)
{
    ssh_buf_t b;

    xSemaphoreTakeRecursive(ch->tx_lock, portMAX_DELAY);

    ssh_buf_init(&b, ch->conn->out_buf, sizeof(ch->conn->out_buf));
    ssh_put_u8(&b, SSH_MSG_CHANNEL_REQUEST);
    ssh_put_u32(&b, ch->peer_chan);
    ssh_put_cstr(&b, "exit-status");
    ssh_put_u8(&b, 0);                  /* never wants a reply */
    ssh_put_u32(&b, status);

    const esp_err_t err = send_packet(ch, &b);
    xSemaphoreGiveRecursive(ch->tx_lock);
    return err;
}

/* One lock across both packets, not one each: EOF and CLOSE belong together and
 * nothing should be able to land between them. */
static void close_channel(ssh_chan_t *ch)
{
    ssh_buf_t b;

    xSemaphoreTakeRecursive(ch->tx_lock, portMAX_DELAY);

    ssh_buf_init(&b, ch->conn->out_buf, sizeof(ch->conn->out_buf));
    ssh_put_u8(&b, SSH_MSG_CHANNEL_EOF);
    ssh_put_u32(&b, ch->peer_chan);
    send_packet(ch, &b);

    ssh_buf_init(&b, ch->conn->out_buf, sizeof(ch->conn->out_buf));
    ssh_put_u8(&b, SSH_MSG_CHANNEL_CLOSE);
    ssh_put_u32(&b, ch->peer_chan);
    send_packet(ch, &b);

    xSemaphoreGiveRecursive(ch->tx_lock);
}

/*
 * Start where a login would: the account's home directory.
 *
 * Falls back to / when the directory is not actually there. A rootfs can be
 * replaced wholesale by storage-flash or edited on the device, and refusing to
 * open a shell because a directory went missing would be a poor trade.
 */
static void apply_account(espix_session_t *session, const char *user)
{
    espix_user_t account;

    if (espix_auth_lookup(user, &account) != ESP_OK) {
        /*
         * Authentication passed but the account will not resolve -- /etc/passwd
         * changed underneath the login, or will no longer parse. Run as nobody
         * rather than leaving the credential as it was found: the session
         * struct is zero-initialised, and zero is root.
         */
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "%s: no account record; running as nobody", user);
        session->uid     = ESPIX_UID_NOBODY;
        session->gid     = ESPIX_UID_NOBODY;
        session->ngroups = 0;
        return;
    }

    session->uid = account.uid;
    session->gid = account.gid;

    /*
     * Every group this account is in, resolved now. The permission check reads
     * the set rather than the file, so a change to /etc/group takes effect at
     * the next login rather than mid-session -- which is what `newgrp` exists
     * for on a real system and is the same bargain espix makes for the uid.
     */
    session->ngroups = (uint8_t)espix_auth_groups(user, session->groups,
                                                  ESPIX_NGROUPS_MAX);

    if (account.home[0] == '\0') {
        return;
    }

    struct stat st;
    if (stat(account.home, &st) == 0 && S_ISDIR(st.st_mode)) {
        strlcpy(session->home, account.home, sizeof(session->home));
        strlcpy(session->cwd,  account.home, sizeof(session->cwd));
    } else {
        espix_klog(ESPIX_KLOG_WARN, TAG, "%s: no home at %s; starting at /",
                   user, account.home);
    }
}

/*
 * End a session: kill what it owns, then report and close.
 *
 * Anything backgrounded writes through a stream whose cookie is the session,
 * which lives on the caller's stack, so nothing may outlive it.
 *
 * The transmit lock is probed rather than waited on. A process killed just
 * above may have died inside a write still holding it -- espix_proc_kill()
 * deletes the task outright and cannot unwind what it held. Closing abruptly
 * costs the client a warning; blocking forever would strand this task and its
 * buffers for the life of the system.
 */
static void finish_session(ssh_chan_t *ch, espix_session_t *session)
{
    const size_t orphans = espix_proc_hangup(session);

    if (orphans > 0) {
        espix_klog(ESPIX_KLOG_INFO, TAG, "%s: killed %u process%s on exit",
                   session->user, (unsigned)orphans, orphans == 1 ? "" : "es");
    }

    if (xSemaphoreTakeRecursive(ch->tx_lock, pdMS_TO_TICKS(RX_WAIT_MS)) == pdTRUE) {
        /*
         * Held across the send, not released before it. Taking the lock and
         * giving it straight back only proved the lock had been free a moment
         * ago -- an app still finishing its output could acquire it in the gap
         * and be inside send_data() when the packets below overwrote the
         * buffer it was using. Keeping it means anything in flight has
         * finished, which is what the check was for.
         */
        send_exit_status(ch, (uint32_t)session->last_status);
        close_channel(ch);
        xSemaphoreGiveRecursive(ch->tx_lock);
    } else {
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "transmit lock held by a killed process; closing abruptly");
    }
}

esp_err_t ssh_channel_run(ssh_conn_t *c)
{
    esp_err_t err = ESP_OK;

    /*
     * On the heap, not this task's stack. The struct carries a whole channel
     * packet in `pending`, so at SSH_CHANNEL_MAX_PACKET it is over 2KB — and
     * this stack also runs every shell command, which needs the room far more
     * than a receive buffer does.
     */
    ssh_chan_t *ch = calloc(1, sizeof(*ch));
    if (ch == NULL) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "out of memory for a channel");
        return ESP_ERR_NO_MEM;
    }

    ch->conn         = c;
    ch->local_window = LOCAL_WINDOW;
    ch->cols         = TERM_COLS_DEF;
    ch->rows         = TERM_ROWS_DEF;
    ch->tx_lock      = xSemaphoreCreateRecursiveMutex();
    ch->rx_lock      = xSemaphoreCreateMutex();

    if (ch->tx_lock == NULL || ch->rx_lock == NULL) {
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
    ch->peer_chan        = ssh_get_u32(&in);
    ch->peer_window      = ssh_get_u32(&in);
    ch->peer_max_packet  = ssh_get_u32(&in);

    if (in.bad || type == NULL) {
        err = ESP_ERR_INVALID_SIZE;
        goto out;
    }
    if (type_len != strlen("session") ||
        memcmp(type, "session", type_len) != 0) {
        ssh_buf_t b;
        ssh_buf_init(&b, c->out_buf, sizeof(c->out_buf));
        ssh_put_u8(&b, SSH_MSG_CHANNEL_OPEN_FAILURE);
        ssh_put_u32(&b, ch->peer_chan);
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
        ssh_put_u32(&b, ch->peer_chan);
        ssh_put_u32(&b, 0);             /* our channel id; only ever one */
        ssh_put_u32(&b, LOCAL_WINDOW);
        ssh_put_u32(&b, SSH_CHANNEL_MAX_PACKET);
        if (ssh_packet_write(c, &b) != ESP_OK) {
            err = ESP_FAIL;
            goto out;
        }
    }

    /* Requests until the client asks for a shell. */
    while (!ch->want_shell && !ch->want_sftp && !ch->want_exec && !ch->closed) {
        if (ssh_packet_read(c) != ESP_OK) {
            err = ESP_FAIL;
            goto out;
        }

        ssh_buf_t req;
        ssh_buf_read_from(&req, c->in_payload, c->in_len);
        const uint8_t msg = ssh_get_u8(&req);

        if (msg == SSH_MSG_CHANNEL_REQUEST) {
            handle_channel_request(ch, &req);
        } else if (msg == SSH_MSG_CHANNEL_CLOSE ||
                   msg == SSH_MSG_CHANNEL_EOF ||
                   msg == SSH_MSG_DISCONNECT) {
            goto out;
        }
        /* Anything else before a shell is noise. */
    }

    /*
     * A subsystem carries bytes, not a terminal: no editor, no greeting, none
     * of what an interactive shell prints, all of which would corrupt a
     * transfer.
     *
     * It does get a session, though, which it did not before. Not for stdio --
     * SFTP prints nothing and spawns nothing -- but because a session is where
     * an identity lives, and without one every file operation SFTP made ran on
     * a task espix_fs_access_check() reads as the kernel. An authenticated
     * client could fetch files its own shell login was refused.
     */
    if (ch->want_sftp) {
        espix_session_t session = {
            .name      = "sftp",
            .cwd       = "/",
            .transport = ch,
            .fg_pid    = ESPIX_PID_NONE,
            .uid       = ESPIX_UID_NOBODY,
            .gid       = ESPIX_UID_NOBODY,
        };
        strlcpy(session.user, c->user, sizeof(session.user));
        apply_account(&session, c->user);

        s_raw_chan = ch;
        espix_sftp_run(c, &session);
        s_raw_chan = NULL;

        send_exit_status(ch, 0);
        close_channel(ch);
        goto out;
    }

    /*
     * `ssh host <cmd>`: the same dispatch an interactive shell uses, with one
     * string standing in for the line editor. No editor, no history, no motd,
     * and login is false so `logout` correctly declines -- nobody logged in to
     * run one command.
     */
    if (ch->want_exec) {
        espix_session_t session = {
            .name      = "ssh-exec",
            .cwd       = "/",
            .write     = chan_write,
            .poll_interrupt = chan_poll_interrupt,
            .transport = ch,
            /* Overwritten by apply_account(); nobody rather than 0 so that a
             * path which forgets to set it fails closed instead of open. */
            .uid       = ESPIX_UID_NOBODY,
            .gid       = ESPIX_UID_NOBODY,
            .fg_pid    = ESPIX_PID_NONE,
            .ansi      = ch->has_pty,
            .open_stream = chan_open_stream,
        };
        strlcpy(session.user, c->user, sizeof(session.user));
        apply_account(&session, c->user);

        /* Without a terminal the client is a pipe, so newlines stay bare. */
        ch->raw_out = !ch->has_pty;

        espix_klog(ESPIX_KLOG_INFO, TAG, "exec for %s: %s", c->user,
                   ch->exec_cmd);

        /*
         * scp's pre-9.0 protocol arrives as `scp -t <path>` on an exec channel,
         * and espix has no such command -- so it used to be refused outright,
         * which at least told the client something true. Now that exec works,
         * the same request would reach the shell and come back as "command not
         * found", which points at the wrong problem. Say what is actually
         * wrong instead.
         */
        if (strncmp(ch->exec_cmd, "scp ", 4) == 0 ||
            strcmp(ch->exec_cmd, "scp") == 0) {
            espix_printf(&session,
                         "espix: the scp protocol is not implemented; "
                         "espix serves scp over SFTP, so drop -O\n");
            session.last_status = 127;
        } else {
            espix_shell_set_current(&session);
            (void)espix_shell_run_line(&session, ch->exec_cmd);
            espix_shell_set_current(NULL);
        }

        finish_session(ch, &session);
        goto out;
    }

    espix_klog(ESPIX_KLOG_INFO, TAG, "shell for %s (%ux%u)", c->user,
               ch->cols, ch->rows);

    /*
     * The editor identifies this session by the socket fd, since its callbacks
     * carry no context pointer. Register before creating the instance: probing
     * and the first prompt both call straight back into them.
     */
    editor_map_add(c->fd, ch);

    /* Belongs to the user and outlives this connection, so reconnecting finds
     * what was typed last time. Not freed at logout. */
    ch->history = espix_history_for(c->user);

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

    if (esp_linenoise_create_instance(&ed_cfg, &ch->editor) != ESP_OK) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "cannot create the line editor");
        editor_map_remove(c->fd);
        err = ESP_ERR_NO_MEM;
        goto out;
    }

    /* Load the user's history into this instance up front; otherwise the first
     * arrow-up of a reconnected session finds nothing until a command runs. */
    espix_history_apply(ch->history, ch->editor);

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
        .poll_interrupt = chan_poll_interrupt,
        .transport = ch,
        /* Overwritten by apply_account(); nobody rather than 0 so that a
         * path which forgets to set it fails closed instead of open. */
        .uid       = ESPIX_UID_NOBODY,
        .gid       = ESPIX_UID_NOBODY,
        .fg_pid    = ESPIX_PID_NONE,
        .ansi      = true,          /* the client asked for a pty */
        .open_stream = chan_open_stream,
    };
    strlcpy(session.user, c->user, sizeof(session.user));

    /*
     * A login shell, so `logout` is valid here, and it starts in the user's
     * home directory the way a login does everywhere else. The home comes from
     * the account record, whose `home` field espix_auth has always filled in
     * and nothing has read until now.
     *
     * Falls back to / when the directory is not actually there. A rootfs can be
     * replaced wholesale by storage-flash or edited on the device, and refusing
     * to open a shell because a directory went missing would be a poor trade.
     */
    session.login = true;
    apply_account(&session, c->user);

    /* Identical to what the serial console prints, by construction: both go
     * through the same command. */
    espix_shell_exec(&session, "motd");

    espix_shell_session_run(&session);

    esp_linenoise_delete_instance(ch->editor);
    editor_map_remove(c->fd);

    finish_session(ch, &session);

    espix_klog(ESPIX_KLOG_INFO, TAG, "%s logged out", c->user);

out:
    if (ch->tx_lock != NULL) {
        vSemaphoreDelete(ch->tx_lock);
    }
    if (ch->rx_lock != NULL) {
        vSemaphoreDelete(ch->rx_lock);
    }
    free(ch->exec_cmd);
    free(ch);
    return err;
}
