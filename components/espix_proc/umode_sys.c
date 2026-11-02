/*
 * U-mode syscalls (docs/USER-MODE.md): each checks the app's pointers and
 * descriptors, then calls the function an M-mode app reaches for the same call.
 */
#include "sdkconfig.h"

#if CONFIG_ESPIX_UMODE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#include "esp_attr.h"
#include "esp_cpu.h"
#include "riscv/encoding.h"

#include "espix_kernel.h"
#include "espix_syscall.h"
#include "espix_umode.h"

#define PG 4096u

typedef struct {
    espix_um_thread_t *t;
    espix_um_proc_t   *p;
    RvExcFrame        *f;
    const uint32_t    *a;     /* a0-a5, in the frame */
} um_call_t;

_Static_assert(offsetof(RvExcFrame, a5) - offsetof(RvExcFrame, a0) == 5 * sizeof(uint32_t), "a0-a5 in order");

typedef int32_t (*um_sys_fn)(um_call_t *c);

/* The errno a kernel call left, as a syscall result. */
static int32_t kret(int r)
{
    return (r < 0) ? -errno : r;
}

/* ------------------------------------------------------------------ */
/* Descriptors                                                         */
/* ------------------------------------------------------------------ */

void espix_um_fds_init(espix_um_proc_t *p)
{
    for (int fd = 0; fd < 3; fd++) {
        p->fds[fd] = (espix_um_fd_t){ .kind = ESPIX_UM_FD_STD, .std = fd };
    }
}

static espix_um_fd_t *fd_get(espix_um_proc_t *p, uint32_t fd)
{
    return (fd < ESPIX_UM_FDS && p->fds[fd].kind != ESPIX_UM_FD_FREE) ? &p->fds[fd] : NULL;
}

/* e into the lowest free number from `from`; the number, or -EMFILE. The process's threads share the table. */
static int fd_install(espix_um_proc_t *p, const espix_um_fd_t *e, uint32_t from)
{
    int n = -EMFILE;
    xSemaphoreTake(p->lock, portMAX_DELAY);
    for (uint32_t fd = from; fd < ESPIX_UM_FDS; fd++) {
        if (p->fds[fd].kind == ESPIX_UM_FD_FREE) {
            p->fds[fd] = *e;
            n = (int)fd;
            break;
        }
    }
    xSemaphoreGive(p->lock);
    return n;
}

/* Number n's entry out of the table, to release outside the lock. */
static espix_um_fd_t fd_take(espix_um_proc_t *p, uint32_t n)
{
    xSemaphoreTake(p->lock, portMAX_DELAY);
    const espix_um_fd_t e = p->fds[n];
    p->fds[n] = (espix_um_fd_t){ 0 };
    xSemaphoreGive(p->lock);
    return e;
}

static int fd_release(espix_um_fd_t *e)
{
    int r = 0;
    if (e->kind == ESPIX_UM_FD_FILE) {
        r = kret(close(e->kfd));
    } else if (e->kind == ESPIX_UM_FD_DIR) {
        r = kret(closedir(e->dir));
    }
    *e = (espix_um_fd_t){ 0 };
    return r;
}

void espix_um_close_fds(espix_proc_slot_t *slot)
{
    espix_um_thread_t *t = (slot->app != NULL) ? slot->app->um : NULL;
    if (t == NULL) {
        return;
    }
    for (int fd = 0; fd < ESPIX_UM_FDS; fd++) {
        fd_release(&t->proc->fds[fd]);
    }
}

/* A copy of e: a second kernel descriptor for a file, so each closes its own. */
static int32_t fd_dup(const espix_um_fd_t *e, espix_um_fd_t *d)
{
    *d = *e;
    d->cloexec = false;
    if (e->kind == ESPIX_UM_FD_FILE) {
        d->kfd = espix_abi_dup(e->kfd);
        if (d->kfd < 0) {
            return -errno;
        }
    } else if (e->kind == ESPIX_UM_FD_DIR) {
        return -EINVAL;
    }
    return 0;
}

