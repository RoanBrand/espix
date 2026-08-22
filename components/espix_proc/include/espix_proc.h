/*
 * espix process table.
 *
 * A "process" is a FreeRTOS task plus the bookkeeping that makes it
 * addressable from a shell: a pid, a name, an exit status, and — for apps
 * loaded off the filesystem — the relocated ELF image it is running.
 *
 * There is no memory isolation between processes on chips without an MMU; see
 * the crash-handling model in the project README. What this table buys is
 * identity: something for `ps` to list, `kill` to target, and the fault path
 * to name when a task dies.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"

#include "espix_kernel.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESPIX_PROC_NAME_MAX 24

typedef enum {
    ESPIX_PROC_FREE = 0,    /* table slot unused */
    ESPIX_PROC_READY,       /* accepted, task not started yet */
    ESPIX_PROC_RUNNING,
    ESPIX_PROC_EXITED,      /* returned normally */
    ESPIX_PROC_FAULTED,     /* killed by the fault handler */
    ESPIX_PROC_KILLED,      /* killed by request */
} espix_proc_state_t;

typedef struct {
    espix_pid_t        pid;
    char               name[ESPIX_PROC_NAME_MAX];
    char               path[ESPIX_PATH_MAX];
    TaskHandle_t       task;
    espix_proc_state_t state;
    int                exit_code;
    int64_t            started_us;
    size_t             image_bytes;   /* relocated ELF size, 0 for kernel tasks */
    espix_session_t   *session;       /* stdio/cwd owner, may be NULL */
} espix_proc_info_t;

esp_err_t espix_proc_init(void);

/*
 * Load the ELF at `abs_path` and run it as a new process. Returns as soon as
 * the process is admitted; use espix_proc_wait() to block for completion.
 *
 * `argv` is copied, so the caller's buffers need not outlive the call.
 */
esp_err_t espix_proc_spawn_elf(const char *abs_path, int argc, char **argv,
                               espix_session_t *session, espix_pid_t *out_pid);

/*
 * Block until `pid` leaves the running state. Returns ESP_ERR_TIMEOUT if it is
 * still running when `timeout` expires.
 */
esp_err_t espix_proc_wait(espix_pid_t pid, int *out_exit_code, TickType_t timeout);

/*
 * Kill a process. Unsafe in the general case on a shared-address-space system
 * — the task may hold a lock or own heap blocks it will now never free — so it
 * reclaims the ELF image but makes no attempt to undo anything else the task
 * did. See the reaper notes in espix_fault.
 */
esp_err_t espix_proc_kill(espix_pid_t pid);

/* Copy up to `n` live entries into `out`. Returns how many were written. */
size_t espix_proc_snapshot(espix_proc_info_t *out, size_t n);

/* Look up the process owning `task`, or ESPIX_PID_NONE. Safe to call from a
 * restricted context: it only reads the table. */
espix_pid_t espix_proc_pid_of_task(TaskHandle_t task);

const char *espix_proc_state_str(espix_proc_state_t state);

#ifdef __cplusplus
}
#endif
