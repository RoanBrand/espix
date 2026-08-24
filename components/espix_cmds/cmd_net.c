/*
 * Network commands: ip, ifconfig, route, ping, wifi, hostname.
 *
 * `ip` and the net-tools pair (ifconfig/route) render the same espix_ifinfo_t
 * data, so the legacy views cannot drift from the modern ones.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "ping/ping_sock.h"

#include "espix_cmds_priv.h"
#include "espix_kernel.h"
#include "espix_net.h"
#include "espix_shell.h"

#define IFLIST_MAX 4
#define SCAN_MAX   24

/* ------------------------------------------------------------------ */
/* Shared rendering                                                    */
/* ------------------------------------------------------------------ */

static const char *kind_flags(espix_if_kind_t kind)
{
    switch (kind) {
    case ESPIX_IF_LO:  return "LOOPBACK";
    case ESPIX_IF_ETH: return "BROADCAST,MULTICAST";
    default:           return "BROADCAST,MULTICAST";
    }
}

static void mac_str(const uint8_t mac[6], char *buf, size_t len)
{
    snprintf(buf, len, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void print_ip_iface(espix_session_t *s, const espix_ifinfo_t *i,
                           bool with_addr)
{
    espix_printf(s, "%d: %s: <%s%s> mtu %u\n",
                 i->index, i->name,
                 i->up ? "UP," : "",
                 kind_flags(i->kind),
                 (unsigned)i->mtu);

    if (i->has_mac) {
        char mac[18];
        mac_str(i->mac, mac, sizeof(mac));
        espix_printf(s, "    link/ether %s\n", mac);
    }

    if (with_addr && i->has_addr) {
        char ip[ESPIX_IP4STR_MAX];
        espix_printf(s, "    inet %s/%d\n",
                     espix_net_ip4str(i->ip, ip, sizeof(ip)),
                     espix_net_prefix_len(i->netmask));
    }
}

/* ------------------------------------------------------------------ */
/* ip                                                                  */
/* ------------------------------------------------------------------ */

static int ip_show(espix_session_t *s, const char *dev, bool with_addr)
{
    espix_ifinfo_t ifs[IFLIST_MAX];
    const size_t   n = espix_net_iflist(ifs, IFLIST_MAX);

    for (size_t i = 0; i < n; i++) {
        if (dev != NULL && strcmp(dev, ifs[i].name) != 0) {
            continue;
        }
        print_ip_iface(s, &ifs[i], with_addr);
    }
    return 0;
}

static int ip_route(espix_session_t *s)
{
    char     dev[ESPIX_IF_NAME_MAX];
    uint32_t gw = 0;

    if (espix_net_default_route(dev, sizeof(dev), &gw)) {
        char g[ESPIX_IP4STR_MAX];
        espix_printf(s, "default via %s dev %s\n",
                     espix_net_ip4str(gw, g, sizeof(g)), dev);
    }

    /* On-link routes, which are implicit in each interface's address/mask. */
    espix_ifinfo_t ifs[IFLIST_MAX];
    const size_t   n = espix_net_iflist(ifs, IFLIST_MAX);

    for (size_t i = 0; i < n; i++) {
        if (!ifs[i].has_addr || ifs[i].kind == ESPIX_IF_LO) {
            continue;
        }
        char net[ESPIX_IP4STR_MAX];
        espix_printf(s, "%s/%d dev %s scope link\n",
                     espix_net_ip4str(ifs[i].ip & ifs[i].netmask,
                                      net, sizeof(net)),
                     espix_net_prefix_len(ifs[i].netmask),
                     ifs[i].name);
    }
    return 0;
}

static int cmd_ip(espix_session_t *s, int argc, char **argv)
{
    const char *obj = (argc > 1) ? argv[1] : "addr";
    const char *dev = (argc > 2) ? argv[2] : NULL;

    /* Accept the usual abbreviations: ip a, ip l, ip r. */
    if (strncmp(obj, "a", 1) == 0) {
        return ip_show(s, dev, true);
    }
    if (strncmp(obj, "l", 1) == 0) {
        return ip_show(s, dev, false);
    }
    if (strncmp(obj, "r", 1) == 0) {
        return ip_route(s);
    }

    espix_printf(s, "usage: ip {addr|link|route} [dev]\n");
    return 1;
}

/* ------------------------------------------------------------------ */
/* ifconfig / route — same data, net-tools format                       */
/* ------------------------------------------------------------------ */

static int cmd_ifconfig(espix_session_t *s, int argc, char **argv)
{
    const char *dev = (argc > 1) ? argv[1] : NULL;

    espix_ifinfo_t ifs[IFLIST_MAX];
    const size_t   n = espix_net_iflist(ifs, IFLIST_MAX);
    bool           shown = false;

    for (size_t i = 0; i < n; i++) {
        const espix_ifinfo_t *f = &ifs[i];
        if (dev != NULL && strcmp(dev, f->name) != 0) {
            continue;
        }
        shown = true;

        espix_printf(s, "%-9s Link encap:%s", f->name,
                     f->kind == ESPIX_IF_LO ? "Local Loopback" : "Ethernet");
        if (f->has_mac) {
            char mac[18];
            mac_str(f->mac, mac, sizeof(mac));
            espix_printf(s, "  HWaddr %s", mac);
        }
        espix_puts(s, "\n");

        if (f->has_addr) {
            char ip[ESPIX_IP4STR_MAX], nm[ESPIX_IP4STR_MAX];
            espix_printf(s, "          inet addr:%s  Mask:%s\n",
                         espix_net_ip4str(f->ip, ip, sizeof(ip)),
                         espix_net_ip4str(f->netmask, nm, sizeof(nm)));
        }

        espix_printf(s, "          %s  MTU:%u\n\n",
                     f->up ? "UP RUNNING" : "DOWN", (unsigned)f->mtu);
    }

    if (dev != NULL && !shown) {
        espix_printf(s, "ifconfig: %s: no such interface\n", dev);
        return 1;
    }
    return 0;
}

static int cmd_route(espix_session_t *s, int argc, char **argv)
{
    (void)argc;
    (void)argv;

    espix_printf(s, "%-16s %-16s %-16s %-6s %s\n",
                 "Destination", "Gateway", "Genmask", "Flags", "Iface");

    char     dev[ESPIX_IF_NAME_MAX];
    uint32_t gw = 0;
    if (espix_net_default_route(dev, sizeof(dev), &gw)) {
        char g[ESPIX_IP4STR_MAX];
        espix_printf(s, "%-16s %-16s %-16s %-6s %s\n", "default",
                     espix_net_ip4str(gw, g, sizeof(g)), "0.0.0.0", "UG", dev);
    }

    espix_ifinfo_t ifs[IFLIST_MAX];
    const size_t   n = espix_net_iflist(ifs, IFLIST_MAX);
    for (size_t i = 0; i < n; i++) {
        if (!ifs[i].has_addr || ifs[i].kind == ESPIX_IF_LO) {
            continue;
        }
        char net[ESPIX_IP4STR_MAX], nm[ESPIX_IP4STR_MAX];
        espix_printf(s, "%-16s %-16s %-16s %-6s %s\n",
                     espix_net_ip4str(ifs[i].ip & ifs[i].netmask,
                                      net, sizeof(net)),
                     "0.0.0.0",
                     espix_net_ip4str(ifs[i].netmask, nm, sizeof(nm)),
                     "U", ifs[i].name);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* ping                                                                */
/* ------------------------------------------------------------------ */

#define PING_DONE_BIT BIT0

typedef struct {
    espix_session_t   *s;
    const char        *host;
    EventGroupHandle_t done;
} ping_ctx_t;

static void ping_success(esp_ping_handle_t hdl, void *args)
{
    ping_ctx_t *c = args;
    uint32_t    elapsed = 0, seqno = 0, size = 0;
    uint8_t     ttl = 0;
    ip_addr_t   target;

    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TTL, &ttl, sizeof(ttl));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target, sizeof(target));
    esp_ping_get_profile(hdl, ESP_PING_PROF_SIZE, &size, sizeof(size));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed, sizeof(elapsed));

    espix_printf(c->s, "%u bytes from %s: icmp_seq=%u ttl=%u time=%u ms\n",
                 (unsigned)size, ipaddr_ntoa(&target),
                 (unsigned)seqno, (unsigned)ttl, (unsigned)elapsed);
}

static void ping_timeout(esp_ping_handle_t hdl, void *args)
{
    ping_ctx_t *c = args;
    uint32_t    seqno = 0;

    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    espix_printf(c->s, "Request timeout for icmp_seq %u\n", (unsigned)seqno);
}

static void ping_end(esp_ping_handle_t hdl, void *args)
{
    ping_ctx_t *c = args;
    uint32_t    tx = 0, rx = 0, total = 0;

    esp_ping_get_profile(hdl, ESP_PING_PROF_REQUEST, &tx, sizeof(tx));
    esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &rx, sizeof(rx));
    esp_ping_get_profile(hdl, ESP_PING_PROF_DURATION, &total, sizeof(total));

    const unsigned loss = (tx > 0) ? (unsigned)(100 * (tx - rx) / tx) : 100;

    espix_printf(c->s, "\n--- %s ping statistics ---\n", c->host);
    espix_printf(c->s,
                 "%u packets transmitted, %u received, %u%% packet loss, "
                 "time %ums\n",
                 (unsigned)tx, (unsigned)rx, loss, (unsigned)total);

    xEventGroupSetBits(c->done, PING_DONE_BIT);
}

