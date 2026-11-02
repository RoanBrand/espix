/*
 * U-mode sockets, poll and name lookup (docs/USER-MODE.md): lwIP's calls behind the process's
 * descriptors, its option values and sockaddrs passed through unchanged. lwIP's socket is always
 * non-blocking; a call the app made blocking waits in espix_abi_select(), which a signal ends.
 */
#include "sdkconfig.h"

#if CONFIG_ESPIX_UMODE

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>

#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include <sys/poll.h>

#include "esp_random.h"
#include "esp_timer.h"
#include "riscv/encoding.h"

#include "espix_kernel.h"
#include "espix_umode.h"
#include "umode_sys.h"

#define PG 4096u

/* socket()'s and accept()'s type flags: the open flags they stand for, as on Linux. */
#define UM_SOCK_NONBLOCK O_NONBLOCK
#define UM_SOCK_CLOEXEC  O_CLOEXEC

/* user/include's socket, netinet, netdb and poll headers say the same: a change is an ABI change. */
_Static_assert(SOCK_STREAM == 1 && SOCK_DGRAM == 2 && SOCK_RAW == 3 && AF_INET == 2 && AF_INET6 == 10 &&
               UM_SOCK_NONBLOCK == 0x4000 && UM_SOCK_CLOEXEC == 0x40000, "socket types");
_Static_assert(SOL_SOCKET == 0xfff && SO_REUSEADDR == 0x4 && SO_KEEPALIVE == 0x8 && SO_BROADCAST == 0x20 &&
               SO_LINGER == 0x80 && SO_SNDBUF == 0x1001 && SO_RCVBUF == 0x1002 && SO_SNDTIMEO == 0x1005 &&
               SO_RCVTIMEO == 0x1006 && SO_ERROR == 0x1007 && SO_TYPE == 0x1008 && SO_BINDTODEVICE == 0x100b,
               "socket options");
_Static_assert(MSG_PEEK == 0x1 && MSG_WAITALL == 0x2 && MSG_OOB == 0x4 && MSG_DONTWAIT == 0x8 && MSG_MORE == 0x10 &&
               MSG_NOSIGNAL == 0x20, "send and receive flags");
_Static_assert(IPPROTO_TCP == 6 && IPPROTO_UDP == 17 && IPPROTO_IPV6 == 41 && TCP_NODELAY == 1 && TCP_KEEPIDLE == 3 &&
               TCP_KEEPINTVL == 4 && TCP_KEEPCNT == 5 && IPV6_V6ONLY == 27 && IP_TOS == 1 && IP_TTL == 2 &&
               IP_ADD_MEMBERSHIP == 3 && IP_MULTICAST_TTL == 5, "protocol options");
_Static_assert(sizeof(struct sockaddr) == 16 && sizeof(struct sockaddr_in) == 16 && sizeof(struct sockaddr_in6) == 28 &&
               sizeof(struct sockaddr_storage) == 28 && offsetof(struct sockaddr_in, sin_port) == 2, "sockaddrs");
_Static_assert(POLLIN == 0x1 && POLLRDNORM == 0x2 && POLLRDBAND == 0x4 && POLLOUT == 0x8 && POLLWRBAND == 0x10 &&
               POLLERR == 0x20 && POLLHUP == 0x40 && POLLNVAL == 0x80 && sizeof(struct pollfd) == 8, "poll");
_Static_assert(EAI_NONAME == 200 && EAI_SERVICE == 201 && EAI_FAIL == 202 && EAI_MEMORY == 203 && EAI_FAMILY == 204 &&
               AI_PASSIVE == 0x1 && AI_CANONNAME == 0x2 && AI_NUMERICHOST == 0x4 && AI_NUMERICSERV == 0x8, "netdb");
_Static_assert((uint32_t)FIONREAD == 0x4004667fu && (uint32_t)FIONBIO == 0x8004667eu, "ioctls");

/* The largest datagram, and so the bounce buffer one passes through. */
#define UM_DGRAM_MAX 65535u
#define UM_OPT_MAX   64
#define UM_POLL_MAX  64

