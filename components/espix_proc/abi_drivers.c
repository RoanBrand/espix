/*
 * Peripheral surface for loadable apps.
 *
 * espix drives no RMT channel and configures no GPIO of its own, so none of
 * this is here for the firmware's benefit. It is here because espix is a
 * platform for other people's apps, and an app that cannot reach a peripheral
 * is not much of an app. The same argument the network ABI makes in
 * espix_net/abi.c: this is a surface espix owns and publishes, not one it
 * inherits from whatever the loader happened to ship.
 *
 * Naming a symbol in this table does two jobs at once. It publishes it to apps,
 * and — because the table takes its address — it forces the linker to keep the
 * driver in the firmware at all. Without the reference, --gc-sections would
 * drop code nothing else calls, and the export would resolve to nothing.
 *
 * That is the real cost of this file: the RMT and GPIO drivers are now linked
 * into every espix build. Measure it before adding more.
 *
 * The list below is not a guess. It is what an Arduino sketch driving a WS2812
 * through Adafruit_NeoPixel actually left undefined, read off the built app
 * with readelf and checked against the firmware's own symbol table.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_rx.h"
#include "driver/rmt_tx.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "soc/gpio_struct.h"
#include "soc/uart_struct.h"

#include "esp_log.h"

#include <sys/ioctl.h>
#include <sys/select.h>

#include "esp_elf.h"

#include "esp_vfs.h"
#include "esp_vfs_eventfd.h"

#include "espix_fs.h"
#include "espix_kernel.h"
#include "espix_proc.h"
#include "espix_shell.h"
#include "espix_proc_priv.h"

#define TAG "abi"

/* Provided by libgcc for 64-bit division, which Arduino's timing helpers do.
 * Declared rather than included: there is no header for it. */
extern unsigned long long __udivdi3(unsigned long long a, unsigned long long b);

/* libgcc soft-double helpers elf_loader's table used to answer for (R-P3.1). */
extern int __ltdf2(double a, double b);
extern int __gtdf2(double a, double b);
extern unsigned int __fixunsdfsi(double a);
extern double __floatunsidf(unsigned int i);
extern double __divdf3(double a, double b);

/* ROM printf. Arduino's logging macros reach for it directly. */
extern int ets_printf(const char *fmt, ...);

int espix_term_size(int *cols, int *rows);

/*
 * libgcc's soft-double helpers. The S31 FPU is single precision, so double
 * arithmetic and comparison in an app land in these; the firmware links libgcc
 * and the table only takes their addresses. Declared here because there is no
 * header for them, exactly as __udivdi3 is above.
 */
extern double    __adddf3(double, double);
extern double    __subdf3(double, double);
extern double    __muldf3(double, double);
extern long long __divdi3(long long, long long);
extern int       __eqdf2(double, double);
extern float     __addsf3(float, float);
extern float     __subsf3(float, float);
extern float     __mulsf3(float, float);
extern float     __divsf3(float, float);
extern int       __eqsf2(float, float);
extern int       __nesf2(float, float);
extern int       __ltsf2(float, float);
extern int       __lesf2(float, float);
extern int       __gtsf2(float, float);
extern int       __gesf2(float, float);
extern int       __unordsf2(float, float);
extern int       __fixsfsi(float);
extern float     __floatsisf(int);
extern double    __extendsfdf2(float);
extern int       __fixdfsi(double);
extern double    __floatsidf(int);
extern int       __gedf2(double, double);
extern int       __ledf2(double, double);
extern float     __truncdfsf2(double);

/*
 * select() with the process's wake eventfd folded in.
 *
 * A process blocked in lwip_select() cannot be reached: the only thing lwIP's
 * select waits on is its own sockets, so a signal has nothing to disturb. The
 * VFS select waits on sockets and VFS fds alike, so this wrapper adds the
 * process's wake eventfd (R-P6.6) to the caller's read set, calls
 * esp_vfs_select(), and on return delivers whatever wrote it and reports EINTR,
 * which is what POSIX says a signal does to select().
 *
 * The eventfd stays internal: it is in the set this call builds, and the bit is
 * cleared again before the caller sees it.
 */
