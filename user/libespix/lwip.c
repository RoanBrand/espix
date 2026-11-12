/* lwIP's names, which ESP-IDF's socket headers inline socket() and the rest to: an IDF-project app's sockets are its own process's, as a POSIX app's are. */

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdint.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

int lwip_socket(int domain, int type, int protocol)                 { return socket(domain, type, protocol); }
int lwip_bind(int s, const struct sockaddr *a, socklen_t n)          { return bind(s, a, n); }
int lwip_listen(int s, int backlog)                                  { return listen(s, backlog); }
int lwip_accept(int s, struct sockaddr *a, socklen_t *n)             { return accept(s, a, n); }
int lwip_connect(int s, const struct sockaddr *a, socklen_t n)       { return connect(s, a, n); }
ssize_t lwip_send(int s, const void *p, size_t n, int f)             { return send(s, p, n, f); }
ssize_t lwip_recv(int s, void *p, size_t n, int f)                   { return recv(s, p, n, f); }
int lwip_shutdown(int s, int how)                                    { return shutdown(s, how); }
int lwip_close(int s)                                                { return close(s); }
int lwip_fcntl(int s, int cmd, int val)                              { return fcntl(s, cmd, val); }

ssize_t lwip_sendto(int s, const void *p, size_t n, int f, const struct sockaddr *to, socklen_t tl)
{
    return sendto(s, p, n, f, to, tl);
}

ssize_t lwip_recvfrom(int s, void *p, size_t n, int f, struct sockaddr *from, socklen_t *fl)
{
    return recvfrom(s, p, n, f, from, fl);
}

int lwip_setsockopt(int s, int level, int opt, const void *v, socklen_t n)
{
    return setsockopt(s, level, opt, v, n);
}

int lwip_getsockopt(int s, int level, int opt, void *v, socklen_t *n)
{
    return getsockopt(s, level, opt, v, n);
}

int lwip_getsockname(int s, struct sockaddr *a, socklen_t *n)        { return getsockname(s, a, n); }
int lwip_getpeername(int s, struct sockaddr *a, socklen_t *n)        { return getpeername(s, a, n); }

int lwip_select(int n, fd_set *r, fd_set *w, fd_set *x, struct timeval *t)
{
    return select(n, r, w, x, t);
}

int lwip_getaddrinfo(const char *node, const char *serv, const struct addrinfo *hints, struct addrinfo **res)
{
    return getaddrinfo(node, serv, hints, res);
}

void lwip_freeaddrinfo(struct addrinfo *ai)                          { freeaddrinfo(ai); }
struct hostent *lwip_gethostbyname(const char *name)                 { return gethostbyname(name); }

const char *lwip_inet_ntop(int af, const void *src, char *dst, socklen_t n)
{
    return inet_ntop(af, src, dst, n);
}

int lwip_inet_pton(int af, const char *src, void *dst)               { return inet_pton(af, src, dst); }

uint16_t lwip_htons(uint16_t x)                                      { return __builtin_bswap16(x); }
uint32_t lwip_htonl(uint32_t x)                                      { return __builtin_bswap32(x); }
uint32_t ipaddr_addr(const char *cp)                                 { return inet_addr(cp); }

/* An ip4_addr_t is one network-order word, as a struct in_addr is. */
char *ip4addr_ntoa(const void *addr)                                 { return inet_ntoa(*(const struct in_addr *)addr); }