/* ------------------------------------------------------------------ */
/* References                                                          */
/* ------------------------------------------------------------------ */

static espix_um_sock_t *sock_new(int kfd, int type)
{
    espix_um_sock_t *s = calloc(1, sizeof(*s));
    if (s == NULL || lwip_fcntl(kfd, F_SETFL, O_NONBLOCK) != 0) {
        free(s);
        return NULL;
    }
    s->kfd  = kfd;
    s->refs = 1;
    s->type = type;
    return s;
}

int um_sock_close(espix_um_sock_t *s)
{
    if (__atomic_sub_fetch(&s->refs, 1, __ATOMIC_ACQ_REL) != 0) {
        return 0;
    }
    const int r = um_kret(lwip_close(s->kfd));
    free(s);
    return r;
}

/* Descriptor fd's socket, held until um_sock_close(); NULL with *err. */
static espix_um_sock_t *sock_get(um_call_t *c, uint32_t fd, int32_t *err)
{
    espix_um_sock_t *s = NULL;
    *err = -EBADF;
    xSemaphoreTake(c->p->lock, portMAX_DELAY);
    espix_um_fd_t *e = um_fd_get(c->p, fd);
    if (e != NULL && e->kind == ESPIX_UM_FD_SOCK) {
        s = e->sock;
        __atomic_add_fetch(&s->refs, 1, __ATOMIC_RELAXED);
    } else if (e != NULL) {
        *err = -ENOTSOCK;
    }
    xSemaphoreGive(c->p->lock);
    return s;
}

static int32_t sock_install(um_call_t *c, espix_um_sock_t *s, bool cloexec)
{
    const espix_um_fd_t e = { .kind = ESPIX_UM_FD_SOCK, .cloexec = cloexec, .sock = s };
    const int fd = um_fd_install(c->p, &e, 0);
    if (fd < 0) {
        um_sock_close(s);
    }
    return fd;
}

/* ------------------------------------------------------------------ */
/* Waiting                                                             */
/* ------------------------------------------------------------------ */

static int64_t deadline_of(int64_t us)
{
    return (us > 0) ? esp_timer_get_time() + us : -1;
}

/* Until kfd can be read (or written), a signal is due, or the deadline passes: 0, -EINTR or -EAGAIN. */
static int32_t sock_wait(int kfd, bool out, int64_t deadline)
{
    fd_set set;
    FD_ZERO(&set);
    FD_SET(kfd, &set);
    struct timeval tv, *tvp = NULL;
    if (deadline >= 0) {
        const int64_t us = MAX(deadline - esp_timer_get_time(), 0);
        tv = (struct timeval){ .tv_sec = us / 1000000, .tv_usec = us % 1000000 };
        tvp = &tv;
    }
    const int rc = espix_abi_select(kfd + 1, out ? NULL : &set, out ? &set : NULL, NULL, tvp);
    return (rc > 0) ? 0 : (rc == 0) ? -EAGAIN : -errno;
}

/* ------------------------------------------------------------------ */
/* Addresses                                                           */
/* ------------------------------------------------------------------ */

static int32_t addr_in(um_call_t *c, uint32_t uaddr, uint32_t len, struct sockaddr_storage *k)
{
    if (len > sizeof(*k)) {
        return -EINVAL;
    }
    memset(k, 0, sizeof(*k));
    return espix_um_copyin(c->p->as, k, uaddr, len);
}

/* A sockaddr out to the app: what its buffer holds, and the whole length at ulen. */
static int32_t addr_out(um_call_t *c, uint32_t uaddr, uint32_t ulen, const void *k, socklen_t klen)
{
    if (uaddr == 0 || ulen == 0) {
        return 0;
    }
    socklen_t have;
    int32_t r = espix_um_copyin(c->p->as, &have, ulen, sizeof(have));
    const socklen_t n = MIN(have, klen);
    if (r == 0 && n > 0) {
        r = espix_um_copyout(c->p->as, uaddr, k, n);
    }
    return (r == 0) ? espix_um_copyout(c->p->as, ulen, &klen, sizeof(klen)) : r;
}

