/*
 * "This task is on its way out", announced rather than polled.
 *
 * Four places used to sleep 10 ms and re-read a task handle until the task
 * cleared it -- canvas_console, rfb, and audio twice. The task can just say so:
 * it calls espix_task_exited() as its last act, and the waiter blocks in
 * espix_task_exit_wait() until that happens (R-P7.1).
 *
 * task stays a plain TaskHandle_t because xTaskCreate*() writes it through its
 * address; NULL means nothing is running.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The task-notification index a process waits on for signals, and the only one
 * a signal ever touches. Index 0 belongs to stream buffers, pthread_join and
 * espix's own hand-offs; a signal that woke those waits -- xTaskAbortDelay did --
 * let a task carry on without the lock or the join it was waiting for. A wait
 * on this index is woken only by a notify on it, so a signal ends exactly the
 * waits that are waiting for one, and a notify sent first is remembered.
 *
 * A shell's waits are on it too: a child's exit notifies its parent here, and
 * a transport notifies it when input arrives.
 */
#define ESPIX_NOTIFY_SIG_IDX 1

typedef struct {
    TaskHandle_t      task;
    SemaphoreHandle_t done;
} espix_task_exit_t;

/*
 * Get the notification ready for a new task. Call before creating it, and again
 * before each restart: a give left over from a previous run is discarded, or
 * the next wait would take it and claim an exit that has not happened. False
 * when the semaphore cannot be created.
 */
bool espix_task_exit_init(espix_task_exit_t *t);

/* The exiting task's last act: clear the handle, then wake the waiter. */
void espix_task_exited(espix_task_exit_t *t);

/*
 * Block until the task has exited, or timeout_ms passes. Zero waits forever.
 * True when it has exited -- including when nothing was running, which is not a
 * wait at all.
 */
bool espix_task_exit_wait(espix_task_exit_t *t, uint32_t timeout_ms);

/*
 * espix's own tasks. Once espix_proc is up, one is a kernel process (KTHREAD)
 * of its own -- or a thread of one, with `thread` -- so ps names it, a pid
 * reaches it, and nothing of espix runs unaccounted for. Before that it is a
 * plain task, which espix_proc adopts when it starts. Components below
 * espix_proc start their tasks here for that reason, as the shell does through
 * its ops table.
 *
 * fn returning ends the task; it must not vTaskDelete() itself.
 */
typedef void (*espix_kthread_fn_t)(void *arg);

typedef struct {
    const char        *name;
    espix_kthread_fn_t fn;
    void              *arg;
    uint32_t           stack;           /* bytes */
    UBaseType_t        priority;
    BaseType_t         core;            /* a core, or tskNO_AFFINITY */
    bool               internal_stack;  /* internal RAM; else PSRAM first */
    bool               thread;          /* a thread of the caller's process */
} espix_kthread_t;

/* The heap a task stack comes from first; see CONFIG_ESPIX_INTERNAL_STACKS. */
#if CONFIG_ESPIX_INTERNAL_STACKS
#define ESPIX_STACK_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#else
#define ESPIX_STACK_CAPS MALLOC_CAP_SPIRAM
#endif

/* *out is the task, written before it runs. */
esp_err_t espix_kthread_start(const espix_kthread_t *k, TaskHandle_t *out);

/* espix_proc's side; registering it adopts the tasks started before. */
typedef struct {
    esp_err_t (*start)(const espix_kthread_t *k, TaskHandle_t *out);
    void      (*adopt)(TaskHandle_t task, const char *name);
    void      (*gone)(TaskHandle_t task);   /* an adopted task is ending */
} espix_kthread_ops_t;

void espix_kthread_set_ops(const espix_kthread_ops_t *ops);

/* CONFIG_ESPIX_HEAP_CHECK_MS's thread; nothing when that is 0. */
void espix_heapcheck_start(void);

#ifdef __cplusplus
}
#endif
