/*
 * nfsd -- the NFSv3 server. Run it as a unit ("nfsd always nfsd"), or the way
 * a Unix daemon runs in the foreground, which is the same thing.
 */

#include <stdio.h>
#include <string.h>

#include "espix_cmds_priv.h"
#include "espix_nfsd.h"
#include "espix_svc.h"

/* Run by a unit, stop when the unit is asked to; run by hand, never. */
static bool keep_going(void)
{
    return !espix_svc_stopping();
}

static int cmd_nfsd(espix_session_t *s, int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "exports") == 0) {
        const int n = espix_nfsd_export_count();
        for (int i = 0; i < n; i++) {
            const char *path, *who;
            bool        ro;
            if (espix_nfsd_export_info(i, &path, &ro, &who)) {
                espix_printf(s, "nfsd:   %s  %s  %s\n", path, who,
                             ro ? "ro" : "rw");
            }
        }
        if (n == 0) {
            espix_printf(s, "nfsd: no exports in /etc/exports\n");
        }
        return 0;
    }

    if (argc > 1 && (strcmp(argv[1], "start") != 0)) {
        espix_eprintf(s, "usage: nfsd [start|exports]\n");
        return 1;
    }

    espix_printf(s, "nfsd: serving; stop the unit or close the session to end it\n");
    const esp_err_t e = espix_nfsd_run(keep_going);
    if (e != ESP_OK) {
        espix_eprintf(s, "nfsd: %s\n", esp_err_to_name(e));
        return 1;
    }
    return 0;
}

static espix_cmd_t s_nfsd_cmds[] = {
    { .name = "nfsd", .fn = cmd_nfsd,
      .help = "serve /etc/exports over NFSv3",
      .usage = "nfsd [start|exports]" },
};

void espix_cmds_register_nfsd(void)
{
    espix_cmds_register_table(s_nfsd_cmds,
                              sizeof(s_nfsd_cmds) / sizeof(s_nfsd_cmds[0]));
}
