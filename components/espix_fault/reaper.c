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

#define REAPER_QUEUE_LEN   4
#define REAPER_STACK_SIZE  3072
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

static void reaper_task(void *arg)
{
    (void)arg;

    for (;;) {
        TaskHandle_t victim = NULL;
        if (xQueueReceive(s_queue, &victim, portMAX_DELAY) != pdTRUE ||
            victim == NULL) {
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

    if (xTaskCreate(reaper_task, "espix:reaper", REAPER_STACK_SIZE, NULL,
                    REAPER_PRIORITY, NULL) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
