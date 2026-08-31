/*
 * Time surface for loadable apps.
 *
 * The ELF loader's own table publishes exactly two time symbols,
 * clock_gettime() and strftime(), which between them let an app read a
 * timespec and format a struct tm it has no way to obtain. Everything in the
 * middle -- time(), gettimeofday(), and the conversions -- was missing, so an
 * app calling time(NULL) failed to load rather than failing to work: an
 * unresolved symbol is a load-time error.
 *
 * Separate from abi_drivers.c on purpose. That file is the *peripheral*
 * surface, and says so at the top; the cost it warns about is linking RMT and
 * GPIO into every build. The cost here is different in kind -- newlib's
 * timezone machinery, pulled in by localtime_r() and mktime() -- and belongs
 * where it can be stated and measured on its own.
 *
 * settimeofday() is deliberately included. An app has no more business setting
 * the system clock than a Unix process does without privilege, but espix has
 * no privilege boundary to enforce that with (see the note on the trusted-code
 * model in README), and leaving it out would only mean an app that wanted it
 * failed to load rather than being denied.
 */

#include <sys/time.h>
#include <time.h>

#include "esp_elf.h"

#include "espix_kernel.h"
#include "espix_proc_priv.h"

#define TAG "abi"

static esp_elf_symbol_table_t s_time_syms[] = {

    /* Reading the clock. time() is the one an Arduino sketch reaches for. */
    ESP_ELFSYM_EXPORT(time),
    ESP_ELFSYM_EXPORT(gettimeofday),
    ESP_ELFSYM_EXPORT(settimeofday),

    /*
     * Calendar conversions. The _r forms are what espix's own code uses and
     * what an app should prefer, but the plain ones are what most example code
     * calls, and an app failing to load over a missing localtime() would be a
     * poor lesson in thread safety.
     */
    ESP_ELFSYM_EXPORT(localtime),
    ESP_ELFSYM_EXPORT(localtime_r),
    ESP_ELFSYM_EXPORT(gmtime),
    ESP_ELFSYM_EXPORT(gmtime_r),
    ESP_ELFSYM_EXPORT(mktime),
    ESP_ELFSYM_EXPORT(difftime),

    /*
     * tzset() so an app can pick up a TZ it set for itself. espix has already
     * called this at boot with the contents of /etc/timezone, so an app that
     * simply wants local time need not.
     */
    ESP_ELFSYM_EXPORT(tzset),

    ESP_ELFSYM_END
};

void espix_proc_abi_time_register(void)
{
    if (esp_elf_register_symbol(s_time_syms) != 0) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "could not publish time symbols to apps");
        return;
    }

    espix_klog(ESPIX_KLOG_DEBUG, TAG, "time syscalls published to apps");
}
