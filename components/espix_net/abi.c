/*
 * Network syscall surface for loadable apps.
 *
 * Until R-P3.1 the ELF loader's own IDF table exported half of lwip — socket,
 * bind, listen, accept, connect, send, recv, htons, htonl — searched before
 * anything espix registered. That table is off now, so this one is the whole
 * lwIP surface: the socket half that used to be below, and the lifecycle and
 * name-resolution half it never had.
 *
 * esp_elf_register_symbol() lets espix publish the ABI itself, with no fork of
 * the component. Everything here is part of it: once an app links against a
 * name, removing it breaks that app. Referencing these names is also what keeps
 * the stack linked -- nothing else in the firmware would have a reason to if
 * espix does not use it.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_elf.h"

#include "espix_gai.h"
#include "espix_kernel.h"
#include "espix_net.h"
#include "espix_net_priv.h"
#include "espix_proc.h"
#include "espix_syscall.h"

#define TAG "abi"

/*
 * Note the lwip_ prefixes. In lwip, getaddrinfo/inet_ntop/ntohs and friends are
 * *macros* over lwip_getaddrinfo/lwip_inet_ntop/lwip_htons, so the symbol an
 * app's object file actually references is the prefixed one. Exporting the
 * unprefixed name would both fail to compile (the macro expands in our own
 * initialiser) and resolve nothing at load time.
 */
/*
 * SIGPIPE on a send whose connection is gone.
 *
 * POSIX raises SIGPIPE when a write to a socket (or pipe) finds the reading end
 * closed; with SIGPIPE ignored or blocked, the write returns EPIPE instead.
 * lwIP has no such signal and does not even produce EPIPE: it maps ERR_CLSD
 * ("connection closed") to ENOTCONN and ERR_RST to ECONNRESET. So the wrapper
 * triggers on the errors that mean the connection can no longer be written to.
 *
 * The rough edge is that lwIP also maps ERR_CONN ("not connected at all") to
 * ENOTCONN, so a send on a socket that was never connected raises SIGPIPE here
 * where Linux would not. The socket API does not carry enough to tell the two
 * apart, and sending on an unconnected socket is a caller bug either way; it is
 * written down rather than guessed around.
 *
 * Delivery is still cooperative (D4): the bit is set and the app reaches it at
 * its next delivery point, so the failed send returns first. On Linux, a
 * process killed by SIGPIPE never sees the error.
 */
static void raise_sigpipe(void)
{
    switch (errno) {
    case EPIPE:
    case ENOTCONN:
    case ECONNRESET:
    case ECONNABORTED:
        break;
    default:
        return;
    }

    const espix_pid_t pid = espix_proc_self_pid();
    if (pid != ESPIX_PID_NONE) {
        (void)espix_proc_signal(pid, SIGPIPE);
    }
}

static ssize_t espix_lwip_send(int fd, const void *data, size_t size, int flags)
{
    const ssize_t n = lwip_send(fd, data, size, flags);
    if (n < 0) {
        raise_sigpipe();
    }
    return n;
}

static ssize_t espix_lwip_sendto(int fd, const void *data, size_t size,
                                 int flags, const struct sockaddr *to,
                                 socklen_t tolen)
{
    const ssize_t n = lwip_sendto(fd, data, size, flags, to, tolen);
    if (n < 0) {
        raise_sigpipe();
    }
    return n;
}

/*
 * gethostname(), from the one source of truth.
 *
 * The name is /etc/hostname, applied to every netif at boot and changed by
 * `hostname`. A program that asks -- Mosquitto does, for the default $SYS
 * topic root -- should see what the shell's `hostname` prints, not a second
 * copy of it.
 *
 * A real global symbol rather than a static behind the table below, because it
 * is not only apps that need it: libsmb2 calls gethostname() from the kernel,
 * inside smb2_serve_port(), and the link would otherwise have no definition to
 * find. Publishing the same address to the ABI table keeps one implementation.
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

/* lwIP has the codes and not the text. */
static const char *espix_gai_strerror_abi(int err)
{
    return espix_gai_strerror(err);
}

