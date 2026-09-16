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
 *   5. time     — after the filesystem, since it reads /etc/timezone; before
 *                 networking, because it starts the SNTP client from the
 *                 IP_EVENT that networking is about to raise.
 *   6. net      — after the filesystem, since it reads /etc/hostname and
 *                 /etc/wifi.conf. Returns immediately; association and DHCP
 *                 run on the event loop, so an absent or unreachable network
 *                 never delays the prompt.
 *   7. usb      — the OTG port as a host, when the role is host. Independent of
 *                 everything above and, like networking, done as soon as the
 *                 stack is up: an empty socket is the normal case, and devices
 *                 appear on the USB task as they are plugged in.
 *   8. commands — need the registry, and the filesystem to act on.
 *   9. console  — takes over this task and does not return.
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
#include "espix_time.h"
#include "espix_usb.h"

#define TAG "espix"

/*
 * Give every attached storage device a name in /dev, and take the names away
 * when it goes.
 *
 * This is the only place that knows both halves: espix_fs has no idea what USB
 * is, espix_usb knows nothing about the VFS, and this file already depends on
 * both. Doing it here is what keeps those two components independent of each
 * other, which is the reason the hook exists at all.
 *
 * Per disk *and* per partition, the way every other system names them, and what
 * makes `mount /dev/sda1 /mnt` work. A superfloppy has no partitions, so its
 * disk node is the only one there is -- and mounting the disk itself is then the
 * only way to reach the filesystem on it.
 */
static void usb_dev_nodes(const espix_usb_dev_t *dev, bool attached)
{
    if (attached) {
        (void)espix_dev_register_block(dev->name, dev->size);
        for (size_t i = 0; i < dev->nparts; i++) {
            (void)espix_dev_register_block(dev->parts[i].name,
                                           dev->parts[i].size);
        }
        return;
    }

    /* Detach hands the row over intact -- before it is released -- so the
     * partitions are still there to be named. */
    espix_dev_unregister_block(dev->name);
    for (size_t i = 0; i < dev->nparts; i++) {
        espix_dev_unregister_block(dev->parts[i].name);
    }
}

void app_main(void)
{
    espix_kernel_early_init();

    ESP_ERROR_CHECK(espix_fault_init());
    ESP_ERROR_CHECK(espix_fs_mount_root());
    ESP_ERROR_CHECK(espix_proc_init());

    /* Before networking: SSH will authenticate against this, and it warns while
     * the shipped default password is still in place. */
    ESP_ERROR_CHECK(espix_auth_init());

    /*
     * Must precede networking: this registers the IP_EVENT handler that starts
     * the SNTP client, and the address it waits for is about to arrive. Not
     * fatal either -- a wrong clock is worse than no clock only to code that
     * assumes it is right, and espix's own timestamps are monotonic.
     */
    if (espix_time_init() != ESP_OK) {
        ESP_LOGW(TAG, "system time unavailable; the clock stays at the epoch");
    }

    /* Not fatal: no network is a perfectly usable espix. */
    const esp_err_t net_err = espix_net_init();
    if (net_err != ESP_OK) {
        ESP_LOGW(TAG, "networking unavailable: %s", esp_err_to_name(net_err));
    }

    /*
     * The other use of the OTG port, and the reason the port has a role: only
     * one of USB-NCM and USB host can have it. Also not fatal -- the board works
     * with nothing in the socket, which is how it usually is.
     */
    const esp_err_t usb_err = espix_usb_init();
    if (usb_err != ESP_OK) {
        ESP_LOGW(TAG, "usb host unavailable: %s", esp_err_to_name(usb_err));
    }

    /* Installed whether or not the stack came up: an empty socket is the normal
     * case, and a device-role build simply never hears anything. */
    espix_usb_set_dev_hook(usb_dev_nodes);

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
