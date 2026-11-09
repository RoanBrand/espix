/*
 * Pipes: a ring between two VFS descriptors, its reading end and its writing
 * end, for M-mode and U-mode apps alike. A read waits for data or the last
 * writer's close (the end), a write for room or the last reader's close
 * (EPIPE); either wait ends early for a signal, as a terminal's read does.
 * Writes of PIPE_BUF or less are never interleaved with another's.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/select.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_vfs.h"

#include "espix_kernel.h"
#include "espix_proc_priv.h"

#define TAG "pipe"

#define PIPES       16
#define PIPE_SIZE   4096
#define PIPE_ATOMIC 512     /* PIPE_BUF */
#define WAITERS     4

#ifndef FIONREAD
#define FIONREAD 0x4004667f
#define FIONBIO  0x8004667e
#endif

/* IDF's reverse lookup, as espix_fs/vfs.c declares it: which driver a descriptor is, and its local number. */
const void *get_vfs_for_fd(int fd);
const void *get_vfs_for_index(int index);
int get_local_fd(const void *vfs, int fd);

typedef struct sel {
    int           local;
    esp_vfs_select_sem_t sem;
    fd_set       *rd, *wr;
    int           fd;
    struct sel   *next_in_pipe;
    struct sel   *next_in_call;
} sel_t;

typedef struct {
    SemaphoreHandle_t lock;
    uint8_t          *buf;
    uint32_t          rd, wr;           /* free-running; used = wr - rd */
    uint8_t           ends[2];          /* descriptors open on each end */
    bool              nonblock[2];
    TaskHandle_t      waiters[WAITERS];
    sel_t            *selects;
} pipe_t;

static pipe_t      *s_pipes[PIPES];
static portMUX_TYPE s_table = portMUX_INITIALIZER_UNLOCKED;
static esp_vfs_id_t s_vfs   = -1;

static const esp_vfs_fs_ops_t s_ops;

/* local is the pipe's index times two, plus 1 for the writing end. */
static pipe_t *by_local(int local)
{
    if (local < 0 || local >= 2 * PIPES) {
        return NULL;
    }
    portENTER_CRITICAL(&s_table);
    pipe_t *const p = s_pipes[local / 2];
    portEXIT_CRITICAL(&s_table);
    return p;
}

static uint32_t used_locked(const pipe_t *p)
{
    return p->wr - p->rd;
}

/* n bytes between the ring at pos and buf, in at most two pieces. */
static void ring_copy(pipe_t *p, uint32_t pos, uint8_t *buf, size_t n, bool in)
{
    const size_t at = pos % PIPE_SIZE, first = MIN(n, PIPE_SIZE - at);
    if (in) {
        memcpy(p->buf + at, buf, first);
        memcpy(p->buf, buf + first, n - first);
    } else {
        memcpy(buf, p->buf + at, first);
        memcpy(buf + first, p->buf, n - first);
    }
}

static void wake_locked(pipe_t *p)
{
    for (int i = 0; i < WAITERS; i++) {
        if (p->waiters[i] != NULL) {
            xTaskNotifyGiveIndexed(p->waiters[i], ESPIX_NOTIFY_SIG_IDX);
        }
    }
    for (sel_t *s = p->selects; s != NULL; s = s->next_in_pipe) {
        esp_vfs_select_triggered(s->sem);
    }
}

static void waiter_set_locked(pipe_t *p, TaskHandle_t task, bool on)
{
    for (int i = 0; i < WAITERS; i++) {
        if (on ? p->waiters[i] == NULL : p->waiters[i] == task) {
            p->waiters[i] = on ? task : NULL;
            return;
        }
    }
}

