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
                                const char *allowed_ip, const char *psk_b64,
                                const char *access);

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

/* What clients are told to resolve with: the public pair unless set. The home
 * router's address is the one that resolves the house's names. */
esp_err_t espix_net_vpn_dns_get(char *out, size_t len);
esp_err_t espix_net_vpn_dns_set(const char *list);
esp_err_t   espix_net_vpn_peer_del(const char *name);
esp_err_t   espix_net_vpn_peer_addr_by_name(const char *name, char *out,
                                            size_t len);

/* The config as key=value, with a default when the key is absent. */
esp_err_t espix_net_vpn_conf_get(const char *key, char *out, size_t len);
esp_err_t espix_net_vpn_conf_set(const char *key, const char *value);

/* The tunnel network: the server's address, a /24 mask, the next free client
 * address, and a fresh pre-shared key. */
esp_err_t   espix_net_vpn_server_addr(char *out, size_t len);
void        espix_net_vpn_mask(char *out, size_t len);
esp_err_t   espix_net_vpn_next_addr(char *out, size_t len);
esp_err_t   espix_net_vpn_psk(char *out, size_t len);
const char *espix_net_vpn_peer_access(int i);
const char *espix_net_vpn_peer_psk(int i);
esp_err_t   espix_net_vpn_random_subnet(char *out, size_t len);
esp_err_t   espix_net_vpn_ensure_keys(void);
/* 0640 with the login group: readable by the shell user, by nobody else. */
void        espix_net_vpn_secure(const char *path);