/* ------------------------------------------------------------------ */
/* Moving data                                                         */
/* ------------------------------------------------------------------ */

/* The errors that mean a stream can no longer be written: SIGPIPE's, unless MSG_NOSIGNAL. */
static int32_t send_failed(um_call_t *c, espix_um_sock_t *s, int err, int flags)
{
    if (s->type != SOCK_STREAM) {
        return -err;
    }
    /* lwIP says ENOTCONN for a connection that has gone; Linux says EPIPE. */
    if (err == ENOTCONN && s->connected) {
        err = EPIPE;
    }
    if ((err == EPIPE || err == ECONNRESET || err == ECONNABORTED) && !(flags & MSG_NOSIGNAL)) {
        (void)espix_um_raise(c->t, SIGPIPE);
    }
    return -err;
}

static int32_t sock_send(um_call_t *c, espix_um_sock_t *s, uint32_t uva, uint32_t len, int flags,
                         const struct sockaddr *to, socklen_t tolen)
{
    const bool block = !s->nonblock && !(flags & MSG_DONTWAIT);
    const int kflags = (flags & ~MSG_NOSIGNAL) | MSG_DONTWAIT;
    const int64_t deadline = deadline_of(s->snd_us);

    /* A datagram goes whole, so it is copied in first. */
    if (s->type != SOCK_STREAM || to != NULL) {
        if (len > UM_DGRAM_MAX) {
            return -EMSGSIZE;
        }
        void *buf = malloc(MAX(len, 1u));
        if (buf == NULL) {
            return -ENOMEM;
        }
        int32_t r = espix_um_copyin(c->p->as, buf, uva, len);
        while (r == 0) {
            const ssize_t n = lwip_sendto(s->kfd, buf, len, kflags, to, tolen);
            if (n >= 0) {
                r = (int32_t)n;
                break;
            }
            r = -errno;
            if (r != -EAGAIN || !block) {
                r = send_failed(c, s, -r, flags);
                break;
            }
            r = sock_wait(s->kfd, true, deadline);
        }
        free(buf);
        return r;
    }

    uint32_t done = 0;
    while (done < len) {
        const uint32_t va = uva + done;
        const void *k = espix_um_user_pin(c->p->as, va, PTE_R);
        if (k == NULL) {
            return done ? (int32_t)done : -EFAULT;
        }
        const uint32_t want = MIN(len - done, PG - (va & (PG - 1)));
        const ssize_t n = lwip_send(s->kfd, k, want, kflags);
        const int err = errno;
        espix_um_user_unpin(c->p->as);
        if (n > 0) {
            done += (uint32_t)n;
            s->connected = true;
            continue;
        }
        if (n < 0 && err == EAGAIN && block) {
            const int32_t r = sock_wait(s->kfd, true, deadline);
            if (r != 0) {
                return done ? (int32_t)done : r;
            }
            continue;
        }
        if (done > 0) {
            break;
        }
        return (n < 0) ? send_failed(c, s, err, flags) : 0;
    }
    return (int32_t)done;
}

