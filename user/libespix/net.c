/* Sockets, poll, select and name lookup as espix syscalls; the address conversions are plain C. */
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "espix_gai.h"
#include "sys.h"

_Static_assert(sizeof(struct sockaddr_storage) == sizeof(((struct espix_addrinfo *)0)->addr), "a whole sockaddr");

int socket(int domain, int type, int protocol)
{
    return (int)espix_ret(espix_sys3(ESPIX_SYS_SOCKET, domain, type, protocol));
}

int bind(int fd, const struct sockaddr *addr, socklen_t len)
{
    return (int)espix_ret(espix_sys3(ESPIX_SYS_BIND, fd, addr, len));
}

int listen(int fd, int backlog)
{
    return (int)espix_ret(espix_sys2(ESPIX_SYS_LISTEN, fd, backlog));
}

int accept4(int fd, struct sockaddr *addr, socklen_t *len, int flags)
{
    return (int)espix_ret(espix_sys6(ESPIX_SYS_ACCEPT, fd, (long)addr, (long)len, flags, 0, 0));
}

int accept(int fd, struct sockaddr *addr, socklen_t *len)
{
    return accept4(fd, addr, len, 0);
}

int connect(int fd, const struct sockaddr *addr, socklen_t len)
{
    return (int)espix_ret(espix_sys3(ESPIX_SYS_CONNECT, fd, addr, len));
}

int getsockname(int fd, struct sockaddr *addr, socklen_t *len)
{
    return (int)espix_ret(espix_sys3(ESPIX_SYS_GETSOCKNAME, fd, addr, len));
}

int getpeername(int fd, struct sockaddr *addr, socklen_t *len)
{
    return (int)espix_ret(espix_sys3(ESPIX_SYS_GETPEERNAME, fd, addr, len));
}

int setsockopt(int fd, int level, int name, const void *val, socklen_t len)
{
    return (int)espix_ret(espix_sys5(ESPIX_SYS_SETSOCKOPT, fd, level, name, val, len));
}

int getsockopt(int fd, int level, int name, void *val, socklen_t *len)
{
    return (int)espix_ret(espix_sys5(ESPIX_SYS_GETSOCKOPT, fd, level, name, val, len));
}

ssize_t sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr *to, socklen_t tolen)
{
    return espix_ret(espix_sys6(ESPIX_SYS_SENDTO, fd, (long)buf, (long)len, flags, (long)to, (long)tolen));
}

ssize_t recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr *from, socklen_t *fromlen)
{
    return espix_ret(espix_sys6(ESPIX_SYS_RECVFROM, fd, (long)buf, (long)len, flags, (long)from, (long)fromlen));
}

ssize_t send(int fd, const void *buf, size_t len, int flags)
{
    return sendto(fd, buf, len, flags, NULL, 0);
}

ssize_t recv(int fd, void *buf, size_t len, int flags)
{
    return recvfrom(fd, buf, len, flags, NULL, NULL);
}

int shutdown(int fd, int how)
{
    return (int)espix_ret(espix_sys2(ESPIX_SYS_SHUTDOWN, fd, how));
}

int ioctl(int fd, unsigned long req, ...)
{
    va_list ap;
    va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    if (req == TIOCGWINSZ) {
        int cols, rows;
        if (!isatty(fd) || espix_sys2(ESPIX_SYS_TERM_SIZE, &cols, &rows) != 0) {
            errno = ENOTTY;
            return -1;
        }
        *(struct winsize *)arg = (struct winsize){ .ws_row = (unsigned short)rows, .ws_col = (unsigned short)cols };
        return 0;
    }
    return (int)espix_ret(espix_sys3(ESPIX_SYS_IOCTL, fd, req, arg));
}

/* ------------------------------------------------------------------ */
/* poll and select                                                     */
/* ------------------------------------------------------------------ */

int poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
    return (int)espix_ret(espix_sys3(ESPIX_SYS_POLL, fds, nfds, timeout));
}

