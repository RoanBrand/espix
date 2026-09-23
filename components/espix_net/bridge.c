/*
 * L2 bridging: br0 over lwIP's 802.1D bridge, via esp_netif's bridge glue.
 *
 * The bridge owns the address and, if it serves, the DHCP server; its ports
 * carry frames and have neither. That is how Linux does it -- br-lan holds the
 * address, the AP and the wire are ports -- and on ESP it is also forced: an
 * esp_netif's flags and ip_info are fixed when it is created, so a port has to
 * be created as a port. The port set therefore comes from /etc/bridge.conf at
 * boot and changes are applied by rebooting; membership is setup, not a
 * hot-plug, so nothing is rebuilt under live clients.
 *
 * The station (wlan0) is never a port: 802.11 frames carry three addresses, and
 * a bridge needs the fourth to carry both peer addresses across the link.
 */

#include <stdio.h>
#include <string.h>

#include "esp_netif.h"
#include "esp_netif_defaults.h"
#include "esp_netif_br_glue.h"
#include "sdkconfig.h"

#include "espix_fs.h"
#include "espix_kernel.h"
#include "espix_net.h"
#include "espix_net_priv.h"

#define TAG "bridge"

#define BR_NAME      "br0"
#define BR_CONF_PATH "/etc/bridge.conf"
#define BR_MAX_PORTS 4

static esp_netif_t                *s_br;
static esp_netif_br_glue_handle_t  s_glue;
static char                        s_ports[BR_MAX_PORTS][ESPIX_IF_NAME_MAX];
static size_t                      s_port_count;
static bool                        s_server;

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

static size_t conf_ports(char out[][ESPIX_IF_NAME_MAX], size_t n)
{
    char   buf[128] = {0};
    size_t count    = 0;

    if (!espix_fs_conf_get(BR_CONF_PATH, "ports", buf, sizeof(buf))) {
        return 0;
    }

    char *p = buf;
    while (p != NULL && *p != '\0' && count < n) {
        char *comma = strchr(p, ',');
        if (comma != NULL) {
            *comma = '\0';
        }
        strlcpy(out[count], p, ESPIX_IF_NAME_MAX);
        count++;
        p = (comma != NULL) ? comma + 1 : NULL;
    }
    return count;
}

static bool conf_server(void)
{
    char buf[16] = {0};
    if (!espix_fs_conf_get(BR_CONF_PATH, "addr", buf, sizeof(buf))) {
        return false;               /* client: the "extend the LAN" case */
    }
    return strcmp(buf, "server") == 0;
}

static esp_err_t write_conf(const char ports[][ESPIX_IF_NAME_MAX], size_t n,
                            bool server)
{
    FILE *f = fopen(BR_CONF_PATH, "w");
    if (f == NULL) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "cannot write %s", BR_CONF_PATH);
        return ESP_FAIL;
    }

    fprintf(f, "# espix bridge configuration -- applied at boot\n");
    fprintf(f, "ports=");
    for (size_t i = 0; i < n; i++) {
        fprintf(f, "%s%s", i ? "," : "", ports[i]);
    }
    fprintf(f, "\naddr=%s\n", server ? "server" : "client");
    fclose(f);

    (void)espix_fs_ensure_mode(BR_CONF_PATH, 0600);
    return ESP_OK;
}

bool espix_net_bridge_wants(const char *name)
{
    char ports[BR_MAX_PORTS][ESPIX_IF_NAME_MAX];
    const size_t n = conf_ports(ports, BR_MAX_PORTS);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(ports[i], name) == 0) {
            return true;
        }
    }
    return false;
}

esp_err_t espix_net_bridge_conf_add(const char *port)
{
    char ports[BR_MAX_PORTS][ESPIX_IF_NAME_MAX];
    size_t n = conf_ports(ports, BR_MAX_PORTS);

    for (size_t i = 0; i < n; i++) {
        if (strcmp(ports[i], port) == 0) {
            return ESP_OK;
        }
    }
    if (n >= BR_MAX_PORTS) {
        return ESP_ERR_NO_MEM;
    }
    strlcpy(ports[n++], port, ESPIX_IF_NAME_MAX);
    return write_conf(ports, n, conf_server());
}

esp_err_t espix_net_bridge_conf_del(const char *port)
{
    char ports[BR_MAX_PORTS][ESPIX_IF_NAME_MAX];
    const size_t n = conf_ports(ports, BR_MAX_PORTS);
    size_t w = 0;

    for (size_t i = 0; i < n; i++) {
        if (strcmp(ports[i], port) != 0) {
            strlcpy(ports[w++], ports[i], ESPIX_IF_NAME_MAX);
        }
    }
    return write_conf(ports, w, conf_server());
}

esp_err_t espix_net_bridge_conf_addr(bool server)
{
    char ports[BR_MAX_PORTS][ESPIX_IF_NAME_MAX];
    const size_t n = conf_ports(ports, BR_MAX_PORTS);
    return write_conf(ports, n, server);
}

/* ------------------------------------------------------------------ */
/* Apply                                                               */
/* ------------------------------------------------------------------ */

