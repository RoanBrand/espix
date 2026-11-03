/*
 * The session terminal (espix_tty.h). One object per session: an input ring the
 * line discipline fills, termios, the foreground group and the window size. Its
 * one VFS descriptor is an ordinary esp_vfs driver's, so select(), poll(),
 * tcgetattr() and ioctl() reach it the way they reach any other.
 *
 * Canonical input is kept as a line being edited plus completed lines in the
 * ring; `ends` marks where each completed line stops, which is also how an EOF
 * typed on an empty line reaches a reader as a read of 0.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <sys/select.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_vfs.h"

#include "espix_kernel.h"
#include "espix_task.h"
#include "espix_tty.h"

#define TAG "tty"

#define TTY_MAX     24
#define LINE_CAP    255     /* a canonical line, as Linux's 4095 is, smaller */
#define ENDS_MAX    16      /* completed lines waiting to be read */
#define WAITERS_MAX 4       /* tasks blocked in a read at once */
#define ECHO_CHUNK  64      /* input bytes processed per lock, echo written after */

/* lwIP's numbers, which the rest of the system already uses for these two. */
#ifndef FIONREAD
#define FIONREAD 0x4004667f
#define FIONBIO  0x8004667e
#endif

typedef struct select_arg {
    espix_tty_t          *t;
    esp_vfs_select_sem_t  sem;
    fd_set               *rd, *wr;
    int                   fd;
    struct select_arg    *next_in_tty;
    struct select_arg    *next_in_call;
} select_arg_t;

struct espix_tty {
    SemaphoreHandle_t lock;
    int               index;            /* the VFS's local fd */
    int               fd;               /* the global one, or -1 once closed */
    int               refs;             /* atomic */
    bool              pipe;
    bool              hung;             /* the transport is gone */
    bool              eof_in;           /* the input's end, after what is buffered */
    bool              full;             /* input was refused: tell the producer when there is room */
    bool              nonblock;         /* O_NONBLOCK on the descriptor */
    espix_tty_ops_t   ops;
    struct termios    tio;
    struct winsize    ws;
    pid_t             fg;
    int               intrs;            /* ^C in a row, at the same group */
    uint8_t          *in;
    size_t            cap;
    uint32_t          rd, wr;           /* positions, free-running; used = wr - rd */
    uint32_t          ends[ENDS_MAX];
    int               nends;
    uint8_t           line[LINE_CAP];
    size_t            llen;
    TaskHandle_t      waiters[WAITERS_MAX];
    select_arg_t     *selects;
};

static const esp_vfs_fs_ops_t s_ops;

static espix_tty_t      *s_ttys[TTY_MAX];
static portMUX_TYPE      s_table = portMUX_INITIALIZER_UNLOCKED;
static esp_vfs_id_t      s_vfs   = -1;
static espix_tty_hooks_t s_hooks;

void espix_tty_set_hooks(const espix_tty_hooks_t *hooks)
{
    s_hooks = *hooks;
}

static bool canonical(const espix_tty_t *t)
{
    return !t->pipe && (t->tio.c_lflag & ICANON);
}

static size_t used_locked(const espix_tty_t *t)
{
    return t->wr - t->rd;
}

/* What a read could take now: a canonical reader one line at most (0 for an EOF mark), a raw one all of it. */
static size_t avail_locked(const espix_tty_t *t)
{
    return canonical(t) ? (t->nends ? t->ends[0] - t->rd : 0) : used_locked(t);
}

/* Whether a read returns now, data or not. */
static bool ready_locked(const espix_tty_t *t)
{
    return (canonical(t) ? t->nends > 0 : used_locked(t) > 0) || t->hung || t->eof_in;
}

static void wake_locked(espix_tty_t *t)
{
    for (int i = 0; i < WAITERS_MAX; i++) {
        if (t->waiters[i] != NULL) {
            xTaskNotifyGiveIndexed(t->waiters[i], ESPIX_NOTIFY_SIG_IDX);
        }
    }
    for (select_arg_t *a = t->selects; a != NULL; a = a->next_in_tty) {
        if (a->rd != NULL) {
            esp_vfs_select_triggered(a->sem);
        }
    }
}

static void flush_input_locked(espix_tty_t *t)
{
    t->rd    = t->wr;
    t->nends = 0;
    t->llen  = 0;
}

