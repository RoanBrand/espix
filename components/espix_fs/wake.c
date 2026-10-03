/*
 * The per-process wake descriptor: an eventfd whose only job is to be readable
 * when a signal arrives, so a process blocked in select() can be reached. It
 * is the primitive R-P6.6 was missing -- IDF ships eventfd(2), so there is
 * nothing to invent, only to hand to the process that needs it.
 *
 * The fd is internal. espix creates it at spawn and adds it to the read set the
 * select wrapper builds; the app never sees it as one of its own descriptors.
 */

#include <stdint.h>
#include <unistd.h>

#include "esp_vfs_eventfd.h"

#include "espix_fs.h"
#include "espix_kernel.h"

static const char *TAG = "espix:fs";

esp_err_t espix_fs_wake_init(void)
{
    /*
     * One per process, so a full table can still arm every one, plus a little
     * slack for an app that creates its own.
     */
    const esp_vfs_eventfd_config_t config = {
        .max_fds = CONFIG_ESPIX_PROC_MAX + 4,
    };

    const esp_err_t err = esp_vfs_eventfd_register(&config);
    if (err != ESP_OK) {
        /* Already registered is what a re-mount would see, and is not a
         * failure for the second caller. Anything else is worth a line. */
        if (err != ESP_ERR_INVALID_STATE) {
            espix_klog(ESPIX_KLOG_WARN, TAG, "eventfd: %s",
                       esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}

int espix_fs_wake_create(void)
{
    return eventfd(0, 0);
}

int espix_fs_wake_notify(int fd)
{
    if (fd < 0) {
        return -1;
    }
    const uint64_t one = 1;
    return (write(fd, &one, sizeof(one)) == (ssize_t)sizeof(one)) ? 0 : -1;
}

int espix_fs_wake_drain(int fd)
{
    if (fd < 0) {
        return -1;
    }
    uint64_t value = 0;
    return (read(fd, &value, sizeof(value)) >= 0) ? 0 : -1;
}

void espix_fs_wake_close(int fd)
{
    if (fd >= 0) {
        close(fd);
    }
}
