/* Internal to the espix_proc component: the process table representation
 * shared between proc.c (table, wait, kill) and exec.c (loading and running). */
#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "esp_elf.h"

#include "espix_proc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESPIX_PROC_MAX CONFIG_ESPIX_PROC_MAX

typedef struct {
    espix_proc_info_t info;

    esp_elf_t elf;
    bool      elf_valid;
    uint8_t  *image;        /* raw file bytes, freed once the ELF is torn down */

    /* argv storage: one allocation holding the char* array followed by the
     * argument strings, so the whole vector frees in one call. */
    void  *argv_block;
    int    argc;
    char **argv;

    /*
     * Set when someone has asked this process to stop. Volatile because the
     * app polls it from its own task while another task writes it, and no lock
     * is taken on the read path: a single bool needs none, and an app should
     * not block to ask whether it is still wanted.
     */
    volatile bool stop_requested;
} espix_proc_slot_t;

/* Table access. The lock covers slot allocation and state transitions; readers
 * that must not block (the fault path) read without it and tolerate a torn
 * view, which is why `pid` is written last on allocation. */
extern espix_proc_slot_t  g_espix_procs[ESPIX_PROC_MAX];
extern SemaphoreHandle_t  g_espix_proc_lock;
extern EventGroupHandle_t g_espix_proc_events;

/* Claim a free slot (preferring one never used, else the oldest finished one).
 * Returns NULL if the table is full of live processes. Caller must hold the
 * lock. */
espix_proc_slot_t *espix_proc_alloc_slot(void);

/* Release everything a finished slot owns: ELF image, argv block. Caller must
 * NOT hold the lock. */
void espix_proc_release_resources(espix_proc_slot_t *slot);

/* Record a terminal state and wake anyone in espix_proc_wait(). */
void espix_proc_finish(espix_proc_slot_t *slot, espix_proc_state_t state,
                       int exit_code);

/* Monotonic pid allocation; pids are never reused. Caller must hold the lock. */
espix_pid_t espix_proc_next_pid(void);

/* Publish the C++ runtime an app needs to resolve at load time. See
 * abi_cxx.cpp, the one C++ translation unit in espix. */
void espix_proc_abi_cxx_register(void);

/* Publish the peripheral surface an app needs. See abi_drivers.c: naming a
 * symbol there is also what keeps its driver linked into the firmware. */
void espix_proc_abi_drivers_register(void);
void espix_proc_abi_time_register(void);

#ifdef __cplusplus
}
#endif