/* ------------------------------------------------------------------ */
/* Lifetime                                                            */
/* ------------------------------------------------------------------ */

espix_tty_t *espix_tty_get(espix_tty_t *t)
{
    __atomic_add_fetch(&t->refs, 1, __ATOMIC_RELAXED);
    return t;
}

void espix_tty_put(espix_tty_t *t)
{
    if (__atomic_sub_fetch(&t->refs, 1, __ATOMIC_ACQ_REL) != 0) {
        return;
    }
    portENTER_CRITICAL(&s_table);
    s_ttys[t->index] = NULL;
    portEXIT_CRITICAL(&s_table);
    vSemaphoreDelete(t->lock);
    heap_caps_free(t->in);
    free(t);
}

static espix_tty_t *by_fd(int fd)
{
    if (fd < 0 || fd >= TTY_MAX) {
        return NULL;
    }
    portENTER_CRITICAL(&s_table);
    espix_tty_t *t = s_ttys[fd];
    portEXIT_CRITICAL(&s_table);
    return t;
}

esp_err_t espix_tty_init(void)
{
    if (s_vfs >= 0) {
        return ESP_OK;
    }
    const esp_err_t err = esp_vfs_register_fs_with_id(&s_ops, ESP_VFS_FLAG_STATIC | ESP_VFS_FLAG_CONTEXT_PTR, NULL,
                                                      &s_vfs);
    if (err != ESP_OK) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "could not register the tty driver: %s", esp_err_to_name(err));
    }
    return err;
}

static void defaults(espix_tty_t *t)
{
    memset(&t->tio, 0, sizeof(t->tio));
    t->tio.c_cc[VMIN] = 1;      /* a read waits for a byte, a pipe's included */
    if (t->pipe) {
        return;
    }
    t->tio.c_iflag = ICRNL;
    t->tio.c_oflag = OPOST | ONLCR;
    t->tio.c_cflag = CREAD | CLOCAL;
    t->tio.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN;
    t->tio.c_cc[VINTR]  = 0x03;
    t->tio.c_cc[VQUIT]  = 0x1c;
    t->tio.c_cc[VERASE] = 0x7f;
    t->tio.c_cc[VKILL]  = 0x15;
    t->tio.c_cc[VEOF]   = 0x04;
    t->tio.c_cc[VSTART] = 0x11;
    t->tio.c_cc[VSTOP]  = 0x13;
    t->tio.c_cc[VSUSP]  = 0x1a;
    t->tio.c_cc[VMIN]   = 1;
    t->tio.c_cc[VTIME]  = 0;
}

