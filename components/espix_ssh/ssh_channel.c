/*
 * SSH session channel (RFC 4254), and the bridge from it to an espix session.
 *
 * One channel per connection: a "session" (a shell, an exec, sftp) or a
 * "direct-tcpip" forward. With a pty the client sends raw keystrokes and
 * expects the *server* to echo and edit: the session's tty does that for apps,
 * and esp_linenoise for the shell's prompt, through the callbacks below -- the
 * same editor the serial console runs, with its own instance and history.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/param.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_linenoise.h"

#include "sdkconfig.h"

#include "espix_auth.h"
#include "espix_kernel.h"
#include "espix_net.h"
#include "espix_proc.h"
#include "espix_shell.h"
#include "espix_tty.h"
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

/*
 * Each channel's input is a tty (components/espix_tty): a terminal for a shell
 * or an exec with a pty, a pipe for an exec without one, for sftp and for a
 * forward. The reader task decrypts every packet after the handoff and puts
 * CHANNEL_DATA into it (chan_dispatch()); the connection task never reads the
 * socket again, it is just the session.
 */
#define TTY_INPUT_PTY  1024
#define TTY_INPUT_PIPE 8192

/* How long a writer waits for the peer to open its window before giving the channel up. */
#define WINDOW_WAIT_MS 30000

/*
 * How long the transmit lock may stay taken before its holder is taken for
 * dead. A writer holds it for one packet's send, which a peer that stops
 * draining can stretch to the transport's own write timeout.
 */
#define TX_WAIT_MS   250
#define TX_DEAD_MS 30000

/*
 * A pty's size: what the client asked for, within reason. A client that sends
 * zero (or nonsense) gets the classic 80x24 rather than a zero-width terminal
 * that would wrap every character onto its own line.
 */
#define TERM_COLS_MIN 20
#define TERM_ROWS_MIN 2
#define TERM_COLS_DEF 80
#define TERM_ROWS_DEF 24

/* Concurrent sessions the fd -> channel map has room for. */
#define SSH_MAX_SESSIONS CONFIG_ESPIX_SSH_MAX_SESSIONS

typedef struct {
    ssh_conn_t *conn;
    uint32_t    peer_chan;
    uint32_t    peer_window;        /* atomic: the reader adds, writers take */
    uint32_t    peer_max_packet;
    uint32_t    local_window;       /* the reader's alone after the handoff */
    bool        want_shell;
    bool        want_sftp;

    /* The channel's one cross-task exit condition: the reader sets it on a CLOSE, a writer on a failed send. */
    volatile bool closed;

    /* A killed process died holding tx_lock, so nobody may ask for it again; see chan_task_gone(). */
    volatile bool tx_orphaned;

    /* `ssh host <cmd>`: the command, copied out of the packet buffer. */
    bool        want_exec;
    char       *exec_cmd;

    /* Over ESPIX_LINE_MAX: answered with the limit and 130 rather than refused (R-P7.10). */
    bool        exec_too_long;
    size_t      exec_len;

    /* Whether the client asked for a terminal: the input is then a terminal tty, and stderr shares stdout's stream. */
    bool        has_pty;

    /* The client has sent CHANNEL_EOF: no more input, but output still goes. */
    bool        eof_seen;

    uint16_t    cols;
    uint16_t    rows;

    espix_tty_t     *tty;           /* the channel's input, and its output's line discipline */
    espix_session_t *session;       /* the session on this channel, once there is one */
    TaskHandle_t     shell;         /* the connection task, woken for a signal key */
    volatile bool    intr;          /* ^C, for the session's foreground wait */

    /* Data the tty could not take yet (a pipe that is full), handed over when it has room. */
    uint8_t     hold[SSH_CHANNEL_MAX_PACKET];
    size_t      hold_len, hold_pos;

    /* sftp's raw reads land here. */
    uint8_t     raw[SSH_CHANNEL_MAX_PACKET];

    /* Line editing, one instance per session; the history belongs to the user. */
    esp_linenoise_handle_t editor;
    espix_history_t       *history;

    /* Our answer to the editor's last terminal query, read ahead of the keys typed before it. */
    char        reply[24];
    uint8_t     reply_len, reply_pos;
    bool        edit_eof;           /* the editor read the end of input */

    /* Writers waiting for the peer's window: the reader gives window_sem once for each when it grows. */
    SemaphoreHandle_t window_sem;
    volatile int      window_waiters;

    /*
     * Packets must not interleave -- they share conn->out_buf and a sequence
     * number the MAC covers -- so every packet is built and sent under this,
     * and only for that packet: a writer waiting for the peer's window lets it
     * go, because the reader may need it to answer while it waits.
     */
    SemaphoreHandle_t tx_lock;
} ssh_chan_t;

/* ------------------------------------------------------------------ */
/* Sending                                                             */
/* ------------------------------------------------------------------ */

/*
 * Take the transmit lock, or decide nobody is ever giving it back.
 *
 * Blocking forever is fine only where something is guaranteed to unblock it,
 * and nothing guarantees that here: a force-killed process dies holding the
 * lock, and only the owner may give a FreeRTOS mutex back. So the wait is
 * bounded, and failing it closes the channel, which every waiter tests.
 */
static bool chan_tx_take(ssh_chan_t *ch)
{
    /*
     * Checked before asking, never after: taking a mutex reaches into the
     * recorded holder's TCB for priority inheritance, and a deleted holder's
     * TCB is freed memory. chan_task_gone() sets this at the only moment the
     * system knows.
     */
    if (ch->tx_orphaned) {
        return false;
    }
    for (unsigned waited = 0; waited < TX_DEAD_MS; waited += TX_WAIT_MS) {
        if (xSemaphoreTake(ch->tx_lock, pdMS_TO_TICKS(TX_WAIT_MS)) == pdTRUE) {
            return true;
        }
        if (ch->closed) {
            return false;
        }
    }
    espix_klog(ESPIX_KLOG_WARN, TAG,
               "transmit lock held for %ds, probably by a killed process; "
               "abandoning the channel", TX_DEAD_MS / 1000);
    ch->closed = true;
    return false;
}

/* The writers waiting for window, woken: it grew, or the channel closed. */
static void window_grew(ssh_chan_t *ch)
{
    for (int n = ch->window_waiters; n > 0; n--) {
        (void)xSemaphoreGive(ch->window_sem);
    }
}

static void chan_closed(ssh_chan_t *ch)
{
    ch->closed = true;
    window_grew(ch);
}

