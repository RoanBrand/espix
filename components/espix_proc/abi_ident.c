/*
 * The app's own identity.
 *
 * Four functions where a bigger system might have more: espix has one credential
 * per process and no setuid execution, so uid and euid are the same number, and
 * getuid/getgid are published alongside rather than as a second answer. They are
 * here because an app that calls one and finds it missing does not fail politely
 * -- it fails to *load*, taking every suite that uses the app with it. That is
 * how this gap was found.
 */

#include <sys/types.h>
#include <unistd.h>

#include "esp_elf.h"
#include "espix_kernel.h"
#include "espix_proc.h"
#include "espix_proc_priv.h"

#define TAG "abi"

/*
 * 65534 for a task that is not one of espix's, which an app cannot be. Spelled
 * rather than zero because a failure path answering 0 says "you are root", and
 * that is the one answer this module exists to get right.
 */
#define IDENT_UNKNOWN 65534

static uint16_t ident_uid(void)
{
    uint16_t uid = 0;

    if (!espix_proc_cred_of_task(xTaskGetCurrentTaskHandle(), &uid, NULL, NULL,
                                 NULL)) {
        return IDENT_UNKNOWN;
    }
    return uid;
}

static uint16_t ident_gid(void)
{
    uint16_t gid = 0;

    if (!espix_proc_cred_of_task(xTaskGetCurrentTaskHandle(), NULL, &gid, NULL,
                                 NULL)) {
        return IDENT_UNKNOWN;
    }
    return gid;
}

uid_t getuid(void)  { return (uid_t)ident_uid(); }
uid_t geteuid(void) { return (uid_t)ident_uid(); }
gid_t getgid(void)  { return (gid_t)ident_gid(); }
gid_t getegid(void) { return (gid_t)ident_gid(); }

static const struct esp_elfsym s_ident_syms[] = {
    ESP_ELFSYM_EXPORT(getuid),
    ESP_ELFSYM_EXPORT(geteuid),
    ESP_ELFSYM_EXPORT(getgid),
    ESP_ELFSYM_EXPORT(getegid),
    ESP_ELFSYM_END
};

void espix_proc_abi_ident_register(void)
{
    if (esp_elf_register_symbol(s_ident_syms) != 0) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "could not publish identity to apps");
        return;
    }

    espix_klog(ESPIX_KLOG_DEBUG, TAG, "identity published to apps");
}
