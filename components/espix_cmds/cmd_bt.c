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
        espix_printf(s, "%-18s %-7s %s\n",
                     espix_bt_bdastr(devs[i].bda, bda, sizeof(bda)),
                     devs[i].bonded ? "bonded" : "",
                     devs[i].name[0] ? devs[i].name : "(unknown)");
    }
}

static int cmd_bt(espix_session_t *s, int argc, char **argv)
{
    const char *sub = (argc > 1) ? argv[1] : "devices";

    if (strcmp(sub, "power") == 0) {
        const char *arg = (argc > 2) ? argv[2] : "on";
        if (strcmp(arg, "off") == 0) {
            espix_eprintf(s, "bluetoothctl: power off is not wired yet\n");
            return 1;
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

    if (strcmp(sub, "info") == 0) {
        if (argc < 3) {
            espix_eprintf(s, "usage: bluetoothctl info <addr>\n");
            return 1;
        }
        uint8_t bda[ESPIX_BDA_LEN];
        if (espix_bt_parse_bda(argv[2], bda) != ESP_OK) {
            espix_eprintf(s, "bluetoothctl: bad address '%s'\n", argv[2]);
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
        espix_printf(s, "power        bring the controller up\n");
        espix_printf(s, "scan on|off  classic inquiry\n");
        espix_printf(s, "devices      discovered and bonded devices\n");
        espix_printf(s, "info <addr>  details for one\n");
        espix_printf(s, "agent [pin]  pairing policy (default pin 0000)\n");
        return 0;
    }

    espix_eprintf(s, "usage: bluetoothctl {power|scan {on|off}|devices|"
                     "info <addr>|agent [pin]|help}\n");
    return 1;
}

static espix_cmd_t s_bt_cmds[] = {
    { .name = "bluetoothctl", .fn = cmd_bt,
      .help = "Bluetooth: power, scan, devices, pairing",
      .usage = "bluetoothctl {power|scan {on|off}|devices|info <addr>|agent [pin]}" },
};

#endif /* CONFIG_ESPIX_BT */

void espix_cmds_register_bt(void)
{
#if CONFIG_ESPIX_BT
    espix_cmds_register_table(s_bt_cmds,
                              sizeof(s_bt_cmds) / sizeof(s_bt_cmds[0]));
#endif
}