espix_tty_t *espix_tty_create(const espix_tty_ops_t *ops, size_t inbuf, bool pipe)
{
    if (s_vfs < 0) {
        return NULL;
    }
    espix_tty_t *t = calloc(1, sizeof(*t));
    if (t == NULL) {
        return NULL;
    }
    t->cap  = MAX(inbuf, (size_t)LINE_CAP + 1);
    t->in   = heap_caps_malloc(t->cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (t->in == NULL) {
        t->in = malloc(t->cap);
    }
    t->lock = xSemaphoreCreateMutex();
    t->ops  = *ops;
    t->pipe = pipe;
    t->fd   = -1;
    t->ws   = (struct winsize){ .ws_row = 24, .ws_col = 80 };
    defaults(t);
    if (t->in == NULL || t->lock == NULL) {
        goto fail;
    }

    portENTER_CRITICAL(&s_table);
    t->index = -1;
    for (int i = 0; i < TTY_MAX; i++) {
        if (s_ttys[i] == NULL) {
            s_ttys[i] = t;
            t->index  = i;
            break;
        }
    }
    portEXIT_CRITICAL(&s_table);
    if (t->index < 0) {
        goto fail;
    }
    t->refs = 2;    /* the creator's, and the descriptor's */
    if (esp_vfs_register_fd_with_local_fd(s_vfs, t->index, false, &t->fd) != ESP_OK) {
        portENTER_CRITICAL(&s_table);
        s_ttys[t->index] = NULL;
        portEXIT_CRITICAL(&s_table);
        goto fail;
    }
    return t;

fail:
    if (t->lock != NULL) {
        vSemaphoreDelete(t->lock);
    }
    heap_caps_free(t->in);
    free(t);
    return NULL;
}

void espix_tty_hangup(espix_tty_t *t)
{
    xSemaphoreTake(t->lock, portMAX_DELAY);
    t->hung = true;
    wake_locked(t);
    const int fd = t->fd;
    xSemaphoreGive(t->lock);
    if (fd >= 0) {
        close(fd);      /* the descriptor's reference goes with it (vfs_close) */
    }
    espix_tty_put(t);
}

int espix_tty_fileno(const espix_tty_t *t)
{
    return t->fd;
}

bool espix_tty_is_pipe(const espix_tty_t *t)
{
    return t->pipe;
}

/* ------------------------------------------------------------------ */
/* Input: the line discipline                                          */
/* ------------------------------------------------------------------ */

/* Bytes the line discipline wants written back to the terminal, collected under the lock. */
typedef struct {
    char   buf[ECHO_CHUNK * 3];
    size_t n;
} echo_t;

static void echo_add(echo_t *e, const char *s, size_t n)
{
    n = MIN(n, sizeof(e->buf) - e->n);
    memcpy(e->buf + e->n, s, n);
    e->n += n;
}

/* c as the terminal should show it: ^X for a control character, under IEXTEN. */
static void echo_char(espix_tty_t *t, echo_t *e, uint8_t c)
{
    if (!(t->tio.c_lflag & ECHO)) {
        return;
    }
    if (c < 0x20 && c != '\n' && c != '\t' && (t->tio.c_lflag & IEXTEN)) {
        const char ctl[2] = { '^', (char)(c + '@') };
        echo_add(e, ctl, 2);
    } else {
        echo_add(e, (const char *)&c, 1);
    }
}

static bool ring_put_locked(espix_tty_t *t, const uint8_t *src, size_t n)
{
    if (t->cap - used_locked(t) < n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        t->in[(t->wr + i) % t->cap] = src[i];
    }
    t->wr += n;
    return true;
}

/* The line being edited into the ring, as one read's worth; false when it does not fit yet. */
static bool commit_line_locked(espix_tty_t *t)
{
    if (t->nends == ENDS_MAX || !ring_put_locked(t, t->line, t->llen)) {
        return false;
    }
    t->ends[t->nends++] = t->wr;
    t->llen = 0;
    return true;
}

/* One character's erase: a UTF-8 sequence is one character. */
static void erase_one_locked(espix_tty_t *t, echo_t *e)
{
    while (t->llen > 0) {
        const uint8_t c = t->line[--t->llen];
        if ((c & 0xc0) != 0x80) {
            if ((t->tio.c_lflag & (ECHO | ECHOE)) == (ECHO | ECHOE)) {
                echo_add(e, (c < 0x20 && (t->tio.c_lflag & IEXTEN)) ? "\b \b\b \b" : "\b \b",
                         (c < 0x20 && (t->tio.c_lflag & IEXTEN)) ? 6 : 3);
            }
            return;
        }
    }
}

static void signal_fg(espix_tty_t *t, int sig)
{
    if (t->fg > 0 && s_hooks.signal_group != NULL) {
        s_hooks.signal_group(t->fg, sig);
    }
}

/* One input byte; false when it cannot be taken yet (a pipe's input is full). */
static bool input_one_locked(espix_tty_t *t, uint8_t c, echo_t *e, int *sig)
{
    const tcflag_t lf = t->tio.c_lflag, inf = t->tio.c_iflag;
    if (t->pipe) {
        return ring_put_locked(t, &c, 1);
    }
    if (t->ops.key != NULL) {
        t->ops.key(t->ops.ctx, c);
    }
    if (inf & ISTRIP) {
        c &= 0x7f;
    }
    if (c == '\r') {
        if (inf & IGNCR) {
            return true;
        }
        if (inf & ICRNL) {
            c = '\n';
        }
    } else if (c == '\n' && (inf & INLCR)) {
        c = '\r';
    }

    if (lf & ISIG) {
        const cc_t *cc = t->tio.c_cc;
        int s = 0;
        if (c != 0 && c == cc[VINTR]) {
            /* The third ^C at a group that is still there insists, unless the session handles ^C itself. */
            s = (t->ops.signal == NULL && ++t->intrs >= 3) ? SIGKILL : SIGINT;
        } else if (c != 0 && c == cc[VQUIT]) {
            s = SIGQUIT;
        } else if (c != 0 && c == cc[VSUSP]) {
            s = SIGTSTP;
        }
        if (s != 0) {
            echo_char(t, e, c);
            if (!(lf & NOFLSH)) {
                flush_input_locked(t);
            }
            *sig = s;
            return true;
        }
    }
    t->intrs = 0;

    if (!(lf & ICANON)) {
        /* A terminal drops what does not fit, as Linux's does; it never holds the transport back. */
        if (ring_put_locked(t, &c, 1)) {
            echo_char(t, e, c);
        }
        return true;
    }

    const cc_t *cc = t->tio.c_cc;
    if (c == cc[VERASE] || c == '\b') {
        erase_one_locked(t, e);
    } else if (c == cc[VKILL]) {
        while (t->llen > 0) {
            erase_one_locked(t, e);
        }
    } else if (c == 0x17 && (lf & IEXTEN)) {
        while (t->llen > 0 && t->line[t->llen - 1] == ' ') {
            erase_one_locked(t, e);
        }
        while (t->llen > 0 && t->line[t->llen - 1] != ' ') {
            erase_one_locked(t, e);
        }
    } else if (c == cc[VEOF] && c != 0) {
        if (!commit_line_locked(t)) {
            t->llen = 0;
        }
    } else if (c == '\n' || (c == cc[VEOL] && c != 0)) {
        if (t->llen == LINE_CAP) {
            t->llen--;
        }
        t->line[t->llen++] = c;
        if (!commit_line_locked(t)) {
            t->llen = 0;    /* a terminal whose reader is not keeping up: the line goes, as Linux's would */
        }
        if ((lf & ECHO) || (lf & ECHONL)) {
            echo_add(e, "\n", 1);
        }
    } else if (t->llen < LINE_CAP - 1) {
        t->line[t->llen++] = c;
        echo_char(t, e, c);
    }
    return true;
}

/* Echo through output processing, to the transport's non-blocking path where it has one. */
static void echo_out(espix_tty_t *t, const char *buf, size_t len)
{
    if (t->ops.echo == NULL) {
        (void)espix_tty_write(t, buf, len);
        return;
    }
    const bool onlcr = (t->tio.c_oflag & (OPOST | ONLCR)) == (OPOST | ONLCR);
    char out[ECHO_CHUNK * 6];
    size_t n = 0;
    for (size_t i = 0; i < len && n < sizeof(out) - 1; i++) {
        if (buf[i] == '\n' && onlcr) {
            out[n++] = '\r';
        }
        out[n++] = buf[i];
    }
    t->ops.echo(t->ops.ctx, out, n);
}

size_t espix_tty_input(espix_tty_t *t, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    size_t done = 0;
    while (done < len) {
        echo_t e = { .n = 0 };
        int sig = 0;
        bool stopped = false;
        xSemaphoreTake(t->lock, portMAX_DELAY);
        const bool was = ready_locked(t);
        const size_t chunk = MIN(len - done, (size_t)ECHO_CHUNK);
        size_t i = 0;
        for (; i < chunk && sig == 0; i++) {
            if (!input_one_locked(t, p[done + i], &e, &sig)) {
                t->full = true;
                stopped = true;
                break;
            }
        }
        done += i;
        if (!was && ready_locked(t)) {
            wake_locked(t);
        }
        const pid_t fg = t->fg;
        xSemaphoreGive(t->lock);
        if (e.n > 0) {
            echo_out(t, e.buf, e.n);
        }
        const bool taken = sig != 0 && t->ops.signal != NULL && t->ops.signal(t->ops.ctx, sig);
        if (sig != 0 && !taken && fg > 0 && s_hooks.signal_group != NULL) {
            s_hooks.signal_group(fg, sig);
        }
        if (stopped) {
            break;
        }
    }
    return done;
}

void espix_tty_input_eof(espix_tty_t *t)
{
    xSemaphoreTake(t->lock, portMAX_DELAY);
    t->eof_in = true;
    wake_locked(t);
    xSemaphoreGive(t->lock);
}

/* ------------------------------------------------------------------ */
/* Readers and writers                                                 */
/* ------------------------------------------------------------------ */

/* Up to len bytes out of the ring: a canonical reader stops at its line's end. */
static size_t take_locked(espix_tty_t *t, uint8_t *dst, size_t len)
{
    size_t n = MIN(len, avail_locked(t));
    for (size_t i = 0; i < n; i++) {
        dst[i] = t->in[(t->rd + i) % t->cap];
    }
    t->rd += n;
    while (t->nends > 0 && (int32_t)(t->ends[0] - t->rd) <= 0) {
        memmove(t->ends, t->ends + 1, (size_t)--t->nends * sizeof(t->ends[0]));
        if (canonical(t)) {
            break;
        }
    }
    return n;
}

static void waiter_set_locked(espix_tty_t *t, TaskHandle_t task, bool on)
{
    for (int i = 0; i < WAITERS_MAX; i++) {
        if (on ? t->waiters[i] == NULL : t->waiters[i] == task) {
            t->waiters[i] = on ? task : NULL;
            return;
        }
    }
}

ssize_t espix_tty_read(espix_tty_t *t, void *buf, size_t len, bool nonblock)
{
    if (len == 0) {
        return 0;
    }
    uint8_t *dst = buf;
    size_t got = 0;
    const TaskHandle_t self = xTaskGetCurrentTaskHandle();
    const pid_t me = (!t->pipe && s_hooks.self_pgid != NULL) ? s_hooks.self_pgid() : 0;
    for (;;) {
        /* Job control: a background group's read stops it, and is tried again once it is continued. */
        if (me > 0 && t->fg > 0 && me != t->fg && !t->hung) {
            const int r = s_hooks.background ? s_hooks.background() : -EIO;
            if (r != 0) {
                return got ? (ssize_t)got : r;
            }
            continue;
        }
        xSemaphoreTake(t->lock, portMAX_DELAY);
        const bool canon = canonical(t);
        const cc_t vmin = t->tio.c_cc[VMIN], vtime = t->tio.c_cc[VTIME];
        /* An EOF mark is an empty line: taking it is this read's 0. */
        const bool eol = canon && t->nends > 0 && t->ends[0] == t->rd;
        got += take_locked(t, dst + got, len - got);
        const bool room = t->full && t->cap - used_locked(t) > LINE_CAP;
        if (room) {
            t->full = false;
        }
        /* Done: a canonical line, the end of input, or as much as VMIN asks. */
        const size_t want = canon ? 1 : (vmin ? MIN((size_t)vmin, len) : 1);
        const bool done = canon ? (got > 0 || eol || t->hung || t->eof_in)
                                : (got >= want || ((t->hung || t->eof_in) && used_locked(t) == 0) ||
                                   (vmin == 0 && vtime == 0));
        if (!done) {
            waiter_set_locked(t, self, true);
        }
        xSemaphoreGive(t->lock);
        if (room && t->ops.room != NULL) {
            t->ops.room(t->ops.ctx);
        }
        if (done) {
            return (ssize_t)got;
        }
        if (nonblock) {
            xSemaphoreTake(t->lock, portMAX_DELAY);
            waiter_set_locked(t, self, false);
            xSemaphoreGive(t->lock);
            return got ? (ssize_t)got : -EAGAIN;
        }
        const int r = s_hooks.interrupted ? s_hooks.interrupted() : 0;
        TickType_t wait = portMAX_DELAY;
        if (!canon && vtime > 0 && (vmin == 0 || got > 0)) {
            wait = pdMS_TO_TICKS((uint32_t)vtime * 100);
        }
        const uint32_t woke = (r == 0) ? ulTaskNotifyTakeIndexed(ESPIX_NOTIFY_SIG_IDX, pdTRUE, wait) : 1;
        xSemaphoreTake(t->lock, portMAX_DELAY);
        waiter_set_locked(t, self, false);
        xSemaphoreGive(t->lock);
        if (r != 0) {
            return got ? (ssize_t)got : r;
        }
        if (woke == 0) {
            return (ssize_t)got;    /* VTIME ran out */
        }
    }
}

size_t espix_tty_readable(espix_tty_t *t)
{
    xSemaphoreTake(t->lock, portMAX_DELAY);
    const size_t n = avail_locked(t);
    xSemaphoreGive(t->lock);
    return n;
}

/* Output processing, then one call to fn: one write reaches the transport as one burst. */
static ssize_t output(espix_tty_t *t, const void *buf, size_t len,
                      ssize_t (*fn)(void *ctx, const void *buf, size_t len))
{
    if (t->hung) {
        return -EIO;
    }
    void *const ctx = t->ops.ctx;
    const bool onlcr = !t->pipe && (t->tio.c_oflag & (OPOST | ONLCR)) == (OPOST | ONLCR);
    const char *p = buf;
    size_t nl = 0;
    for (size_t i = 0; onlcr && i < len; i++) {
        nl += p[i] == '\n';
    }
    if (nl == 0) {
        return fn(ctx, buf, len);
    }
    char stack[256];
    char *out = (len + nl <= sizeof(stack)) ? stack : malloc(len + nl);
    if (out == NULL) {
        return -ENOMEM;
    }
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        if (p[i] == '\n') {
            out[n++] = '\r';
        }
        out[n++] = p[i];
    }
    const ssize_t w = fn(ctx, out, n);
    if (out != stack) {
        free(out);
    }
    return (w < 0) ? w : (ssize_t)len;
}

