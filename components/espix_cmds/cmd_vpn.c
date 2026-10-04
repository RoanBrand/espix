/*
 * vpn -- the WireGuard server.
 *
 * PiVPN's shape: a few verbs over a config, so a client is a command away
 * rather than a file to hand-edit. This is the first cut -- up, down, status,
 * which makes the interface reachable; the client verbs (add, list, qr,
 * remove) arrive with key generation.
 */

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_random.h"

#include "qrcode.h"

#include "espix_cmds_priv.h"
#include "espix_fs.h"
#include "espix_net.h"
#include "espix_net_vpn.h"

/* The network the device sits on, for the "home" level. */
static void lan_network(char *out, size_t len)
{
    espix_ifinfo_t ifs[8];
    const size_t   n = espix_net_iflist(ifs, 8);

    out[0] = 0;
    for (size_t i = 0; i < n; i++) {
        if (!ifs[i].has_addr || ifs[i].kind == ESPIX_IF_LO) {
            continue;
        }
        const uint32_t net = ifs[i].ip & ifs[i].netmask;
        int            bits = 0;
        for (uint32_t m = ifs[i].netmask; m != 0; m >>= 1) {
            bits += (int)(m & 1);
        }
        snprintf(out, len, "%u.%u.%u.%u/%d",
                 (unsigned)(net & 0xff), (unsigned)((net >> 8) & 0xff),
                 (unsigned)((net >> 16) & 0xff), (unsigned)((net >> 24) & 0xff),
                 bits);
        return;
    }
}

/*
 * What a client sends into the tunnel, which is what it will reach. What it is
 * really allowed to reach is the firewall that is not there yet; this is the
 * client's side of the agreement, recorded per peer so the firewall has
 * something to enforce.
 */
static void allowed_ips(const char *access, char *out, size_t len)
{
    char subnet[24], srv[24];

    (void)espix_net_vpn_conf_get("subnet", subnet, sizeof(subnet));
    (void)espix_net_vpn_server_addr(srv, sizeof(srv));

    if (strcmp(access, "espix") == 0) {
        snprintf(out, len, "%s/32", srv);
    } else if (strcmp(access, "clients") == 0) {
        snprintf(out, len, "%s", subnet);
    } else if (strcmp(access, "home") == 0) {
        char lan[40] = {0};
        lan_network(lan, sizeof(lan));
        if (lan[0] != 0) {
            snprintf(out, len, "%s, %s", lan, subnet);
        } else {
            snprintf(out, len, "%s", subnet);
        }
    } else {
        snprintf(out, len, "0.0.0.0/0, ::/0");
    }
}

/* A client is a keypair, a config file, and a peer with no endpoint. The
 * address is the next free one in 10.6.0.0/24; the peer is what makes the
 * server accept that key. */
/* The home router's address, which is what resolves the house's names. */
static bool router_dns(char *out, size_t len)
{
    uint32_t gw = 0;

    if (!espix_net_default_route(NULL, 0, &gw) || gw == 0) {
        return false;
    }
    snprintf(out, len, "%u.%u.%u.%u",
             (unsigned)(gw & 0xff), (unsigned)((gw >> 8) & 0xff),
             (unsigned)((gw >> 16) & 0xff), (unsigned)((gw >> 24) & 0xff));
    return true;
}

/*
 * The server's settings, once. Asked for when there is nothing yet -- no
 * server key means no server section, which is what a client-only config looks
 * like -- or given as key=value for a script.
 */