/* Over poll. exceptfds is left empty: nothing here raises an exceptional condition. */
int select(int n, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *tv)
{
    struct pollfd p[FD_SETSIZE];
    nfds_t k = 0;
    if (n < 0 || n > FD_SETSIZE || (tv != NULL && (tv->tv_sec < 0 || tv->tv_usec < 0))) {
        errno = EINVAL;
        return -1;
    }
    for (int fd = 0; fd < n; fd++) {
        const short ev = ((rd != NULL && FD_ISSET(fd, rd)) ? POLLIN : 0) |
                         ((wr != NULL && FD_ISSET(fd, wr)) ? POLLOUT : 0);
        if (ev != 0) {
            p[k++] = (struct pollfd){ .fd = fd, .events = ev };
        }
    }
    int ms = -1;
    if (tv != NULL) {
        const long long t = (long long)tv->tv_sec * 1000 + (tv->tv_usec + 999) / 1000;
        ms = (t > INT_MAX) ? INT_MAX : (int)t;
    }
    const int rc = poll(p, k, ms);
    if (rc < 0) {
        return -1;
    }
    for (nfds_t i = 0; i < k; i++) {
        if (p[i].revents & POLLNVAL) {
            errno = EBADF;
            return -1;
        }
    }
    if (rd != NULL) {
        FD_ZERO(rd);
    }
    if (wr != NULL) {
        FD_ZERO(wr);
    }
    if (ex != NULL) {
        FD_ZERO(ex);
    }
    int ready = 0;
    for (nfds_t i = 0; i < k; i++) {
        if ((p[i].events & POLLIN) && (p[i].revents & (POLLIN | POLLHUP | POLLERR))) {
            FD_SET(p[i].fd, rd);
            ready++;
        }
        if ((p[i].events & POLLOUT) && (p[i].revents & (POLLOUT | POLLHUP | POLLERR))) {
            FD_SET(p[i].fd, wr);
            ready++;
        }
    }
    return ready;
}

/* ------------------------------------------------------------------ */
/* Names                                                               */
/* ------------------------------------------------------------------ */

int h_errno;

const struct in6_addr in6addr_any      = IN6ADDR_ANY_INIT;
const struct in6_addr in6addr_loopback = IN6ADDR_LOOPBACK_INIT;

int gethostname(char *name, size_t len)
{
    return (int)espix_ret(espix_sys2(ESPIX_SYS_GETHOSTNAME, name, len));
}

ssize_t getrandom(void *buf, size_t len, unsigned int flags)
{
    return espix_ret(espix_sys3(ESPIX_SYS_GETRANDOM, buf, len, flags));
}

int getentropy(void *buf, size_t len)
{
    if (len > 256) {
        errno = EIO;
        return -1;
    }
    return (getrandom(buf, len, 0) == (ssize_t)len) ? 0 : -1;
}

/* One result with its sockaddr, in one allocation. */
struct ai_node {
    struct addrinfo         ai;
    struct sockaddr_storage sa;
};

static struct addrinfo *ai_new(const struct espix_addrinfo *e, int socktype, int protocol)
{
    struct ai_node *n = calloc(1, sizeof(*n));
    if (n == NULL) {
        return NULL;
    }
    memcpy(&n->sa, e->addr, e->addrlen);
    n->ai = (struct addrinfo){ .ai_flags = e->flags, .ai_family = e->family, .ai_socktype = socktype,
                               .ai_protocol = protocol, .ai_addrlen = e->addrlen,
                               .ai_addr = (struct sockaddr *)&n->sa };
    return &n->ai;
}

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res)
{
    struct espix_addrinfo h = { 0 }, out[8];
    if (hints != NULL) {
        h = (struct espix_addrinfo){ .flags = hints->ai_flags, .family = hints->ai_family,
                                     .socktype = hints->ai_socktype, .protocol = hints->ai_protocol };
    }
    if (node == NULL && service == NULL) {
        return EAI_NONAME;
    }
    const long n = espix_sys5(ESPIX_SYS_GETADDRINFO, node, service, hints ? &h : NULL, out, 8);
    if (n < 0) {
        if (-n >= EAI_NONAME && -n <= TRY_AGAIN) {
            return (int)-n;
        }
        errno = (int)-n;
        return EAI_SYSTEM;
    }
    if (n == 0) {
        return EAI_NONAME;
    }
    /* No type asked for is each of them, as on Linux: lwIP answers with one. */
    static const int any[][2] = { { SOCK_STREAM, IPPROTO_TCP }, { SOCK_DGRAM, IPPROTO_UDP } };
    struct addrinfo *head = NULL, **tail = &head;
    for (long i = 0; i < n; i++) {
        const bool each = out[i].socktype == 0 && out[i].protocol == 0;
        for (size_t t = 0; t < (each ? 2 : 1); t++) {
            struct addrinfo *ai = each ? ai_new(&out[i], any[t][0], any[t][1])
                                       : ai_new(&out[i], out[i].socktype, out[i].protocol);
            if (ai == NULL) {
                freeaddrinfo(head);
                return EAI_MEMORY;
            }
            *tail = ai;
            tail = &ai->ai_next;
        }
    }
    if (hints != NULL && (hints->ai_flags & AI_CANONNAME) && node != NULL) {
        head->ai_canonname = strdup(node);
    }
    *res = head;
    return 0;
}

