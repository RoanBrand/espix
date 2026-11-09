/*
 * U-mode syscalls (docs/USER-MODE.md): each checks the app's pointers and
 * descriptors, then calls the function an M-mode app reaches for the same call.
 */
#include "sdkconfig.h"

#if CONFIG_ESPIX_UMODE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <signal.h>
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
#include "esp_heap_caps.h"
#include "riscv/encoding.h"

#include "espix_fs.h"
#include "espix_kernel.h"
#include "espix_syscall.h"
#include "espix_tty.h"
#include "espix_umode.h"
#include "umode_sys.h"

#define PG 4096u

_Static_assert(offsetof(RvExcFrame, a5) - offsetof(RvExcFrame, a0) == 5 * sizeof(uint32_t), "a0-a5 in order");

typedef int32_t (*um_sys_fn)(um_call_t *c);

/* ------------------------------------------------------------------ */
/* Descriptors                                                         */
/* ------------------------------------------------------------------ */

void espix_um_fds_init(espix_um_proc_t *p)
{
    for (int fd = 0; fd < 3; fd++) {
        p->fds[fd] = (espix_um_fd_t){ .kind = ESPIX_UM_FD_STD, .std = fd };
    }
}

espix_um_fd_t *um_fd_get(espix_um_proc_t *p, uint32_t fd)
{
    return (fd < ESPIX_UM_FDS && p->fds[fd].kind != ESPIX_UM_FD_FREE) ? &p->fds[fd] : NULL;
}

/* The process's threads share the table. */
int um_fd_install(espix_um_proc_t *p, const espix_um_fd_t *e, uint32_t from)
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

int um_fd_release(espix_um_fd_t *e)
{
    int r = 0;
    if (e->kind == ESPIX_UM_FD_FILE) {
        r = um_kret(close(e->kfd));
    } else if (e->kind == ESPIX_UM_FD_DIR) {
        r = um_kret(closedir(e->dir));
    } else if (e->kind == ESPIX_UM_FD_SOCK) {
        r = um_sock_close(e->sock);
    }
    *e = (espix_um_fd_t){ 0 };
    return r;
}

void espix_um_fds_drop(espix_um_proc_t *p)
{
    for (int fd = 0; fd < ESPIX_UM_FDS; fd++) {
        um_fd_release(&p->fds[fd]);
    }
}

void espix_um_fds_cloexec(espix_um_proc_t *p)
{
    for (int fd = 0; fd < ESPIX_UM_FDS; fd++) {
        if (p->fds[fd].cloexec) {
            um_fd_release(&p->fds[fd]);
        }
    }
}

void espix_um_close_fds(espix_proc_slot_t *slot)
{
    espix_um_thread_t *t = (slot->app != NULL) ? slot->app->um : NULL;
    if (t == NULL) {
        return;
    }
    espix_um_fds_drop(t->proc);
    espix_um_objs_release(t->proc);
    /* A vfork's child leaves its parent's mappings alone. */
    if (t->proc->vfork == NULL && t->proc->as != NULL) {
        espix_um_unmap_files(t->proc->as);
    }
}

/* A copy of e: a second kernel descriptor for a file, so each closes its own; a socket is shared. */
static int32_t fd_dup(const espix_um_fd_t *e, espix_um_fd_t *d)
{
    *d = *e;
    d->cloexec = false;
    if (e->kind == ESPIX_UM_FD_SOCK) {
        __atomic_add_fetch(&e->sock->refs, 1, __ATOMIC_RELAXED);
    } else if (e->kind == ESPIX_UM_FD_FILE) {
        d->kfd = espix_abi_dup(e->kfd);
        if (d->kfd < 0) {
            return -errno;
        }
    } else if (e->kind == ESPIX_UM_FD_DIR) {
        return -EINVAL;
    }
    return 0;
}