/*
 * A process on this session has been force-deleted; write off what it held.
 *
 * Only the owner of a mutex can give it back, so a task deleted inside a send
 * leaves tx_lock held for good -- and its recorded holder is a freed TCB, which
 * every later take would walk. xSemaphoreGetMutexHolder() only reads the stored
 * handle, so comparing it here is safe. Most kills catch a process holding
 * nothing, and those sessions carry on.
 */
static void chan_task_gone(espix_session_t *s, void *task)
{
    ssh_chan_t *ch = s->transport;

    if (ch == NULL || task == NULL) {
        return;
    }
    if (ch->tx_lock != NULL && xSemaphoreGetMutexHolder(ch->tx_lock) == (TaskHandle_t)task) {
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "a killed process still held the transmit lock; closing the "
                   "channel rather than waiting on a lock nobody can release");
        ch->tx_orphaned = true;
        chan_closed(ch);
    }
}

/* Until the peer's window is open: true, or false when it never opened or the channel closed. */
static bool wait_for_window(ssh_chan_t *ch)
{
    const TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(WINDOW_WAIT_MS);
    __atomic_add_fetch(&ch->window_waiters, 1, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&ch->peer_window, __ATOMIC_SEQ_CST) == 0 && !ch->closed) {
        const int32_t left = (int32_t)(until - xTaskGetTickCount());
        if (left <= 0 || xSemaphoreTake(ch->window_sem, (TickType_t)left) != pdTRUE) {
            if (__atomic_load_n(&ch->peer_window, __ATOMIC_SEQ_CST) == 0) {
                espix_klog(ESPIX_KLOG_WARN, TAG, "peer window never opened");
                ch->closed = true;
            }
            break;
        }
    }
    __atomic_sub_fetch(&ch->window_waiters, 1, __ATOMIC_SEQ_CST);
    return !ch->closed;
}

/*
 * One control packet, built by the caller in conn->out_buf under tx_lock.
 *
 * A failure closes the channel: a packet that failed may have gone out in part,
 * and there is no un-sending those bytes nor any way for the peer to
 * resynchronise, so the stream is finished either way.
 */
static esp_err_t send_built(ssh_chan_t *ch, ssh_buf_t *b)
{
    const esp_err_t err = ssh_packet_write(ch->conn, b);
    if (err != ESP_OK) {
        chan_closed(ch);
    }
    return err;
}

/* A control packet with no body beyond the peer's channel: EOF, CLOSE, SUCCESS, FAILURE. */
static esp_err_t send_simple(ssh_chan_t *ch, uint8_t msg)
{
    if (!chan_tx_take(ch)) {
        return ESP_FAIL;
    }
    ssh_buf_t b;
    ssh_buf_init(&b, ch->conn->out_buf, sizeof(ch->conn->out_buf));
    ssh_put_u8(&b, msg);
    ssh_put_u32(&b, ch->peer_chan);
    const esp_err_t err = send_built(ch, &b);
    xSemaphoreGive(ch->tx_lock);
    return err;
}

/* Up to len bytes of one stream in one packet, as much as the window allows; how many, or -1. */
static ssize_t send_chunk(ssh_chan_t *ch, const char *data, size_t len, uint32_t ext)
{
    size_t chunk = MIN(len, __atomic_load_n(&ch->peer_window, __ATOMIC_SEQ_CST));
    chunk = MIN(chunk, (size_t)ch->peer_max_packet);
    chunk = MIN(chunk, (size_t)SSH_MAX_PACKET - 64);
    if (chunk == 0) {
        return 0;
    }
    ssh_buf_t b;
    ssh_buf_init(&b, ch->conn->out_buf, sizeof(ch->conn->out_buf));
    ssh_put_u8(&b, ext ? SSH_MSG_CHANNEL_EXTENDED_DATA : SSH_MSG_CHANNEL_DATA);
    ssh_put_u32(&b, ch->peer_chan);
    if (ext) {
        ssh_put_u32(&b, ext);
    }
    ssh_put_string(&b, data, chunk);
    if (send_built(ch, &b) != ESP_OK) {
        return -1;
    }
    __atomic_sub_fetch(&ch->peer_window, (uint32_t)chunk, __ATOMIC_SEQ_CST);
    return (ssize_t)chunk;
}

/*
 * One writer for both streams: `ext` is 0 for CHANNEL_DATA or a type code for
 * CHANNEL_EXTENDED_DATA. It waits for window rather than truncating, and lets
 * the lock go while it waits.
 */
static esp_err_t send_stream(ssh_chan_t *ch, const char *data, size_t len, uint32_t ext)
{
    while (len > 0) {
        if (!chan_tx_take(ch)) {
            return ESP_FAIL;
        }
        const ssize_t n = ch->closed ? -1 : send_chunk(ch, data, len, ext);
        xSemaphoreGive(ch->tx_lock);
        if (n < 0) {
            return ESP_FAIL;
        }
        if (n == 0 && !wait_for_window(ch)) {
            return ESP_FAIL;
        }
        data += n;
        len  -= (size_t)n;
    }
    return ESP_OK;
}

static esp_err_t send_data(ssh_chan_t *ch, const char *data, size_t len)
{
    return send_stream(ch, data, len, 0);
}

/* ------------------------------------------------------------------ */
/* The tty's side                                                      */
/* ------------------------------------------------------------------ */

static ssize_t tty_out(void *ctx, const void *buf, size_t len)
{
    ssh_chan_t *ch = ctx;
    return (send_stream(ch, buf, len, 0) == ESP_OK) ? (ssize_t)len : -EIO;
}

/* Standard error: its own stream on a pipe, interleaved with stdout on a terminal as a pty has it. */
static ssize_t tty_out_err(void *ctx, const void *buf, size_t len)
{
    ssh_chan_t *ch = ctx;
    return (send_stream(ch, buf, len, ch->has_pty ? 0 : SSH_EXTENDED_DATA_STDERR) == ESP_OK) ? (ssize_t)len : -EIO;
}

/*
 * Echo, on the reader task: it must not wait for window, which only the reader
 * itself would see open. What the window does not take is not shown, as a
 * terminal whose output is stuck would not show it either.
 */
static void tty_echo(void *ctx, const void *buf, size_t len)
{
    ssh_chan_t *ch = ctx;
    if (chan_tx_take(ch)) {
        if (!ch->closed) {
            (void)send_chunk(ch, buf, len, 0);
        }
        xSemaphoreGive(ch->tx_lock);
    }
}

