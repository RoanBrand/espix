/*
 * SSH session channel (RFC 4254), and the bridge from it to an espix session.
 *
 * One channel, of type "session", carrying an interactive shell. The client
 * requests a pty, which means it sends raw keystrokes and expects the *server*
 * to echo and to do the line editing — so the crude editor here is not a
 * shortcut around a missing feature, it is the feature.
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "espix_auth.h"
#include "espix_kernel.h"
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
} ssh_chan_t;

/* ------------------------------------------------------------------ */
/* Sending                                                             */
/* ------------------------------------------------------------------ */

static esp_err_t send_data(ssh_chan_t *ch, const char *data, size_t len)
{
    ssh_conn_t *c = ch->conn;

    while (len > 0 && !ch->closed) {
        /* Wait for window rather than truncating: dropping output silently is
         * far worse to debug than a stalled write. */
        unsigned waited = 0;
        while (ch->peer_window == 0 && waited++ < WINDOW_WAIT_PACKETS) {
            if (ssh_packet_read(c) != ESP_OK) {
                ch->closed = true;
                return ESP_FAIL;
            }
            if (c->in_payload[0] == SSH_MSG_CHANNEL_WINDOW_ADJUST) {
                ssh_buf_t in;
                ssh_buf_read_from(&in, c->in_payload, c->in_len);
                ssh_skip(&in, 1);
                ssh_get_u32(&in);                   /* recipient channel */
                ch->peer_window += ssh_get_u32(&in);
            }
        }
        if (ch->peer_window == 0) {
            espix_klog(ESPIX_KLOG_WARN, TAG, "peer window never opened");
            ch->closed = true;
            return ESP_FAIL;
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
            return ESP_FAIL;
        }

        ch->peer_window -= chunk;
        data += chunk;
        len  -= chunk;
    }

    return ESP_OK;
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

    for (size_t i = 0; i < len; i++) {
        if (data[i] != '\n') {
            continue;
        }
        if (i > start && send_data(ch, data + start, i - start) != ESP_OK) {
            return -1;
        }
        if (send_data(ch, "\r\n", 2) != ESP_OK) {
            return -1;
        }
        start = i + 1;
    }

    if (start < len && send_data(ch, data + start, len - start) != ESP_OK) {
        return -1;
    }
    return (int)len;
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

    if (ssh_packet_write(ch->conn, &b) == ESP_OK) {
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

        if (ssh_packet_read(c) != ESP_OK) {
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
    return ssh_packet_write(ch->conn, &b);
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
    return ssh_packet_write(ch->conn, &b);
}

static void close_channel(ssh_chan_t *ch)
{
    ssh_buf_t b;

    ssh_buf_init(&b, ch->conn->out_buf, sizeof(ch->conn->out_buf));
    ssh_put_u8(&b, SSH_MSG_CHANNEL_EOF);
    ssh_put_u32(&b, ch->peer_chan);
    ssh_packet_write(ch->conn, &b);

    ssh_buf_init(&b, ch->conn->out_buf, sizeof(ch->conn->out_buf));
    ssh_put_u8(&b, SSH_MSG_CHANNEL_CLOSE);
    ssh_put_u32(&b, ch->peer_chan);
    ssh_packet_write(ch->conn, &b);
}

esp_err_t ssh_channel_run(ssh_conn_t *c)
{
    ssh_chan_t ch = {
        .conn         = c,
        .local_window = LOCAL_WINDOW,
        .cols         = 80,
        .rows         = 24,
    };

    /* CHANNEL_OPEN */
    if (ssh_packet_read(c) != ESP_OK) {
        return ESP_FAIL;
    }
    if (c->in_payload[0] != SSH_MSG_CHANNEL_OPEN) {
        return ESP_ERR_INVALID_RESPONSE;
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
        return ESP_ERR_INVALID_SIZE;
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
        return ESP_ERR_NOT_SUPPORTED;
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
            return ESP_FAIL;
        }
    }

    /* Requests until the client asks for a shell. */
    while (!ch.want_shell && !ch.closed) {
        if (ssh_packet_read(c) != ESP_OK) {
            return ESP_FAIL;
        }

        ssh_buf_t req;
        ssh_buf_read_from(&req, c->in_payload, c->in_len);
        const uint8_t msg = ssh_get_u8(&req);

        if (msg == SSH_MSG_CHANNEL_REQUEST) {
            handle_channel_request(&ch, &req);
        } else if (msg == SSH_MSG_CHANNEL_CLOSE ||
                   msg == SSH_MSG_CHANNEL_EOF ||
                   msg == SSH_MSG_DISCONNECT) {
            return ESP_OK;
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
        /*
         * Not fd-backed, despite there being a socket: everything must be
         * wrapped in CHANNEL_DATA and encrypted, so a raw fd would bypass the
         * protocol entirely. Giving apps their own stdout here needs a FILE*
         * backed by a callback, which is the remaining piece.
         */
        .fd_in     = -1,
        .fd_out    = -1,
    };
    strlcpy(session.user, c->user, sizeof(session.user));

    espix_printf(&session, "espix %s\n", espix_version());
    if (espix_auth_is_default()) {
        espix_printf(&session,
                     "warning: '%s' still has the default password\n",
                     c->user);
    }
    espix_printf(&session, "\n");

    espix_shell_session_run(&session);

    send_exit_status(&ch, (uint32_t)session.last_status);
    close_channel(&ch);

    espix_klog(ESPIX_KLOG_INFO, TAG, "%s logged out", c->user);
    return ESP_OK;
}
