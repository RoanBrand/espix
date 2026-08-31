/*
 * espix networking core: init order, the interface table, hostname, and the
 * read-only views the commands render.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "lwip/netdb.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "espix_fs.h"
#include "espix_kernel.h"
#include "espix_net.h"
#include "espix_net_priv.h"

#define TAG "net"

#define HOSTNAME_PATH "/etc/hostname"

static espix_if_entry_t s_ifs[ESPIX_IF_MAX];
static size_t           s_if_count;
static char             s_hostname[ESPIX_HOSTNAME_MAX];
static bool             s_inited;

/* ------------------------------------------------------------------ */
/* Formatting helpers                                                  */
/* ------------------------------------------------------------------ */

const char *espix_net_ip4str(uint32_t addr, char *buf, size_t len)
{
    /* Same byte order as esp_ip4_addr_t.addr: octets ascending from the LSB. */
    snprintf(buf, len, "%u.%u.%u.%u",
             (unsigned)(addr & 0xff),
             (unsigned)((addr >> 8) & 0xff),
             (unsigned)((addr >> 16) & 0xff),
             (unsigned)((addr >> 24) & 0xff));
    return buf;
}

int espix_net_prefix_len(uint32_t netmask)
{
    int bits = 0;
    for (int i = 0; i < 32; i++) {
        if (netmask & (1u << i)) {
            bits++;
        }
    }
    return bits;
}

/* ------------------------------------------------------------------ */
/* Interface table                                                     */
/* ------------------------------------------------------------------ */

esp_err_t espix_net_register_if(const char *name, espix_if_kind_t kind,
                                esp_netif_t *netif)
{
    if (name == NULL || s_if_count >= ESPIX_IF_MAX) {
        return ESP_ERR_NO_MEM;
    }

    espix_if_entry_t *e = &s_ifs[s_if_count];
    strlcpy(e->name, name, sizeof(e->name));
    e->kind  = kind;
    e->netif = netif;
    s_if_count++;

    /*
     * Apply the hostname here rather than in the WiFi path. lwip stores it per
     * netif, so a device with several interfaces otherwise ends up announcing
     * different names on each — which is exactly the arduino-esp32 behaviour
     * where WiFi reports esp32s3-xxxxxx and USB-NCM reports "espressif".
     */
    if (netif != NULL && s_hostname[0] != '\0') {
        esp_netif_set_hostname(netif, s_hostname);
    }

    espix_klog(ESPIX_KLOG_INFO, TAG, "%s: registered", e->name);
    return ESP_OK;
}

const espix_if_entry_t *espix_net_find_if(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < s_if_count; i++) {
        if (strcmp(s_ifs[i].name, name) == 0) {
            return &s_ifs[i];
        }
    }
    return NULL;
}

const char *espix_net_name_of(esp_netif_t *netif)
{
    for (size_t i = 0; i < s_if_count; i++) {
        if (s_ifs[i].netif == netif) {
            return s_ifs[i].name;
        }
    }
    return NULL;
}

static void fill_ifinfo(const espix_if_entry_t *e, espix_ifinfo_t *out)
{
    memset(out, 0, sizeof(*out));
    strlcpy(out->name, e->name, sizeof(out->name));
    out->kind = e->kind;

    if (e->kind == ESPIX_IF_LO) {
        /* Synthesised: lwip's loopback is per-interface, not a netif of its
         * own, so there is nothing to query. Reported for familiarity. */
        out->up       = true;
        out->has_addr = true;
        out->ip       = 0x0100007f;   /* 127.0.0.1 */
        out->netmask  = 0x000000ff;   /* /8 */
        out->mtu      = 65535;
        out->index    = 1;
        return;
    }

    if (e->netif == NULL) {
        return;
    }

    out->up    = esp_netif_is_netif_up(e->netif);
    out->index = esp_netif_get_netif_impl_index(e->netif);

    uint16_t mtu = 0;
    if (esp_netif_get_mtu(e->netif, &mtu) == ESP_OK) {
        out->mtu = mtu;
    }

    if (esp_netif_get_mac(e->netif, out->mac) == ESP_OK) {
        out->has_mac = true;
    }

    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(e->netif, &ip) == ESP_OK && ip.ip.addr != 0) {
        out->has_addr = true;
        out->ip       = ip.ip.addr;
        out->netmask  = ip.netmask.addr;
        out->gw       = ip.gw.addr;
    }
}

size_t espix_net_iflist(espix_ifinfo_t *out, size_t n)
{
    size_t count = 0;
    for (size_t i = 0; i < s_if_count && count < n; i++) {
        fill_ifinfo(&s_ifs[i], &out[count++]);
    }
    return count;
}

