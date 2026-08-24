/*
 * espix networking.
 *
 * This is a naming and presentation layer, not a network stack. ESP-IDF's
 * esp_netif already provides the abstraction a Unix user expects — interfaces
 * with addresses, a default route, DHCP, DNS — so espix's job is to give those
 * objects Linux-shaped names (wlan0, eth0, usb0, lo) and expose them through
 * the commands people already know.
 *
 * Addresses are carried as raw uint32 in the same representation esp_netif
 * uses, so this header stays free of esp_netif types and callers do not need
 * to know about them. Use espix_net_ip4str() to format one.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESPIX_IF_NAME_MAX   8
#define ESPIX_SSID_MAX      33      /* 32 + NUL */
#define ESPIX_PSK_MAX       65      /* 64 + NUL */
#define ESPIX_HOSTNAME_MAX  33
#define ESPIX_IP4STR_MAX    16      /* "255.255.255.255" */

typedef enum {
    ESPIX_IF_LO = 0,
    ESPIX_IF_WIFI_STA,
    ESPIX_IF_WIFI_AP,
    ESPIX_IF_ETH,
    ESPIX_IF_USB,
} espix_if_kind_t;

typedef struct {
    char            name[ESPIX_IF_NAME_MAX];
    espix_if_kind_t kind;
    int             index;          /* ifindex, as lwip sees it */
    uint16_t        mtu;
    bool            up;
    bool            has_mac;
    uint8_t         mac[6];
    bool            has_addr;
    uint32_t        ip;
    uint32_t        netmask;
    uint32_t        gw;
} espix_ifinfo_t;

/*
 * Brings up NVS, the event loop, esp_netif and the interface table, resolves
 * the hostname, and starts WiFi if /etc/wifi.conf names a network.
 *
 * Returns as soon as the bring-up is *started*: association and DHCP happen on
 * the event loop. A missing config or an unreachable AP must never delay the
 * shell, so this never blocks on the network.
 */
esp_err_t espix_net_init(void);

/* Interfaces, in table order. Returns how many were written. */
size_t    espix_net_iflist(espix_ifinfo_t *out, size_t n);
esp_err_t espix_net_ifinfo(const char *name, espix_ifinfo_t *out);

/* Default route, i.e. whichever interface esp_netif considers default. */
bool      espix_net_default_route(char *ifname, size_t len, uint32_t *gw);

/* Nameservers of the default interface, as DHCP supplied them. */
size_t    espix_net_dns(uint32_t *out, size_t n);

const char *espix_net_hostname(void);

/*
 * Apply `name` to every interface — the hostname is per-netif in lwip, so
 * setting it once globally is not a thing. `persist` also rewrites
 * /etc/hostname.
 */
esp_err_t espix_net_set_hostname(const char *name, bool persist);

/* Format an address in the representation used by this header. */
const char *espix_net_ip4str(uint32_t addr, char *buf, size_t len);

/* Contiguous-netmask -> prefix length. 24 for 255.255.255.0. */
int espix_net_prefix_len(uint32_t netmask);

/* ------------------------------------------------------------------ */
/* WiFi station                                                        */
/* ------------------------------------------------------------------ */

typedef enum {
    ESPIX_WIFI_OFF = 0,     /* driver not started */
    ESPIX_WIFI_IDLE,        /* started, not associated */
    ESPIX_WIFI_CONNECTING,
    ESPIX_WIFI_CONNECTED,   /* associated; may still be waiting on DHCP */
} espix_wifi_state_t;

typedef struct {
    char     ssid[ESPIX_SSID_MAX];
    uint8_t  bssid[6];
    int8_t   rssi;
    uint8_t  channel;
    bool     secure;
} espix_ap_t;

typedef struct {
    espix_wifi_state_t state;
    char               ssid[ESPIX_SSID_MAX];
    uint8_t            bssid[6];
    int8_t             rssi;
    uint8_t            channel;
    unsigned           retries;

    /*
     * Why we stopped, when state is IDLE after failures. Without this an
     * abandoned connect is indistinguishable from never having tried.
     * `last_reason` is a wifi_err_reason_t, kept as int so this header does
     * not drag in esp_wifi.
     */
    bool               gave_up;
    int                last_reason;
    unsigned           retry_delay_ms;   /* 0 when no retry is pending */
} espix_wifi_status_t;

/*
 * Connect, and write the credentials to /etc/wifi.conf so the config is always
 * inspectable with `cat`. Pass NULL for both to re-read the file instead.
 */
esp_err_t espix_net_wifi_connect(const char *ssid, const char *psk);
esp_err_t espix_net_wifi_disconnect(void);

/* Blocking active scan. */
esp_err_t espix_net_wifi_scan(espix_ap_t *out, size_t n, size_t *found);
esp_err_t espix_net_wifi_status(espix_wifi_status_t *out);

/* Resolve a hostname or dotted-quad to an address. */
esp_err_t espix_net_resolve(const char *host, uint32_t *out_ip);

#ifdef __cplusplus
}
#endif
