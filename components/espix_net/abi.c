/*
 * The network's answers to the syscall layer: gethostname() for the kernel and
 * its apps' GETHOSTNAME syscall, and the interface list getifaddrs() reads. An
 * app's sockets are its syscalls; lwIP's names are libespix's (user/libespix/lwip.c).
 */

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "espix_kernel.h"
#include "espix_net.h"
#include "espix_net_priv.h"
#include "espix_proc.h"
#include "espix_syscall.h"

/*
 * gethostname(), from the one source of truth.
 *
 * The name is /etc/hostname, applied to every netif at boot and changed by
 * `hostname`. A program that asks -- Mosquitto does, for the default $SYS
 * topic root -- should see what the shell's `hostname` prints, not a second
 * copy of it.
 *
 * A real global symbol: libsmb2 calls gethostname() from the kernel, inside
 * smb2_serve_port(), and an app's GETHOSTNAME syscall reaches it too.
 *
 * POSIX: truncate and NUL-terminate, and report ENAMETOOLONG if it did not fit.
 */
int gethostname(char *name, size_t len)
{
    if (name == NULL || len == 0) {
        errno = EINVAL;
        return -1;
    }

    const char *host = espix_net_hostname();
    if (host == NULL) {
        host = "";
    }

    const size_t n = strlen(host);
    if (n >= len) {
        memcpy(name, host, len - 1);
        name[len - 1] = '\0';
        errno = ENAMETOOLONG;
        return -1;
    }

    memcpy(name, host, n + 1);
    return 0;
}

/*
 * The interface table as getifaddrs() entries, by the names `ip` prints: one per
 * address, or one with no address for an interface that has none. An app reaches
 * it by syscall.
 */
static size_t net_ifaddrs(struct espix_ifaddr *out, size_t max)
{
    espix_ifinfo_t *ifs = calloc(ESPIX_NET_IF_LIMIT, sizeof(*ifs));
    if (ifs == NULL) {
        return 0;
    }
    const size_t n = espix_net_iflist(ifs, ESPIX_NET_IF_LIMIT);
    size_t k = 0;
    for (size_t i = 0; i < n && k < max; i++) {
        const espix_ifinfo_t *f = &ifs[i];
        const uint32_t flags = (f->up ? IFF_UP | IFF_RUNNING : 0) |
                               ((f->kind == ESPIX_IF_LO) ? IFF_LOOPBACK : IFF_BROADCAST | IFF_MULTICAST);
        const size_t first = k;
        if (f->has_addr && f->ip != 0) {
            struct espix_ifaddr *e = &out[k++];
            *e = (struct espix_ifaddr){ .flags = flags };
            strlcpy(e->name, f->name, sizeof(e->name));
            const struct sockaddr_in a = { .sin_len = sizeof(a), .sin_family = AF_INET, .sin_addr.s_addr = f->ip };
            const struct sockaddr_in m = { .sin_len = sizeof(m), .sin_family = AF_INET, .sin_addr.s_addr = f->netmask };
            memcpy(e->addr, &a, sizeof(a));
            memcpy(e->netmask, &m, sizeof(m));
        }
        for (uint8_t j = 0; j < f->n_ip6 && k < max; j++) {
            struct sockaddr_in6 a = { .sin6_len = sizeof(a), .sin6_family = AF_INET6, .sin6_scope_id = (uint32_t)f->index };
            if (inet_pton(AF_INET6, f->ip6[j], &a.sin6_addr) != 1) {
                continue;
            }
            struct sockaddr_in6 m = { .sin6_len = sizeof(m), .sin6_family = AF_INET6 };
            for (int b = 0; b < f->ip6_prefix[j] && b < 128; b++) {
                m.sin6_addr.s6_addr[b / 8] |= (uint8_t)(0x80 >> (b % 8));
            }
            struct espix_ifaddr *e = &out[k++];
            *e = (struct espix_ifaddr){ .flags = flags };
            strlcpy(e->name, f->name, sizeof(e->name));
            memcpy(e->addr, &a, sizeof(a));
            memcpy(e->netmask, &m, sizeof(m));
        }
        if (k == first && k < max) {
            out[k] = (struct espix_ifaddr){ .flags = flags };
            strlcpy(out[k++].name, f->name, sizeof(out[0].name));
        }
    }
    free(ifs);
    return k;
}

void espix_net_abi_register(void)
{
    espix_proc_set_ifaddrs(net_ifaddrs);
}
