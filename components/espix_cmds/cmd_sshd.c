/*
 * sshd -- the SSH server, run as a unit ("sshd  always  sshd"). Stopping it
 * stops new logins; the ones already open carry on.
 */

#include "sdkconfig.h"

#include "espix_cmds_priv.h"
#include "espix_kernel.h"
#include "espix_proc.h"

#if CONFIG_ESPIX_SSH_ENABLED

#include "espix_ssh.h"

static bool keep_going(void)
{
    return !espix_proc_stopping() && !espix_shutdown_started();
}

static int cmd_sshd(espix_session_t *s, int argc, char **argv)
{
    (void)argc;
    (void)argv;

    const esp_err_t e = espix_ssh_run(keep_going);
    if (e != ESP_OK) {
        espix_eprintf(s, "sshd: %s\n", (e == ESP_ERR_INVALID_STATE)
                                       ? "already running" : esp_err_to_name(e));
        return 1;
    }
    return 0;
}

static espix_cmd_t s_sshd_cmds[] = {
    { .name = "sshd", .fn = cmd_sshd,
      .help = "serve SSH logins; run it as a unit",
      .usage = "sshd" },
};

void espix_cmds_register_sshd(void)
{
    espix_cmds_register_table(s_sshd_cmds,
                              sizeof(s_sshd_cmds) / sizeof(s_sshd_cmds[0]));
}

#else

void espix_cmds_register_sshd(void)
{
}

#endif
