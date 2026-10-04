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

#include "espix_cmds_priv.h"
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
    (void)mkdir("/etc/vpn", 0755);
    (void)mkdir("/etc/vpn/clients", 0755);

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

    char path[160], addr[32];
    strlcpy(path, "/etc/vpn/clients/", sizeof(path));
    strlcat(path, name, sizeof(path));
    strlcat(path, ".conf", sizeof(path));
    snprintf(addr, sizeof(addr), "10.6.0.%d", n + 2);

    FILE *f = fopen(path, "w");
    if (f == NULL) {
        espix_eprintf(s, "vpn: cannot write %s (try sudo)\n", path);
        return 1;
    }
    fprintf(f, "[Interface]\n");
    fprintf(f, "Address = %s/32\n", addr);
    fprintf(f, "PrivateKey = %s\n", priv);
    fprintf(f, "DNS = 1.1.1.1, 8.8.8.8\n");
    fprintf(f, "\n[Peer]\n");
    fprintf(f, "PublicKey = %s\n", spub);
    fprintf(f, "Endpoint = %s:51820\n", endpoint);
    fprintf(f, "AllowedIPs = 0.0.0.0/0, ::/0\n");
    fprintf(f, "PersistentKeepalive = 25\n");
    fclose(f);

    if (espix_net_vpn_peer_add(pub, addr) != ESP_OK) {
        espix_eprintf(s, "vpn: wrote %s but did not admit the peer\n", path);
        return 1;
    }

    espix_printf(s, "vpn: %s is %s, in %s\n", name, addr, path);
    return 0;
}

static int cmd_vpn(espix_session_t *s, int argc, char **argv)
{
    if (argc == 1 || strcmp(argv[1], "status") == 0) {
        espix_printf(s, "vpn: wg0 is %s\n",
                     espix_net_vpn_is_up() ? "up" : "down");
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
      .usage = "vpn [up|down|status|add <name>|endpoint [host]]" },
};

void espix_cmds_register_vpn(void)
{
    espix_cmds_register_table(s_vpn_cmds,
                              sizeof(s_vpn_cmds) / sizeof(s_vpn_cmds[0]));
}