/* A signal key: the session's foreground wait acts on it, with its messages and its escalation (fg.c). */
static void tty_signal(void *ctx, int sig)
{
    ssh_chan_t *ch = ctx;
    espix_session_t *s = ch->session;
    if (sig == SIGTSTP) {
        if (s != NULL) {
            s->susp = true;
        }
    } else if (sig == SIGINT) {
        ch->intr = true;
    } else if (s != NULL && s->fg_pgid != ESPIX_PID_NONE) {
        (void)espix_proc_signal_group(s->fg_pgid, sig);
    }
    if (ch->shell != NULL) {
        (void)xTaskNotifyGiveIndexed(ch->shell, ESPIX_NOTIFY_SIG_IDX);
    }
}

/* Each key, for a command that asks for the last one (espix_shell_take_key()). */
static void tty_key(void *ctx, int c)
{
    ssh_chan_t *ch = ctx;
    if (ch->session != NULL) {
        ch->session->last_key = c;
    }
}

static void tty_room(void *ctx)
{
    ssh_rx_resume(((ssh_chan_t *)ctx)->conn);
}

/* ------------------------------------------------------------------ */
/* The reader's side: every packet after the handoff                  */
/* ------------------------------------------------------------------ */

/*
 * Tops our window back up as data is handed on, so a peer can only send what
 * the tty has taken: a full pipe holds the client back through SSH's own flow
 * control, instead of through a buffer the size of the window.
 */
static void adjust_local_window(ssh_chan_t *ch, uint32_t consumed)
{
    ch->local_window -= consumed;
    if (ch->local_window > LOCAL_WINDOW / 2 || !chan_tx_take(ch)) {
        return;
    }
    const uint32_t add = LOCAL_WINDOW - ch->local_window;
    ssh_buf_t b;
    ssh_buf_init(&b, ch->conn->out_buf, sizeof(ch->conn->out_buf));
    ssh_put_u8(&b, SSH_MSG_CHANNEL_WINDOW_ADJUST);
    ssh_put_u32(&b, ch->peer_chan);
    ssh_put_u32(&b, add);
    if (send_built(ch, &b) == ESP_OK) {
        ch->local_window += add;
    }
    xSemaphoreGive(ch->tx_lock);
}

/* What was held, into the tty as it makes room. */
static int hand_over(ssh_chan_t *ch)
{
    while (ch->hold_pos < ch->hold_len) {
        const size_t took = espix_tty_input(ch->tty, ch->hold + ch->hold_pos, ch->hold_len - ch->hold_pos);
        if (took == 0) {
            return SSH_RX_HOLD;
        }
        ch->hold_pos += took;
        adjust_local_window(ch, (uint32_t)took);
    }
    return SSH_RX_OK;
}

static void set_term_size(ssh_chan_t *ch, uint32_t cols, uint32_t rows);
static esp_err_t handle_channel_request(ssh_chan_t *ch, ssh_buf_t *in);

/* The connection is over or the channel closed: readers see the end, writers stop, the shell wakes. */
static void chan_gone(ssh_chan_t *ch)
{
    chan_closed(ch);
    if (ch->tty != NULL) {
        espix_tty_input_eof(ch->tty);
    }
    if (ch->shell != NULL) {
        (void)xTaskNotifyGiveIndexed(ch->shell, ESPIX_NOTIFY_SIG_IDX);
    }
}

static int chan_dispatch(void *ctx, const uint8_t *payload, size_t len)
{
    ssh_chan_t *ch = ctx;

    if (payload == NULL) {
        if (len == SSH_RX_GONE) {
            chan_gone(ch);
            return SSH_RX_STOP;
        }
        return hand_over(ch);
    }

    ssh_buf_t in;
    ssh_buf_read_from(&in, (uint8_t *)payload, len);
    const uint8_t msg = ssh_get_u8(&in);

    switch (msg) {
    case SSH_MSG_CHANNEL_DATA: {
        ssh_get_u32(&in);               /* recipient channel */
        size_t         n = 0;
        const uint8_t *data = ssh_get_string(&in, &n);
        /* The client is bound by the maximum packet we advertised; more is a protocol violation. */
        if (in.bad || data == NULL || n > sizeof(ch->hold)) {
            espix_klog(ESPIX_KLOG_WARN, TAG, "malformed channel data (%u)", (unsigned)n);
            chan_gone(ch);
            return SSH_RX_STOP;
        }
        if (ch->tty == NULL) {
            return SSH_RX_OK;
        }
        /* Copied out first: the next packet decrypts over the buffer these bytes are in. */
        memcpy(ch->hold, data, n);
        ch->hold_len = n;
        ch->hold_pos = 0;
        return hand_over(ch);
    }

    case SSH_MSG_CHANNEL_WINDOW_ADJUST:
        ssh_get_u32(&in);
        __atomic_add_fetch(&ch->peer_window, ssh_get_u32(&in), __ATOMIC_SEQ_CST);
        window_grew(ch);
        return SSH_RX_OK;

    case SSH_MSG_CHANNEL_REQUEST:
        handle_channel_request(ch, &in);
        return SSH_RX_OK;

    /* The client sends no more input; output goes on. */
    case SSH_MSG_CHANNEL_EOF:
        ch->eof_seen = true;
        if (ch->tty != NULL) {
            espix_tty_input_eof(ch->tty);
        }
        return SSH_RX_OK;

    case SSH_MSG_CHANNEL_CLOSE:
    case SSH_MSG_DISCONNECT:
        chan_gone(ch);
        return SSH_RX_STOP;

    case SSH_MSG_CHANNEL_OPEN: {
        /* A second channel (`ssh -L` without -N, the moment the port is used): refuse it and say why. */
        size_t type_len = 0;
        (void)ssh_get_string(&in, &type_len);
        const uint32_t sender = ssh_get_u32(&in);
        if (chan_tx_take(ch)) {
            ssh_buf_t b;
            ssh_buf_init(&b, ch->conn->out_buf, sizeof(ch->conn->out_buf));
            ssh_put_u8(&b, SSH_MSG_CHANNEL_OPEN_FAILURE);
            ssh_put_u32(&b, sender);
            ssh_put_u32(&b, 1);         /* administratively prohibited */
            ssh_put_cstr(&b, "one channel per connection; use ssh -N -L");
            ssh_put_cstr(&b, "");
            (void)send_built(ch, &b);
            xSemaphoreGive(ch->tx_lock);
        }
        return SSH_RX_OK;
    }

    case SSH_MSG_GLOBAL_REQUEST: {
        /* keepalive@openssh.com and the like: none is supported, and a client that asks wants to hear so. */
        (void)ssh_get_string(&in, NULL);
        if (ssh_get_bool(&in) && chan_tx_take(ch)) {
            ssh_buf_t b;
            ssh_buf_init(&b, ch->conn->out_buf, sizeof(ch->conn->out_buf));
            ssh_put_u8(&b, SSH_MSG_REQUEST_FAILURE);
            (void)send_built(ch, &b);
            xSemaphoreGive(ch->tx_lock);
        }
        return SSH_RX_OK;
    }

    default:
        /* An unknown message mid-session is better ignored than a reason to drop someone's shell. */
        espix_klog(ESPIX_KLOG_DEBUG, TAG, "ignoring message %u", msg);
        return SSH_RX_OK;
    }
}

