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
#include "esp_crypto_lock.h"
#include "esp_log.h"

#include "espix_fault.h"
#include "espix_kernel.h"

#define TAG "coredump"

/*
 * Every espcoredump call that verifies an image has to hold the SHA/AES lock.
 *
 * espcoredump checksums with the *ROM* SHA on every target but the original
 * ESP32 -- ets_sha_enable(), ets_sha_init(), ets_sha_update(), ets_sha_finish(),
 * ets_sha_disable() (components/espcoredump/src/core_dump_sha.c). Those drive
 * the SHA peripheral directly and take no lock of any kind, which is right for
 * the panic path they were written for, where nothing else is running.
 *
 * esp_core_dump_image_check() is not the panic path. It is a public API called
 * from an ordinary task, and here it is reachable from a shell command, so it
 * can land in the middle of somebody else's SHA. When it does, ets_sha_enable()
 * resets the peripheral and ets_sha_disable() turns it off underneath the other
 * operation, which then reads its digest back as all zeroes -- and IDF's
 * fault-injection check in sha_hal_read_digest() calls abort() on exactly that.
 * The whole device reboots.
 *
 * Found by running the test suite four suites at a time: the health monitor
 * asked `coredump` every ten seconds while workers logged in, and a login is
 * PBKDF2 at 20 000 iterations of HMAC-SHA256. The dump named it in one step --
 * a panic in sha_hal_read_digest() under psa_key_derivation_output_bytes() in
 * one sshd:conn task, while the other sat in esp_core_dump_image_check().
 *
 * esp_crypto_sha_aes_lock is the same lock esp_sha_acquire_hardware() takes, so
 * holding it here puts the ROM path back in the same queue as everything else.
 * Nothing inside these calls takes it again -- the ROM SHA is lockless, which is
 * the entire problem -- so there is no re-entry to deadlock on.
 *
 * Reported upstream; see docs/UPSTREAM.md.
 */
esp_err_t espix_fault_coredump_status(espix_coredump_info_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Every check re-verifies the image and logs about it at info level. That
     * is boot-time detail, not something to print each time `coredump` runs. */
    esp_log_level_set("esp_core_dump_flash", ESP_LOG_WARN);

    memset(out, 0, sizeof(*out));

    size_t                  addr = 0;
    size_t                  size = 0;
    esp_core_dump_summary_t summary;
    bool                    present     = false;
    bool                    has_summary = false;

    esp_crypto_sha_aes_lock_acquire();
    /* image_check() verifies the checksum, so a half-written dump from a panic
     * during a panic reports absent rather than garbage. */
    if (esp_core_dump_image_check() == ESP_OK &&
        esp_core_dump_image_get(&addr, &size) == ESP_OK) {
        present = true;
        /* Summary is best-effort: a valid dump whose summary cannot be parsed
         * is still worth reporting and still worth pulling off with idf.py. */
        has_summary = (esp_core_dump_get_summary(&summary) == ESP_OK);
    }
    esp_crypto_sha_aes_lock_release();

    if (!present) {
        return ESP_OK;
    }

    out->present    = true;
    out->flash_addr = addr;
    out->size       = size;

    if (has_summary) {
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