static int32_t sock_recv(um_call_t *c, espix_um_sock_t *s, uint32_t uva, uint32_t len, int flags,
                         uint32_t ufrom, uint32_t ufromlen)
{
    const bool block = !s->nonblock && !(flags & MSG_DONTWAIT);
    const int kflags = flags | MSG_DONTWAIT;
    const int64_t deadline = deadline_of(s->rcv_us);

    /* A datagram, a peek or a sender's address takes one call, so it comes through a buffer. */
    if (s->type != SOCK_STREAM || (flags & MSG_PEEK) || ufrom != 0) {
        const uint32_t cap = MIN(len, UM_DGRAM_MAX);
        void *buf = malloc(MAX(cap, 1u));
        if (buf == NULL) {
            return -ENOMEM;
        }
        struct sockaddr_storage from;
        socklen_t fromlen = sizeof(from);
        int32_t r;
        for (;;) {
            const ssize_t n = lwip_recvfrom(s->kfd, buf, cap, kflags, (struct sockaddr *)&from, &fromlen);
            if (n >= 0) {
                r = espix_um_copyout(c->p->as, uva, buf, (size_t)n);
                if (r == 0) {
                    r = addr_out(c, ufrom, ufromlen, &from, fromlen);
                }
                if (r == 0) {
                    r = (int32_t)n;
                }
                break;
            }
            r = -errno;
            if (r != -EAGAIN || !block || (r = sock_wait(s->kfd, false, deadline)) != 0) {
                break;
            }
        }
        free(buf);
        return r;
    }

    uint32_t done = 0;
    while (done < len) {
        const uint32_t va = uva + done;
        void *k = espix_um_user_pin(c->p->as, va, PTE_W);
        if (k == NULL) {
            return done ? (int32_t)done : -EFAULT;
        }
        const uint32_t want = MIN(len - done, PG - (va & (PG - 1)));
        const ssize_t n = lwip_recv(s->kfd, k, want, kflags);
        const int err = errno;
        espix_um_user_unpin(c->p->as);
        if (n >= 0) {
            done += (uint32_t)n;
            if ((uint32_t)n < want) {
                break;
            }
            continue;
        }
        if (done > 0) {
            break;
        }
        if (err != EAGAIN || !block) {
            return -err;
        }
        const int32_t r = sock_wait(s->kfd, false, deadline);
        if (r != 0) {
            return r;
        }
    }
    return (int32_t)done;
}

int32_t um_sock_read(um_call_t *c, uint32_t fd, uint32_t uva, uint32_t len)
{
    int32_t r;
    espix_um_sock_t *s = sock_get(c, fd, &r);
    if (s != NULL) {
        r = sock_recv(c, s, uva, len, 0, 0, 0);
        um_sock_close(s);
    }
    return r;
}

int32_t um_sock_write(um_call_t *c, uint32_t fd, uint32_t uva, uint32_t len)
{
    int32_t r;
    espix_um_sock_t *s = sock_get(c, fd, &r);
    if (s != NULL) {
        r = sock_send(c, s, uva, len, 0, NULL, 0);
        um_sock_close(s);
    }
    return r;
}

int32_t um_sock_fcntl(um_call_t *c, uint32_t fd, int cmd, uint32_t arg)
{
    int32_t r;
    espix_um_sock_t *s = sock_get(c, fd, &r);
    if (s == NULL) {
        return r;
    }
    if (cmd == F_SETFL) {
        s->nonblock = (arg & O_NONBLOCK) != 0;
        r = 0;
    } else {
        r = O_RDWR | (s->nonblock ? O_NONBLOCK : 0);
    }
    um_sock_close(s);
    return r;
}

/* ------------------------------------------------------------------ */
/* The calls                                                           */
/* ------------------------------------------------------------------ */

int32_t um_sys_socket(um_call_t *c)
{
    const int type = (int)(c->a[1] & ~(UM_SOCK_NONBLOCK | UM_SOCK_CLOEXEC));
    const int kfd = lwip_socket((int)c->a[0], type, (int)c->a[2]);
    if (kfd < 0) {
        return -errno;
    }
    espix_um_sock_t *s = sock_new(kfd, type);
    if (s == NULL) {
        lwip_close(kfd);
        return -ENOMEM;
    }
    s->nonblock = (c->a[1] & UM_SOCK_NONBLOCK) != 0;
    return sock_install(c, s, (c->a[1] & UM_SOCK_CLOEXEC) != 0);
}

int32_t um_sys_bind(um_call_t *c)
{
    int32_t r;
    espix_um_sock_t *s = sock_get(c, c->a[0], &r);
    if (s == NULL) {
        return r;
    }
    struct sockaddr_storage a;
    r = addr_in(c, c->a[1], c->a[2], &a);
    if (r == 0) {
        r = um_kret(lwip_bind(s->kfd, (struct sockaddr *)&a, c->a[2]));
    }
    um_sock_close(s);
    return r;
}

