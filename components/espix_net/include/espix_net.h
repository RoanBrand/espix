/*
 * espix networking — placeholder.
 *
 * Nothing is implemented yet, on purpose: the filesystem, shell and exec path
 * come first, and networking is what SSH/SCP (roadmap item 4) will sit on.
 * This header exists to fix the shape of the interface the rest of the system
 * will use, so bringing it up later does not ripple.
 *
 * Planned surface:
 *   espix_net_init()                 bring up esp_netif + the event loop
 *   espix_net_wifi_join(ssid, psk)   station mode
 *   espix_net_eth_start()            for P4 / S31 targets
 *   espix_net_status(...)            backs `ifconfig` / `ip addr`
 *
 * Per-chip notes that will matter here:
 *   - P4 has no built-in WiFi or BT; wireless needs a companion chip (usually
 *     an ESP32-C6 over SDIO/SPI), so on that target WiFi is not a driver
 *     bring-up but a whole transport.
 *   - S31 has gigabit Ethernet but cannot drive Ethernet and a display at the
 *     same time.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Placeholder so the component has a symbol and links; returns ESP_ERR_NOT_SUPPORTED. */
esp_err_t espix_net_init(void);

#ifdef __cplusplus
}
#endif
