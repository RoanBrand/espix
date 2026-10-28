/*
 * What U-mode apps cannot have yet, failing cleanly: sockets and select (M6),
 * threads (M3), alarm and espix's signal calls (M4). docs/USER-MODE.md.
 */
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "sys.h"

static int nosys(void)
{
    errno = ENOSYS;
    return -1;
}

int socket(int d, int t, int p) { (void)d; (void)t; (void)p; return nosys(); }
int bind(int fd, const struct sockaddr *a, socklen_t l) { (void)fd; (void)a; (void)l; return nosys(); }
int listen(int fd, int b) { (void)fd; (void)b; return nosys(); }
int accept(int fd, struct sockaddr *a, socklen_t *l) { (void)fd; (void)a; (void)l; return nosys(); }
int connect(int fd, const struct sockaddr *a, socklen_t l) { (void)fd; (void)a; (void)l; return nosys(); }
int getsockname(int fd, struct sockaddr *a, socklen_t *l) { (void)fd; (void)a; (void)l; return nosys(); }
int getpeername(int fd, struct sockaddr *a, socklen_t *l) { (void)fd; (void)a; (void)l; return nosys(); }
int setsockopt(int fd, int lv, int n, const void *v, socklen_t l) { (void)fd; (void)lv; (void)n; (void)v; (void)l; return nosys(); }
int getsockopt(int fd, int lv, int n, void *v, socklen_t *l) { (void)fd; (void)lv; (void)n; (void)v; (void)l; return nosys(); }
ssize_t send(int fd, const void *b, size_t n, int f) { (void)fd; (void)b; (void)n; (void)f; return nosys(); }
ssize_t recv(int fd, void *b, size_t n, int f) { (void)fd; (void)b; (void)n; (void)f; return nosys(); }
int shutdown(int fd, int h) { (void)fd; (void)h; return nosys(); }

int select(int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *tv)
{
    (void)n; (void)r; (void)w; (void)e; (void)tv;
    return nosys();
}

/* One thread until M3: a mutex is always free, and no second thread starts. */
int pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg)
{
    (void)t; (void)a; (void)fn; (void)arg;
    return EAGAIN;
}
int pthread_join(pthread_t t, void **ret) { (void)t; (void)ret; return ESRCH; }
int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a) { (void)m; (void)a; return 0; }
int pthread_mutex_destroy(pthread_mutex_t *m) { (void)m; return 0; }
int pthread_mutex_lock(pthread_mutex_t *m) { (void)m; return 0; }
int pthread_mutex_trylock(pthread_mutex_t *m) { (void)m; return 0; }
int pthread_mutex_unlock(pthread_mutex_t *m) { (void)m; return 0; }

unsigned alarm(unsigned secs)
{
    (void)secs;
    return 0;
}

/* espix's own calls an M-mode app makes; a U-mode app gets signals asynchronously instead (M4). */
int espix_sigcheck(void) { return 0; }
int espix_term_size(int *cols, int *rows) { (void)cols; (void)rows; return nosys(); }
