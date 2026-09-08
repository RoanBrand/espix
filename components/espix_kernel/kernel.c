/*
 * espix kernel core: identity, uptime, boot banner.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "espix_kernel.h"
#include "espix_kernel_priv.h"

#define TAG "kernel"

static const char *s_version = ESPIX_VERSION_STR;

const char *espix_version(void)
{
    return s_version;
}

/*
 * Which build this is, as opposed to which release: the git describe that
 * CONFIG_APP_PROJECT_VER carries, `2ebf416-dirty` and the like.
 *
 * Separate from espix_version() because the two answer different questions and
 * conflating them is what made this necessary. `espix 0.3.0` is a promise about
 * behaviour; this is the identity of the bytes actually running, and only the
 * second one can tell you the board is not running what you just compiled.
 *
 * That is not hypothetical. `make test` builds the test app and runs the suite;
 * it does not flash. A board can therefore be tested for hours against an image
 * from an uncommitted tree that no longer exists, and nothing in espix's output
 * would have contradicted you -- which is exactly what happened, and cost a
 * debugging session spent explaining a panic in code nobody could check out.
 * tests/run.sh now refuses to start when this disagrees with build/espix.bin.
 */
const char *espix_build_id(void)
{
    static char id[48];

    if (id[0] != '\0') {
        return id;
    }

    const esp_app_desc_t *desc = esp_app_get_description();
    const char *ver = (desc != NULL && desc->version[0] != '\0')
                          ? desc->version : "unknown";

    /*
     * Two halves, because neither is sufficient on its own.
     *
     * The git describe is what a person recognises, and it is what makes the
     * mismatch message mean something at a glance. But it says `-dirty` for
     * *any* modified tree, so two different working trees at the same commit
     * produce the same string -- and "I rebuilt without flashing" is precisely
     * the case that must not slip through.
     *
     * So it carries the ELF SHA prefix too, which is a content hash and cannot
     * collide that way. It is the same identity espcoredump uses to decide
     * whether a stored dump belongs to the running image, and espix already
     * compares it for that in espix_fault/coredump.c -- so this is the
     * established answer to "are these the same bytes", not a second one.
     */
    const char *sha = esp_app_get_elf_sha256_str();
    snprintf(id, sizeof(id), "%s+%s", ver, (sha != NULL) ? sha : "?");
    return id;
}

const char *espix_target(void)
{
    return CONFIG_IDF_TARGET;
}

int64_t espix_uptime_us(void)
{
    return esp_timer_get_time();
}

static const char *chip_model_name(esp_chip_model_t model)
{
    switch (model) {
    case CHIP_ESP32:   return "ESP32";
    case CHIP_ESP32S2: return "ESP32-S2";
    case CHIP_ESP32S3: return "ESP32-S3";
    case CHIP_ESP32C3: return "ESP32-C3";
    case CHIP_ESP32C6: return "ESP32-C6";
    case CHIP_ESP32H2: return "ESP32-H2";
    case CHIP_ESP32P4: return "ESP32-P4";
    case CHIP_ESP32C61: return "ESP32-C61";
    case CHIP_ESP32S31: return "ESP32-S31";
    default:           return CONFIG_IDF_TARGET;
    }
}

const char *espix_chip_model(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    return chip_model_name(chip.model);
}

size_t espix_uname(char *buf, size_t len, bool all)
{
    if (buf == NULL || len == 0) {
        return 0;
    }

    if (!all) {
        return (size_t)snprintf(buf, len, "espix");
    }

    esp_chip_info_t chip;
    esp_chip_info(&chip);

    /*
     * The build id sits where Linux puts its own -- `uname -a` there reads
     * `Linux host 6.1.0 #1 SMP PREEMPT_DYNAMIC ... x86_64`, and the `#1 SMP...`
     * field is precisely "which build of this kernel is running". Same job, so
     * same place, rather than a command of its own that nobody would think to
     * run at the moment it matters.
     */
    return (size_t)snprintf(buf, len,
                            "espix %s (%s) %s rev%d.%d %d-core IDF %s",
                            s_version,
                            espix_build_id(),
                            chip_model_name(chip.model),
                            chip.revision / 100, chip.revision % 100,
                            chip.cores,
                            esp_get_idf_version());
}

size_t espix_uptime_str(char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return 0;
    }

    const int64_t secs = esp_timer_get_time() / 1000000;
    const int days  = (int)(secs / 86400);
    const int hours = (int)((secs % 86400) / 3600);
    const int mins  = (int)((secs % 3600) / 60);

    if (days > 0) {
        return (size_t)snprintf(buf, len, "up %d day%s, %d:%02d",
                                days, days == 1 ? "" : "s", hours, mins);
    }
    if (hours > 0) {
        return (size_t)snprintf(buf, len, "up %d:%02d", hours, mins);
    }
    return (size_t)snprintf(buf, len, "up %d min", mins);
}

/* ------------------------------------------------------------------ */
/* Boot barrier                                                        */
/* ------------------------------------------------------------------ */

/* Guarded by a spinlock rather than left as a bare volatile: holds and releases
 * come from different tasks (init on the main task, release from the event
 * loop), so the increment must not be split. */
static unsigned     s_boot_pending;
static portMUX_TYPE s_boot_lock = portMUX_INITIALIZER_UNLOCKED;

void espix_kernel_boot_hold(void)
{
    portENTER_CRITICAL_SAFE(&s_boot_lock);
    s_boot_pending++;
    portEXIT_CRITICAL_SAFE(&s_boot_lock);
}

void espix_kernel_boot_release(void)
{
    portENTER_CRITICAL_SAFE(&s_boot_lock);
    if (s_boot_pending > 0) {
        s_boot_pending--;
    }
    portEXIT_CRITICAL_SAFE(&s_boot_lock);
}

unsigned espix_kernel_boot_pending(void)
{
    portENTER_CRITICAL_SAFE(&s_boot_lock);
    const unsigned n = s_boot_pending;
    portEXIT_CRITICAL_SAFE(&s_boot_lock);
    return n;
}

/* ------------------------------------------------------------------ */

void espix_kernel_early_init(void)
{
    espix_klog_install_esp_log_hook();
    espix_klog(ESPIX_KLOG_INFO, TAG, "espix %s starting on %s",
               s_version, CONFIG_IDF_TARGET);
}
