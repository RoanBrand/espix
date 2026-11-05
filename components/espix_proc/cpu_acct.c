/*
 * Per-core CPU time accounting.
 *
 * /proc/stat needs user, system and idle time.  Idle already comes from the
 * IDLE tasks' run-time counters.  The user/system split is sampled from the
 * FreeRTOS tick: when the tick interrupt arrives, mstatus.MPP still says
 * whether the interrupted code was running in U-mode (an app) or M-mode (a
 * syscall, an ISR, or a kernel task), and the current TCB says whether that
 * code was the idle task.  Each tick adds its 4 ms to that core's system
 * counter; /proc/stat subtracts it from the busy time.
 *
 * This is deliberately a tick-time sample, not a per-syscall timestamp: there
 * is no allocation, no process-table walk and no extra timer.  The counters are
 * two uint64_t per core, and /proc/stat reads them under one short spinlock.
 */
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_err.h"
#include "esp_freertos_hooks.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "riscv/csr.h"
#include "riscv/encoding.h"

#include "espix_proc.h"

#define TAG "cpuacct"

#define CPU_ACCT_CORES  CONFIG_FREERTOS_NUMBER_OF_CORES
#define CPU_ACCT_TICK_US (1000000 / configTICK_RATE_HZ)

/* FreeRTOS's per-core current TCB, global in this kernel.  Reading it is an
 * IRAM-safe load, unlike xTaskGetCurrentTaskHandle() (which lives in flash). */
extern void *pxCurrentTCBs[CPU_ACCT_CORES];

static uint64_t    s_system_us[CPU_ACCT_CORES];
static TaskHandle_t s_idle[CPU_ACCT_CORES];
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool         s_started;

static IRAM_ATTR void cpu_acct_tick(void)
{
    const int core = esp_cpu_get_core_id();
    if (core < 0 || core >= CPU_ACCT_CORES) {
        return;
    }

    /* The idle task is M-mode too, but its ticks are idle, not system. */
    if (pxCurrentTCBs[core] == (void *)s_idle[core]) {
        return;
    }

    /*
     * MPP is the privilege the trap came from: 0 is U-mode, 3 is M-mode.
     * A U-mode app's own code is user time; its syscalls, every ISR and every
     * kernel task are M-mode and count as system.
     */
    if ((RV_READ_CSR(mstatus) & MSTATUS_MPP) == 0) {
        return;
    }

    portENTER_CRITICAL_ISR(&s_lock);
    s_system_us[core] += CPU_ACCT_TICK_US;
    portEXIT_CRITICAL_ISR(&s_lock);
}

esp_err_t espix_cpu_acct_init(void)
{
    if (s_started) {
        return ESP_OK;
    }

    for (int c = 0; c < CPU_ACCT_CORES; c++) {
        s_idle[c] = xTaskGetIdleTaskHandleForCore(c);
        if (s_idle[c] == NULL) {
            ESP_LOGW(TAG, "no idle task handle for core %d", c);
            return ESP_ERR_INVALID_STATE;
        }
    }

    for (int c = 0; c < CPU_ACCT_CORES; c++) {
        const esp_err_t err = esp_register_freertos_tick_hook_for_cpu(cpu_acct_tick, c);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "tick hook on core %d: %s", c, esp_err_to_name(err));
            return err;
        }
    }

    s_started = true;
    return ESP_OK;
}

uint64_t espix_cpu_system_us(int core)
{
    if (core < 0 || core >= CPU_ACCT_CORES) {
        return 0;
    }

    uint64_t us;
    portENTER_CRITICAL(&s_lock);
    us = s_system_us[core];
    portEXIT_CRITICAL(&s_lock);
    return us;
}
