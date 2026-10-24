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
#include "espix_fs.h"
#include "espix_shell.h"
#include "espix_thread.h"

#if CONFIG_ESPIX_THREAD

/* The kernel creates this file on a board that has none; see espix_thread.c.
 * The dataset is not here -- OpenThread keeps it in its own NVS. */
#define THREAD_CONF_PATH "/etc/thread.conf"

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

static bool conf_yes(const char *key)
{
    char v[8];
    return espix_fs_conf_get(THREAD_CONF_PATH, key, v, sizeof(v)) &&
           strcmp(v, "yes") == 0;
}

static bool word_yes(const char *w)
{
    return strcmp(w, "yes") == 0 || strcmp(w, "on") == 0;
}

static bool word_no(const char *w)
{
    return strcmp(w, "no") == 0 || strcmp(w, "off") == 0;
}

static void thread_show_conf(espix_session_t *s)
{
    char v[32];

    espix_printf(s, "%s\n", THREAD_CONF_PATH);
    espix_printf(s, "  enabled        %s\n",
                 espix_fs_conf_get(THREAD_CONF_PATH, "enabled", v, sizeof(v))
                     ? v : "(absent: no)");
    espix_printf(s, "  border_router  %s\n",
                 espix_fs_conf_get(THREAD_CONF_PATH, "border_router", v, sizeof(v))
                     ? v : "(absent: no)");
    espix_printf(s, "  backbone       %s\n",
                 espix_fs_conf_get(THREAD_CONF_PATH, "backbone", v, sizeof(v))
                     ? v : "eth0");
    espix_printf(s, "  attach         %s\n",
                 espix_fs_conf_get(THREAD_CONF_PATH, "attach", v, sizeof(v))
                     ? v : "(absent: no)");
    espix_printf(s, "  (the dataset itself lives in OpenThread's NVS, not here)\n");
}

/*
 * thread conf [key value] -- persist the boot policy. Writing /etc is root's,
 * so this runs unchanged after sudo, the way usb/wifi writers do; the kernel's
 * first-run creation of the file is the privileged caller.
 *
 * enabled and border_router also move the live stack, so 'thread conf enabled
 * yes' is 'thread start' plus 'and do it again next boot'.
 */
static int thread_conf(espix_session_t *s, int argc, char **argv)
{
    if (argc < 3) {
        thread_show_conf(s);
        return 0;
    }

    const char *key = argv[2];

    bool enabled = conf_yes("enabled");
    bool border  = conf_yes("border_router");
    bool attach  = conf_yes("attach");
    char backbone[16] = "eth0";
    (void)espix_fs_conf_get(THREAD_CONF_PATH, "backbone", backbone,
                            sizeof(backbone));

    bool live_enable = false, want_enable = false;
    bool live_border = false, want_border = false;
    bool live_attach = false;

    if (strcmp(key, "backbone") == 0) {
        if (argc < 4) {
            espix_eprintf(s, "usage: thread conf backbone <interface>\n");
            return 1;
        }
        strlcpy(backbone, argv[3], sizeof(backbone));
    } else if (strcmp(key, "enabled") == 0 || strcmp(key, "border_router") == 0 ||
               strcmp(key, "attach") == 0) {
        if (argc < 4 || (!word_yes(argv[3]) && !word_no(argv[3]))) {
            espix_eprintf(s, "usage: thread conf %s {yes|no}\n", key);
            return 1;
        }
        const bool on = word_yes(argv[3]);
        if (strcmp(key, "enabled") == 0) {
            enabled = on;
            live_enable = true;
            want_enable = on;
        } else if (strcmp(key, "border_router") == 0) {
            border = on;
            live_border = true;
            want_border = on;
        } else {
            attach = on;
            live_attach = on;
        }
    } else {
        espix_eprintf(s, "thread: unknown key '%s'\n", key);
        espix_eprintf(s, "  keys: enabled, border_router, backbone, attach\n");
        return 1;
    }

    const esp_err_t werr = espix_thread_conf_write(enabled, border, backbone, attach);
    if (werr != ESP_OK) {
        espix_eprintf(s, "thread: cannot write %s: run it under sudo\n",
                      THREAD_CONF_PATH);
        return 1;
    }

    if (live_enable) {
        const esp_err_t err = want_enable ? espix_thread_start()
                                          : espix_thread_stop();
        if (err != ESP_OK) {
            espix_eprintf(s, "thread: %s: %s\n",
                          want_enable ? "start" : "stop", esp_err_to_name(err));
            return 1;
        }
    } else if (live_border) {
        const esp_err_t err = espix_thread_border_router(want_border);
        if (err != ESP_OK) {
            espix_eprintf(s, "thread: border router: %s\n", esp_err_to_name(err));
            if (err == ESP_ERR_INVALID_STATE) {
                espix_eprintf(s, "  the Thread interface must be up: "
                                "'thread conf attach yes' (or 'thread attach')\n");
            }
            return 1;
        }
    } else if (live_attach) {
        (void)espix_thread_attach();       /* join now; status reports the rest */
    }

    espix_printf(s, "thread: %s=%s saved to %s\n", key, argv[3], THREAD_CONF_PATH);
    return 0;
}

