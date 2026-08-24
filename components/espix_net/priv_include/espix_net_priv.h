/* Internal to the espix_net component. */
#pragma once

#include "esp_netif.h"

#include "espix_net.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The interface table. espix owns the naming: an entry is created when we
 * create the underlying netif, so names are deterministic rather than
 * discovered. `lo` is synthesised and has no netif.
 */
#define ESPIX_IF_MAX 4

typedef struct {
    char            name[ESPIX_IF_NAME_MAX];
    espix_if_kind_t kind;
    esp_netif_t    *netif;      /* NULL for synthesised entries */
} espix_if_entry_t;

/*
 * Register a netif under a Linux-style name. This is also where the hostname
 * gets applied — lwip keeps it per-netif, so doing it here means every future
 * interface (eth0, usb0) inherits it without remembering to.
 */
esp_err_t espix_net_register_if(const char *name, espix_if_kind_t kind,
                                esp_netif_t *netif);

/* Look up an entry by espix name, or NULL. */
const espix_if_entry_t *espix_net_find_if(const char *name);

/* Look up the espix name for a netif, or NULL. */
const char *espix_net_name_of(esp_netif_t *netif);

/* wifi.c */
esp_err_t espix_net_wifi_start(void);

/* abi.c: publish the network syscall surface to loadable apps. */
void espix_net_abi_register(void);

/* Config file helpers, shared by net.c and wifi.c. Both return false when the
 * file or key is absent — a missing config is normal, not an error. */
bool espix_net_conf_get(const char *path, const char *key,
                        char *out, size_t len);
esp_err_t espix_net_conf_write_wifi(const char *ssid, const char *psk);

#ifdef __cplusplus
}
#endif