int32_t um_sys_listen(um_call_t *c)
{
    int32_t r;
    espix_um_sock_t *s = sock_get(c, c->a[0], &r);
    if (s != NULL) {
        r = um_kret(lwip_listen(s->kfd, (int)c->a[1]));
        um_sock_close(s);
    }
    return r;
}

int32_t um_sys_accept(um_call_t *c)
{
    const uint32_t flags = c->a[3];
    if (flags & ~(UM_SOCK_NONBLOCK | UM_SOCK_CLOEXEC)) {
        return -EINVAL;
    }
    int32_t r;
    espix_um_sock_t *s = sock_get(c, c->a[0], &r);
    if (s == NULL) {
        return r;
    }
    const int64_t deadline = deadline_of(s->rcv_us);
    struct sockaddr_storage a;
    socklen_t alen;
    int kfd;
    for (;;) {
        alen = sizeof(a);
        kfd = lwip_accept(s->kfd, (struct sockaddr *)&a, &alen);
        if (kfd >= 0) {
            break;
        }
        r = -errno;
        if (r != -EAGAIN || s->nonblock || (r = sock_wait(s->kfd, false, deadline)) != 0) {
            um_sock_close(s);
            return r;
        }
    }
    um_sock_close(s);

    r = addr_out(c, c->a[1], c->a[2], &a, alen);
    espix_um_sock_t *n = (r == 0) ? sock_new(kfd, SOCK_STREAM) : NULL;
    if (n == NULL) {
        lwip_close(kfd);
        return r ? r : -ENOMEM;
    }
    n->nonblock  = (flags & UM_SOCK_NONBLOCK) != 0;
    n->connected = true;
    return sock_install(c, n, (flags & UM_SOCK_CLOEXEC) != 0);
}

int32_t um_sys_connect(um_call_t *c)
{
    int32_t r;
    espix_um_sock_t *s = sock_get(c, c->a[0], &r);
    if (s == NULL) {
        return r;
    }
    struct sockaddr_storage a;
    r = addr_in(c, c->a[1], c->a[2], &a);
    if (r == 0) {
        r = um_kret(lwip_connect(s->kfd, (struct sockaddr *)&a, c->a[2]));
    }
    if (r == -EISCONN && s->connecting) {
        r = 0;
    } else if ((r == -EINPROGRESS || r == -EALREADY) && !s->nonblock) {
        /* Finished by the next connect() if a signal cuts this one short, as Linux's restart does. */
        s->connecting = true;
        r = sock_wait(s->kfd, true, deadline_of(s->snd_us));
        if (r == 0) {
            int err = 0;
            socklen_t len = sizeof(err);
            r = um_kret(lwip_getsockopt(s->kfd, SOL_SOCKET, SO_ERROR, &err, &len));
            if (r == 0 && err != 0) {
                r = -err;
            }
        } else if (r == -EAGAIN) {
            r = -EINPROGRESS;
        }
    }
    if (r != -EINTR && r != -EINPROGRESS) {
        s->connecting = false;
    }
    if (r == 0) {
        s->connected = true;
    }
    um_sock_close(s);
    return r;
}

int32_t um_sys_sendto(um_call_t *c)
{
    int32_t r;
    espix_um_sock_t *s = sock_get(c, c->a[0], &r);
    if (s == NULL) {
        return r;
    }
    struct sockaddr_storage to;
    r = (c->a[4] != 0) ? addr_in(c, c->a[4], c->a[5], &to) : 0;
    if (r == 0) {
        r = sock_send(c, s, c->a[1], c->a[2], (int)c->a[3],
                      c->a[4] ? (struct sockaddr *)&to : NULL, c->a[4] ? c->a[5] : 0);
    }
    um_sock_close(s);
    return r;
}