/* ------------------------------------------------------------------ */
/* The session's side                                                  */
/* ------------------------------------------------------------------ */

static int chan_write(espix_session_t *s, const char *data, size_t len)
{
    ssh_chan_t *ch = s->transport;
    /* A stopping process takes no lock: it may be killed holding it. */
    if (espix_proc_stopping()) {
        return -1;
    }
    return (espix_tty_write(ch->tty, data, len) < 0) ? -1 : (int)len;
}

static int chan_write_err(espix_session_t *s, const char *data, size_t len)
{
    ssh_chan_t *ch = s->transport;
    if (espix_proc_stopping()) {
        return -1;
    }
    return (espix_tty_write_err(ch->tty, data, len) < 0) ? -1 : (int)len;
}

/* ^C reached the terminal while a foreground job ran (tty_signal()). */
static bool chan_poll_interrupt(espix_session_t *s)
{
    ssh_chan_t *ch = s->transport;
    if (ch->closed) {
        return true;
    }
    const bool hit = ch->intr;
    ch->intr = false;
    return hit;
}

static void chan_wait_input(espix_session_t *s, uint32_t ms)
{
    ssh_chan_t *ch = s->transport;
    if (ch == NULL || ch->closed || ch->intr || s->susp) {
        return;
    }
    (void)ulTaskNotifyTakeIndexed(ESPIX_NOTIFY_SIG_IDX, pdTRUE,
                                  (ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(ms));
}

static void chan_term_size(espix_session_t *s, int *cols, int *rows)
{
    const ssh_chan_t *ch = s->transport;

    *cols = ch->cols;
    *rows = ch->rows;
}

/* A process's streams: its reads and writes are the tty's, so a signal ends a blocked read (espix_tty_read()). */
static int chan_stream_read(void *cookie, char *buf, int len)
{
    espix_session_t *s = cookie;
    ssh_chan_t *ch = s->transport;
    if (len <= 0) {
        return 0;
    }
    const ssize_t n = espix_tty_read(ch->tty, buf, (size_t)len, false);
    if (n < 0) {
        errno = (int)-n;
        return -1;
    }
    return (int)n;
}

static int chan_stream_write(void *cookie, const char *data, int len)
{
    if (len <= 0) {
        return 0;
    }
    return (chan_write(cookie, data, (size_t)len) < 0) ? -1 : len;
}

static int chan_stream_write_err(void *cookie, const char *data, int len)
{
    if (len <= 0) {
        return 0;
    }
    return (chan_write_err(cookie, data, (size_t)len) < 0) ? -1 : len;
}

static FILE *chan_open_stream(espix_session_t *s, espix_stream_t which)
{
    if (which == ESPIX_STREAM_IN) {
        /* Unbuffered: the tty already hands over a line at a time, and a stream buffer would hide input from poll(). */
        FILE *f = funopen(s, chan_stream_read, NULL, NULL, NULL);
        if (f != NULL) {
            setvbuf(f, NULL, _IONBF, 0);
        }
        return f;
    }
    FILE *f = funopen(s, NULL, (which == ESPIX_STREAM_ERR) ? chan_stream_write_err : chan_stream_write, NULL, NULL);
    if (f != NULL) {
        /* Line buffered: output appears as it is produced, but a packet per character would cost a frame each. */
        setvbuf(f, NULL, _IOLBF, 128);
    }
    return f;
}

static void set_term_size(ssh_chan_t *ch, uint32_t cols, uint32_t rows)
{
    if (cols < TERM_COLS_MIN || cols > UINT16_MAX) {
        espix_klog(ESPIX_KLOG_DEBUG, TAG, "client asked for %u columns; using %d", (unsigned)cols, TERM_COLS_DEF);
        cols = TERM_COLS_DEF;
    }
    if (rows < TERM_ROWS_MIN || rows > UINT16_MAX) {
        rows = TERM_ROWS_DEF;
    }
    ch->cols = (uint16_t)cols;
    ch->rows = (uint16_t)rows;
    if (ch->tty != NULL) {
        espix_tty_set_winsize(ch->tty, ch->cols, ch->rows);
    }
}

/* ------------------------------------------------------------------ */
/* The line editor                                                     */
/* ------------------------------------------------------------------ */

/* The editor's callbacks carry only an fd: the tty's, mapped back to its channel. */
static struct {
    int         fd;
    ssh_chan_t *ch;
} s_editor_map[SSH_MAX_SESSIONS];
static portMUX_TYPE s_editor_mux = portMUX_INITIALIZER_UNLOCKED;

static void editor_map_set(int fd, ssh_chan_t *ch)
{
    portENTER_CRITICAL(&s_editor_mux);
    for (int i = 0; i < SSH_MAX_SESSIONS; i++) {
        if (ch != NULL ? s_editor_map[i].ch == NULL : s_editor_map[i].fd == fd) {
            s_editor_map[i].fd = ch ? fd : -1;
            s_editor_map[i].ch = ch;
            break;
        }
    }
    portEXIT_CRITICAL(&s_editor_mux);
}

static ssh_chan_t *editor_map_get(int fd)
{
    ssh_chan_t *ch = NULL;
    portENTER_CRITICAL(&s_editor_mux);
    for (int i = 0; i < SSH_MAX_SESSIONS; i++) {
        if (s_editor_map[i].ch != NULL && s_editor_map[i].fd == fd) {
            ch = s_editor_map[i].ch;
            break;
        }
    }
    portEXIT_CRITICAL(&s_editor_mux);
    return ch;
}

/* The editor's keys: our answer to its last query first, then the tty in its editing mode. */
static ssize_t ssh_edit_read(int fd, void *buf, size_t count)
{
    ssh_chan_t *ch = editor_map_get(fd);
    if (ch == NULL || count == 0) {
        return -1;
    }
    if (ch->reply_pos < ch->reply_len) {
        const size_t n = MIN(count, (size_t)(ch->reply_len - ch->reply_pos));
        memcpy(buf, ch->reply + ch->reply_pos, n);
        ch->reply_pos += n;
        return (ssize_t)n;
    }
    /* Anything but a key (the end of input, the shell being killed) ends the session. */
    const ssize_t n = espix_tty_read(ch->tty, buf, count, false);
    if (n <= 0) {
        ch->edit_eof = true;
    }
    return (n < 0) ? 0 : n;
}

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

/* An answer to a terminal query, as if the client had sent it. */
static void answer(ssh_chan_t *ch, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(ch->reply, sizeof(ch->reply), fmt, ap);
    va_end(ap);
    ch->reply_len = (n > 0 && (size_t)n < sizeof(ch->reply)) ? (uint8_t)n : 0;
    ch->reply_pos = 0;
}

/*
 * The editor's output. Its terminal queries are answered here from what the
 * client told us, rather than sent: the cursor-position probe it makes per
 * prompt would cost a round trip, and a client whose input is not a terminal
 * never answers at all.
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
    if (seq_is(buf, count, "\x1b[6n")) {
        answer(ch, "\x1b[%u;%uR", (unsigned)ch->rows, (unsigned)ch->cols);
        return (ssize_t)count;
    }
    if (seq_is(buf, count, "\x1b[5n")) {
        answer(ch, "\x1b[0n");
        return (ssize_t)count;
    }
    if (seq_is(buf, count, "\x1b[999C")) {
        return (ssize_t)count;
    }
    return (espix_tty_write(ch->tty, buf, count) < 0) ? -1 : (ssize_t)count;
}

/* The editor's mode: keys one at a time and no echo or signal keys, with CR as Enter and LF as CR LF out. */
static void editing_mode(const struct termios *cooked, struct termios *edit)
{
    *edit = *cooked;
    edit->c_lflag &= ~(tcflag_t)(ICANON | ECHO | ECHOE | ECHOK | ECHONL | ISIG | IEXTEN);
    edit->c_cc[VMIN]  = 1;
    edit->c_cc[VTIME] = 0;
}

static int chan_read_line(espix_session_t *s, const char *prompt, char *buf, size_t len)
{
    ssh_chan_t *ch = s->transport;

    if (ch->closed) {
        return -1;
    }
    struct termios cooked, edit;
    espix_tty_getattr(ch->tty, &cooked);
    editing_mode(&cooked, &edit);
    espix_tty_setattr(ch->tty, TCSANOW, &edit);

    esp_linenoise_set_prompt(ch->editor, prompt);
    buf[0] = '\0';
    ch->edit_eof = false;
    const esp_err_t err = esp_linenoise_get_line(ch->editor, buf, len);
    const int saved = errno;
    espix_tty_setattr(ch->tty, TCSANOW, &cooked);

    /* linenoise returns what was typed so far at the end of input; the next prompt ends the session. */
    if (ch->edit_eof && buf[0] == '\0') {
        return -1;
    }
    if (err != ESP_OK) {
        /* EAGAIN is Ctrl-C at the prompt: an empty line, as a shell treats it. */
        return (saved == EAGAIN) ? 0 : -1;
    }
    if (buf[0] != '\0') {
        espix_history_push(ch->history, buf);
        espix_history_apply(ch->history, ch->editor);
    }
    return (int)strlen(buf);
}

/* ------------------------------------------------------------------ */
/* sftp's raw byte stream                                              */
/* ------------------------------------------------------------------ */

esp_err_t ssh_channel_send_raw(ssh_conn_t *c, const void *data, size_t len)
{
    ssh_chan_t *ch = c->chan;
    if (ch == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return send_data(ch, data, len);
}

esp_err_t ssh_channel_recv_raw(ssh_conn_t *c, uint8_t **out, size_t *out_len)
{
    ssh_chan_t *ch = c->chan;
    if (ch == NULL || ch->tty == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const ssize_t n = espix_tty_read(ch->tty, ch->raw, sizeof(ch->raw), false);
    if (n <= 0) {
        return ESP_FAIL;
    }
    *out     = ch->raw;
    *out_len = (size_t)n;
    return ESP_OK;
}

static esp_err_t reply_request(ssh_chan_t *ch, bool ok)
{
    return send_simple(ch, ok ? SSH_MSG_CHANNEL_SUCCESS : SSH_MSG_CHANNEL_FAILURE);
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
            ch->exec_too_long = true;
            ch->exec_len      = cmd_len;
            ch->want_exec     = true;
            ok                = true;
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

            /* Tell the foreground job, the way a terminal does; nothing to tell at the prompt. */
            espix_session_t *resized = ch->session;
            if (resized != NULL && resized->fg_pgid != ESPIX_PID_NONE) {
                (void)espix_proc_signal_group(resized->fg_pgid, SIGWINCH);
            }
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
    if (!chan_tx_take(ch)) {
        return ESP_FAIL;
    }
    ssh_buf_t b;
    ssh_buf_init(&b, ch->conn->out_buf, sizeof(ch->conn->out_buf));
    ssh_put_u8(&b, SSH_MSG_CHANNEL_REQUEST);
    ssh_put_u32(&b, ch->peer_chan);
    ssh_put_cstr(&b, "exit-status");
    ssh_put_u8(&b, 0);                  /* never wants a reply */
    ssh_put_u32(&b, status);
    const esp_err_t err = send_built(ch, &b);
    xSemaphoreGive(ch->tx_lock);
    return err;
}

static void close_channel(ssh_chan_t *ch)
{
    (void)send_simple(ch, SSH_MSG_CHANNEL_EOF);
    (void)send_simple(ch, SSH_MSG_CHANNEL_CLOSE);
}

/* ------------------------------------------------------------------ */
/* direct-tcpip: a TCP connection carried over the channel             */
/* ------------------------------------------------------------------ */

/*
 * Outbound forwarding, which is the server half of `ssh -L`.
 *
 * It is a different privilege from running a shell: an authenticated peer asks
 * espix to connect somewhere, and never sees a login. That is why it has its own
 * setting rather than following from "SSH is on", and why the default is the
 * device itself -- which is what a tunnel to a service here, VNC on 5900, needs.
 *
 * Not implemented, and worth saying so: `ssh -R` -- a client asking espix to
 * *listen* -- is the other direction (tcpip-forward plus forwarded-tcpip) and is
 * a separate piece of work.
 */

#define FORWARD_CONNECT_MS  4000   /* a SYN that black-holes must not park us */
#define FORWARD_STALL_LIMIT 20     /* * SO_SNDTIMEO before giving the tunnel up */

static bool forward_allowed(uint32_t addr_be)
{
#if defined(CONFIG_ESPIX_SSH_TCPIP_FORWARD_ANY)
    (void)addr_be;
    return true;
#elif defined(CONFIG_ESPIX_SSH_TCPIP_FORWARD_LOOPBACK)
    return (ntohl(addr_be) >> 24) == 127;   /* 127.0.0.0/8 */
#else
    (void)addr_be;
    return false;                           /* forwarding off */
#endif
}

/*
 * Connect, without ever parking the connection task indefinitely.
 *
 * The socket is briefly non-blocking so that the connect can be bounded: lwIP's
 * own timeout for an unanswered SYN is far longer than anything an interactive
 * client will wait, and this runs on the task that owns the SSH connection.
 */
static int tcp_connect_to(const struct sockaddr_in *dst, int timeout_ms)
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    if (connect(fd, (const struct sockaddr *)dst, sizeof(*dst)) != 0 &&
        errno != EINPROGRESS && errno != EAGAIN) {
        close(fd);
        return -1;
    }

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(fd, &wfds);

    /* Not const: select() is allowed to write the remaining time back. */
    struct timeval tv = { .tv_sec  = timeout_ms / 1000,
                          .tv_usec = (timeout_ms % 1000) * 1000 };
    if (select(fd + 1, NULL, &wfds, NULL, &tv) != 1) {
        close(fd);
        return -1;
    }

    int       soerr = 0;
    socklen_t slen  = sizeof(soerr);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) != 0 || soerr != 0) {
        close(fd);
        return -1;
    }

    if (flags >= 0) {
        (void)fcntl(fd, F_SETFL, flags);
    }
    return fd;
}

