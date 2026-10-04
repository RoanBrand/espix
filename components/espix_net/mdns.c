/*
 * mDNS/DNS-SD: <hostname>.local, so a client reaches the board without an
 * address.
 *
 * The responder is espressif/mdns rather than anything written here: probing
 * and conflict resolution, the cache-flush bit, per-interface sockets and
 * DNS-SD browsing are the parts of mDNS that look simple and are not. What
 * this file decides is what is advertised -- the name espix already keeps in
 * /etc/hostname, and the door that already exists: SSH, and its sftp
 * subsystem, on 22. No _http._tcp until there is a web UI to point it at.
 *
 * It starts on the address event rather than at init, because the responder
 * needs an interface with an address; the call is idempotent, since the event
 * fires again on every address change.
 */

#include "esp_event.h"
#include "mdns.h"

#include <string.h>
#include <strings.h>

#include "espix_fs.h"
#include "espix_kernel.h"
#include "espix_net.h"

static const char *TAG = "espix:mdns";

#define MDNS_CONF_PATH "/etc/mdns.conf"

/*
 * On unless /etc/mdns.conf says otherwise: the Raspberry Pi bargain, where the
 * name works out of the box and one line turns the announcement off. Read at
 * start, so a change wants a reload rather than a reflash.
 */
static bool mdns_wanted(void)
{
    char v[8] = {0};

    if (espix_fs_conf_get(MDNS_CONF_PATH, "enabled", v, sizeof(v))) {
        return !(strcasecmp(v, "no") == 0 || strcasecmp(v, "off") == 0 ||
                 strcmp(v, "0") == 0);
    }
    return true;
}

static bool s_up;
static esp_event_handler_instance_t s_ip_handler;

esp_err_t espix_net_mdns_start(void)
{
    if (s_up) {
        return ESP_OK;
    }

    if (!mdns_wanted()) {
        espix_klog(ESPIX_KLOG_INFO, TAG, "disabled by %s", MDNS_CONF_PATH);
        s_up = true;                    /* decided once; the event fires often */
        return ESP_OK;
    }

    esp_err_t e = mdns_init();
    if (e != ESP_OK) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "mdns_init: %s", esp_err_to_name(e));
        return e;
    }

    const char *host = espix_net_hostname();
    if (host == NULL || host[0] == 0) {
        host = "espix";
    }

    e = mdns_hostname_set(host);
    if (e != ESP_OK) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "hostname: %s", esp_err_to_name(e));
        return e;
    }

    (void)mdns_instance_name_set("espix");
    (void)mdns_service_add(NULL, "_ssh", "_tcp", 22, NULL, 0);
    (void)mdns_service_add(NULL, "_sftp-ssh", "_tcp", 22, NULL, 0);

    s_up = true;
    espix_klog(ESPIX_KLOG_INFO, TAG, "advertising %s.local (_ssh, _sftp-ssh)",
               host);
    return ESP_OK;
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;

    if (id == IP_EVENT_STA_GOT_IP || id == IP_EVENT_ETH_GOT_IP) {
        (void)espix_net_mdns_start();
    }
}

esp_err_t espix_net_mdns_init(void)
{
    return esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                               on_ip_event, NULL,
                                               &s_ip_handler);
}