int32_t um_sys_recvfrom(um_call_t *c)
{
    int32_t r;
    espix_um_sock_t *s = sock_get(c, c->a[0], &r);
    if (s != NULL) {
        r = sock_recv(c, s, c->a[1], c->a[2], (int)c->a[3], c->a[4], c->a[5]);
        um_sock_close(s);
    }
    return r;
}

int32_t um_sys_shutdown(um_call_t *c)
{
    int32_t r;
    espix_um_sock_t *s = sock_get(c, c->a[0], &r);
    if (s != NULL) {
        r = um_kret(lwip_shutdown(s->kfd, (int)c->a[1]));
        um_sock_close(s);
    }
    return r;
}

static bool is_timeo(int level, int name)
{
    return level == SOL_SOCKET && (name == SO_RCVTIMEO || name == SO_SNDTIMEO);
}

int32_t um_sys_setsockopt(um_call_t *c)
{
    const int level = (int)c->a[1], name = (int)c->a[2];
    const uint32_t len = c->a[4];
    if (len > UM_OPT_MAX) {
        return -EINVAL;
    }
    int32_t r;
    espix_um_sock_t *s = sock_get(c, c->a[0], &r);
    if (s == NULL) {
        return r;
    }
    uint32_t v[UM_OPT_MAX / 4];
    r = espix_um_copyin(c->p->as, v, c->a[3], len);
    if (r == 0 && is_timeo(level, name)) {
        /* The timeouts are ours to keep: lwIP's never block. */
        struct timeval tv;
        if (len < sizeof(tv)) {
            r = -EINVAL;
        } else {
            memcpy(&tv, v, sizeof(tv));
            if (tv.tv_sec < 0 || tv.tv_usec < 0 || tv.tv_usec >= 1000000) {
                r = -EDOM;
            } else {
                *((name == SO_RCVTIMEO) ? &s->rcv_us : &s->snd_us) = (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
            }
        }
    } else if (r == 0) {
        r = um_kret(lwip_setsockopt(s->kfd, level, name, v, len));
    }
    um_sock_close(s);
    return r;
}

int32_t um_sys_getsockopt(um_call_t *c)
{
    const int level = (int)c->a[1], name = (int)c->a[2];
    socklen_t ulen;
    int32_t r = espix_um_copyin(c->p->as, &ulen, c->a[4], sizeof(ulen));
    if (r != 0) {
        return r;
    }
    espix_um_sock_t *s = sock_get(c, c->a[0], &r);
    if (s == NULL) {
        return r;
    }
    uint32_t v[UM_OPT_MAX / 4] = { 0 };
    socklen_t len = MIN(ulen, (socklen_t)UM_OPT_MAX);
    if (is_timeo(level, name)) {
        const int64_t us = (name == SO_RCVTIMEO) ? s->rcv_us : s->snd_us;
        const struct timeval tv = { .tv_sec = us / 1000000, .tv_usec = us % 1000000 };
        len = MIN(len, (socklen_t)sizeof(tv));
        memcpy(v, &tv, len);
    } else {
        r = um_kret(lwip_getsockopt(s->kfd, level, name, v, &len));
    }
    um_sock_close(s);
    if (r == 0) {
        r = espix_um_copyout(c->p->as, c->a[3], v, len);
    }
    return (r == 0) ? espix_um_copyout(c->p->as, c->a[4], &len, sizeof(len)) : r;
}

/* getsockname() and getpeername(). */
static int32_t sock_name(um_call_t *c, int (*fn)(int, struct sockaddr *, socklen_t *))
{
    int32_t r;
    espix_um_sock_t *s = sock_get(c, c->a[0], &r);
    if (s == NULL) {
        return r;
    }
    struct sockaddr_storage a;
    socklen_t alen = sizeof(a);
    r = um_kret(fn(s->kfd, (struct sockaddr *)&a, &alen));
    um_sock_close(s);
    return (r == 0) ? addr_out(c, c->a[1], c->a[2], &a, alen) : r;
}

int32_t um_sys_getsockname(um_call_t *c) { return sock_name(c, lwip_getsockname); }
int32_t um_sys_getpeername(um_call_t *c) { return sock_name(c, lwip_getpeername); }

int32_t um_sys_ioctl(um_call_t *c)
{
    const uint32_t req = c->a[1];
    int32_t r;
    espix_um_sock_t *s = sock_get(c, c->a[0], &r);
    if (s == NULL) {
        return (r == -ENOTSOCK) ? -ENOTTY : r;
    }
    int v = 0;
    if (req == (uint32_t)FIONBIO) {
        r = espix_um_copyin(c->p->as, &v, c->a[2], sizeof(v));
        if (r == 0) {
            s->nonblock = v != 0;
        }
    } else if (req == (uint32_t)FIONREAD) {
        r = um_kret(lwip_ioctl(s->kfd, FIONREAD, &v));
        if (r == 0) {
            r = espix_um_copyout(c->p->as, c->a[2], &v, sizeof(v));
        }
    } else {
        r = -ENOTTY;
    }
    um_sock_close(s);
    return r;
}

/* ------------------------------------------------------------------ */
/* poll                                                                */
/* ------------------------------------------------------------------ */

#define POLL_ANY (POLLIN | POLLRDNORM | POLLOUT | POLLWRNORM)

/*
 * Sockets and devices wait in espix_abi_poll(); a regular file or directory is always ready, as
 * POSIX has it, and so is stdout. stdin is reported readable: the transports have no wake a
 * select can wait on yet (docs/KNOWN-ISSUES.md).
 */
int32_t um_sys_poll(um_call_t *c)
{
    const uint32_t nfds = c->a[1];
    if (nfds > UM_POLL_MAX) {
        return -EINVAL;
    }
    struct pollfd u[nfds ? nfds : 1], k[nfds ? nfds : 1];
    espix_um_sock_t *held[nfds ? nfds : 1];
    int files[nfds ? nfds : 1];
    int32_t r = espix_um_copyin(c->p->as, u, c->a[0], nfds * sizeof(u[0]));
    if (r != 0) {
        return r;
    }

    xSemaphoreTake(c->p->lock, portMAX_DELAY);
    for (uint32_t i = 0; i < nfds; i++) {
        k[i] = (struct pollfd){ .fd = -1, .events = u[i].events };
        u[i].revents = 0;
        held[i] = NULL;
        files[i] = -1;
        if (u[i].fd < 0) {
            continue;
        }
        const espix_um_fd_t *e = um_fd_get(c->p, (uint32_t)u[i].fd);
        if (e == NULL) {
            u[i].revents = POLLNVAL;
        } else if (e->kind == ESPIX_UM_FD_SOCK) {
            held[i] = e->sock;
            __atomic_add_fetch(&e->sock->refs, 1, __ATOMIC_RELAXED);
            k[i].fd = e->sock->kfd;
        } else if (e->kind == ESPIX_UM_FD_FILE) {
            files[i] = e->kfd;
        } else if (e->kind == ESPIX_UM_FD_STD && e->std != 0) {
            u[i].revents = u[i].events & (POLLOUT | POLLWRNORM);
        } else {
            u[i].revents = u[i].events & POLL_ANY;
        }
    }
    xSemaphoreGive(c->p->lock);

    int ready = 0;
    for (uint32_t i = 0; i < nfds; i++) {
        struct stat st;
        if (files[i] >= 0) {
            if (fstat(files[i], &st) != 0) {
                u[i].revents = POLLNVAL;
            } else if (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode)) {
                u[i].revents = u[i].events & POLL_ANY;
            } else {
                k[i].fd = files[i];
            }
        }
        ready += u[i].revents != 0;
    }

    const int rc = espix_abi_poll(k, nfds, ready ? 0 : (int)c->a[2]);
    r = (rc < 0) ? -errno : 0;
    for (uint32_t i = 0; i < nfds; i++) {
        if (held[i] != NULL) {
            um_sock_close(held[i]);
        }
    }
    if (r != 0) {
        return r;
    }

    ready = 0;
    for (uint32_t i = 0; i < nfds; i++) {
        if (k[i].fd >= 0) {
            u[i].revents = k[i].revents;
        }
        ready += u[i].revents != 0;
    }
    r = espix_um_copyout(c->p->as, c->a[0], u, nfds * sizeof(u[0]));
    return (r == 0) ? ready : r;
}

