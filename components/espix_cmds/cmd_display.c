/*
 * vnc: the display service's front door.
 *
 * Nothing is brought up at boot. Starting the server is what allocates the
 * canvas and starts the desktop task, and stopping it gives all of that back --
 * the same laziness Bluetooth and audio already have, for the same reason: a
 * board that never opens a remote screen should not be paying for one.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "espix_cmds_priv.h"
#include "espix_display.h"
#include "espix_net.h"
#include "espix_shell.h"

#define VNC_DEFAULT_PORT 5900

/*
 * The first interface a client could actually reach: not loopback, and with an
 * address. Loopback is excluded because "connect to 127.0.0.1:5900" is not a
 * useful thing to print at someone holding a laptop.
 */
static bool reachable_addr(char *out, size_t len)
{
    espix_ifinfo_t ifs[8];
    const size_t   n = espix_net_iflist(ifs, sizeof(ifs) / sizeof(ifs[0]));

    for (size_t i = 0; i < n; i++) {
        if (ifs[i].kind == ESPIX_IF_LO || !ifs[i].has_addr || ifs[i].ip == 0) {
            continue;
        }
        char buf[ESPIX_IP4STR_MAX];
        espix_net_ip4str(ifs[i].ip, buf, sizeof(buf));
        snprintf(out, len, "%s", buf);
        return true;
    }
    return false;
}

static void print_reach(espix_session_t *s, uint16_t port)
{
    char addr[ESPIX_IP4STR_MAX];

    if (reachable_addr(addr, sizeof(addr))) {
        espix_printf(s, "vnc: point a VNC client at %s:%u\n", addr, (unsigned)port);
    } else {
        espix_printf(s, "vnc: no address yet -- bring wlan0 up, then connect\n");
    }
}

/* How authentication is configured, for status and for the start warning. */
static const char *auth_str(void)
{
    if (!espix_display_vnc_auth_required()) {
        return "off";
    }
    return espix_display_vnc_auth_is_default()
               ? "VNC, built-in default (" ESPIX_DISPLAY_VNC_DEFAULT_PASSWORD ")"
               : "VNC, custom password";
}

static int cmd_vnc(espix_session_t *s, int argc, char **argv)
{
    const char *sub = (argc > 1) ? argv[1] : NULL;

    if (sub == NULL || strcmp(sub, "status") == 0) {
        if (!espix_display_vnc_running()) {
            espix_printf(s, "vnc: stopped (authentication %s)\n", auth_str());
            return 0;
        }
        const uint16_t port = espix_display_vnc_port();
        espix_printf(s, "vnc: listening on port %u, %d client(s)\n",
                     (unsigned)port, espix_display_vnc_clients());
        if (espix_display_vnc_clients() > 0) {
            espix_printf(s, "vnc: connected: %s\n", espix_display_vnc_peer());
        }
        espix_printf(s, "vnc: authentication: %s\n", auth_str());
        print_reach(s, port);
        return 0;
    }

    if (strcmp(sub, "start") == 0) {
        uint16_t port = VNC_DEFAULT_PORT;

        if (argc > 2) {
            const int p = atoi(argv[2]);
            if (p < 1 || p > 65535) {
                espix_eprintf(s, "vnc: %s: not a port\n", argv[2]);
                return 1;
            }
            port = (uint16_t)p;
        }

        const esp_err_t err = espix_display_vnc_start(port);
        if (err == ESP_ERR_INVALID_STATE) {
            espix_eprintf(s, "vnc: already listening on port %u\n",
                          (unsigned)espix_display_vnc_port());
            return 1;
        }
        if (err != ESP_OK) {
            espix_eprintf(s, "vnc: cannot start: %s\n", esp_err_to_name(err));
            return 1;
        }

        espix_printf(s, "vnc: listening on port %u\n", (unsigned)port);
        if (espix_display_vnc_auth_is_default()) {
            espix_printf(s, "vnc: using the built-in password '%s'; a client "
                            "that asks for one gets it, and TigerVNC and "
                            "RealVNC are offered none. Change it with "
                            "'vnc password <pw>'\n",
                         ESPIX_DISPLAY_VNC_DEFAULT_PASSWORD);
        }
        print_reach(s, port);
        return 0;
    }

    if (strcmp(sub, "stop") == 0) {
        if (!espix_display_vnc_running()) {
            espix_printf(s, "vnc: already stopped\n");
            return 0;
        }
        espix_display_vnc_stop();
        espix_printf(s, "vnc: stopped\n");
        return 0;
    }

    /*
     * The password applies to the next connection, so it can be set or cleared
     * while the server is running.
     */
    if (strcmp(sub, "password") == 0) {
        if (argc <= 2) {
            espix_eprintf(s, "vnc: password needs a value\n");
            return 1;
        }
        espix_display_vnc_set_password(argv[2]);
        if (strlen(argv[2]) > 8) {
            espix_printf(s, "vnc: note: VNC uses only the first 8 characters\n");
        }
        espix_printf(s, "vnc: authentication required from now on\n");
        return 0;
    }

    if (strcmp(sub, "nopassword") == 0) {
        espix_display_vnc_set_password(NULL);
        espix_printf(s, "vnc: authentication off for this boot "
                        "(the built-in default returns at the next one)\n");
        return 0;
    }

    espix_eprintf(s, "usage: vnc [start [port] | stop | status | "
                     "password <pw> | nopassword]\n");
    return 1;
}

static espix_cmd_t s_display_cmds[] = {
    { .name = "vnc", .fn = cmd_vnc,
      .help = "serve the desktop over RFB, so any VNC client is the monitor",
      .usage = "vnc [start [port] | stop | status | password <pw> | nopassword]" },
};

void espix_cmds_register_display(void)
{
    espix_cmds_register_table(s_display_cmds,
                              sizeof(s_display_cmds) / sizeof(s_display_cmds[0]));
}
