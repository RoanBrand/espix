/*
 * The WireGuard server's interface and key.
 *
 * The protocol is the wireguard_lwip component's. What is here is the server's
 * shape, and the pieces espix owns: the interface, the key that persists across
 * boots, and -- next -- the peer list.
 */

#include <dirent.h>
#include <stdio.h>
#include <string.h>

#include "esp_random.h"

#include "lwip/ip.h"            /* ip_input */
#include "lwip/ip_addr.h"
#include "lwip/mem.h"
#include "lwip/netif.h"
#include "lwip/udp.h"

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
#define VPN_CLIENTS_DIR "/etc/vpn/clients/"
#define VPN_ADDR      "10.6.0.1"
#define VPN_MASK      "255.255.255.0"
#define VPN_PORT      WIREGUARDIF_DEFAULT_PORT
#define VPN_DNS_DEFAULT "1.1.1.1, 8.8.8.8"
#define VPN_SUBNET_DEFAULT "10.6.0.0/24"
#define VPN_ACCESS_DEFAULT "all"

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

/*
 * The private key is the server's identity, and a file anyone can read is a
 * key anyone has. 0600, on every write and again at the boot read -- a device
 * flashed before this existed carries no mode and would stay 0644 forever,
 * which is what espix_fs_ensure_mode() makes cheap to correct.
 */
static void secure_conf(void)
{
    (void)espix_fs_ensure_mode(VPN_CONF_PATH, 0600);
}

/*
 * The same for every client config, which each hold a private key: the mode is
 * set when one is written, and swept again at the boot read so a device that
 * predates the check does not keep 0644 files for the rest of its life.
 */
static void secure_clients(void)
{
    DIR *d = opendir(VPN_CLIENTS_DIR);
    if (d == NULL) {
        return;
    }

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strstr(e->d_name, ".conf") == NULL) {
            continue;
        }
        char path[160];
        strlcpy(path, VPN_CLIENTS_DIR, sizeof(path));
        strlcat(path, e->d_name, sizeof(path));
        (void)espix_fs_ensure_mode(path, 0600);
    }
    closedir(d);
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
        secure_conf();
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

    /* Through conf_set, not a fresh fopen: making a key must not drop the
     * endpoint, the dns or the network that are already configured. */
    if (espix_net_vpn_conf_set("private_key", s_priv) == ESP_OK) {
        (void)espix_net_vpn_conf_set("public_key", s_pub);
        espix_klog(ESPIX_KLOG_INFO, TAG, "generated a server key in %s",
                   VPN_CONF_PATH);
    } else {
        /* Said plainly: a key that is not saved changes on the next boot. */
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "cannot write %s; this key lasts until the next boot",
                   VPN_CONF_PATH);
    }
    secure_conf();
    return ESP_OK;
}

static void peers_load(void);        /* defined below, with the peer table */

esp_err_t espix_net_vpn_up(void)
{
    if (s_up) {
        return ESP_OK;
    }

    esp_err_t e = server_key();
    if (e != ESP_OK) {
        return e;
    }

    char addr[24], maskstr[24], portstr[16];
    (void)espix_net_vpn_server_addr(addr, sizeof(addr));
    espix_net_vpn_mask(maskstr, sizeof(maskstr));
    (void)espix_net_vpn_conf_get("port", portstr, sizeof(portstr));

    ip4_addr_t ip, mask, gw;
    ip4addr_aton(addr, &ip);
    ip4addr_aton(maskstr, &mask);
    gw = ip;                            /* the tunnel's own address is its gateway */

    s_init.private_key = s_priv;
    s_init.listen_port = (u16_t)atoi(portstr);
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
    espix_net_register_netif("wg0", &s_wg);
    netif_set_up(&s_wg);

    /* Masqueraded out the default route, like the AP's uplink: this is what
     * lets a client reach the internet and the house. */
    if (espix_net_napt_netif(&s_wg, true) != ESP_OK) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "wg0 is up but not masqueraded");
    }

    secure_clients();
    peers_load();

    s_up = true;
    espix_klog(ESPIX_KLOG_INFO, TAG, "wg0 %s/24 on port %d",
               addr, (int)s_init.listen_port);
    return ESP_OK;
}

