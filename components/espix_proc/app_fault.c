/*
 * A fault in an M-mode app ends the app, not the board, as an oops ends the
 * process on a Linux without an MMU. The exception's handler looks only at
 * whose task faulted: an app's, outside a critical section and an interrupt,
 * resumes in espix_app_fault() as task code, which reports it, runs the app's
 * handler for the signal if it has one, and ends the process as exit() does.
 * Any other fault panics as before. On Xtensa the handler is the exception
 * table's (xt_set_exception_handler); on RISC-V a wrap of the vectors' panic
 * entry, which the vectors return from with mret.
 *
 * What it cannot undo: memory the app wrote before it faulted, and a lock the
 * task held inside the kernel when it faulted there. A blown stack, and on
 * Xtensa a break (__builtin_trap()), are not exceptions this sees.
 */
#include "sdkconfig.h"

#include <signal.h>
#include <stdint.h>

#include "esp_attr.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "espix_kernel.h"
#include "espix_proc_priv.h"
#include "espix_shell.h"

#define TAG "fault"

/* What espix_app_fault() is told: the signal, the address and the pc. */
void espix_app_fault(uint32_t sig, uint32_t addr, uint32_t pc) __attribute__((noreturn));

extern int _text_start, _text_end, _iram_text_start, _iram_text_end;

/* The kernel's code, or the ROM's, holds pc: it faulted there on the app's behalf, not in the app's own. */
static bool kernel_code(uint32_t pc)
{
    return (pc >= (uint32_t)&_text_start && pc < (uint32_t)&_text_end) ||
           (pc >= (uint32_t)&_iram_text_start && pc < (uint32_t)&_iram_text_end) ||
           esp_ptr_in_rom((const void *)pc);
}

static const char *what(int sig)
{
    switch (sig) {
    case SIGILL: return "Illegal instruction";
    case SIGBUS: return "Bus error";
    case SIGFPE: return "Floating point exception";
    case SIGTRAP: return "Trace/breakpoint trap";
    default:     return "Segmentation fault";
    }
}

#define KILL_STACK 4096

/* A thread's fault ends its process as a kill from outside it does: no task of the process can do that to it. */
static void kill_task(void *arg)
{
    (void)espix_proc_signal((espix_pid_t)(uintptr_t)arg, SIGKILL);
    vTaskDelete(NULL);
}

void espix_app_fault(uint32_t sig, uint32_t addr, uint32_t pc)
{
    espix_proc_slot_t *const slot = espix_proc_self();
    const bool own = !kernel_code(pc);
    espix_klog(ESPIX_KLOG_WARN, TAG, "pid %d: %s at pc 0x%08x, address 0x%08x, %s",
               (int)slot->info.pid, what((int)sig), (unsigned)pc, (unsigned)addr,
               own ? "in the app" : "in the kernel for it; a lock it held stays held");
    if (espix_proc_fault_handler(slot, (int)sig, addr)) {
        espix_klog(ESPIX_KLOG_INFO, TAG, "pid %d: its handler returned; it ends", (int)slot->info.pid);
    } else {
        espix_eprintf(slot->info.session, "%s: %s (pc 0x%08x, address 0x%08x)\n", slot->info.name,
                      what((int)sig), (unsigned)pc, (unsigned)addr);
    }
    slot->term_sig = (uint8_t)sig;
    if (slot->info.task != xTaskGetCurrentTaskHandle() &&
        xTaskCreate(kill_task, "fault:kill", KILL_STACK, (void *)(uintptr_t)slot->info.pid,
                    CONFIG_ESPIX_PROC_PRIORITY, NULL) == pdPASS) {
        vTaskSuspend(NULL);     /* deleted with the rest of the process */
    }
    espix_proc_exit(128 + (int)sig);
}

/* The faulting task is an app's own, running it: its main task with exit()'s way home, or one of its threads. */
static bool app_task(void)
{
    const espix_proc_slot_t *const slot = pvTaskGetThreadLocalStoragePointer(NULL, ESPIX_TLS_PROC_IDX);
    if (slot == NULL || slot->info.kind != ESPIX_PROC_APP || slot->app == NULL || slot->exiting || slot->loading) {
        return false;
    }
#if CONFIG_ESPIX_UMODE
    /* A U-mode app's faults are its own (umode.c); one in the kernel on its behalf is the kernel's. */
    if (slot->app->um != NULL || slot->app->fork != NULL) {
        return false;
    }
#endif
    /* Its threads carry the slot too; the loader's helper does so only while loading. */
    return slot->info.task != xTaskGetCurrentTaskHandle() || slot->exit_jmp != NULL;
}