esp_err_t espix_net_bridge_apply(void)
{
#if !CONFIG_ESP_NETIF_BRIDGE_EN
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_br != NULL) {
        return ESP_OK;
    }

    char want[BR_MAX_PORTS][ESPIX_IF_NAME_MAX];
    const size_t want_n = conf_ports(want, BR_MAX_PORTS);
    if (want_n == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    /*
     * Resolve the ports first. A bridge with nothing to attach is not a bridge,
     * and this way one is never built only to be torn down.
     */
    esp_netif_t    *nets[BR_MAX_PORTS];
    espix_if_kind_t kinds[BR_MAX_PORTS];
    size_t          n = 0;
    uint8_t         mac[6] = {0};
    bool            have_mac = false;

    for (size_t i = 0; i < want_n && n < BR_MAX_PORTS; i++) {
        const espix_if_entry_t *e = espix_net_find_if(want[i]);
        if (e == NULL || e->netif == NULL) {
            espix_klog(ESPIX_KLOG_WARN, TAG, "%s: not present in this build",
                       want[i]);
            continue;
        }
        if (e->kind == ESPIX_IF_WIFI_STA) {
            espix_klog(ESPIX_KLOG_WARN, TAG,
                       "%s: a station cannot be a port (802.11 three-address "
                       "frames); use the L2 forwarder instead", want[i]);
            continue;
        }
        if (e->kind == ESPIX_IF_LO || e->kind == ESPIX_IF_BRIDGE) {
            continue;
        }

        nets[n]  = e->netif;
        kinds[n] = e->kind;
        strlcpy(s_ports[n], want[i], ESPIX_IF_NAME_MAX);
        n++;

        if (!have_mac && esp_netif_get_mac(e->netif, mac) == ESP_OK) {
            have_mac = true;
        }
    }

    if (n == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    s_port_count = n;
    s_server     = conf_server();

    /* The bridge's MAC is a port's, so the DHCP lease it takes is the lease
     * that port would have had. */
    esp_netif_inherent_config_t base =
        s_server ? (esp_netif_inherent_config_t)ESP_NETIF_INHERENT_DEFAULT_BR_DHCPS()
                 : (esp_netif_inherent_config_t)ESP_NETIF_INHERENT_DEFAULT_BR();
    bridgeif_config_t brcfg = {
        .max_fdb_dyn_entries = 16,
        .max_fdb_sta_entries = 4,
        .max_ports           = BR_MAX_PORTS + 1,
    };
    base.bridge_info = &brcfg;
    if (have_mac) {
        memcpy(base.mac, mac, sizeof(mac));
    }

    const esp_netif_config_t cfg = {
        .base  = &base,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_BR,
    };
    s_br = esp_netif_new(&cfg);
    if (s_br == NULL) {
        return ESP_FAIL;
    }

    s_glue = esp_netif_br_glue_new();
    if (s_glue == NULL) {
        esp_netif_destroy(s_br);
        s_br = NULL;
        return ESP_FAIL;
    }

    (void)kinds;

    const esp_err_t aerr = esp_netif_attach(s_br, s_glue);
    if (aerr != ESP_OK) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "attach: %s", esp_err_to_name(aerr));
        return aerr;
    }

    /*
     * A bridge has no driver of its own, and its ports were already up when
     * it was built, so nothing else will start it. IDF's example builds the
     * bridge before starting the Ethernet drivers, which is what delivers
     * the events; espix builds it after them, so it starts it here. In client
     * mode this also starts the DHCP client.
     */
    esp_netif_action_start(s_br, NULL, 0, NULL);

    /*
     * The lwIP bridge exists only once the netif has started, so the ports
     * are added after it. The glue would normally do this from port events,
     * but espix's ports were already up when the bridge was built, so those
     * events are not coming -- and registering the glue's handlers would only
     * add the ports a second time.
     */
    for (size_t i = 0; i < s_port_count; i++) {
        const esp_err_t perr = esp_netif_bridge_add_port(s_br, nets[i]);
        if (perr != ESP_OK) {
            espix_klog(ESPIX_KLOG_WARN, TAG, "%s: add port: %s",
                       s_ports[i], esp_err_to_name(perr));
        }
    }

    esp_netif_action_connected(s_br, NULL, 0, NULL);

    espix_net_register_if(BR_NAME, ESPIX_IF_BRIDGE, s_br);
    espix_klog(ESPIX_KLOG_INFO, TAG, "%s up: %zu port(s), address %s",
               BR_NAME, s_port_count, s_server ? "server" : "client");
    return ESP_OK;
#endif
}

size_t espix_net_bridge_portlist(char (*out)[ESPIX_IF_NAME_MAX], size_t n)
{
    size_t count = 0;
    for (size_t i = 0; i < s_port_count && count < n; i++) {
        strlcpy(out[count++], s_ports[i], ESPIX_IF_NAME_MAX);
    }
    return count;
}

bool espix_net_bridge_active(void)
{
    return s_br != NULL;
}

bool espix_net_bridge_server(void)
{
    return s_server;
}
