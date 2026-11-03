/*
 * The seam between htop and espix: the POSIX calls htop links against that
 * espix does not have, each answered rather than faked.
 *
 * htop is a Linux program. Most of what it needs is already espix's; these
 * are the exceptions, and every one of them belongs to an optional screen or
 * to saving the config. None is on the path that draws the CPU, memory and
 * process meters.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef AT_FDCWD
#define AT_FDCWD (-100)
#endif
#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

/* No pipes: a pipe is a kernel object espix has not grown yet. The screens
 * that use one -- lsof, strace, the systemd/OpenRC meters -- say why they
 * cannot start instead of starting and hanging. */
int pipe(int fd[2])
{
    (void)fd;
    errno = ENOSYS;
    return -1;
}

/* No fork()/exec() yet (docs/USER-MODE.md, M8), so nothing can be spawned. */
int execlp(const char *file, const char *arg, ...)
{
    (void)file;
    (void)arg;
    errno = ENOSYS;
    return -1;
}

pid_t waitpid(pid_t pid, int *status, int options)
{
    (void)pid;
    (void)status;
    (void)options;
    errno = ECHILD;
    return -1;
}

/* /proc/<pid>/exe, /proc/<pid>/cwd and /proc/self/ns/pid are links on Linux;
 * espix's /proc has no symlinks, so there is nothing to read. */
ssize_t readlink(const char *path, char *buf, size_t size)
{
    (void)path;
    (void)buf;
    (void)size;
    errno = EINVAL;
    return -1;
}

/* A directory's descriptor and fdopendir() would let htop walk /proc/<pid>/fd.
 * espix keeps no such directory, and its VFS does not expose a DIR's fd. */
int dirfd(DIR *dir)
{
    (void)dir;
    errno = ENOTSUP;
    return -1;
}

DIR *fdopendir(int fd)
{
    (void)fd;
    errno = ENOTSUP;
    return NULL;
}

int openat(int dirfd_, const char *path, int flags, ...)
{
    (void)dirfd_;
    (void)path;
    (void)flags;
    errno = ENOTSUP;
    return -1;
}

/* espix applies the same 022 on every create (docs/USER-MODE.md); there is no
 * per-process mask to read or set. htop only narrows it while writing its
 * config, and opens that file 0600 anyway. */
mode_t umask(mode_t mask)
{
    (void)mask;
    return 022;
}

/* A lexical realpath(): espix has no symlinks, so resolving '.' and '..' is
 * the whole of it. htop uses this once, to turn its config path absolute. */
char *realpath(const char *path, char *resolved)
{
    char tmp[PATH_MAX];
    char *out = resolved != NULL ? resolved : malloc(PATH_MAX);
    if (out == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    if (path == NULL || *path == '\0') {
        errno = ENOENT;
        if (resolved == NULL) free(out);
        return NULL;
    }

    if (path[0] != '/') {
        if (getcwd(tmp, sizeof(tmp)) == NULL) {
            if (resolved == NULL) free(out);
            return NULL;
        }
        size_t n = strlen(tmp);
        if (n + 1 + strlen(path) + 1 > sizeof(tmp)) {
            errno = ENAMETOOLONG;
            if (resolved == NULL) free(out);
            return NULL;
        }
        tmp[n++] = '/';
        strcpy(tmp + n, path);
        path = tmp;
    }

    char *o = out;
    *o++ = '/';
    *o = '\0';
    const char *p = path;
    while (*p != '\0') {
        while (*p == '/') p++;
        const char *e = p;
        while (*e != '\0' && *e != '/') e++;
        size_t len = (size_t)(e - p);
        if (len == 0 || (len == 1 && p[0] == '.')) {
            /* nothing */
        } else if (len == 2 && p[0] == '.' && p[1] == '.') {
            if (o > out + 1) {
                o--;
                while (o > out + 1 && o[-1] != '/') o--;
            }
        } else {
            if (o > out + 1) *o++ = '/';
            if ((size_t)(o - out) + len + 1 > PATH_MAX) {
                errno = ENAMETOOLONG;
                if (resolved == NULL) free(out);
                return NULL;
            }
            memcpy(o, p, len);
            o += len;
            *o = '\0';
        }
        p = e;
    }
    if (o == out) *o++ = '/';
    *o = '\0';
    return out;
}
