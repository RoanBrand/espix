/* Processes: fork, exec, wait, pipes, sessions and groups, and what libc builds on them. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "sys.h"

_Static_assert(ESPIX_SYS_VFORK == 98, "vfork.S names the number");

extern char **environ;

void __espix_fork_prepare(void);
void __espix_fork_parent(void);
void __espix_fork_child(void);

/* vfork.S's way out on an error: errno, and -1. */
long __espix_errno_ret(long r)
{
    errno = (int)-r;
    return -1;
}

pid_t fork(void)
{
    __espix_fork_prepare();
    const long r = espix_sys0(ESPIX_SYS_FORK);
    if (r == 0) {
        __espix_fork_child();
    } else {
        __espix_fork_parent();
    }
    return (pid_t)espix_ret(r);
}

int execve(const char *path, char *const argv[], char *const envp[])
{
    return (int)espix_ret(espix_sys3(ESPIX_SYS_EXECVE, path, argv, envp));
}

int execv(const char *path, char *const argv[])
{
    return execve(path, argv, environ);
}

/* Each PATH directory in turn for a name without a slash; the first error that is not "not there" is kept. */
int execvpe(const char *file, char *const argv[], char *const envp[])
{
    if (file == NULL || *file == '\0') {
        errno = ENOENT;
        return -1;
    }
    if (strchr(file, '/') != NULL) {
        return execve(file, argv, envp);
    }
    const char *path = getenv("PATH");
    if (path == NULL) {
        path = "/bin";
    }
    char buf[PATH_MAX];
    int err = ENOENT;
    for (const char *p = path;; p++) {
        const char *end = strchr(p, ':');
        if (end == NULL) {
            end = p + strlen(p);
        }
        const size_t dl = (size_t)(end - p);
        if (dl + 1 + strlen(file) + 1 <= sizeof(buf)) {
            memcpy(buf, p, dl);
            size_t n = dl;
            if (n == 0) {
                buf[n++] = '.';
            }
            buf[n++] = '/';
            strcpy(buf + n, file);
            execve(buf, argv, envp);
            if (errno != ENOENT && errno != ENOTDIR) {
                err = errno;
            }
        }
        if (*end == '\0') {
            break;
        }
        p = end;
    }
    errno = err;
    return -1;
}

int execvp(const char *file, char *const argv[])
{
    return execvpe(file, argv, environ);
}

/* The variadic forms: their arguments into an array on the stack, then the vector call. */
static int execl_common(const char *path, const char *arg0, va_list ap, int how)
{
    va_list count;
    va_copy(count, ap);
    int n = 1;
    while (va_arg(count, const char *) != NULL) {
        n++;
    }
    va_end(count);
    const char *argv[n + 1];
    argv[0] = arg0;
    for (int i = 1; i <= n; i++) {
        argv[i] = va_arg(ap, const char *);
    }
    char *const *envp = (how == 2) ? va_arg(ap, char *const *) : environ;
    return (how == 1) ? execvpe(path, (char *const *)argv, envp) : execve(path, (char *const *)argv, envp);
}

int execl(const char *path, const char *arg0, ...)
{
    va_list ap;
    va_start(ap, arg0);
    const int r = execl_common(path, arg0, ap, 0);
    va_end(ap);
    return r;
}

int execlp(const char *file, const char *arg0, ...)
{
    va_list ap;
    va_start(ap, arg0);
    const int r = execl_common(file, arg0, ap, 1);
    va_end(ap);
    return r;
}

int execle(const char *path, const char *arg0, ...)
{
    va_list ap;
    va_start(ap, arg0);
    const int r = execl_common(path, arg0, ap, 2);
    va_end(ap);
    return r;
}

pid_t waitpid(pid_t pid, int *status, int options)
{
    return (pid_t)espix_ret(espix_sys3(ESPIX_SYS_WAIT4, pid, status, options));
}

pid_t wait(int *status)
{
    return waitpid(-1, status, 0);
}

