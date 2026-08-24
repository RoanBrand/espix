/*
 * espix boot.
 *
 * app_main is the init sequence and nothing else. Keeping the ordering here —
 * rather than inside a kernel component that reaches sideways into the others —
 * is what keeps the component dependency graph acyclic.
 *
 * Order matters:
 *   1. kernel   — the log ring must exist before anything can report.
 *   2. fault    — before the filesystem, so a mount that panics is still
 *                 reported on the next boot.
 *   3. fs       — the rootfs, which everything below reads from.
 *   4. proc     — the process table.
 *   5. net      — after the filesystem, since it reads /etc/hostname and
 *                 /etc/wifi.conf. Returns immediately; association and DHCP
 *                 run on the event loop, so an absent or unreachable network
 *                 never delays the prompt.
 *   6. commands — need the registry, and the filesystem to act on.
 *   7. console  — takes over this task and does not return.
 */

#include "esp_err.h"
#include "esp_log.h"

#include "espix_auth.h"
#include "espix_cmds.h"
#include "espix_fault.h"
#include "espix_fs.h"
#include "espix_kernel.h"
#include "espix_net.h"
#include "espix_proc.h"
#include "espix_shell.h"
#include "espix_ssh.h"

#define TAG "espix"

void app_main(void)
{
    espix_kernel_early_init();
    espix_kernel_print_banner();

    ESP_ERROR_CHECK(espix_fault_init());
    ESP_ERROR_CHECK(espix_fs_mount_root());
    ESP_ERROR_CHECK(espix_proc_init());

    /* Before networking: SSH will authenticate against this, and it warns while
     * the shipped default password is still in place. */
    ESP_ERROR_CHECK(espix_auth_init());

    /* Not fatal: no network is a perfectly usable espix. */
    const esp_err_t net_err = espix_net_init();
    if (net_err != ESP_OK) {
        ESP_LOGW(TAG, "networking unavailable: %s", esp_err_to_name(net_err));
    }

#if CONFIG_ESPIX_SSH_ENABLED
    /* Binds immediately and accepts asynchronously, so this does not wait for
     * an address; a connection simply cannot arrive until one exists. */
    if (net_err == ESP_OK && espix_ssh_start() != ESP_OK) {
        ESP_LOGW(TAG, "ssh server did not start");
    }
#endif

    espix_cmds_register_all();

    const esp_err_t err = espix_console_session_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "console session failed: %s", esp_err_to_name(err));
    }

    /* Falling out of app_main just deletes this task; the rest of the system
     * (reaper, any running apps) keeps going. */
    ESP_LOGW(TAG, "console session ended, no interactive shell remains");
}