static int vpn_setup(espix_session_t *s, int argc, char **argv)
{
    char port[16], endpoint[128], dns[128], subnet[24], access[16];
    char line[128];

    (void)espix_net_vpn_conf_get("port", port, sizeof(port));
    (void)espix_net_vpn_conf_get("endpoint", endpoint, sizeof(endpoint));
    (void)espix_net_vpn_conf_get("dns", dns, sizeof(dns));
    (void)espix_net_vpn_conf_get("access", access, sizeof(access));

    /* A network the device is not already on, unless one is already set. */
    if (espix_fs_conf_get("/etc/vpn.conf", "subnet", subnet, sizeof(subnet))) {
        /* keep it */
    } else {
        (void)espix_net_vpn_random_subnet(subnet, sizeof(subnet));
    }

    if (argc > 2) {
        for (int i = 2; i < argc; i++) {      /* argv[1] is "setup" */
            char *eq = strchr(argv[i], '=');
            if (eq == NULL) {
                espix_eprintf(s, "usage: vpn setup [port=|endpoint=|dns=|subnet=|access=]\n");
                return 1;
            }
            *eq = 0;
            const char *k = argv[i];
            const char *v = eq + 1;

            if (strcmp(k, "port") == 0)          strlcpy(port, v, sizeof(port));
            else if (strcmp(k, "endpoint") == 0) strlcpy(endpoint, v, sizeof(endpoint));
            else if (strcmp(k, "dns") == 0)      strlcpy(dns, v, sizeof(dns));
            else if (strcmp(k, "subnet") == 0)   strlcpy(subnet, v, sizeof(subnet));
            else if (strcmp(k, "access") == 0)   strlcpy(access, v, sizeof(access));
            else {
                espix_eprintf(s, "vpn: unknown setting %s\n", k);
                return 1;
            }
        }
    } else {
        espix_printf(s, "Setting up the VPN server; enter accepts what is in brackets.\n");

        espix_printf(s, "port [%s]: ", port);
        if (s->read_line(s, "", line, sizeof(line)) > 0 && line[0] != 0) {
            strlcpy(port, line, sizeof(port));
        }

        espix_printf(s, "public name or address [%s]: ", endpoint);
        if (s->read_line(s, "", line, sizeof(line)) > 0 && line[0] != 0) {
            strlcpy(endpoint, line, sizeof(endpoint));
        }

        espix_printf(s, "dns for clients (public|router|an address) [%s]: ", dns);
        if (s->read_line(s, "", line, sizeof(line)) > 0 && line[0] != 0) {
            char gw[24];
            if (strcmp(line, "public") == 0) {
                strlcpy(dns, "1.1.1.1, 8.8.8.8", sizeof(dns));
            } else if (strcmp(line, "router") == 0 && router_dns(gw, sizeof(gw))) {
                strlcpy(dns, gw, sizeof(dns));
            } else {
                strlcpy(dns, line, sizeof(dns));
            }
        }

        espix_printf(s, "tunnel network (random|an address) [%s]: ", subnet);
        if (s->read_line(s, "", line, sizeof(line)) > 0 && line[0] != 0) {
            if (strcmp(line, "random") == 0) {
                (void)espix_net_vpn_random_subnet(subnet, sizeof(subnet));
            } else {
                strlcpy(subnet, line, sizeof(subnet));
            }
        }

        espix_printf(s, "access for new clients (all|home|clients|espix) [%s]: ", access);
        if (s->read_line(s, "", line, sizeof(line)) > 0 && line[0] != 0) {
            strlcpy(access, line, sizeof(access));
        }
    }

    if (endpoint[0] == 0) {
        espix_eprintf(s, "vpn: clients dial an endpoint; set one with vpn setup endpoint=<name>\n");
        return 1;
    }

    (void)espix_net_vpn_conf_set("port", port);
    (void)espix_net_vpn_conf_set("endpoint", endpoint);
    (void)espix_net_vpn_conf_set("dns", dns);
    (void)espix_net_vpn_conf_set("subnet", subnet);
    (void)espix_net_vpn_conf_set("access", access);

    if (espix_net_vpn_ensure_keys() != ESP_OK) {
        espix_eprintf(s, "vpn: cannot keep a server key (try sudo)\n");
        return 1;
    }

    espix_printf(s, "vpn: port %s, endpoint %s, dns %s, network %s, access %s\n",
                 port, endpoint, dns, subnet, access);
    espix_printf(s, "vpn: now 'vpn up', then 'vpn add <name>'\n");
    return 0;
}

