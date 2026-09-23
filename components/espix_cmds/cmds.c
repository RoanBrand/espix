/*
 * espix command registration and shared helpers.
 */

#include <string.h>

#include "espix_cmds.h"
#include "espix_cmds_priv.h"
#include "espix_fs.h"
#include "espix_kernel.h"
#include "sdkconfig.h"

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
 * `ls -l`, `ls -lh`, `lsblk` and `df -h` all report sizes, and a volume that
 * reads 3.1T in one command and 3214G in another is a bug report waiting to
 * happen.
 *
 * coreutils rounds the tenths to the nearest and drops to one decimal only below
 * 10: 1412 bytes is "1.4K", 3.638 TiB is "3.6T", and 28.14 GiB is "28G". Matching
 * that exactly matters more than being arithmetically neat, because the point of
 * -h is that the number looks like the one every other tool would have printed.
 *
 * Integer arithmetic throughout. The obvious version wants doubles and ceil(),
 * which drags in libm for a column of a listing.
 *
 * The unit table's last entry is the ceiling, and the loop is bounded by the
 * table rather than by a number: it stopped at 'G' for a while, which printed a
 * 3.1TB volume as "3214G" in every command that reports a size -- df, lsblk and
 * ls alike, since this is the only formatter. A literal bound there is what let
 * that go unnoticed, so the array is now the bound.
 */
void espix_cmd_size(char *out, size_t len, uint64_t bytes, bool human)
{
    if (!human || bytes < 1024) {
        snprintf(out, len, "%llu", (unsigned long long)bytes);
        return;
    }

    static const char units[] = { 'K', 'M', 'G', 'T' };
    uint64_t          div     = 1024;
    size_t            u       = 0;

    while (bytes >= div * 1024 && u + 1 < sizeof(units)) {
        div *= 1024;
        u++;
    }

    /*
     * Tenths, rounded to the nearest -- which is what coreutils does, so "3.6T"
     * for 3.638 TiB and "28G" for 28.14 GiB both come out as the other tools
     * print them. Adding div/2 before the divide is the whole of the rounding;
     * a ceiling here instead would make the digit always round away from zero.
     */
    const uint64_t tenths = (bytes * 10 + div / 2) / div;

    if (tenths < 100) {
        snprintf(out, len, "%u.%u%c", (unsigned)(tenths / 10),
                 (unsigned)(tenths % 10), units[u]);
    } else {
        /*
         * Ten units and up are printed whole, and the tenths are simply
         * truncated: 28.1 GiB is "28G", not "29G". The old `(tenths + 9) / 10`
         * rounded a second time, and reaching 'T' is what showed it: 3.638 TiB
         * came out as "4T".
         */
        snprintf(out, len, "%u%c", (unsigned)(tenths / 10), units[u]);
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
#if CONFIG_ESPIX_USB_ROLE_HOST
    espix_cmds_register_usbhost();
#endif
    espix_cmds_register_ota();
    espix_cmds_register_hash();
    espix_cmds_register_bt();
    espix_cmds_register_exec_fallback();
}