static int forward_connect(const char *host, uint32_t port, bool *refused)
{
    *refused = false;

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char portstr[8];
    snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);

    struct addrinfo *res = NULL;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || res == NULL) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "forward: cannot resolve %s", host);
        return -1;
    }

    int fd = -1;
    for (const struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        const struct sockaddr_in *dst = (const struct sockaddr_in *)ai->ai_addr;
        if (!forward_allowed(dst->sin_addr.s_addr)) {
            *refused = true;
            continue;
        }
        fd = tcp_connect_to(dst, FORWARD_CONNECT_MS);
        if (fd >= 0) {
            break;
        }
    }

    freeaddrinfo(res);
    return fd;
}

/* EOF is "I will send no more", not "stop sending": the other half stays open. */
static void send_eof(ssh_chan_t *ch)
{
    (void)send_simple(ch, SSH_MSG_CHANNEL_EOF);
}

/* The channel's input as a tty (a pipe when nothing types into it), and the inbound protocol handed to the reader. */
static esp_err_t chan_start_input(ssh_chan_t *ch, bool pipe)
{
    const espix_tty_ops_t ops = {
        .write = tty_out, .write_err = tty_out_err, .echo = tty_echo,
        .signal = tty_signal, .key = tty_key, .room = tty_room, .ctx = ch,
    };
    ch->tty = espix_tty_create(&ops, pipe ? TTY_INPUT_PIPE : TTY_INPUT_PTY, pipe);
    if (ch->tty == NULL) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "no memory for the channel's input");
        return ESP_ERR_NO_MEM;
    }
    espix_tty_set_winsize(ch->tty, ch->cols, ch->rows);
    ch->shell      = xTaskGetCurrentTaskHandle();
    ch->conn->chan = ch;
    ssh_rx_handoff(ch->conn, chan_dispatch, ch);
    return ESP_OK;
}

