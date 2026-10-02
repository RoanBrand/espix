/*
 * espix reaper — deferred cleanup for tasks that must be torn down from a
 * context where teardown is not safe.
 *
 * Nothing feeds this queue yet. It exists now because the shape of the
 * eventual "keep running through a fault" path is the part worth fixing early:
 * the panic handler cannot call vTaskDelete() (the scheduler is frozen, and
 * FreeRTOS APIs are off-limits), so it can only hand a task handle to a normal
 * task that does the work afterwards. Building that seam now means the fault
 * handler never has to grow an inline cleanup path that later needs unpicking.
 *
 * What is NOT solved here, and blocks turning CONFIG_ESPIX_FAULT_REAP on:
 *
 *  - Locks held by the dead task. If it died inside malloc() or a VFS
 *    operation, that mutex stays held forever and every other task that needs
 *    it wedges — worse than a clean reboot. Needs either timeout-based
 *    acquisition on shared resources or per-app heap arenas.
 *  - Memory it owned. Heap blocks, open file descriptors and driver handles
 *    are not tracked per process yet, so reaping leaks them.
 *  - Corruption it may already have caused. Without an MMU, a wild write into
 *    another task's stack or the kernel's data faults nothing and is invisible
 *    here; only invalid-address accesses reach the fault handler at all.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "sdkconfig.h"

#include "espix_fault.h"
#include "espix_fault_priv.h"
#include "espix_kernel.h"
#include "espix_proc.h"

#define TAG "reaper"

#define REAPER_QUEUE_LEN   8
/*
 * 8 KB, and PSRAM: R-P1.6 made the reaper run a process's release_resources()
 * -- esp_elf_deinit() among it -- which used to run on the process's own 8 KB
 * stack. It is a background task, so PSRAM is where the stack belongs.
 */
#define REAPER_STACK_SIZE  8192
#define REAPER_PRIORITY    (configMAX_PRIORITIES - 2)

static QueueHandle_t s_queue;

void espix_fault_request_reap(TaskHandle_t task)
{
    if (s_queue == NULL || task == NULL) {
        return;
    }

    /* Callable from the panic path: a non-blocking send, no allocation. The
     * ISR variant is used because panic context is not task context. */
    BaseType_t yield = pdFALSE;
    xQueueSendFromISR(s_queue, &task, &yield);
}

/*
 * The task-context half of the same queue. A process handing itself to the
 * reaper must not lose the request, so this waits rather than dropping it --
 * unlike the panic-path send above, which cannot wait and may.
 */
static void reap_request_task(TaskHandle_t task)
{
    if (s_queue == NULL || task == NULL) {
        return;
    }
    (void)xQueueSend(s_queue, &task, portMAX_DELAY);
}

static void reaper_task(void *arg)
{
    (void)arg;

    for (;;) {
        TaskHandle_t victim = NULL;
        if (xQueueReceive(s_queue, &victim, portMAX_DELAY) != pdTRUE ||
            victim == NULL) {
            continue;
        }

        /*
         * A process that handed itself over comes first: it has already asked
         * for the clean teardown, so run that and delete it -- no kill, no
         * fault log. Only a task espix_proc does not claim falls through to
         * the fault path below.
         */
        if (espix_proc_reaped(victim)) {
            vTaskDeleteWithCaps(victim);
            continue;
        }

        const espix_pid_t pid = espix_proc_pid_of_task(victim);

        if (pid != ESPIX_PID_NONE) {
            espix_klog(ESPIX_KLOG_WARN, TAG, "reaping faulted pid %d", (int)pid);
            espix_proc_kill(pid);
        } else {
            /* Not an espix process — a kernel task faulted. Deleting it would
             * leave the system half-alive with no owner, so refuse and let the
             * report stand. */
            espix_klog(ESPIX_KLOG_ERROR, TAG,
                       "refusing to reap non-process task %s",
                       pcTaskGetName(victim));
        }
    }
}

esp_err_t espix_fault_reaper_start(void)
{
    if (s_queue != NULL) {
        return ESP_OK;
    }

    s_queue = xQueueCreate(REAPER_QUEUE_LEN, sizeof(TaskHandle_t));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /*
     * PSRAM first: the reaper is a background janitor that has never run in
     * anger, and internal RAM is the pool the audio codec needs. Internal stays
     * the fallback so a board without PSRAM still reaps.
     */
    if (xTaskCreateWithCaps(reaper_task, "espix:reaper", REAPER_STACK_SIZE, NULL,
                            REAPER_PRIORITY, NULL, MALLOC_CAP_SPIRAM) != pdPASS &&
        xTaskCreate(reaper_task, "espix:reaper", REAPER_STACK_SIZE, NULL,
                    REAPER_PRIORITY, NULL) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* The direction espix_proc cannot take itself: it must not depend on this
     * component, so the reaper registers its entry point there instead. */
    espix_proc_set_reap_task(reap_request_task);

    return ESP_OK;
}