int pipe2(int fds[2], int flags)
{
    return (int)espix_ret(espix_sys2(ESPIX_SYS_PIPE2, fds, flags));
}

int pipe(int fds[2])
{
    return pipe2(fds, 0);
}

pid_t setsid(void)        { return (pid_t)espix_ret(espix_sys0(ESPIX_SYS_SETSID)); }
int setpgid(pid_t p, pid_t g) { return (int)espix_ret(espix_sys2(ESPIX_SYS_SETPGID, p, g)); }
pid_t getpgid(pid_t pid)  { return (pid_t)espix_ret(espix_sys1(ESPIX_SYS_GETPGID, pid)); }
int setpgrp(void)         { return setpgid(0, 0); }
mode_t umask(mode_t mask) { return (mode_t)espix_sys1(ESPIX_SYS_UMASK, mask); }

/* Weak, as the conveniences below are: an app may bring its own (Doom's system() says no). */
__attribute__((weak)) int daemon(int nochdir, int noclose)
{
    const pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid > 0) {
        _exit(0);
    }
    if (setsid() < 0) {
        return -1;
    }
    if (!nochdir) {
        (void)chdir("/");
    }
    if (!noclose) {
        const int fd = open("/dev/null", O_RDWR);
        if (fd >= 0) {
            dup2(fd, 0);
            dup2(fd, 1);
            dup2(fd, 2);
            if (fd > 2) {
                close(fd);
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ posix_spawn */

enum { FA_OPEN, FA_CLOSE, FA_DUP2, FA_CHDIR };

struct fa {
    int         op, fd, newfd, flags;
    mode_t      mode;
    char       *path;
    struct fa  *next;
};

struct __posix_spawn_file_actions {
    struct fa *head, *tail;
};

struct __posix_spawnattr {
    short    flags;
    pid_t    pgroup;
    sigset_t mask, dfl;
};

int posix_spawn_file_actions_init(posix_spawn_file_actions_t *fa)
{
    *fa = calloc(1, sizeof(**fa));
    return (*fa != NULL) ? 0 : ENOMEM;
}

int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *fa)
{
    for (struct fa *a = (*fa)->head, *n; a != NULL; a = n) {
        n = a->next;
        free(a->path);
        free(a);
    }
    free(*fa);
    *fa = NULL;
    return 0;
}

static int fa_add(posix_spawn_file_actions_t *fa, const struct fa *in, const char *path)
{
    struct fa *a = malloc(sizeof(*a));
    if (a == NULL) {
        return ENOMEM;
    }
    *a = *in;
    a->path = (path != NULL) ? strdup(path) : NULL;
    if (path != NULL && a->path == NULL) {
        free(a);
        return ENOMEM;
    }
    a->next = NULL;
    if ((*fa)->tail != NULL) {
        (*fa)->tail->next = a;
    } else {
        (*fa)->head = a;
    }
    (*fa)->tail = a;
    return 0;
}

int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *fa, int fd, const char *path, int flags,
                                     mode_t mode)
{
    return (fd < 0) ? EBADF : fa_add(fa, &(struct fa){ .op = FA_OPEN, .fd = fd, .flags = flags, .mode = mode }, path);
}

int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *fa, int fd)
{
    return (fd < 0) ? EBADF : fa_add(fa, &(struct fa){ .op = FA_CLOSE, .fd = fd }, NULL);
}

int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *fa, int fd, int newfd)
{
    return (fd < 0 || newfd < 0) ? EBADF : fa_add(fa, &(struct fa){ .op = FA_DUP2, .fd = fd, .newfd = newfd }, NULL);
}

int posix_spawn_file_actions_addchdir_np(posix_spawn_file_actions_t *fa, const char *path)
{
    return fa_add(fa, &(struct fa){ .op = FA_CHDIR }, path);
}

int posix_spawnattr_init(posix_spawnattr_t *at)
{
    *at = calloc(1, sizeof(**at));
    return (*at != NULL) ? 0 : ENOMEM;
}

