/*
 * A process that is espix's own code rather than an ELF: a unit's daemon, a
 * shell command on a task of its own, a driver's worker.
 *
 * It has a slot, a pid and a parent like any process, so ps lists it, its
 * files are closed when it ends, and its exit status reaches whoever waits for
 * it. It has no app block -- no image, argv, environment or paths -- and it is
 * never deleted from outside: it holds kernel locks by design, so a stop is a
 * request it answers (espix_proc_stopping()). See espix_proc_kind_t.
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "espix_kernel.h"
#include "espix_proc_priv.h"
#include "espix_shell.h"

#define SPAWN_FN_STACK 8192

static void fn_task(void *arg)
{
    espix_proc_slot_t *const slot = arg;

    /* Admitted: the spawner holds the lock until the slot is complete. */
    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);
    const espix_proc_fn_t fn     = slot->fn;
    void *const           fn_arg = slot->fn_arg;
    xSemaphoreGive(g_espix_proc_lock);

    espix_proc_reap_self(slot, ESPIX_PROC_EXITED, fn(fn_arg));
}

esp_err_t espix_proc_spawn_fn(const espix_proc_fn_opts_t *o, espix_pid_t *out_pid)
{
    if (o == NULL || o->fn == NULL || o->name == NULL ||
        o->kind == ESPIX_PROC_APP) {
        return ESP_ERR_INVALID_ARG;
    }
    if (g_espix_proc_lock == NULL || espix_shutdown_started()) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);

    espix_proc_slot_t *const slot = espix_proc_alloc_slot();
    if (slot == NULL) {
        xSemaphoreGive(g_espix_proc_lock);
        return ESP_ERR_NO_MEM;
    }

    strlcpy(slot->info.name, o->name, sizeof(slot->info.name));
    slot->info.kind    = o->kind;
    slot->info.pid     = espix_proc_next_pid();
    slot->info.state   = ESPIX_PROC_READY;
    slot->info.session = o->session;

    const espix_proc_slot_t *const parent = espix_proc_self();
    slot->info.ppid = (parent != NULL) ? parent->info.pid : ESPIX_PID_NONE;
    slot->info.pgid = espix_proc_group_for(parent, o->pgid, slot->info.pid);
    slot->info.sid  = (parent != NULL) ? parent->info.sid : slot->info.pid;

    espix_session_t *const s = o->session;
    slot->info.uid     = (s != NULL) ? s->uid : 0;
    slot->info.gid     = (s != NULL) ? s->gid : 0;
    slot->info.ngroups = (s != NULL) ? s->ngroups : 0;
    for (uint8_t i = 0; i < slot->info.ngroups; i++) {
        slot->info.groups[i] = s->groups[i];
    }

    slot->info.started_us = esp_timer_get_time();
    slot->wake_fd         = -1;     /* zero would be fd 0 to the teardown */
    slot->fn              = o->fn;
    slot->fn_arg          = o->arg;

    /*
     * WithCaps either way, because every path that deletes a process task
     * uses vTaskDeleteWithCaps(). PSRAM unless the body touches flash, and
     * internal RAM when PSRAM will not have it.
     */
    const uint32_t stack = (o->stack != 0) ? o->stack : SPAWN_FN_STACK;
    TaskHandle_t   task  = NULL;
    BaseType_t     ok    = pdFAIL;

    if (!o->internal_stack) {
        ok = xTaskCreatePinnedToCoreWithCaps(fn_task, o->name, stack, slot,
                                             o->priority, &task, o->core,
                                             MALLOC_CAP_SPIRAM);
    }
    if (ok != pdPASS) {
        ok = xTaskCreatePinnedToCoreWithCaps(fn_task, o->name, stack, slot,
                                             o->priority, &task, o->core,
                                             MALLOC_CAP_INTERNAL |
                                             MALLOC_CAP_8BIT);
    }
    if (ok != pdPASS) {
        memset(slot, 0, sizeof(*slot));
        xSemaphoreGive(g_espix_proc_lock);
        return ESP_ERR_NO_MEM;
    }

    slot->info.task  = task;
    slot->info.state = ESPIX_PROC_RUNNING;
    if (out_pid != NULL) {
        *out_pid = slot->info.pid;
    }

    xSemaphoreGive(g_espix_proc_lock);
    return ESP_OK;
}

bool espix_proc_kind_of(espix_pid_t pid, espix_proc_kind_t *out)
{
    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);
    const espix_proc_slot_t *const slot = espix_proc_find(pid);
    if (slot != NULL && out != NULL) {
        *out = slot->info.kind;
    }
    xSemaphoreGive(g_espix_proc_lock);
    return slot != NULL;
}

esp_err_t espix_proc_adopt(espix_session_t *s, const char *name,
                           espix_pid_t *out_pid)
{
    if (s == NULL || name == NULL || g_espix_proc_lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);

    if (espix_proc_self() != NULL) {
        xSemaphoreGive(g_espix_proc_lock);
        return ESP_ERR_INVALID_STATE;       /* already a process */
    }
    espix_proc_slot_t *const slot = espix_proc_alloc_slot();
    if (slot == NULL) {
        xSemaphoreGive(g_espix_proc_lock);
        return ESP_ERR_NO_MEM;
    }

    strlcpy(slot->info.name, name, sizeof(slot->info.name));
    slot->info.kind       = ESPIX_PROC_SESSION;
    slot->info.pid        = espix_proc_next_pid();
    slot->info.ppid       = (s->ppid > 0) ? s->ppid : ESPIX_PID_NONE;
    slot->info.pgid       = slot->info.pid;
    slot->info.sid        = slot->info.pid;
    slot->info.session    = s;
    slot->info.started_us = esp_timer_get_time();
    slot->wake_fd         = -1;
    slot->info.task       = xTaskGetCurrentTaskHandle();
    slot->info.state      = ESPIX_PROC_RUNNING;

    if (out_pid != NULL) {
        *out_pid = slot->info.pid;
    }
    xSemaphoreGive(g_espix_proc_lock);
    return ESP_OK;
}

void espix_proc_leave(int status)
{
    espix_proc_slot_t *const slot = espix_proc_self();

    if (slot == NULL || slot->info.kind != ESPIX_PROC_SESSION) {
        return;
    }
    espix_proc_release_resources(slot);
    espix_proc_finish(slot, ESPIX_PROC_EXITED, status);
}
