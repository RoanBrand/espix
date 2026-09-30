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
    ESPIX_IF_BRIDGE,
} espix_if_kind_t;

typedef struct {
    char            name[ESPIX_IF_NAME_MAX];
    espix_if_kind_t kind;
    int             index;          /* ifindex, as lwip sees it */
    uint16_t        mtu;
    bool            up;
    bool            napt;           /* masqueraded behind the default route */
    bool            has_mac;
    uint8_t         mac[6];
    bool            has_addr;
    uint32_t        ip;
    uint32_t        netmask;
    uint32_t        gw;

    /*
     * Ethernet only: what the PHY negotiated. Zero speed means no carrier, so
     * there is one sentinel rather than a separate "has_link" flag. Every other
     * kind leaves these zero and has no such thing to report.
     */
    uint16_t        link_mbps;
    bool            link_full;
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

/*
 * Routing. NAPT on an interface means packets from it are masqueraded behind
 * whichever interface carries the default route, so an AP's clients (or a
 * wired client) reach the uplink under one address. ESP_ERR_NOT_SUPPORTED
 * when ESPIX_NET_ROUTER is off, ESP_ERR_NOT_FOUND for an unknown interface.
 */
esp_err_t espix_net_napt(const char *name, bool enable);
bool      espix_net_napt_enabled(const char *name);

/*
 * L2 bridge. br0 is created at boot from /etc/bridge.conf; these apply that
 * configuration, report it, and edit it. `add`/`del`/`addr` are boot-time,
 * like `usb mode`: a port must have been created as a port, so membership
 * changes reboot rather than rebuild a live bridge.
 */
esp_err_t espix_net_bridge_apply(void);
bool      espix_net_bridge_active(void);
bool      espix_net_bridge_server(void);
size_t    espix_net_bridge_portlist(char (*out)[ESPIX_IF_NAME_MAX], size_t n);
esp_err_t espix_net_bridge_conf_add(const char *port);
esp_err_t espix_net_bridge_conf_del(const char *port);
esp_err_t espix_net_bridge_conf_addr(bool server);

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

/*
 * Whether the station sleeps between beacons.
 *
 * espix's own enum rather than wifi_ps_type_t, for the reason this header
 * already gives about `last_reason` below: it does not drag in esp_wifi. That
 * also keeps the numbering ours, so a renderer cannot quietly depend on IDF's.
 *
 * Until it is set, this reports IDF's default of MIN_MODEM -- which is worth
 * being able to see, because it costs a beacon interval of latency on every
 * exchange and nothing else in the system said so. `wifi status` prints it, and
 * `wifi ps` chooses it; see espix_wifi_set_ps() for what the choice is worth.
 */
typedef enum {
    ESPIX_WIFI_PS_UNKNOWN = 0,  /* driver not started, or the query failed */
    ESPIX_WIFI_PS_NONE,         /* radio stays on */
    ESPIX_WIFI_PS_MIN_MODEM,    /* wakes per DTIM to hear the beacon */
    ESPIX_WIFI_PS_MAX_MODEM,    /* as above, plus a longer listen interval */
} espix_wifi_ps_t;

/*
 * Set the station's sleep behaviour, or ESP_ERR_INVALID_STATE before the driver
 * is up.
 *
 * The default is not free: on MIN_MODEM every exchange can wait for the AP's
 * beacon, which measured as 58ms average and 28ms standard deviation pinging
 * this board, against 4ms and 1.7ms on the cable. Nothing is wrong with the
 * driver -- that is the mode doing exactly what it says -- so the answer is to
 * let it be chosen. Not persisted: a board that wants it off every boot has
 * nowhere to say so yet.
 */
esp_err_t espix_wifi_set_ps(espix_wifi_ps_t mode);

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

    espix_wifi_ps_t    ps;               /* sleep behaviour; see above */
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

/* ------------------------------------------------------------------ */
/* WiFi access point (wlan1)                                           */
/* ------------------------------------------------------------------ */

/*
 * The AP is a separate interface from the station because the two can run at
 * once (APSTA): its clients are NATed out the uplink, which is routing, not a
 * bridge. `started` is the AP being up, not the netif existing -- wlan1 keeps
 * its name when stopped.
 */
typedef struct {
    bool     started;
    bool     napt;                  /* masqueraded out the default route */
    char     ssid[ESPIX_SSID_MAX];
    uint8_t  channel;
    uint32_t ip;
    uint32_t netmask;
    unsigned clients;
} espix_wifi_ap_status_t;

/* No arguments re-reads ap.ssid/ap.psk/ap.channel from /etc/wifi.conf. */
esp_err_t espix_net_wifi_ap_start(const char *ssid, const char *psk, uint8_t channel);
esp_err_t espix_net_wifi_ap_stop(void);
void      espix_net_wifi_ap_status(espix_wifi_ap_status_t *out);

/* ------------------------------------------------------------------ */
/* USB-NCM (usb0)                                                      */
/* ------------------------------------------------------------------ */

typedef enum {
    ESPIX_USB_MODE_SERVER = 0,  /* espix hands the computer an address */
    ESPIX_USB_MODE_CLIENT,      /* espix asks the computer's network for one */
} espix_usb_mode_t;

typedef struct {
    bool             built;     /* compiled in at all */
    bool             started;   /* the interface exists */
    bool             attached;  /* a host has enumerated the link */
    espix_usb_mode_t mode;
    bool             has_addr;
    uint32_t         ip;
    uint32_t         netmask;
} espix_usb_status_t;

/*
 * Always answers, even when USB-NCM is not compiled in -- `built` is then false
 * and the rest is zero, so `usb status` can say so rather than the command
 * vanishing from a build and leaving the user to guess why.
 */
void espix_net_usb_status(espix_usb_status_t *out);

/*
 * Write the mode to /etc/usb.conf. Takes effect at the next boot: the DHCP
 * server and client are different netif flags, fixed when the interface is
 * created, so switching means recreating it -- which would race whatever is
 * in flight, for a setting nobody changes twice in a day.
 */
esp_err_t espix_net_conf_write_usb(espix_usb_mode_t mode);

/* Resolve a hostname or dotted-quad to an address. */
esp_err_t espix_net_resolve(const char *host, uint32_t *out_ip);

/* ------------------------------------------------------------------ */
/* Fetching a file over HTTP(S)                                        */
/* ------------------------------------------------------------------ */

/*
 * There is one of these on purpose. `upgrade` installs a kernel through it and
 * `fetch` downloads everything else, because underneath they are the same
 * operation; a second copy would be a second thing to fix when the redirect or
 * TLS rules change. It is also why no app gets a network handle -- what a
 * program needs off the network it declares in a manifest, and
 * espix_appdata_ensure() is what acts on that.
 *
 * The bytes land in <path>.part and are renamed on success, so an interrupted
 * transfer never leaves a file that looks whole. Room is checked before the
 * first byte and again against the server's Content-Length: this filesystem
 * also holds the kernel, and filling it is not a failure that ends cleanly.
 */
typedef void (*espix_fetch_progress_fn)(void *ctx, const char *path,
                                         size_t done, size_t total);

typedef enum {
    ESPIX_FETCH_OK = 0,
    ESPIX_FETCH_NO_ROOM,    /* larger than the free space */
    ESPIX_FETCH_NO_NET,     /* DNS, connect, TLS, or a non-200 answer */
    ESPIX_FETCH_IO,         /* could not create, write or rename */
    ESPIX_FETCH_BAD_HASH,   /* the bytes do not match expect_sha256 */
    ESPIX_FETCH_ARG,
} espix_fetch_status_t;

typedef struct {
    size_t written;         /* bytes stored, on ESPIX_FETCH_OK */
    size_t free_now;        /* room on / when the transfer started */
    size_t need;            /* bytes the server said, 0 if it did not say */
    int    http_status;     /* the server's answer, 0 if there was none */
    int    err;             /* why an ESPIX_FETCH_IO failed, 0 otherwise */
} espix_fetch_info_t;

/*
 * Download `url` into `path`. `expect_sha256` is 64 hex characters, or NULL;
 * a mismatch discards the file rather than leaving it. `progress` is called as
 * the body arrives and may be NULL; `total` is 0 when the server sent no
 * Content-Length. Both output pointers may be NULL.
 */
espix_fetch_status_t espix_net_fetch(const char *url, const char *path,
                                     const char *expect_sha256,
                                     espix_fetch_progress_fn progress, void *ctx,
                                     espix_fetch_info_t *info);

#ifdef __cplusplus
}
#endif