esp_err_t espix_net_vpn_down(void)
{
    if (!s_up) {
        return ESP_OK;
    }

    (void)espix_net_napt_netif(&s_wg, false);
    espix_net_unregister_netif(&s_wg);
    wireguardif_shutdown(&s_wg);        /* cancels its timer, and only that */

    /*
     * The component stops its timer and leaves everything else behind: the UDP
     * socket still holds the port, and the device it allocated is still ours.
     * Without this the next vpn up cannot bind 51820, netif_add fails with no
     * clue why, and a down/up pair needs a reboot -- which is exactly what it
     * used to do.
     */
    struct wireguard_device *dev = (struct wireguard_device *)s_wg.state;
    if (dev != NULL) {
        if (dev->udp_pcb != NULL) {
            udp_remove(dev->udp_pcb);
        }
        mem_free(dev);
        s_wg.state = NULL;
    }

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

/*
 * The peer table lives in the interface, which is memory: a reboot takes every
 * client with it. /etc/vpn/peers keeps the one thing a client config cannot
 * supply -- the client's own public key, since that file holds the client's
 * private key and the server's public key and nothing else -- and wg0 admits
 * them again as it comes up.
 */
#define VPN_PEERS_PATH "/etc/vpn/peers"
#define VPN_MAX_PEERS  16

typedef struct {
    char name[24];
    char pub[48];
    char addr[20];
    char psk[48];               /* base64, empty when the client has none */
    char access[16];            /* all | home | clients | espix */
    u8_t idx;
} vpn_peer_t;

static vpn_peer_t s_peers[VPN_MAX_PEERS];
static int        s_peer_count;

static void peers_save(void)
{
    FILE *f = fopen(VPN_PEERS_PATH, "w");
    if (f == NULL) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "cannot write %s", VPN_PEERS_PATH);
        return;
    }
    fprintf(f, "# name public_key address preshared_key access\n");
    for (int i = 0; i < s_peer_count; i++) {
        fprintf(f, "%s %s %s %s %s\n", s_peers[i].name, s_peers[i].pub,
                s_peers[i].addr,
                (s_peers[i].psk[0] != 0) ? s_peers[i].psk : "-",
                (s_peers[i].access[0] != 0) ? s_peers[i].access : "-");
    }
    fclose(f);
}

static void peers_store(const char *name, const char *pub, const char *addr,
                        const char *psk, const char *access, u8_t idx)
{
    if (s_peer_count >= VPN_MAX_PEERS) {
        return;
    }
    vpn_peer_t *p = &s_peers[s_peer_count++];
    strlcpy(p->name, name, sizeof(p->name));
    strlcpy(p->pub, pub, sizeof(p->pub));
    strlcpy(p->addr, addr, sizeof(p->addr));
    strlcpy(p->psk, (psk != NULL) ? psk : "", sizeof(p->psk));
    strlcpy(p->access, (access != NULL) ? access : "", sizeof(p->access));
    p->idx = idx;
}

/* One peer into the interface. No endpoint: the client dials us. */
static bool peer_admit(const char *pub, const char *psk_b64, const char *addr,
                       u8_t *idx_out)
{
    uint8_t raw[32];
    const uint8_t *psk = NULL;

    if (psk_b64 != NULL && psk_b64[0] != 0 && psk_b64[0] != '-' &&
        base64_decode(psk_b64, raw, sizeof(raw)) == sizeof(raw)) {
        psk = raw;                      /* peer_init copies it */
    }

    struct wireguardif_peer p;
    wireguardif_peer_init(&p);
    p.public_key    = pub;
    p.preshared_key = psk;
    p.keep_alive    = 25;
    ipaddr_aton(addr, &p.allowed_ip);
    ipaddr_aton("255.255.255.255", &p.allowed_mask);

    u8_t idx = WIREGUARDIF_INVALID_INDEX;
    if (wireguardif_add_peer(&s_wg, &p, &idx) != ERR_OK ||
        idx == WIREGUARDIF_INVALID_INDEX) {
        return false;
    }
    *idx_out = idx;
    return true;
}

static void peers_load(void)
{
    s_peer_count = 0;                  /* a fresh device: admit them again */

    FILE *f = fopen(VPN_PEERS_PATH, "r");
    if (f == NULL) {
        return;
    }

    char line[128];
    while (fgets(line, sizeof(line), f) != NULL) {
        char name[24], pub[48], addr[20];
        if (line[0] == '#') {
            continue;
        }
        char psk[48] = {0}, access[16] = {0};
        int  got = sscanf(line, "%23s %47s %19s %47s %15s", name, pub, addr,
                          psk, access);
        if (got < 3) {
            continue;
        }
        if (got < 4 || strcmp(psk, "-") == 0) {
            psk[0] = 0;
        }
        if (got < 5 || strcmp(access, "-") == 0) {
            access[0] = 0;
        }

        u8_t idx;
        if (peer_admit(pub, psk, addr, &idx)) {
            peers_store(name, pub, addr, psk, access, idx);
        }
    }
    fclose(f);

    if (s_peer_count > 0) {
        espix_klog(ESPIX_KLOG_INFO, TAG, "%d peer(s) admitted", s_peer_count);
    }
}

