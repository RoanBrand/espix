/*
 * bluetoothctl: the names Linux uses, over espix's own Bluetooth layer.
 */
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#include "espix_cmds_priv.h"
#include "espix_shell.h"
#include "espix_bt.h"

#if CONFIG_ESPIX_BT

static void bt_devices(espix_session_t *s)
{
    espix_bt_dev_t devs[32];
    const size_t n = espix_bt_devices(devs, 32);

    if (n == 0) {
        espix_printf(s, "no devices yet; run 'bluetoothctl scan on'\n");
        return;
    }

    char bda[24];
    for (size_t i = 0; i < n; i++) {
        espix_printf(s, "%-18s %-7s %-6s %s\n",
                     espix_bt_bdastr(devs[i].bda, bda, sizeof(bda)),
                     devs[i].bonded ? "bonded" : "",
                     devs[i].connected ? "a2dp" : "",
                     devs[i].name[0] ? devs[i].name : "(unknown)");
    }
}

static int bt_addr_arg(espix_session_t *s, int argc, char **argv,
                       uint8_t out[ESPIX_BDA_LEN])
{
    if (argc < 3) {
        espix_eprintf(s, "usage: bluetoothctl %s <addr>\n", argv[1]);
        return 1;
    }
    if (espix_bt_parse_bda(argv[2], out) != ESP_OK) {
        espix_eprintf(s, "bluetoothctl: bad address '%s'\n", argv[2]);
        return 1;
    }
    return 0;
}