/* ------------------------------------------------------------------ */
/* Names and entropy                                                   */
/* ------------------------------------------------------------------ */

int32_t um_sys_getaddrinfo(um_call_t *c)
{
    char node[256], serv[32];
    int32_t r;
    if (c->a[0] != 0 && (r = espix_um_strin(c->p->as, node, c->a[0], sizeof(node))) < 0) {
        return r;
    }
    if (c->a[1] != 0 && (r = espix_um_strin(c->p->as, serv, c->a[1], sizeof(serv))) < 0) {
        return r;
    }
    struct addrinfo hints = { 0 };
    if (c->a[2] != 0) {
        struct espix_addrinfo h;
        if ((r = espix_um_copyin(c->p->as, &h, c->a[2], sizeof(h))) != 0) {
            return r;
        }
        hints = (struct addrinfo){ .ai_flags = h.flags, .ai_family = h.family,
                                   .ai_socktype = h.socktype, .ai_protocol = h.protocol };
    }
    struct addrinfo *res = NULL;
    const int eai = lwip_getaddrinfo(c->a[0] ? node : NULL, c->a[1] ? serv : NULL,
                                     c->a[2] ? &hints : NULL, &res);
    if (eai != 0) {
        return -eai;
    }
    int32_t n = 0;
    for (const struct addrinfo *ai = res; ai != NULL && (uint32_t)n < c->a[4]; ai = ai->ai_next, n++) {
        struct espix_addrinfo o = { .flags = ai->ai_flags, .family = ai->ai_family,
                                    .socktype = ai->ai_socktype, .protocol = ai->ai_protocol,
                                    .addrlen = MIN(ai->ai_addrlen, sizeof(o.addr)) };
        memcpy(o.addr, ai->ai_addr, o.addrlen);
        if ((r = espix_um_copyout(c->p->as, c->a[3] + n * sizeof(o), &o, sizeof(o))) != 0) {
            n = r;
            break;
        }
    }
    lwip_freeaddrinfo(res);
    return n;
}

