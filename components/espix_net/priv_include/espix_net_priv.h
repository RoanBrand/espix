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
#define ESPIX_IF_MAX 6

typedef struct {
    char            name[ESPIX_IF_NAME_MAX];
    espix_if_kind_t kind;
    esp_netif_t    *netif;      /* NULL for synthesised entries */
    bool            napt;       /* espix turned NAPT on for this one */
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

/* bridge.c: is `name` configured as a bridge port? Used at netif creation time. */
bool espix_net_bridge_wants(const char *name);

/* usb_ncm.c: bring up usb0. Absent when CONFIG_ESPIX_USB_NCM_ENABLED is off,
 * so callers guard on it rather than relying on a stub. */
#if CONFIG_ESPIX_USB_NCM_ENABLED
esp_err_t espix_net_usb_start(void);
#endif

/* eth.c: bring up eth0. Absent when CONFIG_ESPIX_ETH_ENABLED is off (and that
 * option does not exist at all on a target without SOC_EMAC_SUPPORTED). */
#if CONFIG_ESPIX_ETH_ENABLED
esp_err_t espix_net_eth_start(void);
#endif

/* abi.c: publish the network syscall surface to loadable apps. */
void espix_net_abi_register(void);

/* Rewrite /etc/wifi.conf. Reading it back is espix_fs_conf_get(). */
esp_err_t espix_net_conf_write_wifi(const char *ssid, const char *psk);

#ifdef __cplusplus
}
#endif