static int cmd_bt(espix_session_t *s, int argc, char **argv)
{
    const char *sub = (argc > 1) ? argv[1] : "devices";
    uint8_t bda[ESPIX_BDA_LEN];

    if (strcmp(sub, "power") == 0) {
        const char *arg = (argc > 2) ? argv[2] : "on";
        if (strcmp(arg, "off") == 0) {
            const esp_err_t err = espix_bt_shutdown();
            if (err != ESP_OK) {
                espix_eprintf(s, "bluetoothctl: %s\n", esp_err_to_name(err));
                return 1;
            }
            espix_printf(s, "Controller off\n");
            return 0;
        }
        const esp_err_t err = espix_bt_init();
        if (err != ESP_OK) {
            espix_eprintf(s, "bluetoothctl: %s\n", esp_err_to_name(err));
            return 1;
        }
        espix_printf(s, "Controller %s\n", espix_bt_ready() ? "on" : "off");
        return 0;
    }

    if (strcmp(sub, "scan") == 0) {
        const char *arg = (argc > 2) ? argv[2] : "on";
        const esp_err_t ierr = espix_bt_init();
        if (ierr != ESP_OK) {
            if (ierr == ESP_ERR_NOT_SUPPORTED) {
                espix_eprintf(s, "bluetoothctl: Bluetooth is not built into this image\n");
            } else {
                espix_eprintf(s, "bluetoothctl: Bluetooth did not start: %s\n",
                              esp_err_to_name(ierr));
            }
            return 1;
        }
        const esp_err_t err = espix_bt_scan(strcmp(arg, "off") != 0);
        if (err != ESP_OK) {
            espix_eprintf(s, "bluetoothctl: scan: %s\n", esp_err_to_name(err));
            return 1;
        }
        espix_printf(s, "Discovery %s\n",
                     strcmp(arg, "off") == 0 ? "stopped" : "started");
        return 0;
    }

    if (strcmp(sub, "devices") == 0 || strcmp(sub, "list") == 0) {
        bt_devices(s);
        return 0;
    }

    /*
     * The SBC quality dial, for A/B-ing an artefact by ear without a rebuild.
     * It only changes the *next* codec negotiation, so reconnect afterwards.
     */
    if (strcmp(sub, "quality") == 0) {
        if (argc > 2) {
            espix_bt_set_sbc_quality(atoi(argv[2]));
            if (espix_bt_ready()) {
                /*
                 * The dial only applies to a new codec negotiation, and a plain
                 * A2DP disconnect leaves the sink free to re-establish with the
                 * old configuration -- which is why changing it needed a reboot.
                 * Take the controller down; `connect` calls espix_bt_init again.
                 */
                (void)espix_bt_shutdown();
                espix_printf(s, "controller restarted for a fresh negotiation\n");
            }
        }
        espix_printf(s, "sbc quality %d: %s\n", espix_bt_sbc_quality(),
                     espix_bt_sbc_quality() == 0 ? "mono, bitpool <= 35" :
                     espix_bt_sbc_quality() == 1 ? "joint/stereo, bitpool <= 35" :
                                                   "joint stereo, bitpool <= 52");
        espix_printf(s, "reconnect for it to take effect\n");
        return 0;
    }

    if (strcmp(sub, "info") == 0) {
        if (bt_addr_arg(s, argc, argv, bda) != 0) {
            return 1;
        }
        espix_bt_dev_t d;
        if (espix_bt_info(bda, &d) != ESP_OK) {
            espix_eprintf(s, "bluetoothctl: %s: not seen (scan first)\n", argv[2]);
            return 1;
        }
        char b[24];
        espix_printf(s, "Device %s\n", espix_bt_bdastr(d.bda, b, sizeof(b)));
        espix_printf(s, "  Name:   %s\n", d.name[0] ? d.name : "(unknown)");
        espix_printf(s, "  Bonded: %s\n", d.bonded ? "yes" : "no");
        espix_printf(s, "  A2DP:   %s\n", d.connected ? "connected" : "no");
        return 0;
    }

    if (strcmp(sub, "pair") == 0 || strcmp(sub, "connect") == 0) {
        if (bt_addr_arg(s, argc, argv, bda) != 0) {
            return 1;
        }
        if (espix_bt_init() != ESP_OK) {
            espix_eprintf(s, "bluetoothctl: Bluetooth is not up\n");
            return 1;
        }

        /* Say so rather than issuing a request the A2DP state machine drops. */
        espix_bt_dev_t cur;
        if (espix_bt_a2d_connected() && espix_bt_info(bda, &cur) == ESP_OK && cur.connected) {
            espix_printf(s, "%s is already connected\n", argv[2]);
            return 0;
        }

        const esp_err_t err = (sub[1] == 'a')
                                  ? espix_bt_pair(bda)
                                  : espix_bt_connect(bda);
        if (err != ESP_OK) {
            espix_eprintf(s, "bluetoothctl: %s: %s\n", sub, esp_err_to_name(err));
            return 1;
        }
        espix_printf(s, "%s: connecting to %s; watch 'bluetoothctl devices'\n",
                     sub, argv[2]);
        return 0;
    }

    if (strcmp(sub, "disconnect") == 0) {
        if (bt_addr_arg(s, argc, argv, bda) != 0) {
            return 1;
        }
        const esp_err_t err = espix_bt_disconnect(bda);
        if (err != ESP_OK) {
            espix_eprintf(s, "bluetoothctl: %s\n", esp_err_to_name(err));
            return 1;
        }
        return 0;
    }

    if (strcmp(sub, "remove") == 0) {
        if (bt_addr_arg(s, argc, argv, bda) != 0) {
            return 1;
        }
        const esp_err_t err = espix_bt_remove(bda);
        if (err != ESP_OK) {
            espix_eprintf(s, "bluetoothctl: %s\n", esp_err_to_name(err));
            return 1;
        }
        espix_printf(s, "removed %s\n", argv[2]);
        return 0;
    }

    if (strcmp(sub, "trust") == 0) {
        if (bt_addr_arg(s, argc, argv, bda) != 0) {
            return 1;
        }
        /* Bluedroid has no separate trust list: a bonded device is trusted. */
        espix_printf(s, "trust: bonded devices are already trusted\n");
        return 0;
    }

    if (strcmp(sub, "agent") == 0) {
        if (argc > 2 && strcmp(argv[2], "off") != 0) {
            if (espix_bt_set_pin(argv[2]) != ESP_OK) {
                espix_eprintf(s, "bluetoothctl: pin at most %d characters\n",
                              ESPIX_BT_PIN_MAX - 1);
                return 1;
            }
        }
        espix_printf(s, "Agent is on; pin \"%s\", SSP auto-accept\n",
                     espix_bt_pin());
        return 0;
    }

    if (strcmp(sub, "help") == 0) {
        espix_printf(s, "power              bring the controller up\n");
        espix_printf(s, "scan on|off        classic inquiry\n");
        espix_printf(s, "devices            discovered and bonded devices\n");
        espix_printf(s, "info <addr>        details for one\n");
        espix_printf(s, "pair|connect <addr>  bring up the A2DP link\n");
        espix_printf(s, "disconnect <addr>  drop it\n");
        espix_printf(s, "remove <addr>      forget a bond\n");
        espix_printf(s, "agent [pin]        pairing policy (default pin 0000)\n");
        return 0;
    }

    espix_eprintf(s, "usage: bluetoothctl {power|scan {on|off}|devices|"
                     "info|pair|connect|disconnect|remove|trust|agent|help}\n");
    return 1;
}

static espix_cmd_t s_bt_cmds[] = {
    { .name = "bluetoothctl", .fn = cmd_bt,
      .help = "Bluetooth: power, scan, devices, pairing, A2DP",
      .usage = "bluetoothctl {power|scan {on|off}|devices|info|pair|connect|"
               "disconnect|remove|trust|agent}" },
};

#endif /* CONFIG_ESPIX_BT */

void espix_cmds_register_bt(void)
{
#if CONFIG_ESPIX_BT
    espix_cmds_register_table(s_bt_cmds,
                              sizeof(s_bt_cmds) / sizeof(s_bt_cmds[0]));
#endif
}
