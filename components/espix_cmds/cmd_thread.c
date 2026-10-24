/*
 * thread: IEEE 802.15.4 / OpenThread, espix-shaped.
 *
 * This is the espix command, not OpenThread's CLI. Its verbs map to the stack's
 * own API through espix_thread_*(), which is what keeps their output on the
 * invoking session -- a remote shell over SSH sees the same answer the console
 * would. OpenThread's full CLI is still one call away as \`thread cli <line>\`;
 * that path runs on the OT task and writes to the console, so it is a debugging
 * aid rather than the way a script should drive Thread.
 */
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#include "espix_cmds_priv.h"
#include "espix_shell.h"
#include "espix_thread.h"

#if CONFIG_ESPIX_THREAD

static void thread_show_status(espix_session_t *s)
{
    espix_thread_status_t st;
    if (espix_thread_status(&st) != ESP_OK) {
        espix_eprintf(s, "thread: no status\n");
        return;
    }

    if (!st.started) {
        espix_printf(s, "thread0: down (stack not started)\n");
        espix_printf(s, "  run 'thread start', or set enabled=yes in /etc/thread.conf\n");
        return;
    }

    espix_printf(s, "thread0: %s  role %s%s\n",
                 st.if_up ? "UP" : "DOWN",
                 st.role,
                 st.attached ? "" : " (not attached)");
    espix_printf(s, "  ipv6        %s\n", st.ip6_enabled ? "enabled" : "disabled");
    if (st.attached) {
        espix_printf(s, "  rloc16      0x%04x\n", (unsigned)st.rloc16);
    }
    espix_printf(s, "  border      %s\n", st.border_router ? "on" : "off");
    for (size_t i = 0; i < st.n_addrs; i++) {
        espix_printf(s, "  %s%s\n", i == 0 ? "inet6      " : "           ",
                     st.addrs[i]);
    }
    if (st.n_addrs == 0) {
        espix_printf(s, "  inet6       (none yet)\n");
    }
}

static int cmd_thread(espix_session_t *s, int argc, char **argv)
{
    if (argc < 2) {
        espix_printf(s, "usage: thread {status|start|stop|attach|br {on|off}|"
                        "cli <line...>}\n");
        espix_printf(s, "  status   what thread0 is doing\n");
        espix_printf(s, "  start    bring the OpenThread stack up (native 15.4 radio)\n");
        espix_printf(s, "  stop     take it down\n");
        espix_printf(s, "  attach   join the network in the stored dataset\n");
        espix_printf(s, "  br       border routing, if built in\n");
        espix_printf(s, "  cli      OpenThread's own CLI; replies go to the console\n");
        return argc < 2 ? 1 : 1;
    }

    if (strcmp(argv[1], "status") == 0) {
        thread_show_status(s);
        return 0;
    }

    if (strcmp(argv[1], "start") == 0) {
        const esp_err_t err = espix_thread_start();
        if (err != ESP_OK) {
            espix_eprintf(s, "thread: start failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        espix_printf(s, "thread0 up\n");
        return 0;
    }

    if (strcmp(argv[1], "stop") == 0) {
        const esp_err_t err = espix_thread_stop();
        if (err != ESP_OK) {
            espix_eprintf(s, "thread: stop failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        espix_printf(s, "thread0 down\n");
        return 0;
    }

    if (strcmp(argv[1], "attach") == 0) {
        const esp_err_t err = espix_thread_attach();
        if (err != ESP_OK) {
            espix_eprintf(s, "thread: attach failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        espix_printf(s, "thread: attaching\n");
        return 0;
    }

    if (strcmp(argv[1], "br") == 0) {
        if (argc < 3 || (strcmp(argv[2], "on") != 0 && strcmp(argv[2], "off") != 0)) {
            espix_eprintf(s, "usage: thread br {on|off}\n");
            return 1;
        }
        const bool on = strcmp(argv[2], "on") == 0;
        const esp_err_t err = espix_thread_border_router(on);
        if (err != ESP_OK) {
            espix_eprintf(s, "thread: border router %s: %s\n", argv[2],
                          esp_err_to_name(err));
            if (err == ESP_ERR_INVALID_STATE) {
                espix_eprintf(s, "  the Thread interface must be up: "
                                "'thread start' then 'thread attach'\n");
            }
            return 1;
        }
        espix_printf(s, "thread: border router %s\n", on ? "on" : "off");
        return 0;
    }

    if (strcmp(argv[1], "cli") == 0) {
        if (argc < 3) {
            espix_eprintf(s, "usage: thread cli <line...>\n");
            return 1;
        }
        char line[192];
        size_t used = 0;
        for (int i = 2; i < argc; i++) {
            const int n = snprintf(line + used, sizeof(line) - used, "%s%s",
                                   i == 2 ? "" : " ", argv[i]);
            if (n < 0 || (size_t)n >= sizeof(line) - used) {
                espix_eprintf(s, "thread: line too long\n");
                return 1;
            }
            used += (size_t)n;
        }
        const esp_err_t err = espix_thread_cli(line);
        if (err != ESP_OK) {
            espix_eprintf(s, "thread: cli: %s\n", esp_err_to_name(err));
            return 1;
        }
        espix_printf(s, "thread: '%s' sent; the reply appears on the console\n", line);
        return 0;
    }

    espix_eprintf(s, "thread: unknown verb '%s'\n", argv[1]);
    return 1;
}

static espix_cmd_t s_thread_cmds[] = {
    { .name = "thread", .fn = cmd_thread,
      .help = "Thread (IEEE 802.15.4): status, up/down, join, border router",
      .usage = "thread {status|start|stop|attach|br {on|off}|cli <line...>}" },
};

#endif /* CONFIG_ESPIX_THREAD */

void espix_cmds_register_thread(void)
{
#if CONFIG_ESPIX_THREAD
    espix_cmds_register_table(s_thread_cmds,
                              sizeof(s_thread_cmds) / sizeof(s_thread_cmds[0]));
#endif
}