esp_err_t espix_net_ifinfo(const char *name, espix_ifinfo_t *out)
{
    const espix_if_entry_t *e = espix_net_find_if(name);
    if (e == NULL || out == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    fill_ifinfo(e, out);
    return ESP_OK;
}

bool espix_net_default_route(char *ifname, size_t len, uint32_t *gw)
{
    esp_netif_t *def = esp_netif_get_default_netif();
    if (def == NULL) {
        return false;
    }

    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(def, &ip) != ESP_OK || ip.gw.addr == 0) {
        return false;
    }

    const char *name = espix_net_name_of(def);
    if (ifname != NULL) {
        strlcpy(ifname, name ? name : "?", len);
    }
    if (gw != NULL) {
        *gw = ip.gw.addr;
    }
    return true;
}

size_t espix_net_dns(uint32_t *out, size_t n)
{
    esp_netif_t *def = esp_netif_get_default_netif();
    if (def == NULL || out == NULL) {
        return 0;
    }

    static const esp_netif_dns_type_t types[] = {
        ESP_NETIF_DNS_MAIN, ESP_NETIF_DNS_BACKUP,
    };

    size_t count = 0;
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]) && count < n; i++) {
        esp_netif_dns_info_t dns;
        if (esp_netif_get_dns_info(def, types[i], &dns) == ESP_OK &&
            dns.ip.u_addr.ip4.addr != 0) {
            out[count++] = dns.ip.u_addr.ip4.addr;
        }
    }
    return count;
}

esp_err_t espix_net_resolve(const char *host, uint32_t *out_ip)
{
    if (host == NULL || out_ip == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const struct addrinfo hints = {
        .ai_family   = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;

    if (getaddrinfo(host, NULL, &hints, &res) != 0 || res == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    *out_ip = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(res);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Hostname                                                            */
/* ------------------------------------------------------------------ */

const char *espix_net_hostname(void)
{
    return s_hostname;
}

static void generate_hostname(char *out, size_t len)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    /* Same shape arduino-esp32 uses (target + last 3 MAC bytes), but lowercase:
     * DNS is case-insensitive and lowercase is what router UIs display. */
    snprintf(out, len, "%s-%02x%02x%02x", CONFIG_IDF_TARGET,
             mac[3], mac[4], mac[5]);
}

static void load_hostname(void)
{
    FILE *f = fopen(HOSTNAME_PATH, "r");
    if (f != NULL) {
        char line[ESPIX_HOSTNAME_MAX] = {0};
        if (fgets(line, sizeof(line), f) != NULL) {
            line[strcspn(line, "\r\n")] = '\0';
            if (line[0] != '\0') {
                strlcpy(s_hostname, line, sizeof(s_hostname));
            }
        }
        fclose(f);
    }

    if (s_hostname[0] == '\0') {
        generate_hostname(s_hostname, sizeof(s_hostname));
        /* Write it out so it is visible and editable from here on. */
        FILE *w = fopen(HOSTNAME_PATH, "w");
        if (w != NULL) {
            fprintf(w, "%s\n", s_hostname);
            fclose(w);
        } else {
            espix_klog(ESPIX_KLOG_WARN, TAG, "cannot write %s: %s",
                       HOSTNAME_PATH, strerror(errno));
        }
    }

    espix_klog(ESPIX_KLOG_INFO, TAG, "hostname %s", s_hostname);
}

esp_err_t espix_net_set_hostname(const char *name, bool persist)
{
    if (name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(s_hostname, name, sizeof(s_hostname));

    /* Every interface, for the reason in espix_net_register_if(). */
    for (size_t i = 0; i < s_if_count; i++) {
        if (s_ifs[i].netif != NULL) {
            esp_netif_set_hostname(s_ifs[i].netif, s_hostname);
        }
    }

    if (persist) {
        FILE *f = fopen(HOSTNAME_PATH, "w");
        if (f == NULL) {
            return ESP_FAIL;
        }
        fprintf(f, "%s\n", s_hostname);
        fclose(f);
    }

    return ESP_OK;
}

/* ------------------------------------------------------------------ */

esp_err_t espix_net_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    /* esp_wifi keeps calibration and its own state here; this is what finally
     * puts the nvs partition we reserved to use. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "nvs unusable, erasing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "nvs init failed: %s",
                   esp_err_to_name(err));
        return err;
    }

    ESP_ERROR_CHECK(esp_netif_init());

    /* espix_time initialises before this and needs the same loop, so whichever
     * gets there first creates it and the other accepts what it finds. */
    const esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(loop_err);
    }

    /* Before any interface is registered, so each one picks it up. */
    load_hostname();

    espix_net_register_if("lo", ESPIX_IF_LO, NULL);
    espix_net_abi_register();

    s_inited = true;

    /* Never blocks: if there is no config, or the AP is unreachable, the shell
     * still has to come up. */
    err = espix_net_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "wifi start failed: %s",
                   esp_err_to_name(err));
    }

    return ESP_OK;
}
