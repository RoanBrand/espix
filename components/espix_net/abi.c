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
#include <netdb.h>
#include <sys/socket.h>

#include "esp_elf.h"

#include "espix_kernel.h"
#include "espix_net_priv.h"

#define TAG "abi"

/*
 * Note the lwip_ prefixes. In lwip, getaddrinfo/inet_ntop/ntohs and friends are
 * *macros* over lwip_getaddrinfo/lwip_inet_ntop/lwip_htons, so the symbol an
 * app's object file actually references is the prefixed one. Exporting the
 * unprefixed name would both fail to compile (the macro expands in our own
 * initialiser) and resolve nothing at load time.
 */
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
    ESP_ELFSYM_EXPORT(lwip_send),
    ESP_ELFSYM_EXPORT(lwip_sendto),
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