/* A copy of e at the lowest free number from `from`. */
static int32_t fd_dup_from(espix_um_proc_t *p, const espix_um_fd_t *e, uint32_t from, bool cloexec)
{
    espix_um_fd_t d;
    const int32_t r = fd_dup(e, &d);
    if (r < 0) {
        return r;
    }
    d.cloexec = cloexec;
    const int n = fd_install(p, &d, from);
    if (n < 0) {
        fd_release(&d);
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Files                                                               */
/* ------------------------------------------------------------------ */

static int32_t path_in(um_call_t *c, uint32_t uva, char *buf)
{
    const int r = espix_um_strin(c->p->as, buf, uva, ESPIX_PATH_MAX);
    return (r < 0) ? r : 0;
}

/* stdin as a terminal reads it: up to a newline, or what the end of input leaves. */
static int32_t read_std(um_call_t *c, FILE *in, uint32_t uva, uint32_t len)
{
    /* Copied out a chunk at a time: getc() may block, so no user page is held across it. */
    char buf[256];
    uint32_t done = 0;
    bool more = true;
    while (more && done < len) {
        uint32_t n = 0;
        while (n < sizeof(buf) && done + n < len) {
            const int ch = getc(in);
            if (ch == EOF) {
                /* A signal for this thread ended the wait, not the input. */
                if ((c->t->sig_pending & ~c->t->sig_blocked) != 0) {
                    clearerr(in);
                    if (done + n == 0) {
                        return -EINTR;
                    }
                }
                more = false;
                break;
            }
            buf[n++] = (char)ch;
            if (ch == '\n') {
                more = false;
                break;
            }
        }
        if (n > 0 && espix_um_copyout(c->p->as, uva + done, buf, n) < 0) {
            return done ? (int32_t)done : -EFAULT;
        }
        done += n;
    }
    return (int32_t)done;
}

static int32_t sys_read(um_call_t *c)
{
    espix_um_fd_t *e = fd_get(c->p, c->a[0]);
    const uint32_t uva = c->a[1], len = c->a[2];
    if (e == NULL) {
        return -EBADF;
    }
    if (e->kind == ESPIX_UM_FD_STD) {
        return (e->std == 0) ? read_std(c, espix_proc_stdio(0), uva, len) : -EBADF;
    }
    if (e->kind != ESPIX_UM_FD_FILE) {
        return -EISDIR;
    }
    uint32_t done = 0;
    while (done < len) {
        const uint32_t va = uva + done;
        void *k = espix_um_user_pin(c->p->as, va, PTE_W);
        if (k == NULL) {
            return done ? (int32_t)done : -EFAULT;
        }
        const uint32_t want = MIN(len - done, PG - (va & (PG - 1)));
        const ssize_t got = read(e->kfd, k, want);
        espix_um_user_unpin(c->p->as);
        if (got < 0) {
            return done ? (int32_t)done : -errno;
        }
        done += (uint32_t)got;
        if ((uint32_t)got < want) {
            break;
        }
    }
    return (int32_t)done;
}

static int32_t sys_write(um_call_t *c)
{
    espix_um_fd_t *e = fd_get(c->p, c->a[0]);
    const uint32_t uva = c->a[1], len = c->a[2];
    if (e == NULL || (e->kind == ESPIX_UM_FD_STD && e->std == 0)) {
        return -EBADF;
    }
    if (e->kind == ESPIX_UM_FD_DIR) {
        return -EISDIR;
    }
    FILE *const out = (e->kind == ESPIX_UM_FD_STD) ? espix_proc_stdio(e->std) : NULL;
    uint32_t done = 0;
    while (done < len) {
        const uint32_t va = uva + done;
        const void *k = espix_um_user_pin(c->p->as, va, PTE_R);
        if (k == NULL) {
            if (done == 0) {
                return -EFAULT;
            }
            break;
        }
        const uint32_t want = MIN(len - done, PG - (va & (PG - 1)));
        const ssize_t put = out ? (ssize_t)fwrite(k, 1, want, out) : write(e->kfd, k, want);
        espix_um_user_unpin(c->p->as);
        if (put < 0) {
            if (done == 0) {
                return -errno;
            }
            break;
        }
        done += (uint32_t)put;
        if ((uint32_t)put < want) {
            break;
        }
    }
    if (out != NULL) {
        fflush(out);
    }
    return (int32_t)done;
}

static int32_t sys_open(um_call_t *c)
{
    char path[ESPIX_PATH_MAX];
    int32_t r = path_in(c, c->a[0], path);
    const int flags = (int)c->a[1];
    if (r < 0) {
        return r;
    }
    espix_um_fd_t e = { .cloexec = (flags & O_CLOEXEC) != 0 };
    if (flags & O_DIRECTORY) {
        e.kind = ESPIX_UM_FD_DIR;
        e.dir  = opendir(path);
        if (e.dir == NULL) {
            return -errno;
        }
    } else {
        e.kind = ESPIX_UM_FD_FILE;
        e.kfd  = open(path, flags & ~(O_CLOEXEC | O_DIRECTORY), (int)c->a[2]);
        if (e.kfd < 0) {
            return -errno;
        }
    }
    const int fd = fd_install(c->p, &e, 0);
    if (fd < 0) {
        fd_release(&e);
    }
    return fd;
}

static int32_t sys_close(um_call_t *c)
{
    if (fd_get(c->p, c->a[0]) == NULL) {
        return -EBADF;
    }
    espix_um_fd_t e = fd_take(c->p, c->a[0]);
    return fd_release(&e);
}

static int32_t sys_lseek(um_call_t *c)
{
    espix_um_fd_t *e = fd_get(c->p, c->a[0]);
    const int64_t off = (int64_t)(((uint64_t)c->a[2] << 32) | c->a[1]);
    const int whence = (int)c->a[3];
    if (e == NULL) {
        return -EBADF;
    }
    uint64_t pos;
    if (e->kind == ESPIX_UM_FD_FILE) {
        const off_t r = lseek(e->kfd, (off_t)off, whence);
        if (r < 0) {
            return -errno;
        }
        pos = (uint64_t)r;
    } else if (e->kind == ESPIX_UM_FD_DIR && off == 0 && whence == SEEK_SET) {
        rewinddir(e->dir);
        pos = 0;
    } else {
        return (e->kind == ESPIX_UM_FD_DIR) ? -EINVAL : -ESPIPE;
    }
    return espix_um_copyout(c->p->as, c->a[4], &pos, sizeof(pos));
}

static int32_t stat_out(um_call_t *c, uint32_t uva, const struct stat *st)
{
    const struct espix_stat e = {
        .size = (uint64_t)st->st_size, .atime = st->st_atime, .mtime = st->st_mtime, .ctime = st->st_ctime,
        .dev = (uint32_t)st->st_dev, .ino = (uint32_t)st->st_ino, .mode = st->st_mode,
        .nlink = st->st_nlink, .uid = st->st_uid, .gid = st->st_gid, .rdev = (uint32_t)st->st_rdev,
        .blksize = (uint32_t)st->st_blksize, .blocks = (uint32_t)st->st_blocks,
    };
    return espix_um_copyout(c->p->as, uva, &e, sizeof(e));
}

static int32_t sys_fstat(um_call_t *c)
{
    espix_um_fd_t *e = fd_get(c->p, c->a[0]);
    struct stat st = { 0 };
    if (e == NULL) {
        return -EBADF;
    }
    if (e->kind == ESPIX_UM_FD_FILE) {
        if (fstat(e->kfd, &st) != 0) {
            return -errno;
        }
    } else {
        st.st_mode = (e->kind == ESPIX_UM_FD_DIR) ? (S_IFDIR | 0755) : (S_IFCHR | 0620);
        st.st_nlink = 1;
    }
    return stat_out(c, c->a[1], &st);
}

static int32_t sys_stat(um_call_t *c)
{
    char path[ESPIX_PATH_MAX];
    struct stat st;
    const int32_t r = path_in(c, c->a[0], path);
    if (r < 0) {
        return r;
    }
    return (stat(path, &st) != 0) ? -errno : stat_out(c, c->a[1], &st);
}

/* A call that takes one path and an int. */
#define PATH_CALL(name, expr)                       \
    static int32_t sys_##name(um_call_t *c)         \
    {                                               \
        char path[ESPIX_PATH_MAX];                  \
        const int32_t r = path_in(c, c->a[0], path); \
        return (r < 0) ? r : kret(expr);            \
    }

PATH_CALL(unlink, unlink(path))
PATH_CALL(mkdir,  mkdir(path, (mode_t)c->a[1]))
PATH_CALL(rmdir,  rmdir(path))
PATH_CALL(chdir,  espix_abi_chdir(path))
PATH_CALL(chmod,  espix_abi_chmod(path, (mode_t)c->a[1]))
PATH_CALL(truncate, truncate(path, (off_t)(((uint64_t)c->a[2] << 32) | c->a[1])))

static int32_t sys_rename(um_call_t *c)
{
    char from[ESPIX_PATH_MAX], to[ESPIX_PATH_MAX];
    int32_t r = path_in(c, c->a[0], from);
    if (r == 0) {
        r = path_in(c, c->a[1], to);
    }
    return (r < 0) ? r : kret(rename(from, to));
}

static int32_t sys_ftruncate(um_call_t *c)
{
    espix_um_fd_t *e = fd_get(c->p, c->a[0]);
    if (e == NULL || e->kind != ESPIX_UM_FD_FILE) {
        return e ? -EINVAL : -EBADF;
    }
    return kret(ftruncate(e->kfd, (off_t)(((uint64_t)c->a[2] << 32) | c->a[1])));
}

static int32_t sys_fsync(um_call_t *c)
{
    espix_um_fd_t *e = fd_get(c->p, c->a[0]);
    if (e == NULL) {
        return -EBADF;
    }
    return (e->kind == ESPIX_UM_FD_FILE) ? kret(fsync(e->kfd)) : 0;
}

static int32_t sys_utime(um_call_t *c)
{
    char path[ESPIX_PATH_MAX];
    int32_t r = path_in(c, c->a[0], path);
    if (r < 0) {
        return r;
    }
    if (c->a[1] == 0) {
        return kret(utime(path, NULL));
    }
    struct espix_utimbuf u;
    r = espix_um_copyin(c->p->as, &u, c->a[1], sizeof(u));
    const struct utimbuf k = { .actime = (time_t)u.actime, .modtime = (time_t)u.modtime };
    return (r < 0) ? r : kret(utime(path, &k));
}

static int32_t sys_getcwd(um_call_t *c)
{
    char buf[ESPIX_PATH_MAX];
    if (espix_abi_getcwd(buf, sizeof(buf)) == NULL) {
        return -errno;
    }
    const size_t n = strlen(buf) + 1;
    if (n > c->a[1]) {
        return -ERANGE;
    }
    const int r = espix_um_copyout(c->p->as, c->a[0], buf, n);
    return (r < 0) ? r : (int32_t)n;
}

static int32_t sys_dup(um_call_t *c)
{
    espix_um_fd_t *e = fd_get(c->p, c->a[0]);
    if (e == NULL) {
        return -EBADF;
    }
    return fd_dup_from(c->p, e, 0, false);
}

static int32_t sys_dup2(um_call_t *c)
{
    espix_um_fd_t *e = fd_get(c->p, c->a[0]);
    const uint32_t n = c->a[1];
    if (e == NULL || n >= ESPIX_UM_FDS) {
        return -EBADF;
    }
    if (n == c->a[0]) {
        return (int32_t)n;
    }
    espix_um_fd_t d;
    const int32_t r = fd_dup(e, &d);
    if (r < 0) {
        return r;
    }
    xSemaphoreTake(c->p->lock, portMAX_DELAY);
    espix_um_fd_t old = c->p->fds[n];
    c->p->fds[n] = d;
    xSemaphoreGive(c->p->lock);
    fd_release(&old);
    return (int32_t)n;
}

static int32_t sys_fcntl(um_call_t *c)
{
    espix_um_fd_t *e = fd_get(c->p, c->a[0]);
    const int cmd = (int)c->a[1];
    const uint32_t arg = c->a[2];
    if (e == NULL) {
        return -EBADF;
    }
    switch (cmd) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: {
        return (arg >= ESPIX_UM_FDS) ? -EINVAL : fd_dup_from(c->p, e, arg, cmd == F_DUPFD_CLOEXEC);
    }
    case F_GETFD:
        return e->cloexec ? FD_CLOEXEC : 0;
    case F_SETFD:
        e->cloexec = (arg & FD_CLOEXEC) != 0;
        return 0;
    case F_GETFL:
        if (e->kind == ESPIX_UM_FD_FILE) {
            return kret(fcntl(e->kfd, F_GETFL));
        }
        return (e->kind == ESPIX_UM_FD_STD && e->std != 0) ? O_WRONLY : O_RDONLY;
    case F_SETFL:
        return (e->kind == ESPIX_UM_FD_FILE) ? kret(fcntl(e->kfd, F_SETFL, (int)arg)) : 0;
    default:
        return -EINVAL;
    }
}

static int32_t sys_getdents(um_call_t *c)
{
    espix_um_fd_t *e = fd_get(c->p, c->a[0]);
    const uint32_t uva = c->a[1], len = c->a[2];
    if (e == NULL) {
        return -EBADF;
    }
    if (e->kind != ESPIX_UM_FD_DIR) {
        return -ENOTDIR;
    }
    uint32_t done = 0;
    for (;;) {
        const long at = telldir(e->dir);
        errno = 0;
        const struct dirent *de = readdir(e->dir);
        if (de == NULL) {
            if (errno != 0 && done == 0) {
                return -errno;
            }
            break;
        }
        const size_t name = strlen(de->d_name) + 1;
        const uint32_t reclen = (uint32_t)((sizeof(struct espix_dirent) + name + 3) & ~3u);
        if (done + reclen > len) {
            seekdir(e->dir, at);
            if (done == 0) {
                return -EINVAL;
            }
            break;
        }
        uint32_t rec[(sizeof(struct espix_dirent) + sizeof(de->d_name) + 3) / 4] = { 0 };
        struct espix_dirent *d = (struct espix_dirent *)rec;
        d->ino    = (uint32_t)de->d_ino;
        d->reclen = (uint16_t)reclen;
        d->type   = de->d_type;
        memcpy(d->name, de->d_name, name);
        const int r = espix_um_copyout(c->p->as, uva + done, rec, reclen);
        if (r < 0) {
            return r;
        }
        done += reclen;
    }
    return (int32_t)done;
}

static int32_t sys_isatty(um_call_t *c)
{
    espix_um_fd_t *e = fd_get(c->p, c->a[0]);
    if (e == NULL) {
        return -EBADF;
    }
    return (e->kind == ESPIX_UM_FD_STD) || (e->kind == ESPIX_UM_FD_FILE && isatty(e->kfd));
}

/* ------------------------------------------------------------------ */
/* Time                                                                */
/* ------------------------------------------------------------------ */

static int32_t sys_clock_gettime(um_call_t *c)
{
    struct timespec ts;
    if (clock_gettime((clockid_t)c->a[0], &ts) != 0) {
        return -errno;
    }
    const struct espix_timespec e = { .sec = ts.tv_sec, .nsec = (int32_t)ts.tv_nsec };
    return espix_um_copyout(c->p->as, c->a[1], &e, sizeof(e));
}

static int32_t sys_nanosleep(um_call_t *c)
{
    struct espix_timespec u;
    int32_t r = espix_um_copyin(c->p->as, &u, c->a[0], sizeof(u));
    if (r < 0) {
        return r;
    }
    const struct timespec req = { .tv_sec = (time_t)u.sec, .tv_nsec = u.nsec };
    struct timespec rem = { 0 };
    r = kret(espix_abi_nanosleep(&req, &rem));
    if (r == -EINTR && c->a[1] != 0) {
        const struct espix_timespec left = { .sec = rem.tv_sec, .nsec = (int32_t)rem.tv_nsec };
        espix_um_copyout(c->p->as, c->a[1], &left, sizeof(left));
    }
    return r;
}

/* ------------------------------------------------------------------ */
/* Processes                                                           */
/* ------------------------------------------------------------------ */

static int32_t sys_exit(um_call_t *c)
{
    espix_um_exit_group(c->t, (int)c->a[0]);
}

/* ------------------------------------------------------------------ */
/* Threads                                                             */
/* ------------------------------------------------------------------ */

static int32_t sys_thread_create(um_call_t *c)
{
    return espix_um_thread_create(c->t, c->a[0], c->a[1], c->a[2], c->a[3], c->a[4]);
}

static int32_t sys_thread_exit(um_call_t *c)
{
    espix_um_thread_exit(c->t);
}

static int32_t sys_gettid(um_call_t *c)
{
    return (int32_t)c->t->tid;
}

static int32_t sys_sched_yield(um_call_t *c)
{
    (void)c;
    taskYIELD();
    return 0;
}

static int32_t sys_getcpu(um_call_t *c)
{
    (void)c;
    return (int32_t)esp_cpu_get_core_id();
}

static int32_t sys_futex(um_call_t *c)
{
    switch (c->a[1]) {
    case ESPIX_FUTEX_WAIT: {
        int64_t us = -1;
        if (c->a[3] != 0) {
            struct espix_timespec ts;
            const int r = espix_um_copyin(c->p->as, &ts, c->a[3], sizeof(ts));
            if (r < 0) {
                return r;
            }
            if (ts.sec < 0 || ts.nsec < 0 || ts.nsec >= 1000000000) {
                return -EINVAL;
            }
            us = ts.sec * 1000000 + ts.nsec / 1000;
        }
        return espix_um_futex_wait(c->p, c->a[0], c->a[2], us);
    }
    case ESPIX_FUTEX_WAKE:
        return espix_um_futex_wake(c->p, c->a[0], (int32_t)c->a[2]);
    default:
        return -ENOSYS;
    }
}

static int32_t sys_getpid(um_call_t *c)  { (void)c; return espix_abi_getpid(); }
static int32_t sys_getppid(um_call_t *c) { (void)c; return espix_abi_getppid(); }
static int32_t sys_getuid(um_call_t *c)  { (void)c; return (int32_t)getuid(); }
static int32_t sys_geteuid(um_call_t *c) { (void)c; return (int32_t)geteuid(); }
static int32_t sys_getgid(um_call_t *c)  { (void)c; return (int32_t)getgid(); }
static int32_t sys_getegid(um_call_t *c) { (void)c; return (int32_t)getegid(); }
static int32_t sys_getpgrp(um_call_t *c) { (void)c; return espix_abi_getpgrp(); }
static int32_t sys_getsid(um_call_t *c)  { return kret(espix_abi_getsid((pid_t)c->a[0])); }
static int32_t sys_kill(um_call_t *c)    { return kret(espix_abi_kill((pid_t)c->a[0], (int)c->a[1])); }

/* ------------------------------------------------------------------ */
/* Signals (umode_sig.c)                                               */
/* ------------------------------------------------------------------ */

static int32_t sys_sigaction(um_call_t *c)   { return espix_um_sigaction(c->t, (int)c->a[0], c->a[1], c->a[2]); }
static int32_t sys_sigprocmask(um_call_t *c) { return espix_um_sigprocmask(c->t, (int)c->a[0], c->a[1], c->a[2]); }
static int32_t sys_sigpending(um_call_t *c)  { return espix_um_sigpending(c->t, c->a[0]); }
static int32_t sys_sigsuspend(um_call_t *c)  { return espix_um_sigsuspend(c->t, c->a[0]); }
static int32_t sys_raise(um_call_t *c)       { return espix_um_raise(c->t, (int)c->a[0]); }

/* espix_um_kentry() takes it before the table: it returns into the frame, not with a result. */
static int32_t sys_sigreturn(um_call_t *c)
{
    (void)c;
    return -ENOSYS;
}

static int32_t tv_in(struct timeval *tv, const struct espix_timespec *e)
{
    if (e->sec < 0 || e->nsec < 0 || e->nsec >= 1000000000) {
        return -EINVAL;
    }
    *tv = (struct timeval){ .tv_sec = (time_t)e->sec, .tv_usec = e->nsec / 1000 };
    return 0;
}

static struct espix_timespec tv_out(const struct timeval *tv)
{
    return (struct espix_timespec){ .sec = tv->tv_sec, .nsec = (int32_t)tv->tv_usec * 1000 };
}

static int32_t sys_setitimer(um_call_t *c)
{
    struct itimerval nv, ov;
    if (c->a[1] != 0) {
        struct espix_itimerval u;
        int32_t r = espix_um_copyin(c->p->as, &u, c->a[1], sizeof(u));
        if (r == 0) {
            r = tv_in(&nv.it_interval, &u.interval);
        }
        if (r == 0) {
            r = tv_in(&nv.it_value, &u.value);
        }
        if (r != 0) {
            return r;
        }
    }
    const int32_t r = kret(espix_abi_setitimer((int)c->a[0], c->a[1] ? &nv : NULL, &ov));
    if (r < 0 || c->a[2] == 0) {
        return r;
    }
    const struct espix_itimerval o = { .interval = tv_out(&ov.it_interval), .value = tv_out(&ov.it_value) };
    return espix_um_copyout(c->p->as, c->a[2], &o, sizeof(o));
}

static int32_t sys_term_size(um_call_t *c)
{
    int cols, rows;
    if (espix_term_size(&cols, &rows) != 0) {
        return -errno;
    }
    const int32_t r = espix_um_copyout(c->p->as, c->a[0], &cols, sizeof(cols));
    return (r != 0) ? r : espix_um_copyout(c->p->as, c->a[1], &rows, sizeof(rows));
}

/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */

static int32_t sys_brk(um_call_t *c)
{
    return (int32_t)espix_um_brk(c->p->as, c->a[0]);
}

/* ------------------------------------------------------------------ */

#define UM_SYS_ENTRY(name, lname, nr) [nr] = sys_##lname,
static const DRAM_ATTR um_sys_fn s_sys[] = { ESPIX_SYSCALLS(UM_SYS_ENTRY) };

IRAM_ATTR int32_t espix_um_syscall(espix_um_thread_t *t, RvExcFrame *f)
{
    um_call_t c = { .t = t, .p = t->proc, .f = f, .a = (const uint32_t *)&f->a0 };
    if (f->a7 >= sizeof(s_sys) / sizeof(s_sys[0]) || s_sys[f->a7] == NULL) {
        return -ENOSYS;
    }
    return s_sys[f->a7](&c);
}

#endif /* CONFIG_ESPIX_UMODE */
