/*
 * What the syscall files share (umode_sys.c, umode_net.c): a call's context and the descriptor table.
 */
#pragma once

#include <errno.h>

#include "espix_umode.h"

typedef struct {
    espix_um_thread_t *t;
    espix_um_proc_t   *p;
    RvExcFrame        *f;
    const uint32_t    *a;     /* a0-a5, in the frame */
} um_call_t;

/* The errno a kernel call left, as a syscall result. */
static inline int32_t um_kret(int r)
{
    return (r < 0) ? -errno : r;
}

espix_um_fd_t *um_fd_get(espix_um_proc_t *p, uint32_t fd);
/* e into the lowest free number from `from`; the number, or -EMFILE. */
int um_fd_install(espix_um_proc_t *p, const espix_um_fd_t *e, uint32_t from);
int um_fd_release(espix_um_fd_t *e);

/* Sockets (umode_net.c): read, write and fcntl on descriptor fd, and a descriptor's reference dropped. */
int32_t um_sock_read(um_call_t *c, uint32_t fd, uint32_t uva, uint32_t len);
int32_t um_sock_write(um_call_t *c, uint32_t fd, uint32_t uva, uint32_t len);
int32_t um_sock_fcntl(um_call_t *c, uint32_t fd, int cmd, uint32_t arg);
int     um_sock_close(espix_um_sock_t *s);

#define UM_NET_SYS_DECL_(lname) int32_t um_sys_##lname(um_call_t *c);
UM_NET_SYS_DECL_(socket)
UM_NET_SYS_DECL_(bind)
UM_NET_SYS_DECL_(listen)
UM_NET_SYS_DECL_(accept)
UM_NET_SYS_DECL_(connect)
UM_NET_SYS_DECL_(sendto)
UM_NET_SYS_DECL_(recvfrom)
UM_NET_SYS_DECL_(shutdown)
UM_NET_SYS_DECL_(getsockopt)
UM_NET_SYS_DECL_(setsockopt)
UM_NET_SYS_DECL_(getsockname)
UM_NET_SYS_DECL_(getpeername)
UM_NET_SYS_DECL_(poll)
UM_NET_SYS_DECL_(getaddrinfo)
UM_NET_SYS_DECL_(getrandom)
UM_NET_SYS_DECL_(gethostname)
UM_NET_SYS_DECL_(ioctl)
UM_NET_SYS_DECL_(getifaddrs)
#undef UM_NET_SYS_DECL_