ssize_t espix_tty_write(espix_tty_t *t, const void *buf, size_t len)
{
    return output(t, buf, len, t->ops.write);
}

ssize_t espix_tty_write_err(espix_tty_t *t, const void *buf, size_t len)
{
    return output(t, buf, len, t->ops.write_err ? t->ops.write_err : t->ops.write);
}

/* ------------------------------------------------------------------ */
/* termios, the foreground group and the window                        */
/* ------------------------------------------------------------------ */

void espix_tty_getattr(espix_tty_t *t, struct termios *tio)
{
    xSemaphoreTake(t->lock, portMAX_DELAY);
    *tio = t->tio;
    xSemaphoreGive(t->lock);
}

void espix_tty_setattr(espix_tty_t *t, int when, const struct termios *tio)
{
    xSemaphoreTake(t->lock, portMAX_DELAY);
    const bool was_canon = canonical(t);
    t->tio = *tio;
    if (when == TCSAFLUSH) {
        flush_input_locked(t);
    } else if (was_canon && !canonical(t) && t->llen > 0) {
        /* Leaving canonical mode: what was being edited is input now, as Linux has it. */
        (void)commit_line_locked(t);
    }
    if (ready_locked(t)) {
        wake_locked(t);
    }
    xSemaphoreGive(t->lock);
}

