/*
 * smbd -- the SMB2/3 file server. Run it as a unit ("smbd always smbd"), or the
 * way a Unix daemon runs in the foreground, which is the same thing.
 */

#include <stdio.h>
#include <string.h>

#include "espix_cmds_priv.h"
#include "espix_kernel.h"
#include "espix_proc.h"
#include "espix_smbd.h"

/* Run by a unit, it is a process, and stops when asked; run by hand, it stops
 * when the system is going down. Either way the daemon learns it, and returns. */
static bool keep_going(void)
{
    return !espix_proc_stopping() && !espix_shutdown_started();
}

static int cmd_smbd(espix_session_t *s, int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "exports") == 0) {
        espix_printf(s, "smbd: sharing %s as '%s'\n",
                     espix_smbd_path(), espix_smbd_share());
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "start") != 0) {
        espix_eprintf(s, "usage: smbd [start|exports]\n");
        return 1;
    }

    espix_printf(s, "smbd: serving; stop the unit or close the session to end it\n");
    const esp_err_t e = espix_smbd_run(keep_going);
    if (e != ESP_OK) {
        espix_eprintf(s, "smbd: %s\n", esp_err_to_name(e));
        return 1;
    }
    return 0;
}

static espix_cmd_t s_smbd_cmds[] = {
    { .name = "smbd", .fn = cmd_smbd,
      .help = "serve one share over SMB2/3 (see /etc/smb.conf)",
      .usage = "smbd [start|exports]" },
};

void espix_cmds_register_smbd(void)
{
    espix_cmds_register_table(s_smbd_cmds,
                              sizeof(s_smbd_cmds) / sizeof(s_smbd_cmds[0]));
}
