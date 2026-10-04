/*
 * The WireGuard server's interface and key.
 *
 * The protocol is the wireguard_lwip component's. What is here is the server's
 * shape, and the pieces espix owns: the interface, the key that persists across
 * boots, and -- next -- the peer list.
 */

#include <stdio.h>
#include <string.h>

#include "esp_random.h"

#include "lwip/ip.h"            /* ip_input */
#include "lwip/ip_addr.h"
#include "lwip/netif.h"

#include "wireguard.h"
#include "wireguardif.h"

#include "espix_fs.h"
#include "espix_kernel.h"
#include "espix_net.h"
#include "espix_net_priv.h"

#include "lwip/inet.h"

#include "x25519.h"
#include "espix_net_vpn.h"

#define VPN_CONF_PATH "/etc/vpn.conf"
#define VPN_ADDR      "10.6.0.1"
#define VPN_MASK      "255.255.255.0"
#define VPN_PORT      WIREGUARDIF_DEFAULT_PORT

static const char *TAG = "espix:vpn";

static struct netif                 s_wg;
static struct wireguardif_init_data s_init;
static char                         s_priv[48];    /* base64, 32 bytes */
static char                         s_pub[48];    /* base64, the server's public key */
static bool                         s_up;

/* 32 bytes to 44 base64 characters. Only encoding is needed: the component
 * takes keys as base64, and espix stores them that way too. */
static void base64_key(const uint8_t *in, size_t n, char *out)
{
    static const char t[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;

    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = ((uint32_t)in[i] << 16) |
                           ((i + 1 < n ? in[i + 1] : 0) << 8) |
                            (i + 2 < n ? in[i + 2] : 0);
        out[o++] = t[(v >> 18) & 0x3F];
        out[o++] = t[(v >> 12) & 0x3F];
        out[o++] = (i + 1 < n) ? t[(v >> 6) & 0x3F] : '=';
        out[o++] = (i + 2 < n) ? t[v & 0x3F] : '=';
    }
    out[o] = 0;
}

/* The inverse, so a key already in the file can be turned back into bytes to
 * derive its public half. Returns the byte count, or 0. */
static size_t base64_decode(const char *in, uint8_t *out, size_t outlen)
{
    int8_t map[256];
    memset(map, -1, sizeof(map));
    const char *t =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; i < 64; i++) {
        map[(unsigned char)t[i]] = (int8_t)i;
    }

    size_t o = 0;
    uint32_t v = 0;
    int      bits = 0;

    for (const char *p = in; *p != 0 && *p != '='; p++) {
        const int8_t d = map[(unsigned char)*p];
        if (d < 0) {
            continue;
        }
        v = (v << 6) | (uint32_t)d;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= outlen) {
                return 0;
            }
            out[o++] = (uint8_t)(v >> bits);
        }
    }
    return o;
}

/* The key is the server's identity: it has to outlive a reboot or every client
 * stops working, so it is read from /etc/vpn.conf and written there once. */
static esp_err_t server_key(void)
{
    if (espix_fs_conf_get(VPN_CONF_PATH, "private_key", s_priv, sizeof(s_priv))) {
        /* An older file may hold only the private key; the public one is
         * derived from it, since the client config needs it. */
        uint8_t raw[32], pub[32];
        if (base64_decode(s_priv, raw, sizeof(raw)) == sizeof(raw) &&
            x25519_base(pub, raw, 1) == 0) {
            base64_key(pub, sizeof(pub), s_pub);
        }
        return ESP_OK;
    }

    uint8_t raw[32], pub[32];
    esp_fill_random(raw, sizeof(raw));
    raw[0] &= 248;                      /* WireGuard's clamp, as wg genkey does */
    raw[31] = (uint8_t)((raw[31] & 127) | 64);
    if (x25519_base(pub, raw, 1) != 0) {
        return ESP_FAIL;
    }
    base64_key(raw, sizeof(raw), s_priv);
    base64_key(pub, sizeof(pub), s_pub);

    FILE *f = fopen(VPN_CONF_PATH, "w");
    if (f != NULL) {
        fprintf(f, "# espix VPN server. The private key is the server's identity.\n");
        fprintf(f, "private_key=%s\n", s_priv);
        fprintf(f, "public_key=%s\n", s_pub);
        fclose(f);
        espix_klog(ESPIX_KLOG_INFO, TAG, "generated a server key in %s",
                   VPN_CONF_PATH);
    } else {
        /* Said plainly: a key that is not saved changes on the next boot. */
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "cannot write %s; this key lasts until the next boot",
                   VPN_CONF_PATH);
    }
    return ESP_OK;
}

