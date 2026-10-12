/*
 * The periodic heap check of CONFIG_ESPIX_HEAP_CHECK_MS: dates heap corruption
 * to one period rather than to whichever free() trips over it later.
 */

#include "sdkconfig.h"

#include "espix_kernel.h"

#if CONFIG_ESPIX_HEAP_CHECK_MS > 0

#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"

#include "espix_task.h"

static void heapcheck_task(void *arg)
{
    (void)arg;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_ESPIX_HEAP_CHECK_MS));
        if (!heap_caps_check_integrity_all(true)) {
            /* The check printed the corrupt address; the neighbours say who. */
            heap_caps_dump(MALLOC_CAP_INTERNAL);
            abort();
        }
    }
}

void espix_heapcheck_start(void)
{
    (void)espix_kthread_start(&(const espix_kthread_t){
        .name = "heapcheck", .fn = heapcheck_task, .stack = 4096,
        .priority = 1, .core = tskNO_AFFINITY, .internal_stack = true }, NULL);
}

#else

void espix_heapcheck_start(void)
{
}

#endif
