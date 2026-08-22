/*
 * espix process table: allocation, introspection, wait, kill.
 */

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "espix_kernel.h"
#include "espix_proc.h"
#include "espix_proc_priv.h"

#define TAG "proc"

espix_proc_slot_t  g_espix_procs[ESPIX_PROC_MAX];
SemaphoreHandle_t  g_espix_proc_lock;
EventGroupHandle_t g_espix_proc_events;

static espix_pid_t s_next_pid = 1;

const char *espix_proc_state_str(espix_proc_state_t state)
{
    switch (state) {
    case ESPIX_PROC_FREE:    return "free";
    case ESPIX_PROC_READY:   return "ready";
    case ESPIX_PROC_RUNNING: return "run";
    case ESPIX_PROC_EXITED:  return "exit";
    case ESPIX_PROC_FAULTED: return "fault";
    case ESPIX_PROC_KILLED:  return "kill";
    default:                 return "?";
    }
}

static bool state_is_finished(espix_proc_state_t s)
{
    return s == ESPIX_PROC_EXITED || s == ESPIX_PROC_FAULTED ||
           s == ESPIX_PROC_KILLED;
}

esp_err_t espix_proc_init(void)
{
    if (g_espix_proc_lock != NULL) {
        return ESP_OK;
    }

    g_espix_proc_lock = xSemaphoreCreateMutex();
    g_espix_proc_events = xEventGroupCreate();

    if (g_espix_proc_lock == NULL || g_espix_proc_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    memset(g_espix_procs, 0, sizeof(g_espix_procs));

    /* The loader announces its version and entry address on every single load.
     * That is startup chatter, not something a user running an app wants to
     * see, so lift its threshold to warnings. */
    esp_log_level_set("ELF", ESP_LOG_WARN);

    espix_klog(ESPIX_KLOG_INFO, TAG, "process table ready (%d slots)",
               ESPIX_PROC_MAX);
    return ESP_OK;
}

espix_proc_slot_t *espix_proc_alloc_slot(void)
{
    espix_proc_slot_t *oldest_done = NULL;

    for (int i = 0; i < ESPIX_PROC_MAX; i++) {
        espix_proc_slot_t *s = &g_espix_procs[i];

        if (s->info.state == ESPIX_PROC_FREE) {
            return s;
        }
        if (state_is_finished(s->info.state)) {
            if (oldest_done == NULL ||
                s->info.started_us < oldest_done->info.started_us) {
                oldest_done = s;
            }
        }
    }

    if (oldest_done != NULL) {
        /* Reclaiming a finished slot: its resources were released when it
         * finished, so only the bookkeeping needs clearing. */
        memset(oldest_done, 0, sizeof(*oldest_done));
    }
    return oldest_done;
}

void espix_proc_release_resources(espix_proc_slot_t *slot)
{
    if (slot == NULL) {
        return;
    }

    if (slot->elf_valid) {
        esp_elf_deinit(&slot->elf);
        slot->elf_valid = false;
    }

    free(slot->image);
    slot->image = NULL;

    free(slot->argv_block);
    slot->argv_block = NULL;
    slot->argv = NULL;
    slot->argc = 0;
}

void espix_proc_finish(espix_proc_slot_t *slot, espix_proc_state_t state,
                       int exit_code)
{
    if (slot == NULL) {
        return;
    }

    const int index = (int)(slot - g_espix_procs);

    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);
    slot->info.state     = state;
    slot->info.exit_code = exit_code;
    slot->info.task      = NULL;
    xSemaphoreGive(g_espix_proc_lock);

    xEventGroupSetBits(g_espix_proc_events, (EventBits_t)1 << index);
}

static espix_proc_slot_t *find_by_pid(espix_pid_t pid)
{
    for (int i = 0; i < ESPIX_PROC_MAX; i++) {
        if (g_espix_procs[i].info.state != ESPIX_PROC_FREE &&
            g_espix_procs[i].info.pid == pid) {
            return &g_espix_procs[i];
        }
    }
    return NULL;
}

espix_pid_t espix_proc_next_pid(void)
{
    return s_next_pid++;
}

esp_err_t espix_proc_wait(espix_pid_t pid, int *out_exit_code, TickType_t timeout)
{
    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);
    espix_proc_slot_t *slot = find_by_pid(pid);
    const int index = (slot != NULL) ? (int)(slot - g_espix_procs) : -1;
    const espix_proc_state_t state = (slot != NULL) ? slot->info.state
                                                    : ESPIX_PROC_FREE;
    xSemaphoreGive(g_espix_proc_lock);

    if (slot == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    if (!state_is_finished(state)) {
        const EventBits_t bit = (EventBits_t)1 << index;
        const EventBits_t got = xEventGroupWaitBits(g_espix_proc_events, bit,
                                                    pdFALSE, pdTRUE, timeout);
        if ((got & bit) == 0) {
            return ESP_ERR_TIMEOUT;
        }
    }

    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);
    /* Re-check identity: the slot could have been recycled while we waited. */
    const bool same = (slot->info.pid == pid);
    if (same && out_exit_code != NULL) {
        *out_exit_code = slot->info.exit_code;
    }
    xSemaphoreGive(g_espix_proc_lock);

    return same ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t espix_proc_kill(espix_pid_t pid)
{
    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);

    espix_proc_slot_t *slot = find_by_pid(pid);
    if (slot == NULL) {
        xSemaphoreGive(g_espix_proc_lock);
        return ESP_ERR_NOT_FOUND;
    }
    if (state_is_finished(slot->info.state)) {
        xSemaphoreGive(g_espix_proc_lock);
        return ESP_ERR_INVALID_STATE;
    }

    TaskHandle_t task = slot->info.task;
    slot->info.task = NULL;
    xSemaphoreGive(g_espix_proc_lock);

    /*
     * Deleting another task on a system with no memory protection is a blunt
     * instrument: anything it was holding at the time — a VFS mutex, a heap
     * block, an open file — stays held or leaked. We reclaim what the process
     * table owns (its ELF image and argv) and nothing more. This is the same
     * accepted tradeoff described in the project's crash-handling model, and
     * the reason espix_fault's reaper exists as a separate, deferred path.
     */
    if (task != NULL) {
        vTaskDelete(task);
    }

    espix_klog(ESPIX_KLOG_WARN, TAG, "killed pid %d (%s)",
               (int)pid, slot->info.name);

    espix_proc_release_resources(slot);
    espix_proc_finish(slot, ESPIX_PROC_KILLED, -1);

    return ESP_OK;
}

size_t espix_proc_snapshot(espix_proc_info_t *out, size_t n)
{
    if (out == NULL || n == 0) {
        return 0;
    }

    size_t count = 0;

    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);
    for (int i = 0; i < ESPIX_PROC_MAX && count < n; i++) {
        if (g_espix_procs[i].info.state != ESPIX_PROC_FREE) {
            out[count++] = g_espix_procs[i].info;
        }
    }
    xSemaphoreGive(g_espix_proc_lock);

    return count;
}

espix_pid_t espix_proc_pid_of_task(TaskHandle_t task)
{
    if (task == NULL) {
        return ESPIX_PID_NONE;
    }

    /* Lock-free on purpose: the fault handler calls this from panic context,
     * where taking a mutex is not an option. */
    for (int i = 0; i < ESPIX_PROC_MAX; i++) {
        if (g_espix_procs[i].info.task == task) {
            return g_espix_procs[i].info.pid;
        }
    }
    return ESPIX_PID_NONE;
}