esp_err_t espix_net_vpn_up(void)
{
    if (s_up) {
        return ESP_OK;
    }

    esp_err_t e = server_key();
    if (e != ESP_OK) {
        return e;
    }

    ip4_addr_t ip, mask, gw;
    ip4addr_aton(VPN_ADDR, &ip);
    ip4addr_aton(VPN_MASK, &mask);
    gw = ip;                            /* the tunnel's own address is its gateway */

    s_init.private_key = s_priv;
    s_init.listen_port = VPN_PORT;
    s_init.bind_netif  = NULL;          /* every interface, by the routing table */

    /* A raw lwIP netif, not an esp_netif: the component's input function is
     * ip_input and it wants the netif it initialises. */
    s_wg.name[0] = 'w';
    s_wg.name[1] = 'g';
    s_wg.mtu     = WIREGUARDIF_MTU;

    if (netif_add(&s_wg, &ip, &mask, &gw, &s_init, wireguardif_init,
                  ip_input) == NULL) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "cannot add wg0");
        return ESP_FAIL;
    }
    netif_set_up(&s_wg);

    /* Masqueraded out the default route, like the AP's uplink: this is what
     * lets a client reach the internet and the house. */
    if (espix_net_napt_netif(&s_wg, true) != ESP_OK) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "wg0 is up but not masqueraded");
    }

    s_up = true;
    espix_klog(ESPIX_KLOG_INFO, TAG, "wg0 %s/24 on port %d",
               VPN_ADDR, VPN_PORT);
    return ESP_OK;
}

esp_err_t espix_net_vpn_down(void)
{
    if (!s_up) {
        return ESP_OK;
    }

    (void)espix_net_napt_netif(&s_wg, false);
    wireguardif_shutdown(&s_wg);        /* cancels its timers first, as it asks */
    netif_remove(&s_wg);
    s_up = false;
    espix_klog(ESPIX_KLOG_INFO, TAG, "wg0 down");
    return ESP_OK;
}

bool espix_net_vpn_is_up(void)
{
    return s_up;
}

esp_err_t espix_net_vpn_server_pubkey(char *out, size_t len)
{
    if (out == NULL || len == 0 || s_pub[0] == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    strlcpy(out, s_pub, len);
    return ESP_OK;
}

esp_err_t espix_net_vpn_keypair(char *priv_b64, size_t plen,
                                char *pub_b64, size_t publen)
{
    uint8_t priv[32], pub[32];

    esp_fill_random(priv, sizeof(priv));
    priv[0] &= 248;
    priv[31] = (uint8_t)((priv[31] & 127) | 64);

    if (x25519_base(pub, priv, 1) != 0) {
        return ESP_FAIL;
    }

    base64_key(priv, sizeof(priv), priv_b64);
    base64_key(pub, sizeof(pub), pub_b64);
    (void)plen;
    (void)publen;
    return ESP_OK;
}

esp_err_t espix_net_vpn_peer_add(const char *pub_b64, const char *allowed_ip)
{
    if (!s_up) {
        return ESP_ERR_INVALID_STATE;
    }

    struct wireguardif_peer p;
    wireguardif_peer_init(&p);
    p.public_key    = pub_b64;
    p.preshared_key = NULL;
    p.keep_alive    = 25;               /* a phone behind NAT stays reachable */
    ipaddr_aton(allowed_ip, &p.allowed_ip);
    ipaddr_aton("255.255.255.255", &p.allowed_mask);
    /* No endpoint: the client dials us, and we learn where it is. */

    u8_t idx = WIREGUARDIF_INVALID_INDEX;
    if (wireguardif_add_peer(&s_wg, &p, &idx) != ERR_OK ||
        idx == WIREGUARDIF_INVALID_INDEX) {
        return ESP_FAIL;
    }
    espix_klog(ESPIX_KLOG_INFO, TAG, "peer %s at %s", allowed_ip, pub_b64);
    return ESP_OK;
}

esp_err_t espix_net_vpn_endpoint_get(char *out, size_t len)
{
    if (!espix_fs_conf_get(VPN_CONF_PATH, "endpoint", out, len)) {
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t espix_net_vpn_endpoint_set(const char *host)
{
    char priv[48] = {0};
    char pub[48]  = {0};

    (void)espix_fs_conf_get(VPN_CONF_PATH, "private_key", priv, sizeof(priv));
    (void)espix_fs_conf_get(VPN_CONF_PATH, "public_key", pub, sizeof(pub));

    FILE *f = fopen(VPN_CONF_PATH, "w");
    if (f == NULL) {
        return ESP_FAIL;
    }
    fprintf(f, "# espix VPN server. The private key is the server's identity.\n");
    if (priv[0] != 0) {
        fprintf(f, "private_key=%s\n", priv);
    }
    if (pub[0] != 0) {
        fprintf(f, "public_key=%s\n", pub);
    }
    fprintf(f, "endpoint=%s\n", host);
    fclose(f);
    return ESP_OK;
}