static int vpn_add(espix_session_t *s, const char *name, const char *access_arg)
{
    if (!espix_net_vpn_is_up()) {
        espix_eprintf(s, "vpn: wg0 is down; run vpn up first\n");
        return 1;
    }

    char access[16] = {0};
    if (access_arg != NULL && access_arg[0] != 0) {
        strlcpy(access, access_arg, sizeof(access));
    } else {
        char line[32];
        if (s->read_line(s, "access (all|home|clients|espix) [all]: ", line,
                         sizeof(line)) > 0 && line[0] != 0) {
            strlcpy(access, line, sizeof(access));
        }
    }
    if (access[0] == 0) {
        (void)espix_net_vpn_conf_get("access", access, sizeof(access));
    }
    if (access[0] == 0) {
        strlcpy(access, "all", sizeof(access));
    }

    char endpoint[128];
    if (espix_net_vpn_endpoint_get(endpoint, sizeof(endpoint)) != ESP_OK) {
        espix_eprintf(s, "vpn: set the endpoint first:\n"
                         "     vpn endpoint <your public name or address>\n");
        return 1;
    }

    char spub[48], priv[48], pub[48];
    if (espix_net_vpn_server_pubkey(spub, sizeof(spub)) != ESP_OK ||
        espix_net_vpn_keypair(priv, sizeof(priv), pub, sizeof(pub)) != ESP_OK) {
        espix_eprintf(s, "vpn: cannot make a keypair\n");
        return 1;
    }

    /* One level at a time: mkdir wants the parent to exist. */
    char dns[128];
    (void)espix_net_vpn_dns_get(dns, sizeof(dns));

    (void)mkdir("/etc/vpn", 0755);
    (void)mkdir("/etc/vpn/clients", 0755);

    char path[160];
    strlcpy(path, "/etc/vpn/clients/", sizeof(path));
    strlcat(path, name, sizeof(path));
    strlcat(path, ".conf", sizeof(path));

    /* A name that already has an address keeps it, so replacing a lost phone
     * does not move it; a new one takes the next free address in the tunnel
     * network. */
    char addr[32] = {0};
    if (espix_net_vpn_peer_addr_by_name(name, addr, sizeof(addr)) != ESP_OK) {
        if (espix_net_vpn_next_addr(addr, sizeof(addr)) != ESP_OK) {
            espix_eprintf(s, "vpn: the tunnel network is full\n");
            return 1;
        }
    }

    /* Every client gets one, as PiVPN does: it costs a second secret to carry
     * and it is what keeps a recording safe if Curve25519 ever falls. */
    char psk[48] = {0};
    (void)espix_net_vpn_psk(psk, sizeof(psk));

    (void)espix_net_vpn_peer_del(name);      /* the old key, if any */

    FILE *f = fopen(path, "w");
    if (f == NULL) {
        espix_eprintf(s, "vpn: cannot write %s (try sudo)\n", path);
        return 1;
    }
    char allowed[80], port[16];
    allowed_ips(access, allowed, sizeof(allowed));
    (void)espix_net_vpn_conf_get("port", port, sizeof(port));

    fprintf(f, "[Interface]\n");
    fprintf(f, "Address = %s/32\n", addr);
    fprintf(f, "PrivateKey = %s\n", priv);
    if (psk[0] != 0) {
        fprintf(f, "PresharedKey = %s\n", psk);
    }
    fprintf(f, "DNS = %s\n", dns);
    fprintf(f, "\n[Peer]\n");
    fprintf(f, "PublicKey = %s\n", spub);
    fprintf(f, "Endpoint = %s:%s\n", endpoint, port);
    fprintf(f, "AllowedIPs = %s\n", allowed);
    fprintf(f, "PersistentKeepalive = 25\n");
    fclose(f);
    (void)espix_fs_ensure_mode(path, 0600);   /* it holds a private key */

    if (espix_net_vpn_peer_add(name, pub, addr, psk, access) != ESP_OK) {
        espix_eprintf(s, "vpn: wrote %s but did not admit the peer\n", path);
        return 1;
    }

    espix_printf(s, "vpn: %s is %s (%s), in %s\n", name, addr, access, path);
    return 0;
}

/*
 * The QR goes through the session, not printf: the command runs on a session
 * task over SSH or the console, and the only writer that knows where that is
 * is espix_printf. Two modules per cell with the upper half block, so a 57x57
 * code fits an 80-column terminal.
 */
static espix_session_t *s_qr_sess;

