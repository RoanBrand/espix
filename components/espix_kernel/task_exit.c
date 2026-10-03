/*
 * The task-exit notification. See espix_task.h (R-P7.1).
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "espix_task.h"

bool espix_task_exit_init(espix_task_exit_t *t)
{
    if (t->done == NULL) {
        t->done = xSemaphoreCreateBinary();
        if (t->done == NULL) {
            return false;
        }
    } else {
        (void)xSemaphoreTake(t->done, 0);
    }

    t->task = NULL;
    return true;
}

void espix_task_exited(espix_task_exit_t *t)
{
    t->task = NULL;
    if (t->done != NULL) {
        (void)xSemaphoreGive(t->done);
    }
}

bool espix_task_exit_wait(espix_task_exit_t *t, uint32_t timeout_ms)
{
    if (t->done == NULL || t->task == NULL) {
        return true;                    /* nothing is running */
    }

    const TickType_t ticks = (timeout_ms == 0) ? portMAX_DELAY
                                               : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(t->done, ticks) == pdTRUE;
}
