#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/*
 * A WireGuard server (R-P8.1).
 *
 * The protocol is the wireguard_lwip component's; this is the server's shape.
 * A server turns out to be a configuration of that peer-to-peer core rather
 * than an extension of it: every client is a peer with a public key and an
 * allowed address and no endpoint, so the client initiates; the device listens
 * on every interface, because bind_netif NULL means "use the routing table".
 *
 * No peers yet -- espix_net_vpn_up() brings the interface up with the server's
 * key, and the peer list and the client configs come next.
 */
esp_err_t espix_net_vpn_up(void);
esp_err_t espix_net_vpn_down(void);
bool      espix_net_vpn_is_up(void);