static int cmd_ping(espix_session_t *s, int argc, char **argv)
{
    const char *host  = NULL;
    uint32_t    count = 4;   /* bounded: there is no Ctrl-C to stop it yet */

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            count = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else {
            host = argv[i];
        }
    }

    if (host == NULL) {
        espix_printf(s, "usage: ping [-c count] <host>\n");
        return 1;
    }

    uint32_t addr = 0;
    if (espix_net_resolve(host, &addr) != ESP_OK) {
        espix_printf(s, "ping: %s: Name or service not known\n", host);
        return 1;
    }

    ping_ctx_t ctx = { .s = s, .host = host, .done = xEventGroupCreate() };
    if (ctx.done == NULL) {
        return 1;
    }

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.count             = count;
    cfg.target_addr.type  = IPADDR_TYPE_V4;
    cfg.target_addr.u_addr.ip4.addr = addr;

    const esp_ping_callbacks_t cbs = {
        .cb_args         = &ctx,
        .on_ping_success = ping_success,
        .on_ping_timeout = ping_timeout,
        .on_ping_end     = ping_end,
    };

    char ipbuf[ESPIX_IP4STR_MAX];
    espix_printf(s, "PING %s (%s): %u data bytes\n", host,
                 espix_net_ip4str(addr, ipbuf, sizeof(ipbuf)),
                 (unsigned)cfg.data_size);

    esp_ping_handle_t hdl = NULL;
    if (esp_ping_new_session(&cfg, &cbs, &hdl) != ESP_OK) {
        espix_printf(s, "ping: cannot create session\n");
        vEventGroupDelete(ctx.done);
        return 1;
    }

    esp_ping_start(hdl);

    /* Wait for on_ping_end, with a margin over the worst case so a wedged
     * session cannot hold the shell forever. */
    const TickType_t limit =
        pdMS_TO_TICKS((cfg.interval_ms + cfg.timeout_ms) * (count + 2));
    xEventGroupWaitBits(ctx.done, PING_DONE_BIT, pdTRUE, pdTRUE, limit);

    esp_ping_stop(hdl);
    esp_ping_delete_session(hdl);
    vEventGroupDelete(ctx.done);
    return 0;
}

