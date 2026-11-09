/*
 * A POSIX app without an address space (the S3, and M-mode apps on the S31): its entry, and
 * stdio streams over its own descriptors. The rest of libc is the kernel's, shared; its
 * stdin, stdout and stderr are the process's, which the kernel routes through descriptors
 * 0-2 once the first syscall has made the process's table.
 */
#if ESPIX_NOMMU_APP

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "sys.h"

char **environ;

int main(int argc, char **argv, char **envp);

int __espix_main(int argc, char **argv, char **envp)
{
    environ = envp;
    /* The first syscall makes the descriptor table, and routes stdio through it. */
    (void)espix_sys0(ESPIX_SYS_GETPID);
    return main(argc, argv, envp);
}

/* vfork(): the child's state and the parent's resume point, then the child on this task (unistd.h). */
jmp_buf *__espix_vfork_prepare(void)
{
    const long r = espix_sys1(ESPIX_SYS_VFORK, 0);
    if (r < 0 && r > -4096) {
        errno = (int)-r;
        return NULL;
    }
    return (jmp_buf *)r;
}

void __espix_vfork_enter(jmp_buf *jb)
{
    (void)espix_sys1(ESPIX_SYS_VFORK, (long)(uintptr_t)jb);
}

/* ------------------------------------------------------------------ streams */

#define STREAMS_MAX 16

/* Each stream's descriptor, for fileno(): the stream's cookie is the descriptor plus one. */
static struct {
    FILE *f;
    int   fd;
} s_streams[STREAMS_MAX];

static int cookie_fd(void *cookie)
{
    return (int)(intptr_t)cookie - 1;
}

static _ssize_t s_read(void *cookie, void *buf, size_t n)
{
    return read(cookie_fd(cookie), buf, n);
}

static _ssize_t s_write(void *cookie, const void *buf, size_t n)
{
    return write(cookie_fd(cookie), buf, n);
}

static __off_t s_seek(void *cookie, __off_t off, int whence)
{
    return (__off_t)lseek(cookie_fd(cookie), off, whence);
}

static int s_close(void *cookie)
{
    const int fd = cookie_fd(cookie);
    for (int i = 0; i < STREAMS_MAX; i++) {
        if (s_streams[i].f != NULL && s_streams[i].fd == fd) {
            s_streams[i].f = NULL;
            break;
        }
    }
    return close(fd);
}

/* The open flags a mode string asks for, or -1. */
static int mode_flags(const char *mode)
{
    int f;
    switch (mode[0]) {
    case 'r': f = O_RDONLY; break;
    case 'w': f = O_WRONLY | O_CREAT | O_TRUNC; break;
    case 'a': f = O_WRONLY | O_CREAT | O_APPEND; break;
    default:  return -1;
    }
    for (const char *m = mode + 1; *m != '\0'; m++) {
        if (*m == '+') {
            f = (f & ~O_ACCMODE) | O_RDWR;
        } else if (*m == 'x') {
            f |= O_EXCL;
        } else if (*m == 'e') {
            f |= O_CLOEXEC;
        }
    }
    return f;
}

FILE *fdopen(int fd, const char *mode)
{
    const int flags = mode_flags(mode);
    if (flags < 0) {
        errno = EINVAL;
        return NULL;
    }
    int slot = 0;
    while (slot < STREAMS_MAX && s_streams[slot].f != NULL) {
        slot++;
    }
    if (slot == STREAMS_MAX) {
        errno = EMFILE;
        return NULL;
    }
    const int acc = flags & O_ACCMODE;
    FILE *const f = funopen((void *)(intptr_t)(fd + 1), (acc != O_WRONLY) ? s_read : NULL,
                            (acc != O_RDONLY) ? s_write : NULL, s_seek, s_close);
    if (f == NULL) {
        return NULL;
    }
    s_streams[slot].f  = f;
    s_streams[slot].fd = fd;
    return f;
}

FILE *fopen(const char *path, const char *mode)
{
    const int flags = mode_flags(mode);
    if (flags < 0) {
        errno = EINVAL;
        return NULL;
    }
    const int fd = open(path, flags, 0666);
    if (fd < 0) {
        return NULL;
    }
    FILE *const f = fdopen(fd, mode);
    if (f == NULL) {
        close(fd);
    }
    return f;
}

int fileno(FILE *f)
{
    if (f == stdin || f == stdout || f == stderr) {
        return (f == stdin) ? 0 : (f == stdout) ? 1 : 2;
    }
    for (int i = 0; i < STREAMS_MAX; i++) {
        if (s_streams[i].f == f) {
            return s_streams[i].fd;
        }
    }
    errno = EBADF;
    return -1;
}

/* The standard streams follow their descriptors, so reopening one is a dup2 onto it. */
FILE *freopen(const char *path, const char *mode, FILE *f)
{
    const int std = fileno(f);
    if (std < 0 || std > 2) {
        if (f != NULL) {
            fclose(f);
        }
        return (path != NULL) ? fopen(path, mode) : NULL;
    }
    fflush(f);
    const int flags = mode_flags(mode);
    const int fd = (path != NULL && flags >= 0) ? open(path, flags, 0666) : -1;
    if (fd < 0) {
        return NULL;
    }
    const int r = dup2(fd, std);
    close(fd);
    return (r < 0) ? NULL : f;
}

#endif /* ESPIX_NOMMU_APP */
