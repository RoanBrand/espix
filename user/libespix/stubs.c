/* What U-mode apps cannot have yet, failing cleanly: sockets and select (M6). docs/USER-MODE.md. */
#include <arpa/inet.h>
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