/* ------------------------------------------------------------------ */
/* wifi                                                                */
/* ------------------------------------------------------------------ */

static const char *wifi_state_str(espix_wifi_state_t st)
{
    switch (st) {
    case ESPIX_WIFI_OFF:        return "off";
    case ESPIX_WIFI_IDLE:       return "idle";
    case ESPIX_WIFI_CONNECTING: return "connecting";
    case ESPIX_WIFI_CONNECTED:  return "connected";
    default:                    return "?";
    }
}

static int wifi_scan(espix_session_t *s)
{
    espix_ap_t *aps = calloc(SCAN_MAX, sizeof(espix_ap_t));
    if (aps == NULL) {
        return 1;
    }

    size_t found = 0;
    espix_printf(s, "scanning...\n");

    const esp_err_t err = espix_net_wifi_scan(aps, SCAN_MAX, &found);
    if (err != ESP_OK) {
        espix_printf(s, "wifi: scan failed: %s\n", esp_err_to_name(err));
        free(aps);
        return 1;
    }

    espix_printf(s, "%-32s %4s %3s %s\n", "SSID", "RSSI", "CH", "SECURITY");
    for (size_t i = 0; i < found; i++) {
        espix_printf(s, "%-32s %4d %3u %s\n",
                     aps[i].ssid[0] ? aps[i].ssid : "(hidden)",
                     aps[i].rssi, aps[i].channel,
                     aps[i].secure ? "secured" : "open");
    }

    free(aps);
    return 0;
}

