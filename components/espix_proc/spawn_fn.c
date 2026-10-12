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

#include <stdlib.h>
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
    slot->info.pid     = (o->init && espix_proc_find(1) == NULL)
                       ? 1 : espix_proc_next_pid();
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
    if (o->task_out != NULL) {
        *o->task_out = task;
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

    /*
     * A kernel task that becomes a login's shell -- sshd:conn -- keeps its pid
     * and its parent, and is a kernel task again when the login ends.
     */
    espix_proc_slot_t *const self = espix_proc_self();
    if (self != NULL) {
        if (self->info.kind != ESPIX_PROC_KTHREAD || self->info.task !=
            xTaskGetCurrentTaskHandle()) {
            xSemaphoreGive(g_espix_proc_lock);
            return ESP_ERR_INVALID_STATE;
        }
        strlcpy(self->info.name, name, sizeof(self->info.name));
        self->was_kthread  = true;
        self->info.kind    = ESPIX_PROC_SESSION;
        self->info.session = s;
        self->info.pgid    = self->info.pid;
        self->info.sid     = self->info.pid;
        if (out_pid != NULL) {
            *out_pid = self->info.pid;
        }
        xSemaphoreGive(g_espix_proc_lock);
        return ESP_OK;
    }
    espix_proc_slot_t *const slot = espix_proc_alloc_slot();
    if (slot == NULL) {
        xSemaphoreGive(g_espix_proc_lock);
        return ESP_ERR_NO_MEM;
    }

    strlcpy(slot->info.name, name, sizeof(slot->info.name));
    slot->info.kind       = ESPIX_PROC_SESSION;
    slot->info.pid        = espix_proc_next_pid();
    slot->info.ppid       = ESPIX_PID_NONE;
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
    if (slot->was_kthread) {
        xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);
        slot->info.kind    = ESPIX_PROC_KTHREAD;
        slot->info.session = NULL;
        slot->was_kthread  = false;
        xSemaphoreGive(g_espix_proc_lock);
        return;
    }
    espix_proc_release_resources(slot);
    espix_proc_finish(slot, ESPIX_PROC_EXITED, status);
}

/* ------------------------------------------------------- espix_kthread_* --- */

typedef struct {
    espix_kthread_fn_t fn;
    void              *arg;
} kthread_arg_t;

static int kthread_main(void *arg)
{
    const kthread_arg_t st = *(kthread_arg_t *)arg;
    free(arg);
    st.fn(st.arg);
    return 0;
}

static esp_err_t kthread_start(const espix_kthread_t *k, TaskHandle_t *out)
{
    if (k->thread) {
        espix_proc_slot_t *const owner = espix_proc_self();
        /* Not a process, or a full thread table, leaves it a process of its
         * own: counted either way. */
        if (owner != NULL && espix_proc_thread_start(owner, k, out) == ESP_OK) {
            return ESP_OK;
        }
    }

    kthread_arg_t *const a = malloc(sizeof(*a));
    if (a == NULL) {
        return ESP_ERR_NO_MEM;
    }
    *a = (kthread_arg_t){ .fn = k->fn, .arg = k->arg };

    const espix_proc_fn_opts_t o = {
        .name           = k->name,
        .fn             = kthread_main,
        .arg            = a,
        .kind           = ESPIX_PROC_KTHREAD,
        .stack          = k->stack,
        .priority       = k->priority,
        .core           = k->core,
        .internal_stack = k->internal_stack,
        .task_out       = out,
    };
    const esp_err_t err = espix_proc_spawn_fn(&o, NULL);
    if (err != ESP_OK) {
        free(a);
    }
    return err;
}

/* A task started before espix_proc, now given a slot. */
static void kthread_adopt(TaskHandle_t task, const char *name)
{
    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);
    espix_proc_slot_t *const slot = espix_proc_alloc_slot();
    if (slot != NULL) {
        strlcpy(slot->info.name, name, sizeof(slot->info.name));
        slot->info.kind       = ESPIX_PROC_KTHREAD;
        slot->info.pid        = espix_proc_next_pid();
        slot->info.ppid       = ESPIX_PID_NONE;
        slot->info.pgid       = slot->info.pid;
        slot->info.sid        = slot->info.pid;
        slot->info.started_us = esp_timer_get_time();
        slot->wake_fd         = -1;
        slot->info.task       = task;
        slot->info.state      = ESPIX_PROC_RUNNING;
    }
    xSemaphoreGive(g_espix_proc_lock);
}

/* An adopted task is ending: it has no fn_task to reap it. */
static void kthread_gone(TaskHandle_t task)
{
    espix_proc_slot_t *slot = NULL;

    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);
    for (int i = 0; i < ESPIX_PROC_MAX; i++) {
        espix_proc_slot_t *const e = &g_espix_proc_table.slots[i];
        if (e->info.state != ESPIX_PROC_FREE && e->info.task == task &&
            e->info.kind == ESPIX_PROC_KTHREAD && e->fn == NULL) {
            slot = e;
            break;
        }
    }
    xSemaphoreGive(g_espix_proc_lock);

    if (slot != NULL) {
        espix_proc_release_resources(slot);
        espix_proc_finish(slot, ESPIX_PROC_EXITED, 0);
    }
}

const espix_kthread_ops_t g_espix_kthread_ops = {
    .start = kthread_start,
    .adopt = kthread_adopt,
    .gone  = kthread_gone,
};