static void qr_display(esp_qrcode_handle_t q)
{
    const int n     = esp_qrcode_get_size(q);
    const int quiet = 4;          /* the spec's quiet zone, which scanners need */

    /* Characters only, two rows per line: the upper half block, the lower, the
     * full block, or a space -- what qrencode -t UTF8 does. No colours and no
     * escape sequences, because a terminal that mishandles them turns a QR into
     * a solid block, and because a row then fits one espix_printf() call and
     * there is no chunking to leave anything behind. */
    for (int y = -quiet; y < n + quiet; y += 2) {
        char row[768];
        int  o = 0;

        for (int x = -quiet; x < n + quiet; x++) {
            const bool up = esp_qrcode_get_module(q, x, y);
            const bool lo = esp_qrcode_get_module(q, x, y + 1);

            /* The glyph is the LIGHT module and the dark ones are left as the
             * terminal's background: a screen is dark by default, so drawing
             * the dark modules is what inverts the code. */
            if (up && lo) {
                row[o++] = ' ';                                             /* both dark */
            } else if (up) {
                row[o++] = (char)0xE2; row[o++] = (char)0x96; row[o++] = (char)0x84;   /* lower light */
            } else if (lo) {
                row[o++] = (char)0xE2; row[o++] = (char)0x96; row[o++] = (char)0x80;   /* upper light */
            } else {
                row[o++] = (char)0xE2; row[o++] = (char)0x96; row[o++] = (char)0x88;   /* both light */
            }
            if (o > (int)sizeof(row) - 4) {
                break;
            }
        }
        row[o] = 0;
        espix_printf(s_qr_sess, "%s\n", row);
    }
}

static int vpn_qr(espix_session_t *s, const char *name)
{
    char path[160];
    strlcpy(path, "/etc/vpn/clients/", sizeof(path));
    strlcat(path, name, sizeof(path));
    strlcat(path, ".conf", sizeof(path));

    FILE *f = fopen(path, "r");
    if (f == NULL) {
        espix_eprintf(s, "vpn: no %s\n", path);
        return 1;
    }

    char text[512];
    const size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = 0;
    if (n == 0) {
        espix_eprintf(s, "vpn: %s is empty\n", path);
        return 1;
    }

    esp_qrcode_config_t cfg = {
        .display_func      = qr_display,
        .max_qrcode_version = 20,
        .qrcode_ecc_level  = ESP_QRCODE_ECC_LOW,
    };

    s_qr_sess = s;
    const esp_err_t e = esp_qrcode_generate(&cfg, text);
    s_qr_sess = NULL;

    if (e != ESP_OK) {
        espix_eprintf(s, "vpn: cannot encode %s (%s)\n", path,
                      esp_err_to_name(e));
        return 1;
    }
    espix_printf(s, "vpn: scan that with the WireGuard app\n");
    return 0;
}