static int espix_select(int nfds, fd_set *readfds, fd_set *writefds,
                        fd_set *errorfds, struct timeval *timeout)
{
    const int wake = espix_proc_select_begin();

    /* Only a read set can be watched this way. A caller that waits on writes
     * alone still blocks uninterruptibly, which is the old behaviour. */
    if (wake >= 0 && readfds != NULL && wake < FD_SETSIZE) {
        FD_SET(wake, readfds);
        if (nfds <= wake) {
            nfds = wake + 1;
        }
    }

    const int rc = esp_vfs_select(nfds, readfds, writefds, errorfds, timeout);

    bool woken = false;
    if (wake >= 0 && readfds != NULL && wake < FD_SETSIZE &&
        FD_ISSET(wake, readfds)) {
        FD_CLR(wake, readfds);
        (void)espix_fs_wake_drain(wake);
        woken = true;
    }

    /* Cleared before delivery: a handler may call select() again. */
    espix_proc_select_end();

    if (woken) {
        /*
         * Deliver here, on the app's own task. This is the point of the whole
         * exercise: a default-terminating signal ends the process now instead
         * of waiting for a espix_sigcheck() that a program blocked in select()
         * would never make.
         */
        (void)espix_sigcheck();
        errno = EINTR;
        return -1;
    }

    return rc;
}