/* All of it to the socket, through stalls the send timeout bounds. */
static bool tcp_send_all(int fd, const uint8_t *p, size_t n)
{
    int stalled = 0;
    while (n > 0) {
        const ssize_t w = send(fd, p, n, 0);
        if (w > 0) {
            p += w;
            n -= (size_t)w;
            stalled = 0;
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && ++stalled < FORWARD_STALL_LIMIT) {
            continue;
        }
        return false;
    }
    return true;
}

/*
 * The tunnel. The client's bytes arrive in the channel's pipe, put there by the
 * reader task; the socket's go out as CHANNEL_DATA. One select waits on both,
 * and the pipe reads as ended when the client sends EOF or the channel closes.
 */
static esp_err_t forward_pump(ssh_chan_t *ch, int tcp_fd)
{
    bool    tcp_eof  = false;
    bool    peer_eof = false;
    uint8_t buf[SSH_CHANNEL_MAX_PACKET];
    const int in = espix_tty_fileno(ch->tty);

    while (!ch->closed && !(tcp_eof && peer_eof)) {
        fd_set rfds;
        FD_ZERO(&rfds);
        if (!peer_eof) {
            FD_SET(in, &rfds);
        }
        if (!tcp_eof) {
            FD_SET(tcp_fd, &rfds);
        }
        if (select(MAX(in, tcp_fd) + 1, &rfds, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (!peer_eof && FD_ISSET(in, &rfds)) {
            const ssize_t r = espix_tty_read(ch->tty, buf, sizeof(buf), true);
            if (r == 0) {
                (void)shutdown(tcp_fd, SHUT_WR);    /* the far end should see EOF too */
                peer_eof = true;
            } else if (r > 0 && !tcp_send_all(tcp_fd, buf, (size_t)r)) {
                break;
            }
        }
        if (!tcp_eof && FD_ISSET(tcp_fd, &rfds)) {
            const ssize_t r = recv(tcp_fd, buf, sizeof(buf), 0);
            if (r > 0) {
                if (send_data(ch, (const char *)buf, (size_t)r) != ESP_OK) {
                    break;
                }
            } else if (r == 0) {
                send_eof(ch);
                tcp_eof = true;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                break;
            }
        }
    }
    return ch->closed ? ESP_FAIL : ESP_OK;
}

static esp_err_t forward_serve(ssh_chan_t *ch, const char *host, uint32_t port)
{
    ssh_conn_t *c = ch->conn;
    ssh_buf_t   b;

    bool      refused = false;
    const int tcp_fd  = forward_connect(host, port, &refused);
    if (tcp_fd < 0) {
        /*
         * Refused before it was attempted, versus tried and failed: the client
         * should be able to tell those apart, and so should whoever reads the
         * log.
         */
        espix_klog(ESPIX_KLOG_WARN, TAG, "forward to %s:%u refused (%s)",
                   host, (unsigned)port,
                   refused ? "destination not permitted"
                           : "cannot connect");

        ssh_buf_init(&b, c->out_buf, sizeof(c->out_buf));
        ssh_put_u8(&b, SSH_MSG_CHANNEL_OPEN_FAILURE);
        ssh_put_u32(&b, ch->peer_chan);
        ssh_put_u32(&b, 2);                 /* connect failed */
        ssh_put_cstr(&b, refused ? "destination not permitted"
                                 : "connect failed");
        ssh_put_cstr(&b, "");
        (void)ssh_packet_write(c, &b);
        return ESP_FAIL;
    }

    /*
     * Confirm only once there is a connection. A confirmation promises a stream,
     * and sending one for a connect that is still in flight would make "why did
     * nothing arrive" a question the client cannot ask.
     */
    ssh_buf_init(&b, c->out_buf, sizeof(c->out_buf));
    ssh_put_u8(&b, SSH_MSG_CHANNEL_OPEN_CONFIRMATION);
    ssh_put_u32(&b, ch->peer_chan);
    ssh_put_u32(&b, 0);                     /* our channel id; only ever one */
    ssh_put_u32(&b, LOCAL_WINDOW);
    ssh_put_u32(&b, SSH_CHANNEL_MAX_PACKET);
    if (ssh_packet_write(c, &b) != ESP_OK) {
        close(tcp_fd);
        return ESP_FAIL;
    }

    /* Bounded, so a far end that stops reading cannot park this task. */
    const struct timeval snd = { .tv_sec = 5, .tv_usec = 0 };
    (void)setsockopt(tcp_fd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd));

    espix_klog(ESPIX_KLOG_INFO, TAG, "direct-tcpip for %s: %s:%u",
               c->user, host, (unsigned)port);

    const esp_err_t err = (chan_start_input(ch, true) == ESP_OK) ? forward_pump(ch, tcp_fd) : ESP_ERR_NO_MEM;
    close(tcp_fd);
    close_channel(ch);

    espix_klog(ESPIX_KLOG_INFO, TAG, "forward to %s:%u closed%s",
               host, (unsigned)port, err == ESP_OK ? "" : " (broken)");
    return err;
}

/*
 * Start where a login would: the account's home directory.
 *
 * Falls back to / when the directory is not actually there. A rootfs can be
 * replaced wholesale by a rootfs flash or edited on the device, and refusing to
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

    /* Last, because USER, HOME and TERM are read from what was just settled. */
    espix_env_set_login_defaults(session);
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
    /* The session has already ended -- jobs drained, processes hung up -- on
     * whichever path ran it; what is left is its environment table. */
    espix_env_free(session);
    if (ch->tx_orphaned) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "transmit lock held by a killed process; closing abruptly");
        return;
    }
    send_exit_status(ch, (uint32_t)session->last_status);
    close_channel(ch);
}

