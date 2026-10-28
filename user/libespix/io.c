/* POSIX file calls as espix syscalls. off_t is 64 bits here (cmake/offt64.h). */
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

#include "sys.h"

ssize_t read(int fd, void *buf, size_t n)
{
    return espix_ret(espix_sys3(ESPIX_SYS_READ, fd, buf, n));
}

ssize_t write(int fd, const void *buf, size_t n)
{
    return espix_ret(espix_sys3(ESPIX_SYS_WRITE, fd, buf, n));
}

int open(const char *path, int flags, ...)
{
    int mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    return espix_ret(espix_sys3(ESPIX_SYS_OPEN, path, flags, mode));
}

int creat(const char *path, mode_t mode)
{
    return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
}

int close(int fd)
{
    return espix_ret(espix_sys1(ESPIX_SYS_CLOSE, fd));
}

/* Named lseek64 by offt64.h's asm label. */
off_t lseek(int fd, off_t off, int whence)
{
    uint64_t pos = 0;
    const long r = espix_sys5(ESPIX_SYS_LSEEK, fd, (uint32_t)off, (uint32_t)((uint64_t)off >> 32), whence, &pos);
    return (r < 0) ? espix_ret(r) : (off_t)pos;
}

static int to_stat(long r, const struct espix_stat *e, struct stat *st)
{
    if (r < 0) {
        return espix_ret(r);
    }
    *st = (struct stat){
        .st_dev = e->dev, .st_ino = e->ino, .st_mode = e->mode, .st_nlink = e->nlink,
        .st_uid = e->uid, .st_gid = e->gid, .st_rdev = e->rdev, .st_size = (off_t)e->size,
        .st_blksize = e->blksize, .st_blocks = e->blocks,
    };
    st->st_atime = (time_t)e->atime;
    st->st_mtime = (time_t)e->mtime;
    st->st_ctime = (time_t)e->ctime;
    return 0;
}

int fstat(int fd, struct stat *st)
{
    struct espix_stat e;
    return to_stat(espix_sys2(ESPIX_SYS_FSTAT, fd, &e), &e, st);
}

int stat(const char *path, struct stat *st)
{
    struct espix_stat e;
    return to_stat(espix_sys2(ESPIX_SYS_STAT, path, &e), &e, st);
}

/* No symlinks in espix. */
int lstat(const char *path, struct stat *st)
{
    return stat(path, st);
}

int unlink(const char *path)
{
    return espix_ret(espix_sys1(ESPIX_SYS_UNLINK, path));
}

int rename(const char *from, const char *to)
{
    return espix_ret(espix_sys2(ESPIX_SYS_RENAME, from, to));
}

int mkdir(const char *path, mode_t mode)
{
    return espix_ret(espix_sys2(ESPIX_SYS_MKDIR, path, mode));
}

int rmdir(const char *path)
{
    return espix_ret(espix_sys1(ESPIX_SYS_RMDIR, path));
}

int truncate(const char *path, off_t len)
{
    return espix_ret(espix_sys3(ESPIX_SYS_TRUNCATE, path, (uint32_t)len, (uint32_t)((uint64_t)len >> 32)));
}

int ftruncate(int fd, off_t len)
{
    return espix_ret(espix_sys3(ESPIX_SYS_FTRUNCATE, fd, (uint32_t)len, (uint32_t)((uint64_t)len >> 32)));
}

int fsync(int fd)
{
    return espix_ret(espix_sys1(ESPIX_SYS_FSYNC, fd));
}

int utime(const char *path, const struct utimbuf *times)
{
    struct espix_utimbuf u;
    if (times != NULL) {
        u.actime  = times->actime;
        u.modtime = times->modtime;
    }
    return espix_ret(espix_sys2(ESPIX_SYS_UTIME, path, times != NULL ? &u : NULL));
}

int dup(int fd)
{
    return espix_ret(espix_sys1(ESPIX_SYS_DUP, fd));
}

int dup2(int fd, int newfd)
{
    return espix_ret(espix_sys2(ESPIX_SYS_DUP2, fd, newfd));
}

int fcntl(int fd, int cmd, ...)
{
    va_list ap;
    va_start(ap, cmd);
    const long arg = va_arg(ap, long);
    va_end(ap);
    return espix_ret(espix_sys3(ESPIX_SYS_FCNTL, fd, cmd, arg));
}

int isatty(int fd)
{
    return espix_ret(espix_sys1(ESPIX_SYS_ISATTY, fd)) == 1;
}

/* espix's filesystems have no access(); answered from stat() and the owner's bits. */
int access(const char *path, int how)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return -1;
    }
    const mode_t bits = (st.st_uid == geteuid()) ? (st.st_mode >> 6) : (st.st_gid == getegid()) ? (st.st_mode >> 3) : st.st_mode;
    if (((how & R_OK) && !(bits & 4)) || ((how & W_OK) && !(bits & 2)) || ((how & X_OK) && !(bits & 1))) {
        errno = EACCES;
        return -1;
    }
    return 0;
}
