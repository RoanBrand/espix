/*
 * The kernel's half of an app's POSIX calls: espix_syscall(), which libespix calls
 * for every syscall on the S3 (a U-mode app traps instead), the kernel log a
 * syslog() line lands in, and the interface list getifaddrs() reads.
 */
#include <string.h>
#include <syslog.h>

#include "esp_elf.h"

#include "espix_kernel.h"
#include "espix_proc_priv.h"
#include "espix_posix.h"

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

/* The syscall a POSIX app makes, by number, called directly. */
static esp_elf_symbol_table_t s_posix_syms[] = {
    { "espix_syscall", (void *)espix_posix_syscall },
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
        espix_klog(ESPIX_KLOG_WARN, TAG, "could not publish espix_syscall to apps");
    }
}
