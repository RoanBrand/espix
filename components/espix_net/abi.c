/*
 * Network syscall surface for loadable apps.
 *
 * The ELF loader ships a symbol table exporting some of lwip — socket, bind,
 * listen, accept, connect, send, recv, htons, htonl — which is why the whole
 * TCP/IP stack is linked whether or not espix uses it. What it does NOT export
 * is name resolution, so without this an app can only reach raw IP addresses.
 *
 * esp_elf_register_symbol() lets espix extend the table at runtime, so the app
 * ABI is something espix owns rather than inherits, with no fork of the
 * component. Everything added here should be considered part of the ABI: once
 * an app links against it, removing it breaks that app.
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

    /* Socket lifecycle the loader's own table omits. */
    ESP_ELFSYM_EXPORT(lwip_close),
    ESP_ELFSYM_EXPORT(lwip_shutdown),
    ESP_ELFSYM_EXPORT(lwip_getsockopt),
    ESP_ELFSYM_EXPORT(lwip_getsockname),
    ESP_ELFSYM_EXPORT(lwip_getpeername),
    ESP_ELFSYM_EXPORT(lwip_select),
    ESP_ELFSYM_EXPORT(lwip_fcntl),

    /* arpa/inet.h presentation conversion. ntohs/ntohl need nothing: they are
     * macros onto lwip_htons/lwip_htonl, which the loader already exports. */
    ESP_ELFSYM_EXPORT(lwip_inet_ntop),
    ESP_ELFSYM_EXPORT(lwip_inet_pton),

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