int32_t espix_um_fds_fork(espix_um_proc_t *from, espix_um_proc_t *to)
{
    int32_t r = 0;
    xSemaphoreTake(from->lock, portMAX_DELAY);
    for (int fd = 0; fd < ESPIX_UM_FDS && r == 0; fd++) {
        const espix_um_fd_t *e = &from->fds[fd];
        /* A directory stream stays its opener's. */
        if (e->kind == ESPIX_UM_FD_FREE || e->kind == ESPIX_UM_FD_DIR) {
            continue;
        }
        r = fd_dup(e, &to->fds[fd]);
        if (r == 0) {
            to->fds[fd].cloexec = e->cloexec;
            /* Both processes close it themselves; the sweep at either's end must not. */
            if (e->kind == ESPIX_UM_FD_FILE) {
                espix_fs_fd_disown(e->kfd);
            }
        } else {
            to->fds[fd] = (espix_um_fd_t){ 0 };
        }
    }
    xSemaphoreGive(from->lock);
    if (r != 0) {
        espix_um_fds_drop(to);
    }
    return r;
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
    const int n = um_fd_install(p, &d, from);
    if (n < 0) {
        um_fd_release(&d);
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

/*
 * A pipe through a buffer of the kernel's: a read returns what one wait brings, and a write of
 * PIPE_BUF or less goes in whole, neither holding a user page while it waits.
 */
#define PIPE_CHUNK 512

static int32_t pipe_io(um_call_t *c, int kfd, uint32_t uva, uint32_t len, bool out)
{
    char buf[PIPE_CHUNK];
    if (!out) {
        const ssize_t got = read(kfd, buf, MIN(len, sizeof(buf)));
        if (got <= 0) {
            return (got < 0) ? -errno : 0;
        }
        return (espix_um_copyout(c->p->as, uva, buf, (size_t)got) < 0) ? -EFAULT : (int32_t)got;
    }
    uint32_t done = 0;
    while (done < len) {
        const uint32_t n = MIN(len - done, (uint32_t)sizeof(buf));
        if (espix_um_copyin(c->p->as, buf, uva + done, n) < 0) {
            return done ? (int32_t)done : -EFAULT;
        }
        const ssize_t put = write(kfd, buf, n);
        if (put < 0) {
            if (errno == EPIPE) {
                (void)espix_um_raise(c->t, SIGPIPE);
            }
            return done ? (int32_t)done : -errno;
        }
        done += (uint32_t)put;
        if ((uint32_t)put < n) {
            break;
        }
    }
    return (int32_t)done;
}

static int32_t um_sys_read(um_call_t *c)
{
    espix_um_fd_t *e = um_fd_get(c->p, c->a[0]);
    const uint32_t uva = c->a[1], len = c->a[2];
    if (e == NULL) {
        return -EBADF;
    }
    if (e->kind == ESPIX_UM_FD_STD) {
        return (e->std == 0) ? read_std(c, espix_proc_stdio(0), uva, len) : -EBADF;
    }
    if (e->kind == ESPIX_UM_FD_SOCK) {
        return um_sock_read(c, c->a[0], uva, len);
    }
    if (e->kind != ESPIX_UM_FD_FILE) {
        return -EISDIR;
    }
    if (espix_pipe_is(e->kfd)) {
        return pipe_io(c, e->kfd, uva, len, false);
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

static int32_t um_sys_write(um_call_t *c)
{
    espix_um_fd_t *e = um_fd_get(c->p, c->a[0]);
    const uint32_t uva = c->a[1], len = c->a[2];
    if (e == NULL || (e->kind == ESPIX_UM_FD_STD && e->std == 0)) {
        return -EBADF;
    }
    if (e->kind == ESPIX_UM_FD_DIR) {
        return -EISDIR;
    }
    if (e->kind == ESPIX_UM_FD_SOCK) {
        return um_sock_write(c, c->a[0], uva, len);
    }
    if (e->kind == ESPIX_UM_FD_FILE && espix_pipe_is(e->kfd)) {
        return pipe_io(c, e->kfd, uva, len, true);
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

/* pread and pwrite: a file's bytes at an offset, a page of the app's buffer at a time, its own offset untouched. */
static int32_t file_at(um_call_t *c, bool out)
{
    const espix_um_fd_t *e = um_fd_get(c->p, c->a[0]);
    const uint32_t uva = c->a[1], len = c->a[2];
    const int64_t off = (int64_t)((uint64_t)c->a[4] << 32 | c->a[3]);
    if (e == NULL) {
        return -EBADF;
    }
    if (e->kind != ESPIX_UM_FD_FILE) {
        return (e->kind == ESPIX_UM_FD_DIR) ? -EISDIR : -ESPIPE;
    }
    if (off < 0) {
        return -EINVAL;
    }
    uint32_t done = 0;
    while (done < len) {
        const uint32_t va = uva + done;
        void *k = espix_um_user_pin(c->p->as, va, out ? PTE_R : PTE_W);
        if (k == NULL) {
            return done ? (int32_t)done : -EFAULT;
        }
        const uint32_t want = MIN(len - done, PG - (va & (PG - 1)));
        const ssize_t n = out ? pwrite(e->kfd, k, want, (off_t)(off + done))
                              : pread(e->kfd, k, want, (off_t)(off + done));
        espix_um_user_unpin(c->p->as);
        if (n < 0) {
            return done ? (int32_t)done : -errno;
        }
        done += (uint32_t)n;
        if ((uint32_t)n < want) {
            break;
        }
    }
    return (int32_t)done;
}

static int32_t um_sys_pread(um_call_t *c)  { return file_at(c, false); }
static int32_t um_sys_pwrite(um_call_t *c) { return file_at(c, true); }

static int32_t um_sys_open(um_call_t *c)
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
    const int fd = um_fd_install(c->p, &e, 0);
    if (fd < 0) {
        um_fd_release(&e);
    }
    return fd;
}

static int32_t um_sys_close(um_call_t *c)
{
    if (um_fd_get(c->p, c->a[0]) == NULL) {
        return -EBADF;
    }
    espix_um_fd_t e = fd_take(c->p, c->a[0]);
    return um_fd_release(&e);
}

static int32_t um_sys_lseek(um_call_t *c)
{
    espix_um_fd_t *e = um_fd_get(c->p, c->a[0]);
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

static int32_t um_sys_fstat(um_call_t *c)
{
    espix_um_fd_t *e = um_fd_get(c->p, c->a[0]);
    struct stat st = { 0 };
    if (e == NULL) {
        return -EBADF;
    }
    if (e->kind == ESPIX_UM_FD_FILE) {
        if (fstat(e->kfd, &st) != 0) {
            return -errno;
        }
    } else {
        st.st_mode = (e->kind == ESPIX_UM_FD_DIR)  ? (S_IFDIR | 0755)
                   : (e->kind == ESPIX_UM_FD_SOCK) ? (S_IFSOCK | 0777) : (S_IFCHR | 0620);
        st.st_nlink = 1;
    }
    return stat_out(c, c->a[1], &st);
}

static int32_t um_sys_stat(um_call_t *c)
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
    static int32_t um_sys_##name(um_call_t *c)         \
    {                                               \
        char path[ESPIX_PATH_MAX];                  \
        const int32_t r = path_in(c, c->a[0], path); \
        return (r < 0) ? r : um_kret(expr);            \
    }

PATH_CALL(unlink, unlink(path))
PATH_CALL(mkdir,  mkdir(path, (mode_t)c->a[1]))
PATH_CALL(rmdir,  rmdir(path))
PATH_CALL(chdir,  espix_abi_chdir(path))
PATH_CALL(chmod,  espix_abi_chmod(path, (mode_t)c->a[1]))
PATH_CALL(truncate, truncate(path, (off_t)(((uint64_t)c->a[2] << 32) | c->a[1])))

static int32_t um_sys_rename(um_call_t *c)
{
    char from[ESPIX_PATH_MAX], to[ESPIX_PATH_MAX];
    int32_t r = path_in(c, c->a[0], from);
    if (r == 0) {
        r = path_in(c, c->a[1], to);
    }
    return (r < 0) ? r : um_kret(rename(from, to));
}

static int32_t um_sys_ftruncate(um_call_t *c)
{
    espix_um_fd_t *e = um_fd_get(c->p, c->a[0]);
    if (e == NULL || e->kind != ESPIX_UM_FD_FILE) {
        return e ? -EINVAL : -EBADF;
    }
    return um_kret(ftruncate(e->kfd, (off_t)(((uint64_t)c->a[2] << 32) | c->a[1])));
}

static int32_t um_sys_fsync(um_call_t *c)
{
    espix_um_fd_t *e = um_fd_get(c->p, c->a[0]);
    if (e == NULL) {
        return -EBADF;
    }
    if (e->kind == ESPIX_UM_FD_SOCK) {
        return -EINVAL;
    }
    return (e->kind == ESPIX_UM_FD_FILE) ? um_kret(fsync(e->kfd)) : 0;
}

static int32_t um_sys_utime(um_call_t *c)
{
    char path[ESPIX_PATH_MAX];
    int32_t r = path_in(c, c->a[0], path);
    if (r < 0) {
        return r;
    }
    if (c->a[1] == 0) {
        return um_kret(utime(path, NULL));
    }
    struct espix_utimbuf u;
    r = espix_um_copyin(c->p->as, &u, c->a[1], sizeof(u));
    const struct utimbuf k = { .actime = (time_t)u.actime, .modtime = (time_t)u.modtime };
    return (r < 0) ? r : um_kret(utime(path, &k));
}

static int32_t um_sys_getcwd(um_call_t *c)
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

static int32_t um_sys_dup(um_call_t *c)
{
    espix_um_fd_t *e = um_fd_get(c->p, c->a[0]);
    if (e == NULL) {
        return -EBADF;
    }
    return fd_dup_from(c->p, e, 0, false);
}

static int32_t um_sys_dup2(um_call_t *c)
{
    espix_um_fd_t *e = um_fd_get(c->p, c->a[0]);
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
    um_fd_release(&old);
    return (int32_t)n;
}

static int32_t um_sys_fcntl(um_call_t *c)
{
    espix_um_fd_t *e = um_fd_get(c->p, c->a[0]);
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
    case F_SETFL:
        if (e->kind == ESPIX_UM_FD_SOCK) {
            return um_sock_fcntl(c, c->a[0], cmd, arg);
        }
        if (cmd == F_SETFL) {
            return (e->kind == ESPIX_UM_FD_FILE) ? um_kret(fcntl(e->kfd, F_SETFL, (int)arg)) : 0;
        }
        if (e->kind == ESPIX_UM_FD_FILE) {
            return um_kret(fcntl(e->kfd, F_GETFL));
        }
        return (e->kind == ESPIX_UM_FD_STD && e->std != 0) ? O_WRONLY : O_RDONLY;
    default:
        return -EINVAL;
    }
}

static int32_t um_sys_getdents(um_call_t *c)
{
    espix_um_fd_t *e = um_fd_get(c->p, c->a[0]);
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

static int32_t um_sys_isatty(um_call_t *c)
{
    espix_um_fd_t *e = um_fd_get(c->p, c->a[0]);
    if (e == NULL) {
        return -EBADF;
    }
    /* fds 0-2 are a terminal when the session's tty is one; an exec's pipe is not. */
    if (e->kind == ESPIX_UM_FD_STD) {
        const espix_tty_t *const t = c->t->slot->std_tty;
        return t != NULL && !espix_tty_is_pipe(t);
    }
    return e->kind == ESPIX_UM_FD_FILE && isatty(e->kfd);
}

/* ------------------------------------------------------------------ */
/* Time                                                                */
/* ------------------------------------------------------------------ */

static int32_t um_sys_clock_gettime(um_call_t *c)
{
    struct timespec ts;
    if (clock_gettime((clockid_t)c->a[0], &ts) != 0) {
        return -errno;
    }
    const struct espix_timespec e = { .sec = ts.tv_sec, .nsec = (int32_t)ts.tv_nsec };
    return espix_um_copyout(c->p->as, c->a[1], &e, sizeof(e));
}

static int32_t um_sys_nanosleep(um_call_t *c)
{
    struct espix_timespec u;
    int32_t r = espix_um_copyin(c->p->as, &u, c->a[0], sizeof(u));
    if (r < 0) {
        return r;
    }
    const struct timespec req = { .tv_sec = (time_t)u.sec, .tv_nsec = u.nsec };
    struct timespec rem = { 0 };
    r = um_kret(espix_abi_nanosleep(&req, &rem));
    if (r == -EINTR && c->a[1] != 0) {
        const struct espix_timespec left = { .sec = rem.tv_sec, .nsec = (int32_t)rem.tv_nsec };
        espix_um_copyout(c->p->as, c->a[1], &left, sizeof(left));
    }
    return r;
}

/* ------------------------------------------------------------------ */
/* Processes                                                           */
/* ------------------------------------------------------------------ */

static int32_t um_sys_exit(um_call_t *c)
{
    espix_um_exit_group(c->t, (int)c->a[0]);
}

/* ------------------------------------------------------------------ */
/* Threads                                                             */
/* ------------------------------------------------------------------ */

static int32_t um_sys_thread_create(um_call_t *c)
{
    return espix_um_thread_create(c->t, c->a[0], c->a[1], c->a[2], c->a[3], c->a[4]);
}

static int32_t um_sys_thread_exit(um_call_t *c)
{
    espix_um_thread_exit(c->t);
}

static int32_t um_sys_gettid(um_call_t *c)
{
    return (int32_t)c->t->tid;
}

static int32_t um_sys_sched_yield(um_call_t *c)
{
    (void)c;
    taskYIELD();
    return 0;
}

static int32_t um_sys_getcpu(um_call_t *c)
{
    (void)c;
    return (int32_t)esp_cpu_get_core_id();
}

static int32_t um_sys_futex(um_call_t *c)
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

static int32_t um_sys_getpid(um_call_t *c)  { (void)c; return espix_abi_getpid(); }
static int32_t um_sys_getppid(um_call_t *c) { (void)c; return espix_abi_getppid(); }
static int32_t um_sys_getuid(um_call_t *c)  { (void)c; return (int32_t)getuid(); }
static int32_t um_sys_geteuid(um_call_t *c) { (void)c; return (int32_t)geteuid(); }
static int32_t um_sys_getgid(um_call_t *c)  { (void)c; return (int32_t)getgid(); }
static int32_t um_sys_getegid(um_call_t *c) { (void)c; return (int32_t)getegid(); }
static int32_t um_sys_getpgrp(um_call_t *c) { (void)c; return espix_abi_getpgrp(); }
static int32_t um_sys_getsid(um_call_t *c)  { return um_kret(espix_abi_getsid((pid_t)c->a[0])); }
/* A string from the app, cut to size rather than refused: a log line may be long. */
static int32_t str_in_cut(um_call_t *c, uint32_t uva, char *buf, size_t size)
{
    const int r = espix_um_strin(c->p->as, buf, uva, size);
    if (r == -ENAMETOOLONG) {
        buf[size - 1] = '\0';
        return 0;
    }
    return (r < 0) ? r : 0;
}

static int32_t um_sys_syslog(um_call_t *c)
{
    char tag[48], msg[ESPIX_KLOG_LINE_MAX + 1];
    int32_t r = (c->a[1] != 0) ? str_in_cut(c, c->a[1], tag, sizeof(tag)) : 0;
    if (r == 0) {
        r = str_in_cut(c, c->a[2], msg, sizeof(msg));
    }
    if (r != 0) {
        return r;
    }
    espix_syslog_put((int)c->a[0], c->a[1] ? tag : c->t->slot->info.name, msg);
    return 0;
}

/* name, then home and shell, after the ids; -ERANGE when len cannot take them. */
static int32_t pwent_out(um_call_t *c, uint32_t id, uint32_t gid, const char *const *strs, int n)
{
    char rec[sizeof(struct espix_pwent) + 3 * ESPIX_PATH_MAX];
    struct espix_pwent *e = (struct espix_pwent *)rec;
    e->id  = id;
    e->gid = gid;
    size_t len = sizeof(*e);
    for (int i = 0; i < n; i++) {
        const size_t l = strnlen(strs[i], ESPIX_PATH_MAX - 1) + 1;
        memcpy(rec + len, strs[i], l - 1);
        rec[len + l - 1] = '\0';
        len += l;
    }
    return (len > c->a[3]) ? -ERANGE : espix_um_copyout(c->p->as, c->a[2], rec, len);
}

static int32_t um_sys_getpw(um_call_t *c)
{
    char name[64], buf[256];
    struct passwd pw, *res = NULL;
    int r;
    if (c->a[0] != 0) {
        if ((r = espix_um_strin(c->p->as, name, c->a[0], sizeof(name))) < 0) {
            return r;
        }
        r = getpwnam_r(name, &pw, buf, sizeof(buf), &res);
    } else {
        r = getpwuid_r((uid_t)c->a[1], &pw, buf, sizeof(buf), &res);
    }
    if (r != 0 || res == NULL) {
        return r ? -r : -ENOENT;
    }
    const char *const strs[] = { pw.pw_name, pw.pw_dir, pw.pw_shell ? pw.pw_shell : "" };
    return pwent_out(c, pw.pw_uid, pw.pw_gid, strs, 3);
}

static int32_t um_sys_getgr(um_call_t *c)
{
    char name[64], buf[256];
    struct group gr, *res = NULL;
    int r;
    if (c->a[0] != 0) {
        if ((r = espix_um_strin(c->p->as, name, c->a[0], sizeof(name))) < 0) {
            return r;
        }
        r = getgrnam_r(name, &gr, buf, sizeof(buf), &res);
    } else {
        r = getgrgid_r((gid_t)c->a[1], &gr, buf, sizeof(buf), &res);
    }
    if (r != 0 || res == NULL) {
        return r ? -r : -ENOENT;
    }
    const char *const strs[] = { gr.gr_name };
    return pwent_out(c, gr.gr_gid, gr.gr_gid, strs, 1);
}

static int32_t um_sys_kill(um_call_t *c)    { return um_kret(espix_abi_kill((pid_t)c->a[0], (int)c->a[1])); }

static int32_t um_sys_pipe2(um_call_t *c)
{
    const uint32_t flags = c->a[1];
    if (flags & ~(uint32_t)(O_NONBLOCK | O_CLOEXEC)) {
        return -EINVAL;
    }
    int k[2];
    if (espix_pipe(k, (int)(flags & O_NONBLOCK)) != 0) {
        return -errno;
    }
    int32_t n[2] = { -1, -1 };
    for (int i = 0; i < 2; i++) {
        const espix_um_fd_t e = { .kind = ESPIX_UM_FD_FILE, .cloexec = (flags & O_CLOEXEC) != 0, .kfd = k[i] };
        n[i] = um_fd_install(c->p, &e, 0);
    }
    int32_t r = (n[0] < 0) ? n[0] : (n[1] < 0) ? n[1] : espix_um_copyout(c->p->as, c->a[0], n, sizeof(n));
    if (r != 0) {
        for (int i = 0; i < 2; i++) {
            if (n[i] >= 0) {
                espix_um_fd_t e = fd_take(c->p, (uint32_t)n[i]);
                um_fd_release(&e);
            } else {
                close(k[i]);
            }
        }
    }
    return r;
}

static int32_t um_sys_wait4(um_call_t *c)
{
    int st = 0;
    const int32_t r = espix_proc_waitpid((pid_t)c->a[0], &st, (int)c->a[2]);
    if (r > 0 && c->a[1] != 0) {
        const int32_t w = espix_um_copyout(c->p->as, c->a[1], &st, sizeof(st));
        if (w != 0) {
            return w;
        }
    }
    return r;
}

static int32_t um_sys_setsid(um_call_t *c)  { (void)c; return espix_proc_setsid(); }
static int32_t um_sys_setpgid(um_call_t *c) { return espix_proc_setpgid((pid_t)c->a[0], (pid_t)c->a[1]); }
static int32_t um_sys_getpgid(um_call_t *c) { return espix_proc_getpgid((pid_t)c->a[0]); }
static int32_t um_sys_umask(um_call_t *c)   { return espix_proc_umask_set((int)c->a[0]); }
static int32_t um_sys_fork(um_call_t *c)    { return espix_um_fork(c->t, c->f, false); }
static int32_t um_sys_vfork(um_call_t *c)   { return espix_um_fork(c->t, c->f, true); }

/* A NULL-terminated array of strings from the app, packed as copy_argv packs one: the pointers, then the strings. */
static int32_t vec_in(um_call_t *c, uint32_t uvec, char ***out, int *count, size_t *room)
{
    char **v = NULL;
    int n = 0;
    for (uint32_t i = 0;; i++) {
        uint32_t ustr = 0;
        if (uvec != 0 && espix_um_copyin(c->p->as, &ustr, uvec + i * 4, 4) != 0) {
            free(v);
            return -EFAULT;
        }
        char **const nv = realloc(v, (size_t)(n + 1) * sizeof(char *));
        if (nv == NULL) {
            free(v);
            return -ENOMEM;
        }
        v = nv;
        if (uvec == 0 || ustr == 0) {
            v[n] = NULL;
            break;
        }
        char tmp[ESPIX_PATH_MAX];
        int len = espix_um_strin(c->p->as, tmp, ustr, sizeof(tmp));
        char *s = NULL;
        if (len == -ENAMETOOLONG) {
            /* Longer than a path: measured, then copied whole. */
            size_t cap = 2 * sizeof(tmp);
            for (;;) {
                s = malloc(cap);
                len = (s != NULL) ? espix_um_strin(c->p->as, s, ustr, cap) : -ENOMEM;
                if (len != -ENAMETOOLONG || cap >= *room) {
                    break;
                }
                free(s);
                s = NULL;
                cap *= 2;
            }
        } else if (len >= 0) {
            s = strdup(tmp);
            len = (s != NULL) ? len : -ENOMEM;
        }
        if (len < 0 || (size_t)len + 1 > *room) {
            free(s);
            for (int j = 0; j < n; j++) {
                free(v[j]);
            }
            free(v);
            return (len == -ENAMETOOLONG || len >= 0) ? -E2BIG : len;
        }
        *room -= (size_t)len + 1;
        v[n++] = s;
    }
    *out = v;
    *count = n;
    return 0;
}

static void vec_free(char **v)
{
    for (int i = 0; v != NULL && v[i] != NULL; i++) {
        free(v[i]);
    }
    free(v);
}

static int32_t um_sys_execve(um_call_t *c)
{
    char path[ESPIX_PATH_MAX];
    int32_t r = path_in(c, c->a[0], path);
    if (r < 0) {
        return r;
    }
    char **argv = NULL, **envp = NULL;
    int argc = 0, envc = 0;
    size_t room = 64 * 1024;
    r = vec_in(c, c->a[1], &argv, &argc, &room);
    if (r == 0) {
        r = vec_in(c, c->a[2], &envp, &envc, &room);
    }
    if (r == 0) {
        r = espix_um_exec_prepare(c->t, path, argc, argv, envp);
    }
    vec_free(argv);
    vec_free(envp);
    if (r == 0) {
        espix_um_exec_go(c->t);
    }
    return r;
}

/* ------------------------------------------------------------------ */
/* Signals (umode_sig.c)                                               */
/* ------------------------------------------------------------------ */

static int32_t um_sys_sigaction(um_call_t *c)   { return espix_um_sigaction(c->t, (int)c->a[0], c->a[1], c->a[2]); }
static int32_t um_sys_sigprocmask(um_call_t *c) { return espix_um_sigprocmask(c->t, (int)c->a[0], c->a[1], c->a[2]); }
static int32_t um_sys_sigpending(um_call_t *c)  { return espix_um_sigpending(c->t, c->a[0]); }
static int32_t um_sys_sigsuspend(um_call_t *c)  { return espix_um_sigsuspend(c->t, c->a[0]); }
static int32_t um_sys_raise(um_call_t *c)       { return espix_um_raise(c->t, (int)c->a[0]); }

/* espix_um_kentry() takes it before the table: it returns into the frame, not with a result. */
static int32_t um_sys_sigreturn(um_call_t *c)
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

static int32_t um_sys_setitimer(um_call_t *c)
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
    const int32_t r = um_kret(espix_abi_setitimer((int)c->a[0], c->a[1] ? &nv : NULL, &ov));
    if (r < 0 || c->a[2] == 0) {
        return r;
    }
    const struct espix_itimerval o = { .interval = tv_out(&ov.it_interval), .value = tv_out(&ov.it_value) };
    return espix_um_copyout(c->p->as, c->a[2], &o, sizeof(o));
}

static int32_t um_sys_term_size(um_call_t *c)
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

static int32_t um_sys_brk(um_call_t *c)
{
    return (int32_t)espix_um_brk(c->p->as, c->a[0]);
}

static uint32_t prot_flags(uint32_t prot)
{
    return ((prot & ESPIX_PROT_READ) ? PTE_R : 0) | ((prot & ESPIX_PROT_WRITE) ? PTE_R | PTE_W : 0) |
           ((prot & ESPIX_PROT_EXEC) ? PTE_X : 0);
}

static int32_t um_sys_mmap(um_call_t *c)
{
    const uint32_t addr = c->a[0], len = c->a[1], prot = c->a[2], fl = c->a[3];
    const uint32_t type = fl & ESPIX_MAP_TYPE;
    if (len == 0 || (addr & (PG - 1)) || (prot & ~7u) ||
        (type != ESPIX_MAP_SHARED && type != ESPIX_MAP_PRIVATE && type != ESPIX_MAP_SHARED_VALIDATE)) {
        return -EINVAL;
    }
    int kfd = -1;
    if (!(fl & ESPIX_MAP_ANONYMOUS)) {
        const espix_um_fd_t *e = um_fd_get(c->p, c->a[4]);
        if (e == NULL) {
            return -EBADF;
        }
        if (e->kind != ESPIX_UM_FD_FILE) {
            return -ENODEV;
        }
        /* Readable, and writable too for a shared mapping that writes; a filesystem without F_GETFL is trusted. */
        const int acc = fcntl(e->kfd, F_GETFL);
        if (acc >= 0 && ((acc & O_ACCMODE) == O_WRONLY ||
                         (type != ESPIX_MAP_PRIVATE && (prot & ESPIX_PROT_WRITE) && (acc & O_ACCMODE) != O_RDWR))) {
            return -EACCES;
        }
        kfd = espix_abi_dup(e->kfd);
        if (kfd < 0) {
            return -errno;
        }
    }
    const int32_t r = espix_um_mmap(c->p->as, addr, len, prot_flags(prot), fl, kfd, (uint64_t)c->a[5] * PG);
    if (r < 0 && kfd >= 0) {
        close(kfd);
    }
    return r;
}

static int32_t um_sys_munmap(um_call_t *c)
{
    return (c->a[0] & (PG - 1)) ? -EINVAL : espix_um_munmap(c->p->as, c->a[0], c->a[1]);
}

static int32_t um_sys_mprotect(um_call_t *c)
{
    if ((c->a[0] & (PG - 1)) || (c->a[2] & ~7u)) {
        return -EINVAL;
    }
    return espix_um_mprotect(c->p->as, c->a[0], c->a[1], prot_flags(c->a[2]));
}

/* MS_ASYNC 1, MS_INVALIDATE 2, MS_SYNC 4: the write-back is done before it returns either way. */
static int32_t um_sys_msync(um_call_t *c)
{
    if ((c->a[0] & (PG - 1)) || (c->a[2] & ~7u) || (c->a[2] & 5) == 5) {
        return -EINVAL;
    }
    return espix_um_msync(c->p->as, c->a[0], c->a[1]);
}

/* The advice that changes nothing here is taken; DONTNEED and FREE drop the pages. */
static int32_t um_sys_madvise(um_call_t *c)
{
    const uint32_t advice = c->a[2];
    if ((c->a[0] & (PG - 1)) || (advice > 4 && advice != ESPIX_MADV_FREE)) {
        return -EINVAL;
    }
    return espix_um_madvise(c->p->as, c->a[0], c->a[1],
                            advice == ESPIX_MADV_DONTNEED || advice == ESPIX_MADV_FREE);
}

/* A chunk of the vector at a time, copied out after the table's lock is let go. */
static int32_t um_sys_mincore(um_call_t *c)
{
    const uint32_t start = c->a[0], npages = (c->a[1] + PG - 1) / PG;
    if (start & (PG - 1)) {
        return -EINVAL;
    }
    uint8_t vec[256];
    for (uint32_t done = 0; done < npages;) {
        const uint32_t n = MIN(npages - done, (uint32_t)sizeof(vec));
        int32_t r = espix_um_mincore(c->p->as, start + done * PG, n, vec);
        if (r == 0) {
            r = espix_um_copyout(c->p->as, c->a[2] + done, vec, n);
        }
        if (r < 0) {
            return r;
        }
        done += n;
    }
    return 0;
}

/* Frames come from PSRAM. */
static int32_t um_sys_meminfo(um_call_t *c)
{
    const struct espix_meminfo m = {
        .page_size = PG,
        .total     = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / PG,
        .free      = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / PG + espix_um_frames_spare(),
    };
    return espix_um_copyout(c->p->as, c->a[0], &m, sizeof(m));
}

/* ------------------------------------------------------------------ */

#define UM_SYS_ENTRY(name, lname, nr) [nr] = um_sys_##lname,
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