esp_err_t espix_net_vpn_peer_add(const char *name, const char *pub_b64,
                                 const char *allowed_ip, const char *psk_b64,
                                 const char *access)
{
    if (!s_up) {
        return ESP_ERR_INVALID_STATE;
    }

    u8_t idx;
    if (!peer_admit(pub_b64, psk_b64, allowed_ip, &idx)) {
        return ESP_FAIL;
    }

    peers_store(name, pub_b64, allowed_ip, psk_b64, access, idx);
    peers_save();
    espix_klog(ESPIX_KLOG_INFO, TAG, "peer %s at %s (%s)", name, allowed_ip,
               pub_b64);
    return ESP_OK;
}

esp_err_t espix_net_vpn_endpoint_get(char *out, size_t len)
{
    if (!espix_fs_conf_get(VPN_CONF_PATH, "endpoint", out, len)) {
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

/* Rewrites /etc/vpn.conf, keeping whatever it is not asked to change: setting
 * the endpoint must not drop the DNS or the keys, and the other way round. */
esp_err_t espix_net_vpn_conf_get(const char *key, char *out, size_t len)
{
    if (key == NULL || out == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (espix_fs_conf_get(VPN_CONF_PATH, key, out, len)) {
        return ESP_OK;
    }

    /* A config written before a key existed still answers. */
    if (strcmp(key, "dns") == 0) {
        strlcpy(out, VPN_DNS_DEFAULT, len);
        return ESP_OK;
    }
    if (strcmp(key, "subnet") == 0) {
        strlcpy(out, VPN_SUBNET_DEFAULT, len);
        return ESP_OK;
    }
    if (strcmp(key, "access") == 0) {
        strlcpy(out, VPN_ACCESS_DEFAULT, len);
        return ESP_OK;
    }
    if (strcmp(key, "port") == 0) {
        snprintf(out, len, "%d", WIREGUARDIF_DEFAULT_PORT);
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t espix_net_vpn_conf_set(const char *key, const char *value)
{
    char priv[48] = {0}, pub[48] = {0}, ep[128] = {0}, dn[128] = {0};
    char port[16] = {0}, subnet[24] = {0}, access[16] = {0};

    struct { const char *k; char *v; size_t n; } f[] = {
        { "private_key", priv, sizeof(priv) },
        { "public_key",  pub,  sizeof(pub)  },
        { "endpoint",    ep,   sizeof(ep)   },
        { "dns",         dn,   sizeof(dn)   },
        { "port",        port, sizeof(port) },
        { "subnet",      subnet, sizeof(subnet) },
        { "access",      access, sizeof(access) },
    };
    const size_t nf = sizeof(f) / sizeof(f[0]);

    for (size_t i = 0; i < nf; i++) {
        (void)espix_fs_conf_get(VPN_CONF_PATH, f[i].k, f[i].v, f[i].n);
    }
    for (size_t i = 0; i < nf && key != NULL && value != NULL; i++) {
        if (strcmp(f[i].k, key) == 0) {
            strlcpy(f[i].v, value, f[i].n);
        }
    }

    FILE *fp = fopen(VPN_CONF_PATH, "w");
    if (fp == NULL) {
        return ESP_FAIL;
    }
    fprintf(fp, "# espix VPN server. The private key is the server's identity.\n");
    for (size_t i = 0; i < nf; i++) {
        if (f[i].v[0] != 0) {
            fprintf(fp, "%s=%s\n", f[i].k, f[i].v);
        }
    }
    fclose(fp);
    secure_conf();
    return ESP_OK;
}

esp_err_t espix_net_vpn_endpoint_set(const char *host)
{
    return espix_net_vpn_conf_set("endpoint", host);
}

esp_err_t espix_net_vpn_dns_get(char *out, size_t len)
{
    return espix_net_vpn_conf_get("dns", out, len);
}

esp_err_t espix_net_vpn_dns_set(const char *list)
{
    return espix_net_vpn_conf_set("dns", list);
}

/*
 * The tunnel subnet, as the first address and a /24 mask. A random one is
 * chosen at setup so two espix boxes do not both claim 10.6.0.0/24 -- a client
 * that talks to both would otherwise have two routes for one network -- and a
 * person can type one instead.
 */
static void subnet_parts(const char *subnet, unsigned *a, unsigned *b, unsigned *c)
{
    *a = 10; *b = 6; *c = 0;
    (void)sscanf(subnet, "%u.%u.%u", a, b, c);
}

esp_err_t espix_net_vpn_server_addr(char *out, size_t len)
{
    char subnet[24] = {0};
    unsigned a, b, c;

    (void)espix_net_vpn_conf_get("subnet", subnet, sizeof(subnet));
    subnet_parts(subnet, &a, &b, &c);
    snprintf(out, len, "%u.%u.%u.1", a, b, c);
    return ESP_OK;
}

void espix_net_vpn_mask(char *out, size_t len)
{
    strlcpy(out, "255.255.255.0", len);
}

esp_err_t espix_net_vpn_next_addr(char *out, size_t len)
{
    char subnet[24] = {0};
    unsigned a, b, c;

    (void)espix_net_vpn_conf_get("subnet", subnet, sizeof(subnet));
    subnet_parts(subnet, &a, &b, &c);

    for (unsigned host = 2; host < 255; host++) {
        char candidate[24];
        bool used = false;

        snprintf(candidate, sizeof(candidate), "%u.%u.%u.%u", a, b, c, host);
        for (int i = 0; i < s_peer_count; i++) {
            if (strcmp(s_peers[i].addr, candidate) == 0) {
                used = true;
                break;
            }
        }
        if (!used) {
            strlcpy(out, candidate, len);
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t espix_net_vpn_psk(char *out, size_t len)
{
    uint8_t raw[32];

    esp_fill_random(raw, sizeof(raw));
    base64_key(raw, sizeof(raw), out);
    (void)len;
    return ESP_OK;
}

int espix_net_vpn_peer_count(void)
{
    return s_peer_count;
}

const char *espix_net_vpn_peer_name(int i)
{
    return (i >= 0 && i < s_peer_count) ? s_peers[i].name : "";
}

const char *espix_net_vpn_peer_addr(int i)
{
    return (i >= 0 && i < s_peer_count) ? s_peers[i].addr : "";
}

bool espix_net_vpn_peer_session(int i, char *endpoint, size_t len)
{
    if (i < 0 || i >= s_peer_count || !s_up) {
        return false;
    }

    ip_addr_t ip;
    u16_t     port = 0;
    if (wireguardif_peer_is_up(&s_wg, s_peers[i].idx, &ip, &port) != ERR_OK) {
        return false;
    }

    if (endpoint != NULL && len > 0) {
        snprintf(endpoint, len, "%s:%u", ipaddr_ntoa(&ip), (unsigned)port);
    }
    return true;
}

esp_err_t espix_net_vpn_peer_del(const char *name)
{
    for (int i = 0; i < s_peer_count; i++) {
        if (strcmp(s_peers[i].name, name) != 0) {
            continue;
        }

        if (s_up) {
            (void)wireguardif_remove_peer(&s_wg, s_peers[i].idx);
        }
        s_peers[i] = s_peers[--s_peer_count];
        peers_save();
        espix_klog(ESPIX_KLOG_INFO, TAG, "peer %s removed", name);
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t espix_net_vpn_peer_addr_by_name(const char *name, char *out, size_t len)
{
    for (int i = 0; i < s_peer_count; i++) {
        if (strcmp(s_peers[i].name, name) == 0) {
            strlcpy(out, s_peers[i].addr, len);
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

const char *espix_net_vpn_peer_access(int i)
{
    return (i >= 0 && i < s_peer_count && s_peers[i].access[0] != 0)
               ? s_peers[i].access
               : "-";
}

const char *espix_net_vpn_peer_psk(int i)
{
    return (i >= 0 && i < s_peer_count) ? s_peers[i].psk : "";
}

esp_err_t espix_net_vpn_ensure_keys(void)
{
    return server_key();
}

/*
 * A tunnel network that is not the one the device is already on, so two espix
 * boxes do not both claim 10.6.0.0/24 -- a client that talks to both would
 * otherwise have two routes for one network, and one of them would be wrong.
 */
esp_err_t espix_net_vpn_random_subnet(char *out, size_t len)
{
    for (int tries = 0; tries < 16; tries++) {
        const unsigned a = 10;
        const unsigned b = 6 + (esp_random() % 240);
        const unsigned c = esp_random() % 256;

        char candidate[24], head[24];
        snprintf(candidate, sizeof(candidate), "%u.%u.%u.0/24", a, b, c);
        snprintf(head, sizeof(head), "%u.%u.%u", a, b, c);

        espix_ifinfo_t ifs[8];
        const size_t   n = espix_net_iflist(ifs, 8);
        bool           clash = false;
        for (size_t i = 0; i < n && !clash; i++) {
            if (!ifs[i].has_addr) {
                continue;
            }
            char ip[24];
            snprintf(ip, sizeof(ip), "%u.%u.%u",
                     (unsigned)(ifs[i].ip & 0xff),
                     (unsigned)((ifs[i].ip >> 8) & 0xff),
                     (unsigned)((ifs[i].ip >> 16) & 0xff));
            clash = (strcmp(ip, head) == 0);
        }
        if (!clash) {
            strlcpy(out, candidate, len);
            return ESP_OK;
        }
    }
    strlcpy(out, VPN_SUBNET_DEFAULT, len);
    return ESP_OK;
}