void freeaddrinfo(struct addrinfo *res)
{
    while (res != NULL) {
        struct addrinfo *next = res->ai_next;
        free(res->ai_canonname);
        free(res);
        res = next;
    }
}

const char *gai_strerror(int err)
{
    return espix_gai_strerror(err);
}

struct hostent *gethostbyname(const char *name)
{
    static struct in_addr addr;
    static char *list[2] = { (char *)&addr, NULL };
    static char *aliases[1] = { NULL };
    static char host[NI_MAXHOST];
    static struct hostent he = { .h_name = host, .h_aliases = aliases, .h_addrtype = AF_INET,
                                 .h_length = sizeof(addr), .h_addr_list = list };
    const struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res;
    if (getaddrinfo(name, NULL, &hints, &res) != 0) {
        h_errno = HOST_NOT_FOUND;
        return NULL;
    }
    addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    snprintf(host, sizeof(host), "%s", name);
    return &he;
}

/* Numeric only: lwIP has no reverse lookup. */
int getnameinfo(const struct sockaddr *addr, socklen_t len, char *host, socklen_t hostlen,
                char *serv, socklen_t servlen, int flags)
{
    (void)flags;
    const void *a;
    in_port_t port;
    if (addr->sa_family == AF_INET && len >= sizeof(struct sockaddr_in)) {
        a    = &((const struct sockaddr_in *)addr)->sin_addr;
        port = ((const struct sockaddr_in *)addr)->sin_port;
    } else if (addr->sa_family == AF_INET6 && len >= sizeof(struct sockaddr_in6)) {
        a    = &((const struct sockaddr_in6 *)addr)->sin6_addr;
        port = ((const struct sockaddr_in6 *)addr)->sin6_port;
    } else {
        return EAI_FAMILY;
    }
    if (host != NULL && hostlen > 0 && inet_ntop(addr->sa_family, a, host, hostlen) == NULL) {
        return EAI_OVERFLOW;
    }
    if (serv != NULL && servlen > 0 && snprintf(serv, servlen, "%u", ntohs(port)) >= (int)servlen) {
        return EAI_OVERFLOW;
    }
    return 0;
}

/* One allocation per entry: the node, its name and its two sockaddrs. */
struct ifa_node {
    struct ifaddrs          ifa;
    char                    name[16];
    struct sockaddr_storage addr, netmask;
};

int getifaddrs(struct ifaddrs **ifap)
{
    struct espix_ifaddr e[32];
    const long n = espix_sys2(ESPIX_SYS_GETIFADDRS, e, 32);
    if (n < 0) {
        return (int)espix_ret(n);
    }
    struct ifaddrs *head = NULL, **tail = &head;
    for (long i = 0; i < n; i++) {
        struct ifa_node *x = calloc(1, sizeof(*x));
        if (x == NULL) {
            freeifaddrs(head);
            errno = ENOMEM;
            return -1;
        }
        memcpy(x->name, e[i].name, sizeof(x->name));
        memcpy(&x->addr, e[i].addr, sizeof(e[i].addr));
        memcpy(&x->netmask, e[i].netmask, sizeof(e[i].netmask));
        x->ifa = (struct ifaddrs){ .ifa_name = x->name, .ifa_flags = e[i].flags };
        if (x->addr.ss_family != AF_UNSPEC) {
            x->ifa.ifa_addr    = (struct sockaddr *)&x->addr;
            x->ifa.ifa_netmask = (struct sockaddr *)&x->netmask;
        }
        *tail = &x->ifa;
        tail = &x->ifa.ifa_next;
    }
    *ifap = head;
    return 0;
}

void freeifaddrs(struct ifaddrs *ifa)
{
    while (ifa != NULL) {
        struct ifaddrs *next = ifa->ifa_next;
        free(ifa);
        ifa = next;
    }
}

/* ------------------------------------------------------------------ */
/* Address conversion                                                  */
/* ------------------------------------------------------------------ */

/* a.b.c.d, a.b.c, a.b or a, each part decimal, octal or hex: the classic inet_aton(). */
int inet_aton(const char *cp, struct in_addr *inp)
{
    uint32_t part[4];
    int n = 0;
    for (;;) {
        if (*cp < '0' || *cp > '9') {
            return 0;
        }
        char *end;
        const unsigned long v = strtoul(cp, &end, 0);
        if (v > UINT32_MAX || n == 4) {
            return 0;
        }
        part[n++] = (uint32_t)v;
        cp = end;
        if (*cp == '\0') {
            break;
        }
        if (*cp++ != '.') {
            return 0;
        }
    }
    static const uint32_t max[] = { UINT32_MAX, 0xffffff, 0xffff, 0xff };
    uint32_t a = 0;
    for (int i = 0; i < n - 1; i++) {
        if (part[i] > 0xff) {
            return 0;
        }
        a |= part[i] << (24 - 8 * i);
    }
    if (part[n - 1] > max[n - 1]) {
        return 0;
    }
    a |= part[n - 1];
    if (inp != NULL) {
        inp->s_addr = htonl(a);
    }
    return 1;
}