#if CONFIG_IDF_TARGET_ARCH_XTENSA

#include "esp_cpu.h"
#include "esp_ipc.h"
#include "esp_private/cache_utils.h"
#include "xtensa/corebits.h"
#include "xtensa_api.h"
#include "xtensa_context.h"

void espix_app_fault_tramp(void);
void xt_unhandled_exception(void *frame);

static int signal_of(uint32_t cause)
{
    switch (cause) {
    case EXCCAUSE_ILLEGAL:        return SIGILL;
    case EXCCAUSE_DIVIDE_BY_ZERO: return SIGFPE;
    case EXCCAUSE_UNALIGNED:      return SIGBUS;
    default:                      return SIGSEGV;
    }
}

/*
 * In the exception, on the faulting task's stack. Resumed by the exception's
 * return in espix_app_fault_tramp(), which calls espix_app_fault() from the
 * faulting function's frame with a2-a4 as its arguments.
 */
static IRAM_ATTR void on_fault(XtExcFrame *f)
{
    /* The cache first: with it off, nothing in flash or PSRAM may be touched, this test aside. */
    if (!spi_flash_cache_enabled() || (f->ps & PS_INTLEVEL_MASK) != 0 || xPortInIsrContext() ||
        xTaskGetSchedulerState() != taskSCHEDULER_RUNNING || !app_task()) {
        xt_unhandled_exception(f);
        return;
    }
    f->a2 = signal_of((uint32_t)f->exccause);
    f->a3 = f->excvaddr;
    f->a4 = f->pc;
    f->pc = (long)espix_app_fault_tramp;
}

static const int s_causes[] = {
    EXCCAUSE_ILLEGAL, EXCCAUSE_INSTR_ERROR, EXCCAUSE_LOAD_STORE_ERROR, EXCCAUSE_DIVIDE_BY_ZERO,
    EXCCAUSE_UNALIGNED, EXCCAUSE_INSTR_PROHIBITED, EXCCAUSE_LOAD_PROHIBITED, EXCCAUSE_STORE_PROHIBITED,
};

static void install(void *arg)
{
    (void)arg;
    for (size_t i = 0; i < sizeof(s_causes) / sizeof(s_causes[0]); i++) {
        (void)xt_set_exception_handler(s_causes[i], (xt_exc_handler)on_fault);
    }
}

/* Each core has its own table. */
void espix_app_fault_init(void)
{
    install(NULL);
    for (int c = 0; c < portNUM_PROCESSORS; c++) {
        if (c != esp_cpu_get_core_id()) {
            (void)esp_ipc_call_blocking(c, install, NULL);
        }
    }
}

#elif CONFIG_IDF_TARGET_ARCH_RISCV

#include "esp_private/cache_utils.h"
#include "esp_private/panic_internal.h"
#include "riscv/encoding.h"
#include "riscv/rvruntime-frames.h"

/* The vectors' panic_from_exception() is this, and IDF's deliberate aborts call it too, with g_panic_abort set. */
void __real_xt_unhandled_exception(void *frame);

static int signal_of(uint32_t cause)
{
    switch (cause) {
    case 2:  return SIGILL;
    case 3:  return SIGTRAP;
    case 0: case 4: case 6: return SIGBUS;
    default: return SIGSEGV;
    }
}

/* In the exception: a contained one returns, and the vectors' mret resumes espix_app_fault() on the task. */
IRAM_ATTR void __wrap_xt_unhandled_exception(void *frame)
{
    RvExcFrame *const f = frame;
    const uint32_t cause = (uint32_t)f->mcause;
    if (!g_panic_abort && cause <= 7 && spi_flash_cache_enabled() && (f->mstatus & MSTATUS_MPIE) &&
        !xPortInIsrContext() && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING && app_task()) {
        f->a0   = (uint32_t)signal_of(cause);
        f->a1   = f->mtval;
        f->a2   = f->mepc;
        f->ra   = 0;
        f->mepc = (uint32_t)espix_app_fault;
        return;
    }
    __real_xt_unhandled_exception(frame);
}

/* The wrap is the linker's to install. */
void espix_app_fault_init(void)
{
}

#endif /* CONFIG_IDF_TARGET_ARCH_RISCV */