/* Until something changes, or a signal is due: -EINTR then. */
static int wait_change(pipe_t *p)
{
    const TaskHandle_t self = xTaskGetCurrentTaskHandle();
    waiter_set_locked(p, self, true);
    xSemaphoreGive(p->lock);
    const int r = espix_sigcheck_io() ? -EINTR : 0;
    if (r == 0) {
        (void)ulTaskNotifyTakeIndexed(ESPIX_NOTIFY_SIG_IDX, pdTRUE, pdMS_TO_TICKS(1000));
    }
    xSemaphoreTake(p->lock, portMAX_DELAY);
    waiter_set_locked(p, self, false);
    return r;
}

static void pipe_free(int index)
{
    portENTER_CRITICAL(&s_table);
    pipe_t *const p = s_pipes[index];
    s_pipes[index] = NULL;
    portEXIT_CRITICAL(&s_table);
    if (p != NULL) {
        vSemaphoreDelete(p->lock);
        heap_caps_free(p->buf);
        free(p);
    }
}

esp_err_t espix_pipe_init(void)
{
    if (s_vfs >= 0) {
        return ESP_OK;
    }
    const esp_err_t err = esp_vfs_register_fs_with_id(&s_ops, ESP_VFS_FLAG_STATIC | ESP_VFS_FLAG_CONTEXT_PTR, NULL,
                                                      &s_vfs);
    if (err != ESP_OK) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "could not register the pipe driver: %s", esp_err_to_name(err));
    }
    return err;
}