static int cmd_vpn(espix_session_t *s, int argc, char **argv)
{
    if (argc == 1 || strcmp(argv[1], "status") == 0) {
        if (!espix_net_vpn_is_up()) {
            espix_printf(s, "vpn: wg0 is down\n");
            return 0;
        }

        espix_printf(s, "vpn: wg0 is up\n");
        const int n = espix_net_vpn_peer_count();
        for (int i = 0; i < n; i++) {
            char       endpoint[48] = {0};
            const bool live = espix_net_vpn_peer_session(i, endpoint,
                                                         sizeof(endpoint));
            espix_printf(s, "vpn:   %s %s %s%s\n",
                         espix_net_vpn_peer_name(i),
                         espix_net_vpn_peer_addr(i),
                         live ? "up " : "no session ",
                         live ? endpoint : "");
        }
        if (n == 0) {
            espix_printf(s, "vpn:   no peers\n");
        }
        return 0;
    }

    if (strcmp(argv[1], "up") == 0) {
        const esp_err_t e = espix_net_vpn_up();
        if (e != ESP_OK) {
            espix_eprintf(s, "vpn: up: %s\n", esp_err_to_name(e));
            return 1;
        }
        espix_printf(s, "vpn: wg0 up\n");
        return 0;
    }

    if (strcmp(argv[1], "down") == 0) {
        const esp_err_t e = espix_net_vpn_down();
        if (e != ESP_OK) {
            espix_eprintf(s, "vpn: down: %s\n", esp_err_to_name(e));
            return 1;
        }
        espix_printf(s, "vpn: wg0 down\n");
        return 0;
    }

    if (strcmp(argv[1], "endpoint") == 0) {
        if (argc < 3) {
            char            ep[128];
            const esp_err_t e = espix_net_vpn_endpoint_get(ep, sizeof(ep));
            if (e == ESP_OK) {
                espix_printf(s, "vpn: endpoint %s\n", ep);
                return 0;
            }
            if (e != ESP_ERR_NOT_FOUND) {
                /* It holds a private key, so it is 0600: not being able to read
                 * it is not the same as it being unset. */
                espix_eprintf(s, "vpn: cannot read /etc/vpn.conf (try sudo)\n");
                return 1;
            }
            espix_eprintf(s, "vpn: no endpoint yet; "
                             "vpn endpoint <name or address>\n");
            return 1;
        }
        if (espix_net_vpn_endpoint_set(argv[2]) != ESP_OK) {
            espix_eprintf(s, "vpn: cannot write the config (try sudo)\n");
            return 1;
        }
        espix_printf(s, "vpn: endpoint %s\n", argv[2]);
        return 0;
    }

    if (strcmp(argv[1], "qr") == 0) {
        if (argc < 3) {
            espix_eprintf(s, "usage: vpn qr <name>\n");
            return 1;
        }
        return vpn_qr(s, argv[2]);
    }

    if (strcmp(argv[1], "dns") == 0) {
        if (argc < 3) {
            char dns[128];
            (void)espix_net_vpn_dns_get(dns, sizeof(dns));
            espix_printf(s, "vpn: dns %s\n", dns);
            return 0;
        }
        if (strcmp(argv[2], "public") == 0) {
            (void)espix_net_vpn_dns_set("1.1.1.1, 8.8.8.8");
            espix_printf(s, "vpn: dns 1.1.1.1, 8.8.8.8\n");
            return 0;
        }
        if (strcmp(argv[2], "router") == 0) {
            char addr[24];
            if (!router_dns(addr, sizeof(addr))) {
                espix_eprintf(s, "vpn: no default route to take an address from\n");
                return 1;
            }
            (void)espix_net_vpn_dns_set(addr);
            espix_printf(s, "vpn: dns %s\n", addr);
            return 0;
        }
        if (espix_net_vpn_dns_set(argv[2]) != ESP_OK) {
            espix_eprintf(s, "vpn: cannot write the config (try sudo)\n");
            return 1;
        }
        espix_printf(s, "vpn: dns %s\n", argv[2]);
        return 0;
    }

    if (strcmp(argv[1], "list") == 0) {
        DIR *d = opendir("/etc/vpn/clients");
        int  n = 0;
        if (d != NULL) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                char *dot = strstr(e->d_name, ".conf");
                if (dot == NULL) {
                    continue;
                }
                *dot = 0;

                char       addr[32] = {0};
                const bool known =
                    espix_net_vpn_peer_addr_by_name(e->d_name, addr,
                                                    sizeof(addr)) == ESP_OK;
                espix_printf(s, "vpn:   %s %s\n", e->d_name,
                             known ? addr : "(not admitted)");
                n++;
            }
            closedir(d);
        }
        if (n == 0) {
            espix_printf(s, "vpn:   no clients\n");
        }
        return 0;
    }

    if (strcmp(argv[1], "remove") == 0) {
        if (argc < 3) {
            espix_eprintf(s, "usage: vpn remove <name>\n");
            return 1;
        }

        char path[160];
        strlcpy(path, "/etc/vpn/clients/", sizeof(path));
        strlcat(path, argv[2], sizeof(path));
        strlcat(path, ".conf", sizeof(path));

        const esp_err_t e = espix_net_vpn_peer_del(argv[2]);
        const int       gone = unlink(path);
        if (e != ESP_OK && gone != 0) {
            espix_eprintf(s, "vpn: no client named %s\n", argv[2]);
            return 1;
        }
        espix_printf(s, "vpn: removed %s\n", argv[2]);
        return 0;
    }

    if (strcmp(argv[1], "setup") == 0) {
        return vpn_setup(s, argc, argv);
    }

    if (strcmp(argv[1], "add") == 0) {
        if (argc < 3) {
            espix_eprintf(s, "usage: vpn add <name> [all|home|clients|espix]\n");
            return 1;
        }
        return vpn_add(s, argv[2], (argc >= 4) ? argv[3] : NULL);
    }

    espix_eprintf(s, "usage: vpn [up|down|status|add <name>|endpoint [host]]\n");
    return 1;
}

static espix_cmd_t s_vpn_cmds[] = {
    { .name = "vpn", .fn = cmd_vpn,
      .help = "the WireGuard server",
      .usage = "vpn [setup|up|down|status|list|remove <name>|add <name> [access]|qr <name>|endpoint [host]|dns [public|router|list]]" },
};

void espix_cmds_register_vpn(void)
{
    espix_cmds_register_table(s_vpn_cmds,
                              sizeof(s_vpn_cmds) / sizeof(s_vpn_cmds[0]));
}
