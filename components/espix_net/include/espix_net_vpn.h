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

/* The server's public key, base64, for a client's [Peer] section. */
esp_err_t espix_net_vpn_server_pubkey(char *out, size_t len);

/* A fresh client keypair, both base64. WireGuard clamps the private key; this
 * does the same, so the app deriving the public key from it agrees. */
esp_err_t espix_net_vpn_keypair(char *priv_b64, size_t plen,
                                char *pub_b64, size_t publen);

/* Admit a client: a peer with this public key, allowed only this address, and
 * no endpoint -- so the client initiates, from wherever it roams. */
esp_err_t espix_net_vpn_peer_add(const char *name, const char *pub_b64,
                                const char *allowed_ip);

/* The peers this interface knows, for the vpn status command. peer_session()
 * reports whether that peer has a live session and, if so, the endpoint it has
 * roamed to -- the only place a server learns where a phone actually is. */
int         espix_net_vpn_peer_count(void);
const char *espix_net_vpn_peer_name(int i);
const char *espix_net_vpn_peer_addr(int i);
bool        espix_net_vpn_peer_session(int i, char *endpoint, size_t len);

/* What clients dial: your public name or address. */
esp_err_t espix_net_vpn_endpoint_get(char *out, size_t len);
esp_err_t espix_net_vpn_endpoint_set(const char *host);
