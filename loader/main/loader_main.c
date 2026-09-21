/*
 * espix loader: the app that makes one big kernel slot work.
 *
 * It runs only when the boot target is itself: after a fresh flash, after an
 * upgrade asked for a switch, and after a kernel failed to confirm and the
 * bootloader fell back. Its job has no network in it -- find a kernel file on
 * the rootfs, put it in the kernel slot, point the bootloader at the slot,
 * reboot. Downloading is the kernel's job; by the time the loader runs, the
 * file is already here.
 *
 * See docs/OTA.md for the layout and the state machine.
 */
#include <dirent.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "esp_littlefs.h"

#define TAG      "loader"
#define BOOT_DIR "/boot"

static esp_err_t mount_rootfs(void)
{
    const esp_vfs_littlefs_conf_t conf = {
        .base_path              = "/",
        .partition_label        = "storage",
        .format_if_mount_failed = false,
    };
    const esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cannot mount the rootfs: %s", esp_err_to_name(err));
    }
    return err;
}

static const esp_partition_t *kernel_slot(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                    ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
}

/*
 * Copy a kernel file into the kernel slot exactly the way the kernel's own
 * upgrade does -- esp_ota_begin/write/end, so the image is validated before the
 * bootloader is pointed at it.
 */
static esp_err_t install_file(const char *path, const esp_partition_t *slot)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "%s: cannot open", path);
        return ESP_ERR_NOT_FOUND;
    }

    esp_ota_handle_t h = 0;
    esp_err_t err = esp_ota_begin(slot, OTA_WITH_SEQUENTIAL_WRITES, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "begin %s: %s", slot->label, esp_err_to_name(err));
        fclose(f);
        return err;
    }

    static char buf[4096];
    size_t n, done = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        err = esp_ota_write(h, buf, n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "write at %u: %s", (unsigned)done, esp_err_to_name(err));
            esp_ota_abort(h);
            fclose(f);
            return err;
        }
        done += n;
    }
    fclose(f);

    err = esp_ota_end(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "the image was refused: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "%s -> %s (%u bytes)", path, slot->label, (unsigned)done);
    return esp_ota_set_boot_partition(slot);
}

static void list_boot_dir(void)
{
    DIR *d = opendir(BOOT_DIR);
    if (d == NULL) {
        ESP_LOGW(TAG, "no %s directory", BOOT_DIR);
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        ESP_LOGI(TAG, "%s/%s", BOOT_DIR, e->d_name);
    }
    closedir(d);
}

void app_main(void)
{
    ESP_LOGI(TAG, "espix loader, built from app descriptor at %p", (void *)esp_app_get_description());

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }

    const esp_partition_t *slot = kernel_slot();
    if (slot == NULL) {
        ESP_LOGE(TAG, "no kernel slot; nothing this app can do");
        return;
    }

    if (mount_rootfs() == ESP_OK) {
        list_boot_dir();
    }

    /*
     * The prototype does no selection yet: it makes sure the kernel slot is the
     * boot target and goes. The full version reads the /boot state from NVS,
     * installs the pending file, and restores the previous one after a failed
     * try -- see docs/OTA.md.
     */
    if (esp_ota_set_boot_partition(slot) != ESP_OK) {
        ESP_LOGE(TAG, "cannot select %s", slot->label);
        return;
    }
    ESP_LOGI(TAG, "booting %s", slot->label);
    esp_restart();
}
