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

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef __cplusplus
}
#endif
