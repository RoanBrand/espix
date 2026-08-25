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

#include "esp_elf.h"

#include "espix_kernel.h"
#include "espix_proc_priv.h"

#define TAG "abi"

/* Provided by libgcc for 64-bit division, which Arduino's timing helpers do.
 * Declared rather than included: there is no header for it. */
extern unsigned long long __udivdi3(unsigned long long a, unsigned long long b);

/* ROM printf. Arduino's logging macros reach for it directly. */
extern int ets_printf(const char *fmt, ...);

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

    /* Heap and libc the loader's own table happens not to cover. */
    ESP_ELFSYM_EXPORT(heap_caps_calloc),
    ESP_ELFSYM_EXPORT(memset),
    ESP_ELFSYM_EXPORT(strtol),
    ESP_ELFSYM_EXPORT(ets_printf),
    ESP_ELFSYM_EXPORT(__udivdi3),
    ESP_ELFSYM_EXPORT(vsnprintf),

    /* What assert() lands on. An app built with NDEBUG never references it;
     * one built without it fails to load unless this is here. */
    ESP_ELFSYM_EXPORT(__assert_func),

    ESP_ELFSYM_END
};

void espix_proc_abi_drivers_register(void)
{
    if (esp_elf_register_symbol(s_driver_syms) != 0) {
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "could not publish peripheral symbols to apps");
        return;
    }

    espix_klog(ESPIX_KLOG_INFO, TAG, "peripheral syscalls published to apps");
}
