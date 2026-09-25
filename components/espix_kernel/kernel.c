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
 * Which build is running: the first nine hex digits of the image's ELF SHA-256.
 *
 * A content identity rather than a version. `espix 0.3.0` is a promise about
 * behaviour; this is the identity of the bytes actually running, and only it can
 * tell you the board is not running what you just compiled -- which matters
 * because `make test` builds and runs without flashing, so hours can be spent
 * against an image from a tree that no longer exists. tests/run.sh compares this
 * against build/espix.bin and refuses to start when they differ.
 *
 * Nine digits, not the full sixty-four: that is what tests/run.sh reads out of
 * the image and what `uname -v` prints, and it is the same prefix espcoredump
 * already uses to decide whether a stored dump belongs to the running image.
 */
const char *espix_build_id(void)
{
    static char id[16];

    if (id[0] != '\0') {
        return id;
    }

    const char *sha = esp_app_get_elf_sha256_str();
    if (sha == NULL || sha[0] == '\0') {
        return "unknown";
    }

    snprintf(id, sizeof(id), "%.9s", sha);
    return id;
}

bool espix_build_is_release(void)
{
    return ESPIX_BUILD_IS_RELEASE;
}

/*
 * The node name, as Linux keeps it in the kernel rather than in any one
 * subsystem. espix_net owns where it comes from and pushes it here; the kernel
 * only remembers it, for `uname -n`.
 */
static char s_nodename[ESPIX_NODENAME_MAX];

const char *espix_nodename(void)
{
    return (s_nodename[0] != '\0') ? s_nodename : espix_target();
}

void espix_kernel_set_nodename(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return;
    }
    strlcpy(s_nodename, name, sizeof(s_nodename));
}

const char *espix_target(void)
{
    return CONFIG_IDF_TARGET;
}

const char *espix_board(void)
{
    return ESPIX_BOARD;
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

static void field(char *buf, size_t len, size_t *used, const char *text)
{
    if (*used >= len) {
        return;
    }
    const int n = snprintf(buf + *used, len - *used,
                           (*used != 0) ? " %s" : "%s", text);
    if (n > 0) {
        *used += (size_t)n;
    }
}

/*
 * uname(1). The field order is Linux's -- system, node, release, version,
 * machine -- and the split between release and version is the one that matters
 * here: `uname -r` is the version a person quotes, `uname -v` is which build of
 * it is running, and a shell prompt only ever asks for the first.
 */
size_t espix_uname(char *buf, size_t len, const char *flags)
{
    if (buf == NULL || len == 0) {
        return 0;
    }
    if (flags == NULL) {
        flags = "";
    }

    if (flags[0] == '\0') {
        return (size_t)snprintf(buf, len, "espix");
    }

    const bool all = (strchr(flags, 'a') != NULL);
    size_t used = 0;

    if (all || strchr(flags, 's') != NULL) {
        field(buf, len, &used, "espix");
    }
    if (all || strchr(flags, 'n') != NULL) {
        field(buf, len, &used, espix_nodename());
    }
    if (all || strchr(flags, 'r') != NULL) {
        field(buf, len, &used, espix_version());
    }
    if (all || strchr(flags, 'v') != NULL) {
        /*
         * The build field, where Linux puts "#1 SMP PREEMPT ...". A released
         * build has no content identity to report and says so plainly; a
         * development build carries the content hash, which is what "am I
         * running what I think I am" needs. motd applies the same rule.
         */
        char version[16];
        if (espix_build_is_release()) {
            snprintf(version, sizeof(version), "#1");
        } else {
            snprintf(version, sizeof(version), "#%s", espix_build_id());
        }
        field(buf, len, &used, version);
    }
    if (all || strchr(flags, 'm') != NULL) {
        field(buf, len, &used, espix_chip_model());
    }
    if (all) {
        /*
         * The SDK is this platform's "operating system", the last field Linux
         * prints. Chip revision and core count are real facts but neither
         * belongs here -- ps, motd and the boot log say them where somebody is
         * actually looking.
         */
        char os[48];
        snprintf(os, sizeof(os), "ESP-IDF %s", esp_get_idf_version());
        field(buf, len, &used, os);
    }

    return used;
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

    /* Hand the console to a flusher now that there is a scheduler, so no log
     * call in the tree blocks on the UART from here on. */
    espix_klog_start_flusher();
}
