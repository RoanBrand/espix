/*
 * vpn -- the WireGuard server.
 *
 * PiVPN's shape: a few verbs over a config, so a client is a command away
 * rather than a file to hand-edit. This is the first cut -- up, down, status,
 * which makes the interface reachable; the client verbs (add, list, qr,
 * remove) arrive with key generation.
 */

#include <stdio.h>
#include <string.h>

#include "espix_cmds_priv.h"
#include "espix_net_vpn.h"

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

    espix_eprintf(s, "usage: vpn [up|down|status]\n");
    return 1;
}

static espix_cmd_t s_vpn_cmds[] = {
    { .name = "vpn", .fn = cmd_vpn,
      .help = "the WireGuard server",
      .usage = "vpn [up|down|status]" },
};

void espix_cmds_register_vpn(void)
{
    espix_cmds_register_table(s_vpn_cmds,
                              sizeof(s_vpn_cmds) / sizeof(s_vpn_cmds[0]));
}
