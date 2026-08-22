/*
 * espix core dump access.
 *
 * ESP-IDF's panic handler writes a full dump — every task's registers and
 * stacks — into the `coredump` partition. Without something to read it, the
 * only sign of it is a line in the boot log every time, forever, and the only
 * way to act on it is a host-side `idf.py coredump-info`. These wrappers make
 * it visible and clearable from the device.
 */

#include <string.h>

#include "esp_app_desc.h"
#include "esp_core_dump.h"
#include "esp_log.h"

#include "espix_fault.h"
#include "espix_kernel.h"

#define TAG "coredump"

esp_err_t espix_fault_coredump_status(espix_coredump_info_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Every check re-verifies the image and logs about it at info level. That
     * is boot-time detail, not something to print each time `coredump` runs. */
    esp_log_level_set("esp_core_dump_flash", ESP_LOG_WARN);

    memset(out, 0, sizeof(*out));

    /* image_check() verifies the checksum, so a half-written dump from a panic
     * during a panic reports absent rather than garbage. */
    if (esp_core_dump_image_check() != ESP_OK) {
        return ESP_OK;
    }

    size_t addr = 0;
    size_t size = 0;
    if (esp_core_dump_image_get(&addr, &size) != ESP_OK) {
        return ESP_OK;
    }

    out->present    = true;
    out->flash_addr = addr;
    out->size       = size;

    /* Summary is best-effort: a valid dump whose summary cannot be parsed is
     * still worth reporting and still worth pulling off with idf.py. */
    esp_core_dump_summary_t summary;
    if (esp_core_dump_get_summary(&summary) == ESP_OK) {
        strlcpy(out->task, summary.exc_task, sizeof(out->task));
        out->pc = summary.exc_pc;

        /*
         * A dump outlives the firmware that produced it — it sits in flash
         * across reflashes. If it came from a different build, its addresses
         * mean nothing against the running ELF and decoding them silently
         * produces a plausible, wrong function name. Both sides store the
         * SHA as the same truncated hex string, so this is a direct compare.
         */
        const char *running = esp_app_get_elf_sha256_str();
        out->same_build = (running != NULL) &&
                          (strncmp((const char *)summary.app_elf_sha256, running,
                                   sizeof(summary.app_elf_sha256) - 1) == 0);
    }

    return ESP_OK;
}

esp_err_t espix_fault_coredump_erase(void)
{
    const esp_err_t err = esp_core_dump_image_erase();

    if (err == ESP_OK) {
        espix_klog(ESPIX_KLOG_INFO, TAG, "core dump erased");
    }
    return err;
}

void espix_fault_report_coredump(void)
{
    espix_coredump_info_t info;

    if (espix_fault_coredump_status(&info) != ESP_OK || !info.present) {
        return;
    }

    if (info.task[0] != '\0') {
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "core dump in flash: task '%s' at 0x%08x, %u bytes",
                   info.task, (unsigned)info.pc, (unsigned)info.size);
    } else {
        espix_klog(ESPIX_KLOG_WARN, TAG, "core dump in flash: %u bytes",
                   (unsigned)info.size);
    }

    if (!info.same_build) {
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "core dump is from a different build; its addresses do not "
                   "match this firmware");
    }

    espix_klog(ESPIX_KLOG_WARN, TAG,
               "run 'coredump' for details, 'coredump erase' to clear");
}