int posix_spawnattr_destroy(posix_spawnattr_t *at)
{
    free(*at);
    *at = NULL;
    return 0;
}

int posix_spawnattr_setflags(posix_spawnattr_t *at, short f)               { (*at)->flags = f; return 0; }
int posix_spawnattr_getflags(const posix_spawnattr_t *at, short *f)        { *f = (*at)->flags; return 0; }
int posix_spawnattr_setpgroup(posix_spawnattr_t *at, pid_t g)              { (*at)->pgroup = g; return 0; }
int posix_spawnattr_getpgroup(const posix_spawnattr_t *at, pid_t *g)       { *g = (*at)->pgroup; return 0; }
int posix_spawnattr_setsigmask(posix_spawnattr_t *at, const sigset_t *m)   { (*at)->mask = *m; return 0; }
int posix_spawnattr_getsigmask(const posix_spawnattr_t *at, sigset_t *m)   { *m = (*at)->mask; return 0; }
int posix_spawnattr_setsigdefault(posix_spawnattr_t *at, const sigset_t *m) { (*at)->dfl = *m; return 0; }
int posix_spawnattr_getsigdefault(const posix_spawnattr_t *at, sigset_t *m) { *m = (*at)->dfl; return 0; }

/* The child's side, on its parent's memory until the exec: the error it ends with is the parent's to report. */
static void spawn_child(volatile int *err, const char *path, const posix_spawn_file_actions_t *fa,
                        const posix_spawnattr_t *at, char *const argv[], char *const envp[], bool search)
{
    if (at != NULL && *at != NULL) {
        const struct __posix_spawnattr *a = *at;
        if ((a->flags & POSIX_SPAWN_SETPGROUP) && setpgid(0, a->pgroup) != 0) {
            goto fail;
        }
        if (a->flags & POSIX_SPAWN_SETSIGDEF) {
            for (int sig = 1; sig < 32; sig++) {
                if (sigismember(&a->dfl, sig) == 1) {
                    signal(sig, SIG_DFL);
                }
            }
        }
        if ((a->flags & POSIX_SPAWN_SETSIGMASK) && sigprocmask(SIG_SETMASK, &a->mask, NULL) != 0) {
            goto fail;
        }
    }
    for (const struct fa *f = (fa != NULL && *fa != NULL) ? (*fa)->head : NULL; f != NULL; f = f->next) {
        int r = 0;
        switch (f->op) {
        case FA_OPEN: {
            const int fd = open(f->path, f->flags, f->mode);
            r = (fd < 0) ? -1 : 0;
            if (fd >= 0 && fd != f->fd) {
                r = (dup2(fd, f->fd) < 0) ? -1 : 0;
                close(fd);
            }
            break;
        }
        case FA_CLOSE:
            (void)close(f->fd);
            break;
        case FA_DUP2:
            /* The same number: its close-on-exec cleared, as POSIX asks. */
            r = (f->fd == f->newfd) ? fcntl(f->fd, F_SETFD, 0) : dup2(f->fd, f->newfd);
            r = (r < 0) ? -1 : 0;
            break;
        case FA_CHDIR:
            r = chdir(f->path);
            break;
        }
        if (r != 0) {
            goto fail;
        }
    }
    if (search) {
        execvpe(path, argv, envp);
    } else {
        execve(path, argv, envp);
    }
fail:
    *err = errno;
    _exit(127);
}

static int spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *fa, const posix_spawnattr_t *at,
                 char *const argv[], char *const envp[], bool search)
{
    volatile int err = 0;
    const pid_t child = vfork();
    if (child < 0) {
        return errno;
    }
    if (child == 0) {
        spawn_child(&err, path, fa, at, argv, envp ? envp : environ, search);
    }
    /* The child failed before its exec: reap it and say why. */
    if (err != 0) {
        (void)waitpid(child, NULL, 0);
        return err;
    }
    if (pid != NULL) {
        *pid = child;
    }
    return 0;
}

int posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *fa, const posix_spawnattr_t *at,
                char *const argv[], char *const envp[])
{
    return spawn(pid, path, fa, at, argv, envp, false);
}

int posix_spawnp(pid_t *pid, const char *file, const posix_spawn_file_actions_t *fa, const posix_spawnattr_t *at,
                 char *const argv[], char *const envp[])
{
    return spawn(pid, file, fa, at, argv, envp, true);
}

/* ------------------------------------------------------------------ system and popen */

/*
 * A command line: through /bin/sh when there is one; without, its words are run directly, which is
 * what a line with no quoting, globs or operators asks for.
 */
#define WORDS_MAX 32

static int words_of(char *line, char **argv)
{
    int n = 0;
    for (char *s = strtok(line, " \t"); s != NULL && n < WORDS_MAX; s = strtok(NULL, " \t")) {
        argv[n++] = s;
    }
    argv[n] = NULL;
    return n;
}

static int run_line(pid_t *pid, const char *cmd, const posix_spawn_file_actions_t *fa)
{
    if (access("/bin/sh", X_OK) == 0) {
        char *const argv[] = { "sh", "-c", (char *)cmd, NULL };
        return posix_spawn(pid, "/bin/sh", fa, NULL, argv, environ);
    }
    char *line = strdup(cmd);
    char *argv[WORDS_MAX + 1];
    if (line == NULL) {
        return ENOMEM;
    }
    const int r = (words_of(line, argv) > 0) ? posix_spawnp(pid, argv[0], fa, NULL, argv, environ) : ENOENT;
    free(line);
    return r;
}

__attribute__((weak)) int system(const char *cmd)
{
    if (cmd == NULL) {
        return 1;
    }
    pid_t pid;
    const int r = run_line(&pid, cmd, NULL);
    if (r != 0) {
        return 127 << 8;
    }
    int st;
    while (waitpid(pid, &st, 0) < 0) {
        if (errno != EINTR) {
            return -1;
        }
    }
    return st;
}

#define POPEN_MAX 8

static struct {
    FILE *f;
    pid_t pid;
} s_popen[POPEN_MAX];

__attribute__((weak)) FILE *popen(const char *cmd, const char *mode)
{
    const bool rd = mode[0] == 'r';
    if ((mode[0] != 'r' && mode[0] != 'w') || mode[1] != '\0') {
        errno = EINVAL;
        return NULL;
    }
    int slot = 0;
    while (slot < POPEN_MAX && s_popen[slot].f != NULL) {
        slot++;
    }
    if (slot == POPEN_MAX) {
        errno = EMFILE;
        return NULL;
    }
    int p[2];
    if (pipe2(p, O_CLOEXEC) != 0) {
        return NULL;
    }
    posix_spawn_file_actions_t fa;
    pid_t pid;
    int r = posix_spawn_file_actions_init(&fa);
    if (r == 0) {
        r = posix_spawn_file_actions_adddup2(&fa, rd ? p[1] : p[0], rd ? 1 : 0);
    }
    if (r == 0) {
        r = run_line(&pid, cmd, &fa);
    }
    posix_spawn_file_actions_destroy(&fa);
    close(rd ? p[1] : p[0]);
    FILE *f = (r == 0) ? fdopen(rd ? p[0] : p[1], mode) : NULL;
    if (f == NULL) {
        close(rd ? p[0] : p[1]);
        errno = r ? r : errno;
        return NULL;
    }
    s_popen[slot].f   = f;
    s_popen[slot].pid = pid;
    return f;
}

__attribute__((weak)) int pclose(FILE *f)
{
    for (int i = 0; i < POPEN_MAX; i++) {
        if (s_popen[i].f == f) {
            const pid_t pid = s_popen[i].pid;
            s_popen[i].f = NULL;
            fclose(f);
            int st;
            while (waitpid(pid, &st, 0) < 0) {
                if (errno != EINTR) {
                    return -1;
                }
            }
            return st;
        }
    }
    errno = ECHILD;
    return -1;
}
