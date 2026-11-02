/* Sockets for U-mode apps: lwIP's values and sockaddr layout, which the kernel hands to lwIP unchanged. */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t socklen_t;
typedef uint8_t  sa_family_t;

/* Each sockaddr starts with its length, which nothing needs to set. */
struct sockaddr {
    uint8_t     sa_len;
    sa_family_t sa_family;
    char        sa_data[14];
};

struct sockaddr_storage {
    uint8_t     s2_len;
    sa_family_t ss_family;
    char        s2_data1[2];
    uint32_t    s2_data2[3];
    uint32_t    s2_data3[3];
};

struct linger {
    int l_onoff;
    int l_linger;
};

#define AF_UNSPEC 0
#define AF_INET   2
#define AF_INET6  10
#define PF_UNSPEC AF_UNSPEC
#define PF_INET   AF_INET
#define PF_INET6  AF_INET6

#define SOCK_STREAM   1
#define SOCK_DGRAM    2
#define SOCK_RAW      3
#define SOCK_NONBLOCK 0x4000    /* O_NONBLOCK */
#define SOCK_CLOEXEC  0x40000   /* O_CLOEXEC */

#define SOL_SOCKET      0xfff
#define SO_ACCEPTCONN   0x0002
#define SO_REUSEADDR    0x0004
#define SO_KEEPALIVE    0x0008
#define SO_BROADCAST    0x0020
#define SO_LINGER       0x0080
#define SO_REUSEPORT    0x0200
#define SO_SNDBUF       0x1001
#define SO_RCVBUF       0x1002
#define SO_SNDLOWAT     0x1003
#define SO_RCVLOWAT     0x1004
#define SO_SNDTIMEO     0x1005
#define SO_RCVTIMEO     0x1006
#define SO_ERROR        0x1007
#define SO_TYPE         0x1008
#define SO_NO_CHECK     0x100a
#define SO_BINDTODEVICE 0x100b

#define MSG_PEEK     0x01
#define MSG_WAITALL  0x02
#define MSG_OOB      0x04
#define MSG_DONTWAIT 0x08
#define MSG_MORE     0x10
#define MSG_NOSIGNAL 0x20

#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

#define SOMAXCONN 255

int     socket(int domain, int type, int protocol);
int     bind(int fd, const struct sockaddr *addr, socklen_t len);
int     listen(int fd, int backlog);
int     accept(int fd, struct sockaddr *addr, socklen_t *len);
int     accept4(int fd, struct sockaddr *addr, socklen_t *len, int flags);
int     connect(int fd, const struct sockaddr *addr, socklen_t len);
int     getsockname(int fd, struct sockaddr *addr, socklen_t *len);
int     getpeername(int fd, struct sockaddr *addr, socklen_t *len);
int     setsockopt(int fd, int level, int name, const void *val, socklen_t len);
int     getsockopt(int fd, int level, int name, void *val, socklen_t *len);
ssize_t send(int fd, const void *buf, size_t len, int flags);
ssize_t recv(int fd, void *buf, size_t len, int flags);
ssize_t sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr *to, socklen_t tolen);
ssize_t recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr *from, socklen_t *fromlen);
int     shutdown(int fd, int how);

#ifdef __cplusplus
}
#endif
