/*
 * espix kernel core: identity, uptime, boot banner.
 */

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "espix_kernel.h"
#include "espix_kernel_priv.h"

#define TAG "kernel"

#define ESPIX_STR_(x) #x
#define ESPIX_STR(x)  ESPIX_STR_(x)

static const char *s_version = ESPIX_STR(ESPIX_VERSION_MAJOR) "."
                               ESPIX_STR(ESPIX_VERSION_MINOR) "."
                               ESPIX_STR(ESPIX_VERSION_PATCH);

const char *espix_version(void)
{
    return s_version;
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

    return (size_t)snprintf(buf, len,
                            "espix %s %s rev%d.%d %d-core IDF %s",
                            s_version,
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

void espix_kernel_early_init(void)
{
    espix_klog_install_esp_log_hook();
    espix_klog(ESPIX_KLOG_INFO, TAG, "espix %s starting on %s",
               s_version, CONFIG_IDF_TARGET);
}

void espix_kernel_print_banner(void)
{
    char uname[96];
    espix_uname(uname, sizeof(uname), true);

    const esp_app_desc_t *app = esp_app_get_description();

    printf("\n"
           "  ___  ___ _ __ (_)_  __\n"
           " / _ \\/ __| '_ \\| \\ \\/ /   %s\n"
           "|  __/\\__ \\ |_) | |>  <    built %s %s\n"
           " \\___||___/ .__/|_/_/\\_\\\n"
           "          |_|\n\n",
           uname,
           app ? app->date : "?", app ? app->time : "?");
}