esp_err_t ssh_channel_run(ssh_conn_t *c)
{
    esp_err_t err = ESP_OK;

    /*
     * On the heap, not this task's stack, which runs every shell command; and
     * PSRAM first, with the internal fallback the rest of espix uses. It holds
     * two channel packets (hold, raw) and control data -- not DMA, and not on a
     * path that freezes the flash cache.
     */
    ssh_chan_t *ch = heap_caps_calloc(1, sizeof(*ch), MALLOC_CAP_SPIRAM);
    if (ch == NULL) {
        ch = calloc(1, sizeof(*ch));
    }
    if (ch == NULL) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "out of memory for a channel");
        return ESP_ERR_NO_MEM;
    }

    ch->conn         = c;
    ch->local_window = LOCAL_WINDOW;
    ch->cols         = TERM_COLS_DEF;
    ch->rows         = TERM_ROWS_DEF;
    ch->tx_lock      = xSemaphoreCreateMutex();
    ch->window_sem   = xSemaphoreCreateCounting(64, 0);

    if (ch->tx_lock == NULL || ch->window_sem == NULL) {
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
    const bool is_session = (type_len == strlen("session") &&
                             memcmp(type, "session", type_len) == 0);
    const bool is_tcpip   = (type_len == strlen("direct-tcpip") &&
                             memcmp(type, "direct-tcpip", type_len) == 0);

    if (!is_session && !is_tcpip) {
        ssh_buf_t b;
        ssh_buf_init(&b, c->out_buf, sizeof(c->out_buf));
        ssh_put_u8(&b, SSH_MSG_CHANNEL_OPEN_FAILURE);
        ssh_put_u32(&b, ch->peer_chan);
        ssh_put_u32(&b, 3);             /* unknown channel type */
        ssh_put_cstr(&b, "only session and direct-tcpip channels are supported");
        ssh_put_cstr(&b, "");
        ssh_packet_write(c, &b);
        err = ESP_ERR_NOT_SUPPORTED;
        goto out;
    }

    /*
     * direct-tcpip, which is `ssh -L`: an address, a port, and who asked for
     * it. The host is copied out of the packet buffer immediately, because
     * everything downstream reads packets and can block -- and both overwrite
     * what it points at.
     *
     * One channel per connection, so this is `ssh -N -L`: a connection
     * carrying a forward rather than a shell. A client that wants both on one
     * connection is asking for multiplexing, which this server does not do.
     */
    if (is_tcpip) {
        size_t          host_len = 0;
        size_t          orig_len = 0;
        const uint8_t  *host     = ssh_get_string(&in, &host_len);
        const uint32_t  port     = ssh_get_u32(&in);
        (void)ssh_get_string(&in, &orig_len);   /* originator address */
        (void)ssh_get_u32(&in);                 /* originator port */

        if (in.bad || host == NULL || host_len == 0 || host_len > 255) {
            err = ESP_ERR_INVALID_SIZE;
            goto out;
        }

        char hostname[256];
        memcpy(hostname, host, host_len);
        hostname[host_len] = '\0';

        err = forward_serve(ch, hostname, port);
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
    /* The input, now that the channel's kind is known: a pipe when nothing types into it. */
    err = chan_start_input(ch, ch->want_sftp || (ch->want_exec && !ch->has_pty));
    if (err != ESP_OK) {
        goto out;
    }

    if (ch->want_sftp) {
        espix_session_t session = {
            .name      = "sftp",
            .cwd       = "/",
            .transport = ch,
            .fg_pgid    = ESPIX_PID_NONE,
            .uid       = ESPIX_UID_NOBODY,
            .gid       = ESPIX_UID_NOBODY,
        };
        strlcpy(session.user, c->user, sizeof(session.user));
        apply_account(&session, c->user);

        ch->session = &session;
        espix_shell_session_begin(&session);
        espix_sftp_run(c, &session);
        (void)espix_shell_session_end(&session);
        ch->session = NULL;

        /*
         * The environment apply_account() built, which nothing here ever reads.
         *
         * Not finish_session(): that also hangs up processes and sends the
         * close, and this branch has no processes and has just sent its own.
         * What it shares is the one thing that leaks -- the session's env table
         * is heap-allocated at login and the exec and shell paths free it
         * through finish_session(), while this path went straight to `out`.
         *
         * It cost ~440 bytes and 9 blocks of internal heap per transfer, which
         * a test run turns into ~13K. Measured, not guessed: plain ssh was
         * clean, an sftp fetch of a file that does not exist leaked exactly as
         * much as one that works, and heap tracing named
         * espix_env_set_login_defaults() as the allocator.
         */
        espix_env_free(&session);

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
            .write_err = chan_write_err,
            .poll_interrupt = chan_poll_interrupt,
            .wait_input     = chan_wait_input,
            .term_size      = chan_term_size,
            .task_gone      = chan_task_gone,
            .transport = ch,
            /* Overwritten by apply_account(); nobody rather than 0 so that a
             * path which forgets to set it fails closed instead of open. */
            .uid       = ESPIX_UID_NOBODY,
            .gid       = ESPIX_UID_NOBODY,
            .fg_pgid    = ESPIX_PID_NONE,
            .ansi      = ch->has_pty,
            .open_stream = chan_open_stream,
            .tty       = ch->tty,
        };
        strlcpy(session.user, c->user, sizeof(session.user));
        apply_account(&session, c->user);
        ch->session = &session;

        if (ch->exec_too_long) {
            espix_klog(ESPIX_KLOG_INFO, TAG,
                       "exec for %s: %u bytes, over the %d-byte limit",
                       c->user, (unsigned)ch->exec_len, ESPIX_LINE_MAX - 1);
        } else {
            /* A command line can carry a URL with a password in it. */
            char safe[ESPIX_LINE_MAX];
            espix_net_redact_url(ch->exec_cmd, safe, sizeof(safe));
            espix_klog(ESPIX_KLOG_INFO, TAG, "exec for %s: %s", c->user, safe);
        }

        /*
         * scp's pre-9.0 protocol arrives as `scp -t <path>` on an exec channel,
         * and espix has no such command -- so it used to be refused outright,
         * which at least told the client something true. Now that exec works,
         * the same request would reach the shell and come back as "command not
         * found", which points at the wrong problem. Say what is actually
         * wrong instead.
         */
        if (ch->exec_too_long) {
            /*
             * Inside the channel, where the user is looking, and with a status
             * of its own: 130 is what a shell reports for a job ended by a
             * signal, and this is the closest thing to "I could not read it".
             */
            espix_printf(&session,
                         "espix: command line is %u bytes; the limit is %d\n",
                         (unsigned)ch->exec_len, ESPIX_LINE_MAX - 1);
            session.last_status = 130;
        } else if (strncmp(ch->exec_cmd, "scp ", 4) == 0 ||
                   strcmp(ch->exec_cmd, "scp") == 0) {
            espix_printf(&session,
                         "espix: the scp protocol is not implemented; "
                         "espix serves scp over SFTP, so drop -O\n");
            session.last_status = 127;
        } else {
            espix_shell_set_current(&session);
            espix_shell_session_begin(&session);
            (void)espix_shell_run_line(&session, ch->exec_cmd);
            (void)espix_shell_session_end(&session);
            espix_shell_set_current(NULL);
        }

        finish_session(ch, &session);
        ch->session = NULL;
        goto out;
    }

    espix_klog(ESPIX_KLOG_INFO, TAG, "shell for %s (%ux%u)", c->user,
               ch->cols, ch->rows);

    /*
     * The editor identifies this session by the tty's fd, since its callbacks
     * carry no context pointer. Register before creating the instance: the
     * first prompt calls straight back into them.
     */
    const int tty_fd = espix_tty_fileno(ch->tty);
    editor_map_set(tty_fd, ch);

    /* Belongs to the user and outlives this connection, so reconnecting finds
     * what was typed last time. Not freed at logout. */
    ch->history = espix_history_for(c->user);

    esp_linenoise_config_t ed_cfg;
    esp_linenoise_get_instance_config_default(&ed_cfg);

    ed_cfg.in_fd               = tty_fd;
    ed_cfg.out_fd              = tty_fd;
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
        editor_map_set(tty_fd, NULL);
        err = ESP_ERR_NO_MEM;
        goto out;
    }

    /* Load the user's history into this instance up front; otherwise the first
     * arrow-up of a reconnected session finds nothing until a command runs. */
    espix_history_apply(ch->history, ch->editor);

    /*
     * No esp_linenoise_probe(): the client asked for a pty, so escape sequences
     * are a given, and ssh_edit_write() answers the editor's size queries from
     * the size the client sent.
     */

    espix_session_t session = {
        .name      = "ssh",
        .cwd       = "/",
        .read_line = chan_read_line,
        .write     = chan_write,
        .write_err = chan_write_err,
        .poll_interrupt = chan_poll_interrupt,
        .wait_input     = chan_wait_input,
        .term_size      = chan_term_size,
        .task_gone      = chan_task_gone,
        .transport = ch,
        /* Overwritten by apply_account(); nobody rather than 0 so that a
         * path which forgets to set it fails closed instead of open. */
        .uid       = ESPIX_UID_NOBODY,
        .gid       = ESPIX_UID_NOBODY,
        .fg_pgid    = ESPIX_PID_NONE,
        .ansi      = true,          /* the client asked for a pty */
        .open_stream = chan_open_stream,
        .tty       = ch->tty,
    };
    strlcpy(session.user, c->user, sizeof(session.user));
    ch->session = &session;

    /*
     * A login shell, so `logout` is valid here, and it starts in the user's
     * home directory the way a login does everywhere else. The home comes from
     * the account record, whose `home` field espix_auth has always filled in
     * and nothing has read until now.
     *
     * Falls back to / when the directory is not actually there. A rootfs can be
     * replaced wholesale by a rootfs flash or edited on the device, and refusing
     * to open a shell because a directory went missing would be a poor trade.
     */
    session.login = true;
    apply_account(&session, c->user);

    /* Identical to what the serial console prints, by construction: both go
     * through the same command. */
    espix_shell_exec(&session, "motd");

    espix_shell_session_run(&session);

    esp_linenoise_delete_instance(ch->editor);
    editor_map_set(tty_fd, NULL);

    finish_session(ch, &session);
    ch->session = NULL;

    espix_klog(ESPIX_KLOG_INFO, TAG, "%s logged out", c->user);

out:
    /* The reader first, which dispatches into the tty; then readers see the end and writers fail. */
    ssh_rx_stop(c);
    c->chan = NULL;
    if (ch->tty != NULL) {
        espix_tty_hangup(ch->tty);
    }
    if (ch->tx_lock != NULL) {
        vSemaphoreDelete(ch->tx_lock);
    }
    if (ch->window_sem != NULL) {
        vSemaphoreDelete(ch->window_sem);
    }
    free(ch->exec_cmd);
    free(ch);
    return err;
}
