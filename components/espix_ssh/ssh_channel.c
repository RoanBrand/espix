/*
 * SSH session channel (RFC 4254), and the bridge from it to an espix session.
 *
 * One channel, of type "session", carrying an interactive shell. The client
 * requests a pty, which means it sends raw keystrokes and expects the *server*
 * to echo and to do the line editing — so the crude editor here is not a
 * shortcut around a missing feature, it is the feature.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

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

typedef struct {
    ssh_conn_t *conn;
    uint32_t    peer_chan;
    uint32_t    peer_window;
    uint32_t    peer_max_packet;
    uint32_t    local_window;
    bool        want_shell;
    bool        closed;

    uint16_t    cols;
    uint16_t    rows;

    /* Line being assembled from raw keystrokes. */
    char        line[ESPIX_LINE_MAX];
    size_t      line_len;

    /*
     * Bytes received but not yet consumed. One packet can hold more than one
     * line — a paste does — and the surplus has to survive until the shell asks
     * for the next line, or those commands are silently swallowed.
     */
    uint8_t     pending[MAX_PACKET];
    size_t      pending_len;
    size_t      pending_pos;

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
static int chan_write(espix_session_t *s, const char *data, size_t len)
{
    ssh_chan_t *ch = s->transport;
    size_t      start = 0;
    int         result = (int)len;

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

/*
 * Deliberately minimal: printable characters, backspace, Enter, Ctrl-C,
 * Ctrl-D. No history, no arrows, no completion — an escape sequence arrives as
 * ESC '[' 'A' and is simply discarded rather than being mistaken for input.
 *
 * The real editor is a separate piece of work, at which point the console moves
 * onto it too and linenoise retires. Until then this is enough to drive a shell.
 */
static bool consume_byte(ssh_chan_t *ch, uint8_t byte, bool *want_exit)
{
    switch (byte) {
    case '\r':
    case '\n':
        send_data(ch, "\r\n", 2);
        return true;            /* line complete */

    case 0x7f:                  /* DEL, which is what most terminals send */
    case 0x08:                  /* BS */
        if (ch->line_len > 0) {
            ch->line_len--;
            /* Move back, overwrite with a space, move back again. */
            send_data(ch, "\b \b", 3);
        }
        return false;

    case 0x03:                  /* Ctrl-C: abandon the line */
        send_data(ch, "^C\r\n", 4);
        ch->line_len = 0;
        return true;

    case 0x04:                  /* Ctrl-D: end of input, only on an empty line */
        if (ch->line_len == 0) {
            /* Close the prompt line first, or the client prints "Connection
             * closed" onto the end of it. */
            send_data(ch, "\r\n", 2);
            *want_exit = true;
            return true;
        }
        return false;

    default:
        break;
    }

    /* Ignore anything else non-printable, including the ESC that starts an
     * arrow key. Echoing it would corrupt the display; storing it would corrupt
     * the command. */
    if (byte < 0x20 || byte >= 0x7f) {
        return false;
    }

    if (ch->line_len + 1 < sizeof(ch->line)) {
        ch->line[ch->line_len++] = (char)byte;
        send_data(ch, (const char *)&byte, 1);
    }
    return false;
}

static esp_err_t handle_channel_request(ssh_chan_t *ch, ssh_buf_t *in);

/*
 * Drives the packet loop while waiting for a complete line, because everything
 * else the client may send — window adjustments, a resized terminal, a close —
 * arrives interleaved with keystrokes and has to be serviced.
 */
static int chan_read_line(espix_session_t *s, const char *prompt,
                          char *buf, size_t len)
{
    ssh_chan_t *ch = s->transport;
    ssh_conn_t *c  = ch->conn;

    if (ch->closed) {
        return -1;
    }

    ch->line_len = 0;
    if (prompt != NULL && send_data(ch, prompt, strlen(prompt)) != ESP_OK) {
        return -1;
    }

    for (;;) {
        /*
         * Drain what has already arrived before asking for more. Copying out of
         * the packet buffer is what makes this safe: echoing can block on the
         * peer's window, and blocking reads a packet, which decrypts straight
         * over the buffer the bytes would otherwise still be sitting in.
         */
        bool want_exit = false;
        while (ch->pending_pos < ch->pending_len) {
            const uint8_t key = ch->pending[ch->pending_pos++];
            if (consume_byte(ch, key, &want_exit)) {
                if (want_exit) {
                    ch->closed = true;
                    return -1;
                }
                ch->line[ch->line_len] = '\0';
                strlcpy(buf, ch->line, len);
                return (int)strlen(buf);
            }
        }

        xSemaphoreTake(ch->rx_lock, portMAX_DELAY);
        const esp_err_t rd = ssh_packet_read(c);
        xSemaphoreGive(ch->rx_lock);

        if (rd != ESP_OK) {
            ch->closed = true;
            return -1;
        }

        ssh_buf_t in;
        ssh_buf_read_from(&in, c->in_payload, c->in_len);
        const uint8_t msg = ssh_get_u8(&in);

        switch (msg) {
        case SSH_MSG_CHANNEL_DATA: {
            ssh_get_u32(&in);           /* recipient channel */
            size_t         n = 0;
            const uint8_t *data = ssh_get_string(&in, &n);
            if (in.bad || data == NULL) {
                return -1;
            }

            /* The client is bound by the maximum packet size we advertised, so
             * anything larger is a protocol violation rather than a resize. */
            if (n > sizeof(ch->pending)) {
                espix_klog(ESPIX_KLOG_WARN, TAG, "oversized channel data (%u)",
                           (unsigned)n);
                return -1;
            }
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
            ch->closed = true;
            return -1;

        case SSH_MSG_IGNORE:
        case SSH_MSG_DEBUG:
            break;

        case SSH_MSG_DISCONNECT:
            ch->closed = true;
            return -1;

        default:
            /* Not fatal: an unknown request mid-session is better ignored than
             * treated as a reason to drop someone's shell. */
            espix_klog(ESPIX_KLOG_DEBUG, TAG, "ignoring message %u", msg);
            break;
        }
    }
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
        ch->cols = (uint16_t)ssh_get_u32(in);
        ch->rows = (uint16_t)ssh_get_u32(in);
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

    if (REQ_IS("window-change")) {
        ch->cols = (uint16_t)ssh_get_u32(in);
        ch->rows = (uint16_t)ssh_get_u32(in);
        /* Never carries want_reply per RFC 4254 §6.7. */
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
        .cols         = 80,
        .rows         = 24,
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
    while (!ch.want_shell && !ch.closed) {
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

    espix_klog(ESPIX_KLOG_INFO, TAG, "shell for %s (%ux%u)", c->user,
               ch.cols, ch.rows);

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
