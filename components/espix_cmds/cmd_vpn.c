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

#include "qrcode.h"

#include "espix_cmds_priv.h"
#include "espix_fs.h"
#include "espix_net.h"
#include "espix_net_vpn.h"

/* A client is a keypair, a config file, and a peer with no endpoint. The
 * address is the next free one in 10.6.0.0/24; the peer is what makes the
 * server accept that key. */
static int vpn_add(espix_session_t *s, const char *name)
{
    if (!espix_net_vpn_is_up()) {
        espix_eprintf(s, "vpn: wg0 is down; run vpn up first\n");
        return 1;
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
     * does not move it; a new one takes the next free address. */
    char addr[32] = {0};
    if (espix_net_vpn_peer_addr_by_name(name, addr, sizeof(addr)) != ESP_OK) {
        int n = 0;
        DIR *d = opendir("/etc/vpn/clients");
        if (d != NULL) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                if (strstr(e->d_name, ".conf") != NULL) {
                    n++;
                }
            }
            closedir(d);
        }
        snprintf(addr, sizeof(addr), "10.6.0.%d", n + 2);
    }

    (void)espix_net_vpn_peer_del(name);      /* the old key, if any */

    FILE *f = fopen(path, "w");
    if (f == NULL) {
        espix_eprintf(s, "vpn: cannot write %s (try sudo)\n", path);
        return 1;
    }
    fprintf(f, "[Interface]\n");
    fprintf(f, "Address = %s/32\n", addr);
    fprintf(f, "PrivateKey = %s\n", priv);
    fprintf(f, "DNS = %s\n", dns);
    fprintf(f, "\n[Peer]\n");
    fprintf(f, "PublicKey = %s\n", spub);
    fprintf(f, "Endpoint = %s:51820\n", endpoint);
    fprintf(f, "AllowedIPs = 0.0.0.0/0, ::/0\n");
    fprintf(f, "PersistentKeepalive = 25\n");
    fclose(f);
    (void)espix_fs_ensure_mode(path, 0600);   /* it holds a private key */

    if (espix_net_vpn_peer_add(name, pub, addr) != ESP_OK) {
        espix_eprintf(s, "vpn: wrote %s but did not admit the peer\n", path);
        return 1;
    }

    espix_printf(s, "vpn: %s is %s, in %s\n", name, addr, path);
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
            char ep[128];
            if (espix_net_vpn_endpoint_get(ep, sizeof(ep)) == ESP_OK) {
                espix_printf(s, "vpn: endpoint %s\n", ep);
                return 0;
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
            uint32_t gw = 0;
            if (!espix_net_default_route(NULL, 0, &gw) || gw == 0) {
                espix_eprintf(s, "vpn: no default route to take an address from\n");
                return 1;
            }
            char addr[32];
            snprintf(addr, sizeof(addr), "%u.%u.%u.%u",
                     (unsigned)(gw & 0xff), (unsigned)((gw >> 8) & 0xff),
                     (unsigned)((gw >> 16) & 0xff), (unsigned)((gw >> 24) & 0xff));
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

    if (strcmp(argv[1], "add") == 0) {
        if (argc < 3) {
            espix_eprintf(s, "usage: vpn add <name>\n");
            return 1;
        }
        return vpn_add(s, argv[2]);
    }

    espix_eprintf(s, "usage: vpn [up|down|status|add <name>|endpoint [host]]\n");
    return 1;
}

static espix_cmd_t s_vpn_cmds[] = {
    { .name = "vpn", .fn = cmd_vpn,
      .help = "the WireGuard server",
      .usage = "vpn [up|down|status|list|remove <name>|add <name>|qr <name>|endpoint [host]|dns [public|router|list]]" },
};

void espix_cmds_register_vpn(void)
{
    espix_cmds_register_table(s_vpn_cmds,
                              sizeof(s_vpn_cmds) / sizeof(s_vpn_cmds[0]));
}