void espix_tty_makeraw(struct termios *tio)
{
    tio->c_iflag &= ~(tcflag_t)(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    tio->c_oflag &= ~(tcflag_t)OPOST;
    tio->c_lflag &= ~(tcflag_t)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    tio->c_cflag &= ~(tcflag_t)(CSIZE | PARENB);
    tio->c_cflag |= CS8;
    tio->c_cc[VMIN]  = 1;
    tio->c_cc[VTIME] = 0;
}

void espix_tty_make_editing(struct termios *tio)
{
    tio->c_lflag &= ~(tcflag_t)(ICANON | ECHO | ECHOE | ECHOK | ECHONL | ISIG | IEXTEN);
    tio->c_cc[VMIN]  = 1;
    tio->c_cc[VTIME] = 0;
}

void espix_tty_make_keys(struct termios *tio)
{
    tio->c_lflag &= ~(tcflag_t)(ICANON | ECHO | ECHOE | ECHOK | ECHONL | IEXTEN);
    tio->c_lflag |= ISIG;
    tio->c_cc[VMIN]  = 1;
    tio->c_cc[VTIME] = 0;
}

void espix_tty_set_fg(espix_tty_t *t, pid_t pgid)
{
    xSemaphoreTake(t->lock, portMAX_DELAY);
    if (t->fg != pgid) {
        t->intrs = 0;
    }
    t->fg = pgid;
    xSemaphoreGive(t->lock);
}

pid_t espix_tty_get_fg(const espix_tty_t *t)
{
    return t->fg;
}

void espix_tty_set_winsize(espix_tty_t *t, int cols, int rows)
{
    xSemaphoreTake(t->lock, portMAX_DELAY);
    const bool changed = t->ws.ws_col != cols || t->ws.ws_row != rows;
    t->ws.ws_col = (unsigned short)cols;
    t->ws.ws_row = (unsigned short)rows;
    xSemaphoreGive(t->lock);
    if (changed) {
        signal_fg(t, SIGWINCH);
    }
}

void espix_tty_get_winsize(const espix_tty_t *t, int *cols, int *rows)
{
    *cols = t->ws.ws_col;
    *rows = t->ws.ws_row;
}

/* ------------------------------------------------------------------ */
/* The VFS driver behind the descriptor                                */
/* ------------------------------------------------------------------ */

static ssize_t vfs_read(void *ctx, int fd, void *buf, size_t len)
{
    (void)ctx;
    espix_tty_t *t = by_fd(fd);
    if (t == NULL) {
        errno = EBADF;
        return -1;
    }
    const ssize_t r = espix_tty_read(t, buf, len, t->nonblock);
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    return r;
}

static ssize_t vfs_write(void *ctx, int fd, const void *buf, size_t len)
{
    (void)ctx;
    espix_tty_t *t = by_fd(fd);
    if (t == NULL) {
        errno = EBADF;
        return -1;
    }
    const ssize_t r = espix_tty_write(t, buf, len);
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    return r;
}

static int vfs_close(void *ctx, int fd)
{
    (void)ctx;
    espix_tty_t *t = by_fd(fd);
    if (t == NULL) {
        errno = EBADF;
        return -1;
    }
    t->fd = -1;
    espix_tty_put(t);
    return 0;
}

static int vfs_fstat(void *ctx, int fd, struct stat *st)
{
    (void)ctx;
    espix_tty_t *t = by_fd(fd);
    if (t == NULL) {
        errno = EBADF;
        return -1;
    }
    memset(st, 0, sizeof(*st));
    st->st_mode  = t->pipe ? (S_IFIFO | 0600) : (S_IFCHR | 0620);
    st->st_nlink = 1;
    return 0;
}

static int vfs_fcntl(void *ctx, int fd, int cmd, int arg)
{
    (void)ctx;
    espix_tty_t *t = by_fd(fd);
    if (t == NULL) {
        errno = EBADF;
        return -1;
    }
    if (cmd == F_GETFL) {
        return O_RDWR | (t->nonblock ? O_NONBLOCK : 0);
    }
    if (cmd == F_SETFL) {
        t->nonblock = (arg & O_NONBLOCK) != 0;
        return 0;
    }
    errno = EINVAL;
    return -1;
}

static int vfs_ioctl(void *ctx, int fd, int cmd, va_list args)
{
    (void)ctx;
    espix_tty_t *t = by_fd(fd);
    void *arg = va_arg(args, void *);
    if (t == NULL) {
        errno = EBADF;
        return -1;
    }
    switch ((unsigned)cmd) {
    case (unsigned)FIONREAD:
        *(int *)arg = (int)espix_tty_readable(t);
        return 0;
    case (unsigned)FIONBIO:
        t->nonblock = *(int *)arg != 0;
        return 0;
    default:
        break;
    }
    if (t->pipe) {
        errno = ENOTTY;
        return -1;
    }
    switch ((unsigned)cmd) {
    case TIOCGWINSZ:
        *(struct winsize *)arg = t->ws;
        return 0;
    case TIOCSWINSZ:
        espix_tty_set_winsize(t, ((struct winsize *)arg)->ws_col, ((struct winsize *)arg)->ws_row);
        return 0;
    case TIOCGPGRP:
        *(pid_t *)arg = t->fg;
        return 0;
    case TIOCSPGRP:
        espix_tty_set_fg(t, *(pid_t *)arg);
        return 0;
    default:
        errno = ENOTTY;
        return -1;
    }
}

#ifdef CONFIG_VFS_SUPPORT_TERMIOS
static int vfs_tcgetattr(void *ctx, int fd, struct termios *p)
{
    (void)ctx;
    espix_tty_t *t = by_fd(fd);
    if (t == NULL || t->pipe) {
        errno = t ? ENOTTY : EBADF;
        return -1;
    }
    espix_tty_getattr(t, p);
    return 0;
}

static int vfs_tcsetattr(void *ctx, int fd, int when, const struct termios *p)
{
    (void)ctx;
    espix_tty_t *t = by_fd(fd);
    if (t == NULL || t->pipe) {
        errno = t ? ENOTTY : EBADF;
        return -1;
    }
    espix_tty_setattr(t, when, p);
    return 0;
}

static int vfs_tcdrain(void *ctx, int fd)
{
    (void)ctx;
    return by_fd(fd) ? 0 : (errno = EBADF, -1);
}

static int vfs_tcflush(void *ctx, int fd, int select)
{
    (void)ctx;
    espix_tty_t *t = by_fd(fd);
    if (t == NULL) {
        errno = EBADF;
        return -1;
    }
    if (select == TCIFLUSH || select == TCIOFLUSH) {
        xSemaphoreTake(t->lock, portMAX_DELAY);
        flush_input_locked(t);
        xSemaphoreGive(t->lock);
    }
    return 0;
}

static int vfs_tcflow(void *ctx, int fd, int action)
{
    (void)ctx;
    (void)action;
    return by_fd(fd) ? 0 : (errno = EBADF, -1);
}

static pid_t vfs_tcgetsid(void *ctx, int fd)
{
    (void)ctx;
    espix_tty_t *t = by_fd(fd);
    if (t == NULL || t->pipe) {
        errno = t ? ENOTTY : EBADF;
        return -1;
    }
    return t->fg;
}

static int vfs_tcsendbreak(void *ctx, int fd, int duration)
{
    (void)ctx;
    (void)duration;
    return by_fd(fd) ? 0 : (errno = EBADF, -1);
}

static const esp_vfs_termios_ops_t s_termios = {
    .tcsetattr_p   = vfs_tcsetattr,
    .tcgetattr_p   = vfs_tcgetattr,
    .tcdrain_p     = vfs_tcdrain,
    .tcflush_p     = vfs_tcflush,
    .tcflow_p      = vfs_tcflow,
    .tcgetsid_p    = vfs_tcgetsid,
    .tcsendbreak_p = vfs_tcsendbreak,
};
#endif

static esp_err_t vfs_start_select(int nfds, fd_set *rd, fd_set *wr, fd_set *er, esp_vfs_select_sem_t sem,
                                  void **end_args)
{
    select_arg_t *list = NULL;
    bool trigger = false;
    for (int fd = 0; fd < MIN(nfds, TTY_MAX); fd++) {
        espix_tty_t *t = by_fd(fd);
        if (t == NULL || !(FD_ISSET(fd, rd) || FD_ISSET(fd, wr) || FD_ISSET(fd, er))) {
            continue;
        }
        select_arg_t *a = calloc(1, sizeof(*a));
        if (a == NULL) {
            *end_args = list;
            return ESP_ERR_NO_MEM;
        }
        a->t   = espix_tty_get(t);
        a->sem = sem;
        a->fd  = fd;
        FD_CLR(fd, er);
        xSemaphoreTake(t->lock, portMAX_DELAY);
        if (FD_ISSET(fd, rd)) {
            a->rd = rd;
            if (ready_locked(t)) {
                trigger = true;
            } else {
                FD_CLR(fd, rd);
            }
        }
        if (FD_ISSET(fd, wr)) {
            a->wr = wr;
            trigger = true;     /* always writable */
        }
        a->next_in_tty = t->selects;
        t->selects     = a;
        xSemaphoreGive(t->lock);
        a->next_in_call = list;
        list = a;
    }
    *end_args = list;
    if (trigger) {
        esp_vfs_select_triggered(sem);
    }
    return ESP_OK;
}

static esp_err_t vfs_end_select(void *end_args)
{
    for (select_arg_t *a = end_args, *next; a != NULL; a = next) {
        next = a->next_in_call;
        espix_tty_t *t = a->t;
        xSemaphoreTake(t->lock, portMAX_DELAY);
        if (a->rd != NULL && ready_locked(t)) {
            FD_SET(a->fd, a->rd);
        }
        for (select_arg_t **pp = &t->selects; *pp != NULL; pp = &(*pp)->next_in_tty) {
            if (*pp == a) {
                *pp = a->next_in_tty;
                break;
            }
        }
        xSemaphoreGive(t->lock);
        espix_tty_put(t);
        free(a);
    }
    return ESP_OK;
}

static const esp_vfs_select_ops_t s_select = {
    .start_select = vfs_start_select,
    .end_select   = vfs_end_select,
};

static const esp_vfs_fs_ops_t s_ops = {
    .write_p = vfs_write,
    .read_p  = vfs_read,
    .close_p = vfs_close,
    .fstat_p = vfs_fstat,
    .fcntl_p = vfs_fcntl,
    .ioctl_p = vfs_ioctl,
#ifdef CONFIG_VFS_SUPPORT_TERMIOS
    .termios = &s_termios,
#endif
    .select  = &s_select,
};