in_addr_t inet_addr(const char *cp)
{
    struct in_addr a;
    return inet_aton(cp, &a) ? a.s_addr : INADDR_NONE;
}

char *inet_ntoa(struct in_addr in)
{
    static char buf[INET_ADDRSTRLEN];
    return (char *)inet_ntop(AF_INET, &in, buf, sizeof(buf));
}

/* Exactly four decimal parts, no leading zeros. */
static int pton4(const char *s, uint8_t *out)
{
    uint8_t a[4];
    int n = 0;
    while (n < 4) {
        unsigned v = 0;
        const char *start = s;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (unsigned)(*s++ - '0');
            if (v > 255 || (s - start > 1 && *start == '0')) {
                return 0;
            }
        }
        if (s == start) {
            return 0;
        }
        a[n++] = (uint8_t)v;
        if (n < 4 && *s++ != '.') {
            return 0;
        }
    }
    if (*s != '\0') {
        return 0;
    }
    memcpy(out, a, 4);
    return 1;
}

static int hexval(char c)
{
    return (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10
         : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
}

static int pton6(const char *s, uint8_t *out)
{
    uint8_t a[16] = { 0 };
    int n = 0, gap = -1;
    if (s[0] == ':') {
        if (s[1] != ':') {
            return 0;
        }
        gap = 0;
        s += 2;
    }
    while (*s != '\0') {
        const char *end = s;
        unsigned v = 0;
        while (hexval(*end) >= 0 && end - s < 4) {
            v = (v << 4) | (unsigned)hexval(*end++);
        }
        if (*end == '.') {
            if (n > 12 || !pton4(s, a + n)) {
                return 0;
            }
            n += 4;
            break;
        }
        if (end == s || hexval(*end) >= 0 || n > 14) {
            return 0;
        }
        a[n++] = (uint8_t)(v >> 8);
        a[n++] = (uint8_t)v;
        s = end;
        if (*s == ':') {
            s++;
            if (*s == ':') {
                if (gap >= 0) {
                    return 0;
                }
                gap = n;
                s++;
            } else if (*s == '\0') {
                return 0;
            }
        } else if (*s != '\0') {
            return 0;
        }
    }
    if (gap >= 0) {
        if (n == 16) {
            return 0;
        }
        memmove(a + 16 - (n - gap), a + gap, (size_t)(n - gap));
        memset(a + gap, 0, (size_t)(16 - n));
    } else if (n != 16) {
        return 0;
    }
    memcpy(out, a, 16);
    return 1;
}

int inet_pton(int af, const char *src, void *dst)
{
    if (af == AF_INET) {
        return pton4(src, dst);
    }
    if (af == AF_INET6) {
        return pton6(src, dst);
    }
    errno = EAFNOSUPPORT;
    return -1;
}

const char *inet_ntop(int af, const void *src, char *dst, socklen_t size)
{
    char buf[INET6_ADDRSTRLEN];
    const uint8_t *a = src;
    if (af == AF_INET) {
        snprintf(buf, sizeof(buf), "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
    } else if (af == AF_INET6) {
        uint16_t w[8];
        int best = -1, bestlen = 1;
        for (int i = 0; i < 8; i++) {
            w[i] = (uint16_t)((a[2 * i] << 8) | a[2 * i + 1]);
        }
        for (int i = 0; i < 8;) {
            int j = i;
            while (j < 8 && w[j] == 0) {
                j++;
            }
            if (j - i > bestlen) {
                best    = i;
                bestlen = j - i;
            }
            i = (j > i) ? j : i + 1;
        }
        char *p = buf;
        if (best == 0 && bestlen == 5 && w[5] == 0xffff) {
            snprintf(buf, sizeof(buf), "::ffff:%u.%u.%u.%u", a[12], a[13], a[14], a[15]);
        } else {
            for (int i = 0; i < 8; i++) {
                if (best >= 0 && i >= best && i < best + bestlen) {
                    if (i == best) {
                        *p++ = ':';
                    }
                    continue;
                }
                p += sprintf(p, (i != 0) ? ":%x" : "%x", w[i]);
            }
            if (best >= 0 && best + bestlen == 8) {
                *p++ = ':';
            }
            *p = '\0';
        }
    } else {
        errno = EAFNOSUPPORT;
        return NULL;
    }
    if (strlen(buf) >= size) {
        errno = ENOSPC;
        return NULL;
    }
    return strcpy(dst, buf);
}
