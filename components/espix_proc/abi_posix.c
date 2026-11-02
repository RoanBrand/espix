/*
 * POSIX host facilities IDF does not have, answered by the kernel for M-mode
 * apps; libespix answers the same for a U-mode app (docs/USER-MODE.md).
 *
 *   syslog   the kernel log, where `dmesg` reads it; a U-mode app's arrives by
 *            the SYSLOG syscall, through espix_syslog_put()
 *   dlopen   fails and says so: an app is the loadable object, and no loader
 *            runs inside one
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

#include "esp_elf.h"

#include "espix_kernel.h"
#include "espix_proc_priv.h"

#define TAG "abi"

static espix_klog_level_t level_of(int priority)
{
    switch (LOG_PRI(priority)) {
    case LOG_EMERG:
    case LOG_ALERT:
    case LOG_CRIT:
    case LOG_ERR:
        return ESPIX_KLOG_ERROR;
    case LOG_WARNING:
        return ESPIX_KLOG_WARN;
    case LOG_DEBUG:
        return ESPIX_KLOG_DEBUG;
    default:
        return ESPIX_KLOG_INFO;
    }
}

void espix_syslog_put(int priority, const char *tag, const char *msg)
{
    const size_t n = strcspn(msg, "\n");
    espix_klog(level_of(priority), tag, "%.*s", (int)n, msg);
}

static void k_openlog(const char *ident, int option, int facility)
{
    (void)facility;
    espix_proc_slot_t *slot = espix_proc_self();
    if (slot != NULL) {
        slot->log_ident = ident;
        slot->log_opt   = option;
    }
}

static void k_closelog(void)
{
    espix_proc_slot_t *slot = espix_proc_self();
    if (slot != NULL) {
        slot->log_ident = NULL;
        slot->log_opt   = 0;
    }
}

static int k_setlogmask(int mask)
{
    espix_proc_slot_t *slot = espix_proc_self();
    if (slot == NULL) {
        return 0;
    }
    const int old = slot->log_mask ? slot->log_mask : LOG_UPTO(LOG_DEBUG);
    if (mask != 0) {
        slot->log_mask = mask;
    }
    return old;
}

static void k_vsyslog(int priority, const char *format, va_list ap)
{
    espix_proc_slot_t *slot = espix_proc_self();
    if (slot == NULL || (slot->log_mask != 0 && !(slot->log_mask & LOG_MASK(LOG_PRI(priority))))) {
        return;
    }
    char msg[ESPIX_KLOG_LINE_MAX + 1], tag[48];
    vsnprintf(msg, sizeof(msg), format, ap);
    const char *ident = slot->log_ident ? slot->log_ident : slot->info.name;
    if (slot->log_opt & LOG_PID) {
        snprintf(tag, sizeof(tag), "%s[%d]", ident, (int)slot->info.pid);
    } else {
        snprintf(tag, sizeof(tag), "%s", ident);
    }
    if (slot->log_opt & LOG_PERROR) {
        fprintf(espix_proc_stdio(2), "%s: %s\n", tag, msg);
    }
    espix_syslog_put(priority, tag, msg);
}

static void k_syslog(int priority, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    k_vsyslog(priority, format, ap);
    va_end(ap);
}

static void *k_dlopen(const char *file, int flags)
{
    (void)file;
    (void)flags;
    return NULL;
}

static void *k_dlsym(void *handle, const char *symbol)
{
    (void)handle;
    (void)symbol;
    return NULL;
}

static int k_dlclose(void *handle)
{
    (void)handle;
    return -1;
}

static char *k_dlerror(void)
{
    return (char *)"Dynamic loading not supported";
}

static esp_elf_symbol_table_t s_posix_syms[] = {
    { "openlog",    (void *)k_openlog },
    { "closelog",   (void *)k_closelog },
    { "setlogmask", (void *)k_setlogmask },
    { "syslog",     (void *)k_syslog },
    { "vsyslog",    (void *)k_vsyslog },
    { "dlopen",     (void *)k_dlopen },
    { "dlsym",      (void *)k_dlsym },
    { "dlclose",    (void *)k_dlclose },
    { "dlerror",    (void *)k_dlerror },
    ESP_ELFSYM_END
};

static espix_proc_ifaddrs_fn s_ifaddrs;

void espix_proc_set_ifaddrs(espix_proc_ifaddrs_fn fn)
{
    s_ifaddrs = fn;
}

size_t espix_proc_ifaddrs(struct espix_ifaddr *out, size_t max)
{
    return (s_ifaddrs != NULL) ? s_ifaddrs(out, max) : 0;
}

void espix_proc_abi_posix_register(void)
{
    if (esp_elf_register_symbol(s_posix_syms) != 0) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "could not publish syslog and dlopen to apps");
    }
}
