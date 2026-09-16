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
        espix_eprintf(s, "espix: %s: path too long\n", arg ? arg : "");
        return false;
    }
    return true;
}

/*
 * A size in bytes as a column, plain or -h.
 *
 * Shared rather than local to `ls`, because two implementations of "1.5M" drift:
 * `ls -l`, `ls -lh` and now `lsblk` all report sizes, and a stick that reads
 * 28.7G in one command and 28.8G in another is a bug report waiting to happen.
 *
 * coreutils rounds up, and drops to one decimal only below 10: 1412 bytes is
 * "1.4K" and 20796 is "21K", not "20.3K". Matching that exactly matters more
 * than being arithmetically neat, because the point of -h is that the number
 * looks like the one every other tool would have printed.
 *
 * Integer arithmetic throughout. The obvious version wants doubles and ceil(),
 * which drags in libm for a column of a listing.
 */
void espix_cmd_size(char *out, size_t len, uint64_t bytes, bool human)
{
    if (!human || bytes < 1024) {
        snprintf(out, len, "%llu", (unsigned long long)bytes);
        return;
    }

    static const char units[] = { 'K', 'M', 'G' };
    uint64_t          div     = 1024;
    int               u       = 0;

    while (bytes >= div * 1024 && u < 2) {
        div *= 1024;
        u++;
    }

    /* Tenths, rounded up -- never report less than there is. */
    const uint64_t tenths = (bytes * 10 + div - 1) / div;

    if (tenths < 100) {
        snprintf(out, len, "%u.%u%c", (unsigned)(tenths / 10),
                 (unsigned)(tenths % 10), units[u]);
    } else {
        snprintf(out, len, "%u%c", (unsigned)((tenths + 9) / 10), units[u]);
    }
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
    espix_cmds_register_env();
    espix_cmds_register_fs();
    espix_cmds_register_sys();
    espix_cmds_register_run();
    espix_cmds_register_net();
    espix_cmds_register_motd();
    espix_cmds_register_time();
    espix_cmds_register_blk();
    espix_cmds_register_usbhost();
    espix_cmds_register_exec_fallback();
}
