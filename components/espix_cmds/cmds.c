/*
 * espix command registration and shared helpers.
 */

#include <string.h>

#include "espix_cmds.h"
#include "espix_cmds_priv.h"
#include "espix_fs.h"
#include "espix_kernel.h"

#define TAG "cmds"

bool espix_cmd_path(espix_session_t *s, const char *arg,
                    char *out, size_t out_len)
{
    const char *cwd = (s != NULL && s->cwd[0] != '\0') ? s->cwd : "/";

    if (espix_fs_resolve(cwd, arg, out, out_len) != ESP_OK) {
        espix_printf(s, "espix: %s: path too long\n", arg ? arg : "");
        return false;
    }
    return true;
}

void espix_cmds_register_table(espix_cmd_t *table, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        const esp_err_t err = espix_shell_register(&table[i]);
        if (err != ESP_OK) {
            espix_klog(ESPIX_KLOG_ERROR, TAG, "cannot register '%s': %s",
                       table[i].name, esp_err_to_name(err));
        }
    }
}

void espix_cmds_register_all(void)
{
    espix_cmds_register_fs();
    espix_cmds_register_sys();
    espix_cmds_register_run();
}