static esp_elf_symbol_table_t s_driver_syms[] = {

    /* RMT. The WS2812 waveform is generated here rather than bit-banged, which
     * is why a 100Hz tick does not disturb it. */
    ESP_ELFSYM_EXPORT(rmt_new_tx_channel),
    ESP_ELFSYM_EXPORT(rmt_new_rx_channel),
    ESP_ELFSYM_EXPORT(rmt_del_channel),
    ESP_ELFSYM_EXPORT(rmt_enable),
    ESP_ELFSYM_EXPORT(rmt_disable),
    ESP_ELFSYM_EXPORT(rmt_transmit),
    ESP_ELFSYM_EXPORT(rmt_new_copy_encoder),
    ESP_ELFSYM_EXPORT(rmt_del_encoder),
    ESP_ELFSYM_EXPORT(rmt_tx_register_event_callbacks),
    ESP_ELFSYM_EXPORT(rmt_rx_register_event_callbacks),

    /* GPIO. */
    ESP_ELFSYM_EXPORT(gpio_config),
    ESP_ELFSYM_EXPORT(gpio_set_level),

    /*
     * Peripheral register blocks. These are addresses the firmware's linker
     * script PROVIDEs; an app is linked with -nostdlib and no peripheral script
     * of its own, so it has no way to know where they are. Arduino's HAL
     * references them directly.
     */
    { "GPIO",  (void *)&GPIO },
    { "UART0", (void *)&UART0 },
    { "UART1", (void *)&UART1 },
    { "UART2", (void *)&UART2 },

    /*
     * FreeRTOS. The loader's built-in table covers almost none of it, and any
     * app that sleeps, takes a mutex or waits on an event group needs these.
     * Arduino's RMT HAL uses all three.
     */
    ESP_ELFSYM_EXPORT(vTaskDelay),
    ESP_ELFSYM_EXPORT(xTimerPendFunctionCallFromISR),

    ESP_ELFSYM_EXPORT(xEventGroupCreate),
    ESP_ELFSYM_EXPORT(xEventGroupWaitBits),
    ESP_ELFSYM_EXPORT(xEventGroupSetBits),
    ESP_ELFSYM_EXPORT(xEventGroupClearBits),
    ESP_ELFSYM_EXPORT(vEventGroupDelete),
    ESP_ELFSYM_EXPORT(vEventGroupSetBitsCallback),

    ESP_ELFSYM_EXPORT(xQueueCreateMutex),
    ESP_ELFSYM_EXPORT(xQueueGenericSend),
    ESP_ELFSYM_EXPORT(xQueueSemaphoreTake),
    ESP_ELFSYM_EXPORT(vQueueDelete),

    /* Monotonic time, which is how Arduino's millis()/micros() are built. */
    ESP_ELFSYM_EXPORT(esp_timer_get_time),

    /*
     * Heap and libc espix owns outright. memset and strtol used to be listed
     * here and were answered a layer below, so they were unreachable; with the
     * loader's own tables off (R-P3.1) they live in abi_libc.c and these are the
     * names that reach IDF's heap and logging.
     */
    ESP_ELFSYM_EXPORT(heap_caps_calloc),
    ESP_ELFSYM_EXPORT(heap_caps_malloc),
    ESP_ELFSYM_EXPORT(heap_caps_free),
    ESP_ELFSYM_EXPORT(heap_caps_get_free_size),
    ESP_ELFSYM_EXPORT(esp_log),
    ESP_ELFSYM_EXPORT(esp_log_timestamp),
    ESP_ELFSYM_EXPORT(ioctl),
    /* Wrapped, so a signal can interrupt it (R-P6.6). */
    { "select", (void *)espix_select },
    /* The eventfd behind that wake, for an app that wants one of its own. */
    ESP_ELFSYM_EXPORT(eventfd),
    ESP_ELFSYM_EXPORT(__udivdi3),
    ESP_ELFSYM_EXPORT(vsnprintf),

    /*
     * libgcc's soft-double helpers. The S31 FPU is single precision, so an
     * app's double arithmetic and comparisons land in these; the firmware
     * already links libgcc, and naming them is what pulls the ones it does
     * not otherwise use into the image.
     */
    ESP_ELFSYM_EXPORT(__adddf3),
    ESP_ELFSYM_EXPORT(__subdf3),
    ESP_ELFSYM_EXPORT(__muldf3),
    ESP_ELFSYM_EXPORT(__divdi3),
    ESP_ELFSYM_EXPORT(__eqdf2),
    ESP_ELFSYM_EXPORT(__extendsfdf2),
    ESP_ELFSYM_EXPORT(__fixdfsi),
    ESP_ELFSYM_EXPORT(__floatsidf),
    ESP_ELFSYM_EXPORT(__gedf2),
    ESP_ELFSYM_EXPORT(__ledf2),
    ESP_ELFSYM_EXPORT(__truncdfsf2),

    /*
     * The single-precision arithmetic, which is what an Xtensa app build
     * reaches for and a RISC-V one inlines. espix had the double set and the
     * sf<->df conversions, but nothing that did arithmetic on a float -- so
     * Doom, whose engine divides floats, failed to load on the S3 with
     * "undefined symbol: __divsf3" while the same program on the S31
     * references none of these at all.
     *
     * Exporting them cannot shadow hardware: what an app calls is decided when
     * the app is compiled, so a build with an FPU never asks. The cost is the
     * libgcc code linked into the kernel, which these entries are also what
     * makes it link at all -- nothing else here calls them.
     */
    ESP_ELFSYM_EXPORT(__addsf3),
    ESP_ELFSYM_EXPORT(__subsf3),
    ESP_ELFSYM_EXPORT(__mulsf3),
    ESP_ELFSYM_EXPORT(__divsf3),
    ESP_ELFSYM_EXPORT(__eqsf2),
    ESP_ELFSYM_EXPORT(__nesf2),
    ESP_ELFSYM_EXPORT(__ltsf2),
    ESP_ELFSYM_EXPORT(__lesf2),
    ESP_ELFSYM_EXPORT(__gtsf2),
    ESP_ELFSYM_EXPORT(__gesf2),
    ESP_ELFSYM_EXPORT(__unordsf2),
    ESP_ELFSYM_EXPORT(__fixsfsi),
    ESP_ELFSYM_EXPORT(__floatsisf),
    ESP_ELFSYM_EXPORT(__divdf3),
    ESP_ELFSYM_EXPORT(__ltdf2),
    ESP_ELFSYM_EXPORT(__gtdf2),
    ESP_ELFSYM_EXPORT(__fixunsdfsi),
    ESP_ELFSYM_EXPORT(__floatunsidf),

    /* ROM printf: Arduino's logging macros reach for it directly, which is why
     * it stays reachable now that espix owns the name (R-P3.1). */
    ESP_ELFSYM_EXPORT(ets_printf),

    /*
     * The terminal's size, for an app that got SIGWINCH. Not ioctl(TIOCGWINSZ):
     * an app's stdout is a stream over its session, not a tty descriptor, so
     * there is nothing to ioctl -- this is the espix-specific call for it, the
     * same shape as espix_sigcheck.
     */
    ESP_ELFSYM_EXPORT(espix_term_size),

    ESP_ELFSYM_END
};

/*
 * The app-facing form: the same call as espix_shell_term_size(), with errno set
 * the way a system call would, because that is what an app expects to see when
 * the transport has no size to give.
 */
int espix_term_size(int *cols, int *rows)
{
    if (cols == NULL || rows == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (espix_shell_term_size(cols, rows) != 0) {
        errno = ENOTTY;
        return -1;
    }
    return 0;
}

void espix_proc_abi_drivers_register(void)
{
    if (esp_elf_register_symbol(s_driver_syms) != 0) {
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "could not publish peripheral symbols to apps");
        return;
    }

    espix_klog(ESPIX_KLOG_INFO, TAG, "peripheral syscalls published to apps");
}
