/*
 * espix fault interception.
 *
 * The hook is -Wl,--wrap=esp_panic_handler, applied from this component's
 * CMakeLists. ESP-IDF's own test suite uses the same mechanism
 * (components/esp_system/test_apps/esp_system_unity_tests), so this is a
 * supported seam rather than a trick.
 */

#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_attr.h"
#include "esp_private/panic_internal.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "espix_fault.h"
#include "espix_fault_priv.h"
#include "espix_kernel.h"
#include "espix_proc.h"

#define TAG "fault"

#define FAULT_MAGIC 0x58465045u    /* 'XFPE' */

#if CONFIG_ESP_PANIC_HANDLER_IRAM
#error "espix_fault reads task names and the process table from the panic path, \
which requires flash cache to be available. Disable CONFIG_ESP_PANIC_HANDLER_IRAM, \
or reduce __wrap_esp_panic_handler to IRAM-safe operations only."
#endif

/*
 * Survives a software reset (but not a power cycle), which is exactly the
 * lifetime we need: the panic handler writes it, the next boot reads it.
 */
static __NOINIT_ATTR espix_fault_record_t s_record;

/* Snapshot taken at init, before s_record is invalidated. */
static espix_fault_record_t s_last;
static bool                 s_have_last;

static void copy_str(char *dst, size_t dst_len, const char *src)
{
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    /* Not strlcpy: that lives in libc, and this runs from the panic path. */
    size_t i = 0;
    while (src[i] != '\0' && i + 1 < dst_len) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static const char *exception_str(int exception)
{
    switch (exception) {
    case PANIC_EXCEPTION_DEBUG: return "debug";
    case PANIC_EXCEPTION_IWDT:  return "int-wdt";
    case PANIC_EXCEPTION_TWDT:  return "task-wdt";
    case PANIC_EXCEPTION_ABORT: return "abort";
    case PANIC_EXCEPTION_FAULT: return "fault";
    default:                    return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/* The hook                                                            */
/* ------------------------------------------------------------------ */

void __real_esp_panic_handler(void *info);

void __wrap_esp_panic_handler(void *info)
{
    const panic_info_t *pi = info;

    if (pi != NULL) {
        s_record.magic     = FAULT_MAGIC;
        s_record.core      = pi->core;
        s_record.exception = (int)pi->exception;
        s_record.addr      = (uintptr_t)pi->addr;
        s_record.uptime_us = esp_timer_get_time();
        copy_str(s_record.reason, sizeof(s_record.reason), pi->reason);

        /* xTaskGetCurrentTaskHandleForCore() takes no lock (see the comment in
         * freertos_tasks_c_additions.h), so it is safe here — a lock would risk
         * deadlocking against whatever the dying task was holding. */
        TaskHandle_t task = NULL;
        if (pi->core >= 0 && pi->core < CONFIG_FREERTOS_NUMBER_OF_CORES) {
            task = xTaskGetCurrentTaskHandleForCore((BaseType_t)pi->core);
        }

        if (task != NULL) {
            copy_str(s_record.task, sizeof(s_record.task), pcTaskGetName(task));
            s_record.pid = espix_proc_pid_of_task(task);
        } else {
            copy_str(s_record.task, sizeof(s_record.task), "?");
            s_record.pid = ESPIX_PID_NONE;
        }

        panic_print_str("\r\nespix: fault in task '");
        panic_print_str(s_record.task);
        panic_print_str("' (");
        panic_print_str(exception_str(s_record.exception));
        panic_print_str(": ");
        panic_print_str(s_record.reason[0] ? s_record.reason : "?");
        panic_print_str(") — recorded for next boot\r\n");
    }

    /*
     * Delegate: print the register dump / backtrace and reboot as usual.
     *
     * This is where "reap the task and keep running" will eventually branch in.
     * It is not a matter of skipping this call — the scheduler is frozen here
     * and the dying task may hold the heap or VFS lock, so resuming needs the
     * deferred reaper path plus a story for held locks. See espix_fault.h.
     */
    __real_esp_panic_handler(info);
}

/* ------------------------------------------------------------------ */

const espix_fault_record_t *espix_fault_last(void)
{
    return s_have_last ? &s_last : NULL;
}

/*
 * Task watchdog triggers since boot.
 *
 * The watchdog watches the IDLE tasks, and IDLE runs at priority 0 -- so it only
 * runs when nothing else is ready. Any task that stays runnable for the timeout
 * starves it on that core, and IDF says so and carries on:
 * CONFIG_ESP_TASK_WDT_PANIC is off, so this never resets the board.
 *
 * That is not cosmetic here even though nothing crashes. IDLE performs FreeRTOS's
 * deferred task cleanup -- freeing the TCB and stack of anything that called
 * vTaskDelete() -- and espix creates and destroys a task per process and per SSH
 * connection. A starved IDLE defers exactly the reclamation espix leans on
 * hardest, so a trigger is a real event worth counting.
 *
 * Counted rather than left to the log, because the log cannot be trusted to
 * still hold it: klog is CONFIG_ESPIX_KLOG_LINES entries and four parallel test
 * workers churn that in well under a minute, while the test runner polls every
 * ten seconds. A counter cannot be missed by a reader that arrives late.
 *
 * esp_task_wdt_isr_user_handler() is a *weak* hook ESP-IDF's own task_wdt_isr()
 * calls between printing the offending tasks and handling the timeout -- not an
 * override. Defining it adds this and takes nothing away: the "did not reset the
 * watchdog in time" lines, the running-task list and the backtrace all still
 * happen. No IRAM_ATTR: task_wdt_isr() itself is not in IRAM, so putting this
 * there would buy nothing and cost internal RAM.
 */
static volatile uint32_t s_wdt_events;

void esp_task_wdt_isr_user_handler(void)
{
    s_wdt_events++;
}

uint32_t espix_fault_wdt_count(void)
{
    return s_wdt_events;
}

const char *espix_fault_reset_reason_str(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "int-wdt";
    case ESP_RST_TASK_WDT:  return "task-wdt";
    case ESP_RST_WDT:       return "other-wdt";
    case ESP_RST_DEEPSLEEP: return "deep-sleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "sdio";
    default:                return "unknown";
    }
}

esp_err_t espix_fault_init(void)
{
    const esp_reset_reason_t reason = esp_reset_reason();

    if (s_record.magic == FAULT_MAGIC) {
        s_last = s_record;
        s_have_last = true;
        s_record.magic = 0;        /* consume it, so we report it once */

        espix_klog(ESPIX_KLOG_ERROR, TAG,
                   "previous boot: %s in task '%s' at 0x%08x (%s), core %d",
                   exception_str(s_last.exception),
                   s_last.task,
                   (unsigned)s_last.addr,
                   s_last.reason[0] ? s_last.reason : "?",
                   s_last.core);

        if (s_last.pid != ESPIX_PID_NONE) {
            espix_klog(ESPIX_KLOG_ERROR, TAG,
                       "previous boot: the faulting task was espix pid %d",
                       (int)s_last.pid);
        }
    } else {
        /* Uninitialised noinit memory is whatever was there before; only trust
         * it when the magic matches. */
        s_record.magic = 0;
        espix_klog(ESPIX_KLOG_INFO, TAG, "reset reason: %s",
                   espix_fault_reset_reason_str());
    }

    if (reason == ESP_RST_PANIC && !s_have_last) {
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "previous boot panicked before the espix hook recorded it");
    }

    espix_fault_report_coredump();

    return espix_fault_reaper_start();
}
