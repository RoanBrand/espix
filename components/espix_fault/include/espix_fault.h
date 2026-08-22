/*
 * espix fault handling.
 *
 * espix intercepts the ESP-IDF panic path via -Wl,--wrap=esp_panic_handler
 * (the same hook ESP-IDF's own test suite uses). Today the hook is
 * observational: it records who died and why into memory that survives the
 * reset, then delegates to the real handler, so the next boot can report the
 * post-mortem in `dmesg`.
 *
 * The long-term goal — reap the faulting task and keep the system up instead of
 * rebooting — is deliberately NOT implemented yet. The panic handler runs with
 * the scheduler frozen and non-IRAM interrupts off, so cleanup has to be
 * deferred to a reaper task; and on a chip without memory protection, a task
 * that dies holding the heap lock or a VFS mutex wedges everything else. The
 * reaper task and its queue exist here so that work has a defined home, but
 * nothing feeds them yet. See CONFIG_ESPIX_FAULT_REAP.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"

#include "espix_kernel.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESPIX_FAULT_TASK_NAME_MAX 16
#define ESPIX_FAULT_REASON_MAX    32

typedef struct {
    uint32_t magic;
    int      core;
    int      exception;                             /* panic_exception_t */
    uintptr_t addr;                                 /* faulting instruction */
    char     reason[ESPIX_FAULT_REASON_MAX];
    char     task[ESPIX_FAULT_TASK_NAME_MAX];
    espix_pid_t pid;                                /* ESPIX_PID_NONE if not an espix process */
    int64_t  uptime_us;                             /* uptime at the fault */
} espix_fault_record_t;

/*
 * Start the reaper task and, if the previous boot ended in a fault, log the
 * post-mortem to the kernel log. Call early — before the filesystem, so a
 * mount that panics is still reported next boot.
 */
esp_err_t espix_fault_init(void);

/*
 * Details of the fault that ended the previous boot, or NULL if the last reset
 * was clean. Valid for the lifetime of the boot.
 */
const espix_fault_record_t *espix_fault_last(void);

/* Reset reason as a short string, for `dmesg` / `uname`. */
const char *espix_fault_reset_reason_str(void);

/*
 * Ask for `task` to be torn down from normal context. Safe to call from the
 * panic path (queue send only, no allocation). Currently unused — see the
 * header comment.
 */
void espix_fault_request_reap(TaskHandle_t task);

/*
 * Core dump stored in flash by the panic handler.
 *
 * This is the heavyweight companion to espix_fault_last(): that is a one-line
 * summary in noinit RAM, good for a boot message; this is every task's
 * registers and stack, decodable on the host with `idf.py coredump-info`.
 */
typedef struct {
    bool     present;
    size_t   size;
    size_t   flash_addr;
    char     task[ESPIX_FAULT_TASK_NAME_MAX];   /* task that faulted */
    uint32_t pc;                                /* PC at the fault */

    /*
     * False when the dump was produced by a different firmware build than the
     * one running — dumps survive reflashing, and addresses from an old build
     * decode to plausible but wrong functions against the current ELF.
     */
    bool     same_build;
} espix_coredump_info_t;

/* Fills `out`; out->present is false when the partition holds no valid dump. */
esp_err_t espix_fault_coredump_status(espix_coredump_info_t *out);

/* Free the coredump partition. The dump is unrecoverable afterwards. */
esp_err_t espix_fault_coredump_erase(void);

#ifdef __cplusplus
}
#endif
