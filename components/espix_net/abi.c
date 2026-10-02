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
#include <netdb.h>
#include <signal.h>
#include <sys/socket.h>

#include "esp_elf.h"

#include "espix_kernel.h"
#include "espix_net_priv.h"
#include "espix_proc.h"

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

static esp_elf_symbol_table_t s_net_syms[] = {

    /* netdb.h — the actual gap. Without these, apps cannot use hostnames. */
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

    ESP_ELFSYM_END
};

void espix_net_abi_register(void)
{
    if (esp_elf_register_symbol(s_net_syms) != 0) {
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "could not publish network symbols to apps");
        return;
    }

    espix_klog(ESPIX_KLOG_INFO, TAG, "network syscalls published to apps");
}