int32_t um_sys_getrandom(um_call_t *c)
{
    const uint32_t uva = c->a[0], len = c->a[1];
    uint32_t done = 0;
    while (done < len) {
        const uint32_t va = uva + done;
        void *k = espix_um_user_pin(c->p->as, va, PTE_W);
        if (k == NULL) {
            return done ? (int32_t)done : -EFAULT;
        }
        const uint32_t want = MIN(len - done, PG - (va & (PG - 1)));
        esp_fill_random(k, want);
        espix_um_user_unpin(c->p->as);
        done += want;
    }
    return (int32_t)done;
}

int32_t um_sys_getifaddrs(um_call_t *c)
{
    const uint32_t max = MIN(c->a[1], 32u);
    struct espix_ifaddr *e = calloc(MAX(max, 1u), sizeof(*e));
    if (e == NULL) {
        return -ENOMEM;
    }
    const size_t n = espix_proc_ifaddrs(e, max);
    const int32_t r = espix_um_copyout(c->p->as, c->a[0], e, n * sizeof(*e));
    free(e);
    return (r == 0) ? (int32_t)n : r;
}

int32_t um_sys_gethostname(um_call_t *c)
{
    char name[65];
    if (gethostname(name, sizeof(name)) != 0 && errno != ENAMETOOLONG) {
        return -errno;
    }
    const size_t n = strlen(name) + 1;
    return (n > c->a[1]) ? -ENAMETOOLONG : espix_um_copyout(c->p->as, c->a[0], name, n);
}

#endif /* CONFIG_ESPIX_UMODE */
