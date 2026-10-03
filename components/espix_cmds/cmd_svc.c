/*
 * service -- what the supervisor is doing.
 *
 * Listing is the point. A unit is not a job, so the jobs table cannot show it,
 * and without this the only way to know whether a daemon started is to read the
 * klog. The verbs are small because the supervisor does the work: start and
 * stop set the intent, and the next tick acts on it.
 */

#include <stdio.h>
#include <string.h>

#include "espix_cmds_priv.h"
#include "espix_svc.h"

static const char *state_of(const espix_svc_info_t *u)
{
    if (u->pid != ESPIX_PID_NONE || u->running) {
        return "running";
    }
    return u->enabled ? "starting" : "stopped";
}

static int cmd_service(espix_session_t *s, int argc, char **argv)
{
    if (argc == 1) {
        const int n = espix_svc_count();
        if (espix_svc_safe_mode()) {
            espix_printf(s, "safe mode: a core dump is stored; units are not "
                            "started (coredump erase, then service reload)\n");
        }
        if (n == 0) {
            espix_printf(s, "no units\n");
            return 0;
        }

        espix_printf(s, "%-16s %-8s %-6s %s\n", "NAME", "PID", "RESTART",
                     "STATE");
        for (int i = 0; i < n; i++) {
            espix_svc_info_t u;
            if (!espix_svc_info(i, &u)) {
                continue;
            }
            espix_printf(s, "%-16s %-8d %-6s %s\n", u.name,
                         (u.pid == ESPIX_PID_NONE) ? -1 : (int)u.pid,
                         u.always ? "always"
                                  : (u.every_s ? "every" : "once"),
                         state_of(&u));
        }
        return 0;
    }

    if (strcmp(argv[1], "reload") == 0) {
        if (espix_svc_reload() != ESP_OK) {
            espix_eprintf(s, "service: no supervisor\n");
            return 1;
        }
        espix_printf(s, "reloaded\n");
        return 0;
    }

    if (argc != 3) {
        espix_eprintf(s, "usage: service [start|stop|restart] <name> | reload\n");
        return 1;
    }

    esp_err_t err;
    if (strcmp(argv[1], "start") == 0) {
        err = espix_svc_start(argv[2]);
    } else if (strcmp(argv[1], "stop") == 0) {
        err = espix_svc_stop(argv[2]);
    } else if (strcmp(argv[1], "restart") == 0) {
        (void)espix_svc_stop(argv[2]);
        err = espix_svc_start(argv[2]);
    } else {
        espix_eprintf(s, "service: unknown verb '%s'\n", argv[1]);
        return 1;
    }

    if (err != ESP_OK) {
        espix_eprintf(s, "service: %s: %s\n", argv[2], esp_err_to_name(err));
        return 1;
    }
    return 0;
}

static espix_cmd_t s_svc_cmds[] = {
    { .name = "service", .fn = cmd_service,
      .help = "list or manage the units espix runs at boot",
      .usage = "service [start|stop|restart] <name> | reload" },
};

void espix_cmds_register_svc(void)
{
    espix_cmds_register_table(s_svc_cmds,
                              sizeof(s_svc_cmds) / sizeof(s_svc_cmds[0]));
}