/* The CLI's reply, into the stream that asked for it. */
static void cli_to_session(void *ctx, const char *text, size_t len)
{
    espix_session_t *s = (espix_session_t *)ctx;
    (void)len;
    espix_printf(s, "%s", text);
}

static int cmd_thread(espix_session_t *s, int argc, char **argv)
{
    if (argc < 2) {
        espix_printf(s, "usage: thread {status|start|stop|attach|br {on|off}|"
                        "conf [key value]|dataset|join {<hex>|-f <file>}|"
                        "cli <line...>}\n");
        espix_printf(s, "  status   what thread0 is doing\n");
        espix_printf(s, "  start    bring the OpenThread stack up (native 15.4 radio)\n");
        espix_printf(s, "  stop     take it down\n");
        espix_printf(s, "  attach   join the network in the stored dataset\n");
        espix_printf(s, "  br       border routing, if built in\n");
        espix_printf(s, "  conf     show or set the boot policy in /etc/thread.conf\n");
        espix_printf(s, "  dataset  print the active dataset as hex TLVs\n");
        espix_printf(s, "  join     join from a hex dataset, or from a file of one\n");
        espix_printf(s, "  cli      OpenThread's own CLI; the reply comes back here\n");
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

    if (strcmp(argv[1], "dataset") == 0) {
        char hex[2 * 256 + 1];
        const esp_err_t err = espix_thread_dataset_hex(hex, sizeof(hex));
        if (err == ESP_ERR_NOT_FOUND) {
            espix_printf(s, "thread: no active dataset\n");
            return 1;
        }
        if (err != ESP_OK) {
            espix_eprintf(s, "thread: dataset: %s\n", esp_err_to_name(err));
            return 1;
        }
        espix_printf(s, "%s\n", hex);
        return 0;
    }

    if (strcmp(argv[1], "join") == 0) {
        char hex[2 * 256 + 1];
        size_t used = 0;

        if (argc >= 4 && strcmp(argv[2], "-f") == 0) {
            /*
             * A dataset is around 500 hex digits and the shell line limit is
             * 255, so pasting one is not possible; the file form is the one that
             * fits. 'thread dataset > file' makes the file.
             */
            FILE *f = fopen(argv[3], "r");
            if (f == NULL) {
                espix_eprintf(s, "thread: cannot read %s\n", argv[3]);
                return 1;
            }
            int c;
            while ((c = fgetc(f)) != EOF && used < sizeof(hex) - 1) {
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                    continue;
                }
                hex[used++] = (char)c;
            }
            fclose(f);
            if (used == 0) {
                espix_eprintf(s, "thread: %s has no hex in it\n", argv[3]);
                return 1;
            }
        } else {
            if (argc < 3) {
                espix_eprintf(s, "usage: thread join <hex-tlvs> | -f <file>\n");
                return 1;
            }
            for (int i = 2; i < argc; i++) {
                const size_t n = strlen(argv[i]);
                if (used + n >= sizeof(hex)) {
                    espix_eprintf(s, "thread: dataset too long\n");
                    return 1;
                }
                memcpy(hex + used, argv[i], n);
                used += n;
            }
        }
        hex[used] = '\0';

        const esp_err_t err = espix_thread_join(hex);
        if (err != ESP_OK) {
            espix_eprintf(s, "thread: join: %s\n", esp_err_to_name(err));
            return 1;
        }
        espix_printf(s, "thread: joining\n");
        return 0;
    }

    if (strcmp(argv[1], "conf") == 0) {
        return thread_conf(s, argc, argv);
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
        const esp_err_t err = espix_thread_cli(line, cli_to_session, s);
        if (err != ESP_OK) {
            espix_eprintf(s, "thread: cli: %s\n", esp_err_to_name(err));
            return 1;
        }
        return 0;      /* the CLI's own output was the answer */
    }

    if (strcmp(argv[1], "enabled") == 0 ||
        strcmp(argv[1], "border_router") == 0 ||
        strcmp(argv[1], "backbone") == 0 ||
        strcmp(argv[1], "attach") == 0) {
        espix_eprintf(s, "thread: '%s' is a config key; try "
                        "'thread conf %s <value>'\n", argv[1], argv[1]);
        return 1;
    }

    espix_eprintf(s, "thread: unknown verb '%s'\n", argv[1]);
    return 1;
}

static espix_cmd_t s_thread_cmds[] = {
    { .name = "thread", .fn = cmd_thread,
      .help = "Thread (IEEE 802.15.4): status, up/down, join, border router",
      .usage = "thread {status|start|stop|attach|br {on|off}|conf [key value]|"
               "dataset|join {<hex>|-f <file>}|cli <line...>}" },
};

#endif /* CONFIG_ESPIX_THREAD */

void espix_cmds_register_thread(void)
{
#if CONFIG_ESPIX_THREAD
    espix_cmds_register_table(s_thread_cmds,
                              sizeof(s_thread_cmds) / sizeof(s_thread_cmds[0]));
#endif
}