static int wifi_status(espix_session_t *s)
{
    espix_wifi_status_t st;
    if (espix_net_wifi_status(&st) != ESP_OK) {
        return 1;
    }

    espix_printf(s, "state:  %s\n", wifi_state_str(st.state));
    if (st.ssid[0] != '\0') {
        espix_printf(s, "ssid:   %s\n", st.ssid);
    }
    if (st.state == ESPIX_WIFI_CONNECTED) {
        espix_printf(s, "signal: %d dBm\nchannel: %u\n", st.rssi, st.channel);
    }
    if (st.retries > 0) {
        espix_printf(s, "retries: %u\n", st.retries);
    }

    espix_ifinfo_t info;
    if (espix_net_ifinfo("wlan0", &info) == ESP_OK && info.has_addr) {
        char ip[ESPIX_IP4STR_MAX];
        espix_printf(s, "address: %s/%d\n",
                     espix_net_ip4str(info.ip, ip, sizeof(ip)),
                     espix_net_prefix_len(info.netmask));
    }

    uint32_t dns[2];
    const size_t n = espix_net_dns(dns, 2);
    for (size_t i = 0; i < n; i++) {
        char d[ESPIX_IP4STR_MAX];
        espix_printf(s, "dns:     %s\n", espix_net_ip4str(dns[i], d, sizeof(d)));
    }
    return 0;
}

static int cmd_wifi(espix_session_t *s, int argc, char **argv)
{
    const char *sub = (argc > 1) ? argv[1] : "status";

    if (strcmp(sub, "scan") == 0) {
        return wifi_scan(s);
    }
    if (strcmp(sub, "status") == 0) {
        return wifi_status(s);
    }
    if (strcmp(sub, "disconnect") == 0) {
        return espix_net_wifi_disconnect() == ESP_OK ? 0 : 1;
    }
    if (strcmp(sub, "connect") == 0) {
        const char *ssid = (argc > 2) ? argv[2] : NULL;
        const char *psk  = (argc > 3) ? argv[3] : "";

        const esp_err_t err = espix_net_wifi_connect(ssid, ssid ? psk : NULL);
        if (err == ESP_ERR_NOT_FOUND) {
            espix_printf(s, "wifi: no ssid given and none in /etc/wifi.conf\n");
            return 1;
        }
        if (err != ESP_OK) {
            espix_printf(s, "wifi: %s\n", esp_err_to_name(err));
            return 1;
        }
        espix_printf(s, "connecting; watch 'wifi status' or dmesg\n");
        return 0;
    }

    espix_printf(s, "usage: wifi {scan|connect [ssid] [psk]|disconnect|status}\n");
    return 1;
}

/* ------------------------------------------------------------------ */
/* hostname                                                            */
/* ------------------------------------------------------------------ */

static int cmd_hostname(espix_session_t *s, int argc, char **argv)
{
    if (argc < 2) {
        espix_printf(s, "%s\n", espix_net_hostname());
        return 0;
    }

    if (espix_net_set_hostname(argv[1], true) != ESP_OK) {
        espix_printf(s, "hostname: cannot set\n");
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */

static espix_cmd_t s_net_cmds[] = {
    { .name = "ip",       .fn = cmd_ip,
      .help = "show addresses, links and routes",
      .usage = "ip {addr|link|route} [dev]" },
    { .name = "ifconfig", .fn = cmd_ifconfig,
      .help = "show interfaces (net-tools style)",
      .usage = "ifconfig [dev]" },
    { .name = "route",    .fn = cmd_route,
      .help = "show the routing table (net-tools style)",
      .usage = "route" },
    { .name = "ping",     .fn = cmd_ping,
      .help = "send ICMP echo requests",
      .usage = "ping [-c count] <host>" },
    { .name = "wifi",     .fn = cmd_wifi,
      .help = "scan, connect and inspect the WiFi station",
      .usage = "wifi {scan|connect [ssid] [psk]|disconnect|status}" },
    { .name = "hostname", .fn = cmd_hostname,
      .help = "show or set the hostname",
      .usage = "hostname [name]" },
};

void espix_cmds_register_net(void)
{
    espix_cmds_register_table(s_net_cmds,
                             sizeof(s_net_cmds) / sizeof(s_net_cmds[0]));
}
