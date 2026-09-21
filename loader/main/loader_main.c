/*
 * espix loader: the app that makes one big kernel slot work.
 *
 * It runs only when the boot target is itself: after a fresh flash, after an
 * upgrade queued a file, and after a kernel failed to confirm and the bootloader
 * fell back. It never downloads and never writes the rootfs -- it reads the
 * state the kernel left in NVS, installs the named /boot file into the kernel
 * slot, points the bootloader at the slot, and reboots.
 *
 * See docs/OTA.md section 11.
 */
#include <dirent.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "esp_littlefs.h"

#define TAG       "loader"

/*
 * IDF's VFS will not mount at "/" (is_path_prefix_valid requires at least two
 * characters), and the kernel does not register littlefs at a path at all. The
 * loader has no other filesystem, so it mounts the rootfs under a prefix and
 * reaches the images through it.
 */
#define MOUNT_POINT "/fs"
#define BOOT_DIR    MOUNT_POINT "/boot"
#define NVS_NS    "espix_boot"
#define NAME_MAX_ 64

static const esp_partition_t *s_kernel;   /* ota_0 */

static esp_err_t mount_rootfs(void)
{
    const esp_vfs_littlefs_conf_t conf = {
        .base_path              = MOUNT_POINT,
        .partition_label        = "storage",
        .format_if_mount_failed = false,
    };
    const esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cannot mount the rootfs: %s", esp_err_to_name(err));
    }
    return err;
}

static bool state_get(nvs_handle_t h, const char *key, char *out, size_t len)
{
    out[0] = 0;
    size_t l = len;
    return (nvs_get_str(h, key, out, &l) == ESP_OK) && out[0] != 0;
}

static void state_set(nvs_handle_t h, const char *key, const char *val)
{
    if (val != NULL && val[0] != 0) {
        nvs_set_str(h, key, val);
    } else {
        nvs_erase_key(h, key);
    }
    nvs_commit(h);
}

/*
 * Copy a kernel file into the kernel slot. esp_ota_end() runs the image through
 * esp_image_verify(), so a file that is truncated or corrupt is refused here
 * rather than selected.
 */
static esp_err_t install_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "%s: cannot open", path);
        return ESP_ERR_NOT_FOUND;
    }

    esp_ota_handle_t h = 0;
    esp_err_t err = esp_ota_begin(s_kernel, OTA_WITH_SEQUENTIAL_WRITES, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "begin %s: %s", s_kernel->label, esp_err_to_name(err));
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

    ESP_LOGI(TAG, "installed %s into %s (%u bytes)", path, s_kernel->label,
             (unsigned)done);
    return esp_ota_set_boot_partition(s_kernel);
}

static void boot_kernel(void)
{
    if (esp_ota_set_boot_partition(s_kernel) != ESP_OK) {
        ESP_LOGE(TAG, "cannot select %s", s_kernel->label);
        return;
    }
    ESP_LOGI(TAG, "booting %s", s_kernel->label);
    esp_restart();
}

/* Nowhere left to go: say so, slowly, instead of thrashing the slot. */
static void stall(const char *why)
{
    for (;;) {
        ESP_LOGE(TAG, "cannot continue: %s -- reflash the board", why);
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

void app_main(void)
{
    nvs_handle_t nvs = 0;
    char good[NAME_MAX_] = {0};
    char pending[NAME_MAX_] = {0};
    char previous[NAME_MAX_] = {0};
    char path[160];

    ESP_LOGI(TAG, "espix loader");

    /* The loader is running, so it is good: keep the bootloader from marking it
     * aborted and losing it as a fallback target. */
    (void)esp_ota_mark_app_valid_cancel_rollback();

    s_kernel = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                        ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    if (s_kernel == NULL) {
        stall("no kernel slot in the partition table");
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK || nvs_open(NVS_NS, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGW(TAG, "no state to read; booting what is already installed");
        boot_kernel();
        return;
    }

    state_get(nvs, "good", good, sizeof(good));
    state_get(nvs, "pending", pending, sizeof(pending));
    state_get(nvs, "previous", previous, sizeof(previous));

    if (mount_rootfs() != ESP_OK) {
        ESP_LOGW(TAG, "no rootfs; booting what is already installed");
        nvs_close(nvs);
        boot_kernel();
        return;
    }

    /* Did the last try fail? Then the kernel slot holds an image that did not
     * confirm, and the good file is what to put back. */
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    const bool failed = (esp_ota_get_state_partition(s_kernel, &st) == ESP_OK) &&
                        (st == ESP_OTA_IMG_ABORTED || st == ESP_OTA_IMG_INVALID);

    const char *target = NULL;
    if (failed && good[0] != 0) {
        target = good;
        ESP_LOGW(TAG, "%s did not confirm; restoring %s", s_kernel->label, good);
    } else if (!failed && pending[0] != 0) {
        target = pending;
        ESP_LOGI(TAG, "installing the queued image %s", pending);
    }

    if (target != NULL) {
        snprintf(path, sizeof(path), "%s/%s", BOOT_DIR, target);
        FILE *probe = fopen(path, "rb");
        if (probe == NULL) {
            if (failed) {
                /* Nothing to restore. Do not select a failed slot and loop. */
                stall("the known-good image is missing from /boot");
            }
            ESP_LOGW(TAG, "%s is gone; booting what is installed", path);
        } else {
            fclose(probe);
            if (install_file(path) == ESP_OK) {
                state_set(nvs, "pending", NULL);
                nvs_close(nvs);
                esp_restart();
            }
            ESP_LOGE(TAG, "install failed; falling back");
            if (failed) {
                stall("the good image could not be installed");
            }
        }
    }

    nvs_close(nvs);
    boot_kernel();
}
