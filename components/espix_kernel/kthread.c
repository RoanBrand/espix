/*
 * espix_kthread_start(): espix_proc's when it is up, a plain task before.
 */

#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"

#include "espix_task.h"

/* Started before espix_proc: few, and none of them ends early. */
#define KTHREAD_EARLY_MAX 4

static const espix_kthread_ops_t *s_ops;
static portMUX_TYPE                s_lock = portMUX_INITIALIZER_UNLOCKED;

static struct {
    TaskHandle_t task;
    const char  *name;
} s_early[KTHREAD_EARLY_MAX];

typedef struct {
    espix_kthread_fn_t fn;
    void              *arg;
} plain_t;

static void plain_task(void *arg)
{
    plain_t *const p  = arg;
    const plain_t  st = *p;
    free(p);

    st.fn(st.arg);

    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < KTHREAD_EARLY_MAX; i++) {
        if (s_early[i].task == xTaskGetCurrentTaskHandle()) {
            s_early[i].task = NULL;
        }
    }
    portEXIT_CRITICAL(&s_lock);

    /* Adopted meanwhile: its slot goes before the task does. */
    if (s_ops != NULL) {
        s_ops->gone(xTaskGetCurrentTaskHandle());
    }
    vTaskDeleteWithCaps(NULL);
}

esp_err_t espix_kthread_start(const espix_kthread_t *k, TaskHandle_t *out)
{
    if (k == NULL || k->fn == NULL || k->name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ops != NULL) {
        return s_ops->start(k, out);
    }

    plain_t *const p = malloc(sizeof(*p));
    if (p == NULL) {
        return ESP_ERR_NO_MEM;
    }
    *p = (plain_t){ .fn = k->fn, .arg = k->arg };

    TaskHandle_t task = NULL;
    BaseType_t   ok   = pdFAIL;
    if (!k->internal_stack) {
        ok = xTaskCreatePinnedToCoreWithCaps(plain_task, k->name, k->stack, p,
                                             k->priority, &task, k->core,
                                             ESPIX_STACK_CAPS);
    }
    if (ok != pdPASS) {
        ok = xTaskCreatePinnedToCoreWithCaps(plain_task, k->name, k->stack, p,
                                             k->priority, &task, k->core,
                                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (ok != pdPASS) {
        free(p);
        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < KTHREAD_EARLY_MAX; i++) {
        if (s_early[i].task == NULL) {
            s_early[i].task = task;
            s_early[i].name = k->name;
            break;
        }
    }
    portEXIT_CRITICAL(&s_lock);

    if (out != NULL) {
        *out = task;
    }
    return ESP_OK;
}

void espix_kthread_set_ops(const espix_kthread_ops_t *ops)
{
    for (int i = 0; i < KTHREAD_EARLY_MAX; i++) {
        portENTER_CRITICAL(&s_lock);
        const TaskHandle_t task = s_early[i].task;
        const char *const  name = s_early[i].name;
        s_early[i].task = NULL;
        portEXIT_CRITICAL(&s_lock);

        if (task != NULL) {
            ops->adopt(task, name);
        }
    }
    s_ops = ops;
}