int espix_pipe(int fds[2], int flags)
{
    if (s_vfs < 0) {
        errno = ENOSYS;
        return -1;
    }
    pipe_t *p = calloc(1, sizeof(*p));
    if (p != NULL) {
        p->buf  = heap_caps_malloc(PIPE_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        p->lock = xSemaphoreCreateMutex();
    }
    if (p == NULL || p->buf == NULL || p->lock == NULL) {
        if (p != NULL) {
            if (p->lock != NULL) {
                vSemaphoreDelete(p->lock);
            }
            heap_caps_free(p->buf);
            free(p);
        }
        errno = ENOMEM;
        return -1;
    }
    p->ends[0] = p->ends[1] = 1;
    p->nonblock[0] = p->nonblock[1] = (flags & O_NONBLOCK) != 0;

    int index = -1;
    portENTER_CRITICAL(&s_table);
    for (int i = 0; i < PIPES; i++) {
        if (s_pipes[i] == NULL) {
            s_pipes[i] = p;
            index = i;
            break;
        }
    }
    portEXIT_CRITICAL(&s_table);
    if (index < 0) {
        vSemaphoreDelete(p->lock);
        heap_caps_free(p->buf);
        free(p);
        errno = ENFILE;
        return -1;
    }
    if (esp_vfs_register_fd_with_local_fd(s_vfs, 2 * index, false, &fds[0]) != ESP_OK) {
        pipe_free(index);
        errno = EMFILE;
        return -1;
    }
    if (esp_vfs_register_fd_with_local_fd(s_vfs, 2 * index + 1, false, &fds[1]) != ESP_OK) {
        close(fds[0]);
        pipe_free(index);
        errno = EMFILE;
        return -1;
    }
    return 0;
}

/* The local end behind descriptor fd, or -1 when it is not a pipe's. */
static int local_of(int fd)
{
    if (s_vfs < 0) {
        return -1;
    }
    const void *const vfs = get_vfs_for_fd(fd);
    return (vfs != NULL && vfs == get_vfs_for_index(s_vfs)) ? get_local_fd(vfs, fd) : -1;
}

bool espix_pipe_is(int fd)
{
    return local_of(fd) >= 0;
}

int espix_pipe_dup(int fd)
{
    const int local = local_of(fd);
    pipe_t *const p = by_local(local);
    if (p == NULL) {
        errno = EBADF;
        return -1;
    }
    xSemaphoreTake(p->lock, portMAX_DELAY);
    p->ends[local & 1]++;
    xSemaphoreGive(p->lock);
    int nfd;
    if (esp_vfs_register_fd_with_local_fd(s_vfs, local, false, &nfd) != ESP_OK) {
        xSemaphoreTake(p->lock, portMAX_DELAY);
        p->ends[local & 1]--;
        xSemaphoreGive(p->lock);
        errno = EMFILE;
        return -1;
    }
    return nfd;
}

/* ------------------------------------------------------------------ */
/* The VFS driver                                                      */
/* ------------------------------------------------------------------ */

static ssize_t vfs_read(void *ctx, int local, void *buf, size_t len)
{
    (void)ctx;
    pipe_t *const p = by_local(local);
    if (p == NULL || (local & 1)) {
        errno = EBADF;
        return -1;
    }
    if (len == 0) {
        return 0;
    }
    xSemaphoreTake(p->lock, portMAX_DELAY);
    while (used_locked(p) == 0 && p->ends[1] > 0) {
        const int r = p->nonblock[0] ? -EAGAIN : wait_change(p);
        if (r != 0) {
            xSemaphoreGive(p->lock);
            errno = -r;
            return -1;
        }
    }
    const size_t n = MIN(len, (size_t)used_locked(p));
    ring_copy(p, p->rd, buf, n, false);
    p->rd += n;
    if (n > 0) {
        wake_locked(p);
    }
    xSemaphoreGive(p->lock);
    return (ssize_t)n;
}

static ssize_t vfs_write(void *ctx, int local, const void *buf, size_t len)
{
    (void)ctx;
    pipe_t *const p = by_local(local);
    if (p == NULL || !(local & 1)) {
        errno = EBADF;
        return -1;
    }
    size_t done = 0;
    xSemaphoreTake(p->lock, portMAX_DELAY);
    while (done < len) {
        if (p->ends[0] == 0) {
            xSemaphoreGive(p->lock);
            if (done > 0) {
                return (ssize_t)done;
            }
            errno = EPIPE;
            return -1;
        }
        const size_t room = PIPE_SIZE - used_locked(p);
        /* A small write goes in whole or not at all. */
        const size_t need = (len <= PIPE_ATOMIC) ? len : 1;
        if (room < need) {
            const int r = p->nonblock[1] ? -EAGAIN : wait_change(p);
            if (r != 0) {
                xSemaphoreGive(p->lock);
                if (done > 0) {
                    return (ssize_t)done;
                }
                errno = -r;
                return -1;
            }
            continue;
        }
        const size_t n = MIN(len - done, room);
        ring_copy(p, p->wr, (uint8_t *)buf + done, n, true);
        p->wr += n;
        done  += n;
        wake_locked(p);
    }
    xSemaphoreGive(p->lock);
    return (ssize_t)done;
}

static int vfs_close(void *ctx, int local)
{
    (void)ctx;
    pipe_t *const p = by_local(local);
    if (p == NULL) {
        errno = EBADF;
        return -1;
    }
    xSemaphoreTake(p->lock, portMAX_DELAY);
    if (p->ends[local & 1] > 0) {
        p->ends[local & 1]--;
    }
    const bool gone = p->ends[0] == 0 && p->ends[1] == 0 && p->selects == NULL;
    wake_locked(p);
    xSemaphoreGive(p->lock);
    if (gone) {
        pipe_free(local / 2);
    }
    return 0;
}

static int vfs_fstat(void *ctx, int local, struct stat *st)
{
    (void)ctx;
    if (by_local(local) == NULL) {
        errno = EBADF;
        return -1;
    }
    memset(st, 0, sizeof(*st));
    st->st_mode    = S_IFIFO | 0600;
    st->st_nlink   = 1;
    st->st_blksize = PIPE_ATOMIC;
    return 0;
}

static int vfs_fcntl(void *ctx, int local, int cmd, int arg)
{
    (void)ctx;
    pipe_t *const p = by_local(local);
    if (p == NULL) {
        errno = EBADF;
        return -1;
    }
    if (cmd == F_GETFL) {
        return ((local & 1) ? O_WRONLY : O_RDONLY) | (p->nonblock[local & 1] ? O_NONBLOCK : 0);
    }
    if (cmd == F_SETFL) {
        p->nonblock[local & 1] = (arg & O_NONBLOCK) != 0;
        return 0;
    }
    errno = EINVAL;
    return -1;
}

static int vfs_ioctl(void *ctx, int local, int cmd, va_list args)
{
    (void)ctx;
    pipe_t *const p = by_local(local);
    int *const arg = va_arg(args, int *);
    if (p == NULL) {
        errno = EBADF;
        return -1;
    }
    if ((unsigned)cmd == (unsigned)FIONREAD) {
        xSemaphoreTake(p->lock, portMAX_DELAY);
        *arg = (local & 1) ? 0 : (int)used_locked(p);
        xSemaphoreGive(p->lock);
        return 0;
    }
    if ((unsigned)cmd == (unsigned)FIONBIO) {
        p->nonblock[local & 1] = *arg != 0;
        return 0;
    }
    errno = ENOTTY;
    return -1;
}

/* Readable with data or no writer left; writable with room or no reader left (the write then fails). */
static bool ready_locked(const pipe_t *p, int local)
{
    return (local & 1) ? (PIPE_SIZE - used_locked(p) >= PIPE_ATOMIC || p->ends[0] == 0)
                       : (used_locked(p) > 0 || p->ends[1] == 0);
}

static esp_err_t vfs_start_select(int nfds, fd_set *rd, fd_set *wr, fd_set *er, esp_vfs_select_sem_t sem,
                                  void **end_args)
{
    sel_t *list = NULL;
    bool trigger = false;
    /* The sets hold the driver's own numbers. */
    for (int fd = 0; fd < MIN(nfds, 2 * PIPES); fd++) {
        if (!(FD_ISSET(fd, rd) || FD_ISSET(fd, wr) || FD_ISSET(fd, er))) {
            continue;
        }
        const int local = fd;
        pipe_t *const p = by_local(local);
        FD_CLR(fd, er);
        if (p == NULL) {
            continue;
        }
        sel_t *s = calloc(1, sizeof(*s));
        if (s == NULL) {
            *end_args = list;
            return ESP_ERR_NO_MEM;
        }
        *s = (sel_t){ .local = local, .sem = sem, .fd = fd };
        xSemaphoreTake(p->lock, portMAX_DELAY);
        fd_set *const set = (local & 1) ? wr : rd;
        if (FD_ISSET(fd, set)) {
            if (local & 1) {
                s->wr = set;
            } else {
                s->rd = set;
            }
            if (ready_locked(p, local)) {
                trigger = true;
            } else {
                FD_CLR(fd, set);
            }
        }
        FD_CLR(fd, (local & 1) ? rd : wr);
        s->next_in_pipe = p->selects;
        p->selects = s;
        xSemaphoreGive(p->lock);
        s->next_in_call = list;
        list = s;
    }
    *end_args = list;
    if (trigger) {
        esp_vfs_select_triggered(sem);
    }
    return ESP_OK;
}

static esp_err_t vfs_end_select(void *end_args)
{
    for (sel_t *s = end_args, *next; s != NULL; s = next) {
        next = s->next_in_call;
        pipe_t *const p = by_local(s->local);
        if (p != NULL) {
            xSemaphoreTake(p->lock, portMAX_DELAY);
            if (s->rd != NULL && ready_locked(p, s->local)) {
                FD_SET(s->fd, s->rd);
            }
            if (s->wr != NULL && ready_locked(p, s->local)) {
                FD_SET(s->fd, s->wr);
            }
            for (sel_t **pp = &p->selects; *pp != NULL; pp = &(*pp)->next_in_pipe) {
                if (*pp == s) {
                    *pp = s->next_in_pipe;
                    break;
                }
            }
            const bool gone = p->ends[0] == 0 && p->ends[1] == 0 && p->selects == NULL;
            xSemaphoreGive(p->lock);
            if (gone) {
                pipe_free(s->local / 2);
            }
        }
        free(s);
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
    .select  = &s_select,
};