/*
 * The interface table as getifaddrs() entries, by the names `ip` prints: one per
 * address, or one with no address for an interface that has none. Both builds
 * reach it: an M-mode app through getifaddrs() below, a U-mode app by syscall.
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

/* One allocation per entry: the node, its name and its two sockaddrs. */
struct ifa_node {
    struct ifaddrs          ifa;
    char                    name[16];
    struct sockaddr_storage addr, netmask;
};

static int espix_getifaddrs(struct ifaddrs **ifap)
{
    struct espix_ifaddr *e = calloc(32, sizeof(*e));
    if (e == NULL) {
        errno = ENOMEM;
        return -1;
    }
    const size_t n = net_ifaddrs(e, 32);
    struct ifaddrs *head = NULL, **tail = &head;
    for (size_t i = 0; i < n; i++) {
        struct ifa_node *x = calloc(1, sizeof(*x));
        if (x == NULL) {
            freeifaddrs(head);
            free(e);
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
    free(e);
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

static esp_elf_symbol_table_t s_net_syms[] = {

    /* The machine's own name, which is not a lookup: /etc/hostname. */
    { "gethostname", (void *)gethostname },

    /* netdb.h — the actual gap. Without these, apps cannot use hostnames. */
    { "gai_strerror", (void *)espix_gai_strerror_abi },
    ESP_ELFSYM_EXPORT(lwip_getaddrinfo),
    ESP_ELFSYM_EXPORT(lwip_freeaddrinfo),
    ESP_ELFSYM_EXPORT(lwip_gethostbyname),

    /* The socket half the loader's own table used to answer for, absorbed so
     * that espix owns the whole lwIP surface in one place (R-P3.1). Together
     * with the lifecycle calls below these are every lwIP name an app reaches. */
    ESP_ELFSYM_EXPORT(lwip_socket),
    ESP_ELFSYM_EXPORT(lwip_bind),
    ESP_ELFSYM_EXPORT(lwip_listen),
    ESP_ELFSYM_EXPORT(lwip_accept),
    ESP_ELFSYM_EXPORT(lwip_connect),
    /* Wrapped, so a send that finds the connection gone raises SIGPIPE. */
    { "lwip_send",   (void *)espix_lwip_send },
    { "lwip_sendto", (void *)espix_lwip_sendto },
    ESP_ELFSYM_EXPORT(lwip_recv),
    ESP_ELFSYM_EXPORT(lwip_recvfrom),
    ESP_ELFSYM_EXPORT(lwip_setsockopt),

    /* Socket lifecycle the loader's own table omits. */
    ESP_ELFSYM_EXPORT(lwip_close),
    ESP_ELFSYM_EXPORT(lwip_shutdown),
    ESP_ELFSYM_EXPORT(lwip_getsockopt),
    ESP_ELFSYM_EXPORT(lwip_getsockname),
    ESP_ELFSYM_EXPORT(lwip_getpeername),
    ESP_ELFSYM_EXPORT(lwip_select),
    ESP_ELFSYM_EXPORT(lwip_fcntl),

    /* arpa/inet.h presentation conversion. ntohs/ntohl need nothing: they are
     * macros onto lwip_htons/lwip_htonl, above. */
    ESP_ELFSYM_EXPORT(lwip_inet_ntop),
    ESP_ELFSYM_EXPORT(lwip_inet_pton),

    /* Byte-order and address helpers the loader used to answer for. */
    ESP_ELFSYM_EXPORT(ipaddr_addr),
    ESP_ELFSYM_EXPORT(lwip_htons),
    ESP_ELFSYM_EXPORT(lwip_htonl),
    ESP_ELFSYM_EXPORT(ip4addr_ntoa),

    /* ifaddrs.h, from the interface table. */
    { "getifaddrs",  (void *)espix_getifaddrs },
    { "freeifaddrs", (void *)freeifaddrs },

    ESP_ELFSYM_END
};

void espix_net_abi_register(void)
{
    espix_proc_set_ifaddrs(net_ifaddrs);

    if (esp_elf_register_symbol(s_net_syms) != 0) {
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "could not publish network symbols to apps");
        return;
    }

    espix_klog(ESPIX_KLOG_INFO, TAG, "network syscalls published to apps");
}
