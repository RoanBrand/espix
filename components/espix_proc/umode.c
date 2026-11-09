/*
 * U-mode apps on the S31 (M1): the boot hook, the trap hooks the patched IDF
 * vectors call (tools/patch-umode.py), the first syscalls, and exec.
 * docs/USER-MODE.md has the design.
 */
#include "sdkconfig.h"

#if CONFIG_ESPIX_UMODE

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_ipc_isr.h"
#include "esp_private/startup_internal.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/assist_debug_ll.h"
#include "riscv/csr.h"
#include "riscv/encoding.h"
#include "riscv/rvruntime-frames.h"
#include "soc/bus_monitor_reg.h"
#include "soc/cpu_apm_reg.h"

#include "espix_fs.h"
#include "espix_kernel.h"
#include "espix_shell.h"
#include "espix_umode.h"

#define TAG "umode"

#define PG          4096u
#define UVA_MIN     0x00010000u
#define UVA_MAX     0x70000000u
#define USTACK_TOP  ESPIX_UM_STACK_TOP
#define USTACK_MAX  ESPIX_UM_STACK_MAX
#define ARGS_MAX    (64u * 1024u)
#define UM_KICK_PC  0x00000ffcu                 /* below the image, never mapped */

/* Each core's current U-mode thread and its kernel tp; the patched vectors read them on entry from U. */
espix_um_thread_t *espix_um_cur[portNUM_PROCESSORS];
uint32_t espix_um_ktp[portNUM_PROCESSORS];
/* The satp each core last returned to U with, for espix_um_asid_shoot(). */
static uint32_t s_cur_satp[portNUM_PROCESSORS];

static uint32_t s_boot_mscratch[portNUM_PROCESSORS];


/* ASIDs: one per live process; a freed one is flushed on each core before its next use there. */
#define ASIDS 512
static uint32_t    s_asid_used[ASIDS / 32] = { 1 };
static uint32_t    s_asid_stale[portNUM_PROCESSORS][ASIDS / 32];
static portMUX_TYPE s_asid_lock = portMUX_INITIALIZER_UNLOCKED;

/* The images' shared text (load_segments), and vfork's handshake. */
static SemaphoreHandle_t s_texts_lock;
static SemaphoreHandle_t s_vfork_lock;

static uint32_t asid_alloc(void)
{
    uint32_t asid = 0;
    taskENTER_CRITICAL(&s_asid_lock);
    for (uint32_t i = 1; i < ASIDS; i++) {
        if (!(s_asid_used[i / 32] & (1u << (i % 32)))) {
            s_asid_used[i / 32] |= 1u << (i % 32);
            asid = i;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_asid_lock);
    return asid;
}

static void asid_free(uint32_t asid)
{
    taskENTER_CRITICAL(&s_asid_lock);
    s_asid_used[asid / 32] &= ~(1u << (asid % 32));
    for (int c = 0; c < portNUM_PROCESSORS; c++) {
        __atomic_fetch_or(&s_asid_stale[c][asid / 32], 1u << (asid % 32), __ATOMIC_SEQ_CST);
    }
    taskEXIT_CRITICAL(&s_asid_lock);
}

void espix_um_enter(uint32_t pc, uint32_t usp, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t utp);
void espix_um_return(void *frame);
void espix_um_return_to(RvExcFrame *frame, espix_um_thread_t *t);

/* Each core's lockup monitor at boot: its last two traps, the second taken inside the first's handler on a lockup. */
static uint32_t s_lockup[portNUM_PROCESSORS][2][4];

/* mscratch says which side of the boundary a trap came from, so it must start at 0. */
ESP_SYSTEM_INIT_FN(espix_um_core_init, SECONDARY, ESP_SYSTEM_INIT_ALL_CORES, 100)
{
    const uint32_t core = esp_cpu_get_core_id();
    for (int i = 0; i < 2; i++) {
        s_lockup[core][i][0] = assist_debug_ll_lockup_get_cause(core, i);
        s_lockup[core][i][1] = assist_debug_ll_lockup_get_tval(core, i);
        s_lockup[core][i][2] = assist_debug_ll_lockup_get_iaddr(core, i);
        s_lockup[core][i][3] = assist_debug_ll_lockup_get_priv(core, i);
    }
    assist_debug_ll_lockup_monitor_enable(core, true);
    s_boot_mscratch[core] = RV_SWAP_CSR(mscratch, 0);
    RV_WRITE_CSR(mcounteren, 0x7);
    RV_WRITE_CSR(scounteren, 0x7);
    return ESP_OK;
}

void espix_um_init(void)
{
    const esp_reset_reason_t why = esp_reset_reason();
    if (why == ESP_RST_INT_WDT || why == ESP_RST_TASK_WDT || why == ESP_RST_WDT) {
        for (int c = 0; c < portNUM_PROCESSORS; c++) {
            for (int i = 0; i < 2; i++) {
                espix_klog(ESPIX_KLOG_WARN, TAG, "core %d's trap %d before the reset: cause 0x%x, tval 0x%08x at 0x%08x, priv %u",
                           c, i, (unsigned)s_lockup[c][i][0], (unsigned)s_lockup[c][i][1],
                           (unsigned)s_lockup[c][i][2], (unsigned)s_lockup[c][i][3]);
            }
        }
    }
    espix_um_frames_init();
    s_texts_lock = xSemaphoreCreateMutex();
    s_vfork_lock = xSemaphoreCreateMutex();
    /* cpu_apm region 0 (flash and PSRAM) is TEE-only, and U-mode is REE: the page tables isolate instead. */
    REG_SET_BIT(CPU_APM_REGION0_ATTR_REG, 0x0777);
    espix_klog(ESPIX_KLOG_INFO, TAG, "user mode on; mscratch at boot 0x%08x 0x%08x",
               (unsigned)s_boot_mscratch[0], (unsigned)s_boot_mscratch[portNUM_PROCESSORS - 1]);
}

extern StaticTask_t *port_uxCoprocOwner[portNUM_PROCESSORS][SOC_CPU_COPROC_NUM];
RvCoprocSaveArea *pxPortGetCoprocArea(StaticTask_t *task, bool allocate, int coproc);
RvCoprocSaveArea *pxPortUpdateCoprocOwner(int coreid, int coproc, StaticTask_t *owner);
void vPortTaskPinToCore(StaticTask_t *task, int coreid);

IRAM_ATTR void espix_um_fpu_save(void *area)
{
    uint32_t fcsr;
    asm volatile (
        "fsw f0, 0(%0)\n"
        "fsw f1, 4(%0)\n"
        "fsw f2, 8(%0)\n"
        "fsw f3, 12(%0)\n"
        "fsw f4, 16(%0)\n"
        "fsw f5, 20(%0)\n"
        "fsw f6, 24(%0)\n"
        "fsw f7, 28(%0)\n"
        "fsw f8, 32(%0)\n"
        "fsw f9, 36(%0)\n"
        "fsw f10, 40(%0)\n"
        "fsw f11, 44(%0)\n"
        "fsw f12, 48(%0)\n"
        "fsw f13, 52(%0)\n"
        "fsw f14, 56(%0)\n"
        "fsw f15, 60(%0)\n"
        "fsw f16, 64(%0)\n"
        "fsw f17, 68(%0)\n"
        "fsw f18, 72(%0)\n"
        "fsw f19, 76(%0)\n"
        "fsw f20, 80(%0)\n"
        "fsw f21, 84(%0)\n"
        "fsw f22, 88(%0)\n"
        "fsw f23, 92(%0)\n"
        "fsw f24, 96(%0)\n"
        "fsw f25, 100(%0)\n"
        "fsw f26, 104(%0)\n"
        "fsw f27, 108(%0)\n"
        "fsw f28, 112(%0)\n"
        "fsw f29, 116(%0)\n"
        "fsw f30, 120(%0)\n"
        "fsw f31, 124(%0)\n"
        : : "r"(area) : "memory");
    asm volatile ("csrr %0, fcsr" : "=r"(fcsr));
    ((uint32_t *)area)[32] = fcsr;
}

IRAM_ATTR void espix_um_fpu_restore(const void *area)
{
    asm volatile (
        "flw f0, 0(%0)\n"
        "flw f1, 4(%0)\n"
        "flw f2, 8(%0)\n"
        "flw f3, 12(%0)\n"
        "flw f4, 16(%0)\n"
        "flw f5, 20(%0)\n"
        "flw f6, 24(%0)\n"
        "flw f7, 28(%0)\n"
        "flw f8, 32(%0)\n"
        "flw f9, 36(%0)\n"
        "flw f10, 40(%0)\n"
        "flw f11, 44(%0)\n"
        "flw f12, 48(%0)\n"
        "flw f13, 52(%0)\n"
        "flw f14, 56(%0)\n"
        "flw f15, 60(%0)\n"
        "flw f16, 64(%0)\n"
        "flw f17, 68(%0)\n"
        "flw f18, 72(%0)\n"
        "flw f19, 76(%0)\n"
        "flw f20, 80(%0)\n"
        "flw f21, 84(%0)\n"
        "flw f22, 88(%0)\n"
        "flw f23, 92(%0)\n"
        "flw f24, 96(%0)\n"
        "flw f25, 100(%0)\n"
        "flw f26, 104(%0)\n"
        "flw f27, 108(%0)\n"
        "flw f28, 112(%0)\n"
        "flw f29, 116(%0)\n"
        "flw f30, 120(%0)\n"
        "flw f31, 124(%0)\n"
        : : "r"(area) : "memory");
    asm volatile ("csrw fcsr, %0" :: "r"(((const uint32_t *)area)[32]));
}

static const uint32_t s_fpu_zero[33];

/* The FPU becomes the U-mode thread's before it runs, never through IDF's lazy trap from U-mode. */
static IRAM_ATTR void fpu_take(uint32_t core, const espix_um_thread_t *t)
{
    StaticTask_t *const self = t->task;
    RV_SET_CSR(mstatus, 1u << 13);
    if (port_uxCoprocOwner[core][FPU_COPROC_IDX] == self) {
        return;
    }
    RvCoprocSaveArea *const former = pxPortUpdateCoprocOwner(core, FPU_COPROC_IDX, self);
    if (former != NULL && former != (void *)-1) {
        espix_um_fpu_save(former->sa_coprocs[FPU_COPROC_IDX]);
    }
    vPortTaskPinToCore(self, core);
    RvCoprocSaveArea *const sa = pxPortGetCoprocArea(self, false, FPU_COPROC_IDX);
    espix_um_fpu_restore((sa->sa_enable & (1u << FPU_COPROC_IDX)) ? sa->sa_coprocs[FPU_COPROC_IDX] : s_fpu_zero);
    sa->sa_enable |= 1u << FPU_COPROC_IDX;
}

/* The stack guard's two bound checks in one read-modify-write; the HAL takes one each. */
static inline void sp_mon(uint32_t core, bool on)
{
    const uint32_t reg  = BUS_MONITOR_CORE_0_MONTR_ENA_REG +
                          core * (BUS_MONITOR_CORE_1_MONTR_ENA_REG - BUS_MONITOR_CORE_0_MONTR_ENA_REG);
    const uint32_t bits = BUS_MONITOR_CORE_0_SP_SPILL_MIN_ENA | BUS_MONITOR_CORE_0_SP_SPILL_MAX_ENA;
    if (on) {
        REG_SET_BIT(reg, bits);
    } else {
        REG_CLR_BIT(reg, bits);
    }
}

/* On every return to U-mode: the thread's table, the kernel tp for the next entry, no stack guard. */
IRAM_ATTR void espix_um_return(void *frame)
{
    espix_um_return_to(frame, pvTaskGetThreadLocalStoragePointer(NULL, ESPIX_TLS_UM_IDX));
}

IRAM_ATTR void espix_um_return_to(RvExcFrame *frame, espix_um_thread_t *t)
{
    /* A signal, a stop or a kill: the return lands on an unmapped pc instead, and the trap acts on it as
     * task code (espix_um_sig_deliver). The first entry has no frame; its next return will see it. */
    if (frame != NULL && !t->diverted) {
        const espix_proc_slot_t *const s = t->slot;
        if (s->stop_requested || t->proc->exiting || s->sig_stop_req ||
            ((s->sig_pending | t->sig_pending) & ~t->sig_blocked) != 0) {
            t->diverted = true;
            t->sig_pc   = frame->mepc;
            frame->mepc = UM_KICK_PC;
        } else {
            t->sys_intr = false;
            if (t->suspended) {
                t->suspended   = false;
                t->sig_blocked = t->sig_saved;
            }
        }
    }
    const uint32_t core = esp_cpu_get_core_id();
    espix_um_cur[core] = t;
    espix_um_ktp[core] = t->ktp;
    /* Published before the stale check, which espix_um_asid_shoot() does the other way round. */
    __atomic_store_n(&s_cur_satp[core], t->satp, __ATOMIC_SEQ_CST);
    RV_WRITE_CSR(satp, t->satp);
    const uint32_t asid = (t->satp & SATP32_ASID) >> 22;
    const uint32_t bit  = 1u << (asid % 32);
    if (__atomic_load_n(&s_asid_stale[core][asid / 32], __ATOMIC_SEQ_CST) & bit) {
        __atomic_fetch_and(&s_asid_stale[core][asid / 32], ~bit, __ATOMIC_SEQ_CST);
        asm volatile ("sfence.vma zero, %0" :: "r"(asid) : "memory");
    }
    fpu_take(core, t);
    sp_mon(core, false);
}

static IRAM_ATTR void asid_flush_here(void *arg)
{
    asm volatile ("sfence.vma zero, %0" :: "r"((uint32_t)arg) : "memory");
}

void espix_um_asid_shoot(uint32_t asid)
{
    const uint32_t me  = esp_cpu_get_core_id();
    const uint32_t bit = 1u << (asid % 32);
    for (uint32_t c = 0; c < portNUM_PROCESSORS; c++) {
        if (c == me) {
            continue;
        }
        __atomic_fetch_or(&s_asid_stale[c][asid / 32], bit, __ATOMIC_SEQ_CST);
        /* Running it now, or about to without having seen the bit: flush there at once. */
        if (((__atomic_load_n(&s_cur_satp[c], __ATOMIC_SEQ_CST) & SATP32_ASID) >> 22) == asid) {
            esp_ipc_isr_call_blocking(asid_flush_here, (void *)asid);
        }
    }
}

/* A yield request, not an IPC call: only an ordinary interrupt's exit runs espix_um_return(). */
void espix_um_sig_nudge(uint32_t satp)
{
    const uint32_t me = esp_cpu_get_core_id();
    for (uint32_t c = 0; c < portNUM_PROCESSORS; c++) {
        if (c != me && __atomic_load_n(&s_cur_satp[c], __ATOMIC_SEQ_CST) == satp) {
            vPortYieldOtherCore((BaseType_t)c);
        }
    }
}

/* The most kernel stack any U-mode task has used, to size CONFIG_ESPIX_UM_KSTACK from. */
static uint32_t s_kstack_most;

static void kstack_note(void)
{
    const uint32_t used = CONFIG_ESPIX_UM_KSTACK - uxTaskGetStackHighWaterMark(NULL);
    if (used > s_kstack_most) {
        s_kstack_most = used;
        espix_klog(ESPIX_KLOG_INFO, TAG, "kernel stack: %u of %u bytes, the most yet",
                   (unsigned)used, (unsigned)CONFIG_ESPIX_UM_KSTACK);
    }
}

/* Every thread of the process is to end, with status (and the signal behind it) unless another already set one. */
static void stop_all(espix_um_thread_t *t, int status, int sig, bool exec)
{
    espix_um_proc_t *const p = t->proc;
    xSemaphoreTake(p->lock, portMAX_DELAY);
    const bool first = !p->exiting;
    if (first) {
        p->exiting     = true;
        p->exit_status = status;
        p->exit_sig    = (uint8_t)sig;
        p->exec        = exec;
    }
    const bool others = p->threads > 1;
    xSemaphoreGive(p->lock);
    if (first && others) {
        espix_proc_stop_tasks(t->slot);
    }
}

#define THREADS_GONE_MS 2000

static int threads_left(espix_um_proc_t *p)
{
    xSemaphoreTake(p->lock, portMAX_DELAY);
    const int left = p->threads - 1;
    xSemaphoreGive(p->lock);
    return left;
}

/* The main thread, the others gone: the process ends, or an exec has it load its new image (proc_task()). */
static void __attribute__((noreturn)) main_exit(espix_um_thread_t *t)
{
    kstack_note();
    if (t->proc->exec) {
        longjmp(*t->slot->exit_jmp, ESPIX_UM_EXEC_JUMP);
    }
    t->slot->term_sig = t->proc->exit_sig;
    espix_proc_exit(t->proc->exit_status);
    __builtin_unreachable();
}

static void __attribute__((noreturn)) exit_group(espix_um_thread_t *t, int status, int sig, bool exec);

void espix_um_exit_group(espix_um_thread_t *t, int status)
{
    exit_group(t, status, 0, false);
}

void espix_um_exit_sig(espix_um_thread_t *t, int sig)
{
    exit_group(t, 128 + sig, sig, false);
}

static void exit_group(espix_um_thread_t *t, int status, int sig, bool exec)
{
    stop_all(t, status, sig, exec);
    if (t->exit_jmp != NULL) {
        espix_um_thread_exit(t);
    }
    /*
     * The main thread: the others leave at their next return to U, or when
     * their blocking call sees the stop. One that does not is deleted with the
     * process's other threads.
     */
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(THREADS_GONE_MS);
    int left;
    while ((left = threads_left(t->proc)) > 0) {
        const int32_t wait = (int32_t)(deadline - xTaskGetTickCount());
        if (wait <= 0) {
            espix_klog(ESPIX_KLOG_WARN, TAG, "pid %d: %d threads did not end; deleting them",
                       (int)t->slot->info.pid, left);
            break;
        }
        (void)ulTaskNotifyTakeIndexed(ESPIX_NOTIFY_SIG_IDX, pdTRUE, (TickType_t)wait);
    }
    main_exit(t);
}

void espix_um_thread_exit(espix_um_thread_t *t)
{
    espix_um_as_t *const as = t->proc->as;
    if (t->clear_tid != 0) {
        const uint32_t zero = 0;
        if (espix_um_copyout(as, t->clear_tid, &zero, sizeof(zero)) == 0) {
            (void)espix_um_futex_wake(t->proc, t->clear_tid, INT32_MAX);
        }
    }
    if (t->exit_jmp != NULL) {
        if (t->stack != 0) {
            espix_um_tstack_give(as, t->stack);
        }
        kstack_note();
        longjmp(*t->exit_jmp, 1);
    }
    /* The main thread: as on Linux, the process lives until its last thread ends, and then exits 0.
     * Its signals go to the threads left; one that ends the process stops this wait too. */
    espix_proc_slot_t *const slot = t->slot;
    t->gone = true;
    for (;;) {
        (void)espix_sigcheck_mask();
        if (slot->stop_requested || t->proc->exiting) {
            espix_um_exit_sig(t, SIGKILL);
        }
        if (threads_left(t->proc) == 0) {
            espix_um_exit_group(t, 0);
        }
        (void)ulTaskNotifyTakeIndexed(ESPIX_NOTIFY_SIG_IDX, pdTRUE, portMAX_DELAY);
    }
}

/* A thread's kernel side: the task enters U at its start routine, and ends here when it leaves. */
static void thread_main(void *arg)
{
    espix_um_thread_t *const t = arg;
    espix_um_proc_t *const p = t->proc;
    jmp_buf jb;

    t->task     = xTaskGetCurrentTaskHandle();
    t->exit_jmp = &jb;
    espix_pid_t pid;
    (void)espix_proc_thread_of_task(t->task, &pid, &t->tid);
    /* Before any user code runs, so a join never sees the word before the thread has set it. */
    if (t->clear_tid != 0) {
        (void)espix_um_copyout(p->as, t->clear_tid, &t->tid, sizeof(uint32_t));
    }
    asm volatile ("mv %0, tp" : "=r"(t->ktp));
    vTaskSetThreadLocalStoragePointer(NULL, ESPIX_TLS_UM_IDX, t);
    if (setjmp(jb) == 0) {
        espix_um_enter(t->entry, t->usp, t->arg, 0, 0, t->utp);
    }
    vTaskSetThreadLocalStoragePointer(NULL, ESPIX_TLS_UM_IDX, NULL);

    xSemaphoreTake(p->lock, portMAX_DELAY);
    p->threads--;
    xSemaphoreGive(p->lock);
    const TaskHandle_t main_task = t->slot->info.task;
    if (main_task != NULL) {
        xTaskNotifyGiveIndexed(main_task, ESPIX_NOTIFY_SIG_IDX);
    }
    free(t);
}

int32_t espix_um_thread_create(espix_um_thread_t *self, uint32_t entry, uint32_t arg, uint32_t utp,
                               uint32_t ustack, uint32_t ctid)
{
    espix_um_proc_t *const p = self->proc;
    espix_um_thread_t *const t = heap_caps_calloc(1, sizeof(*t), MALLOC_CAP_INTERNAL);
    if (t == NULL) {
        return -ENOMEM;
    }
    *t = (espix_um_thread_t){ .satp = self->satp, .proc = p, .slot = self->slot, .clear_tid = ctid,
                              .entry = entry, .arg = arg, .utp = utp, .usp = ustack & ~15u,
                              .sig_blocked = self->sig_blocked };
    if (ustack == 0) {
        t->stack = t->usp = espix_um_tstack_take(p->as);
        if (t->stack == 0) {
            free(t);
            return -EAGAIN;
        }
    }

    xSemaphoreTake(p->lock, portMAX_DELAY);
    const bool exiting = p->exiting;
    if (!exiting) {
        p->threads++;
    }
    xSemaphoreGive(p->lock);
    if (exiting) {
        if (t->stack != 0) {
            espix_um_tstack_give(p->as, t->stack);
        }
        free(t);
        return -EAGAIN;
    }

    const espix_kthread_t k = {
        .name = self->slot->info.name, .fn = thread_main, .arg = t, .stack = CONFIG_ESPIX_UM_KSTACK,
        .priority = CONFIG_ESPIX_PROC_PRIORITY, .core = tskNO_AFFINITY,
    };
    espix_pid_t tid;
    if (espix_proc_thread_start(self->slot, &k, NULL, &tid) != ESP_OK) {
        xSemaphoreTake(p->lock, portMAX_DELAY);
        p->threads--;
        xSemaphoreGive(p->lock);
        if (t->stack != 0) {
            espix_um_tstack_give(p->as, t->stack);
        }
        free(t);
        return -EAGAIN;
    }
    return (int32_t)tid;
}

static int fault_signal(uint32_t code)
{
    switch (code) {
    case 2:  return SIGILL;
    case 3:  return SIGTRAP;
    case 0: case 4: case 6: return SIGBUS;
    default: return SIGSEGV;
    }
}

/* To the app's handler for the fault's signal, or the end of the process. */
static void fault(espix_um_thread_t *t, RvExcFrame *f, uint32_t code)
{
    espix_proc_slot_t *const slot = t->slot;
    const int sig = fault_signal(code);
    if (espix_um_sig_fault(t, f, sig)) {
        return;
    }
    const char *what = (sig == SIGILL) ? "Illegal instruction" : (sig == SIGBUS) ? "Bus error"
                     : (sig == SIGTRAP) ? "Trace/breakpoint trap" : "Segmentation fault";

    espix_klog(ESPIX_KLOG_WARN, TAG, "pid %d: %s at pc 0x%08x, address 0x%08x (mcause %u)",
               (int)slot->info.pid, what, (unsigned)f->mepc, (unsigned)f->mtval, (unsigned)code);
    espix_eprintf(slot->info.session, "%s: %s (pc 0x%08x, address 0x%08x)\n",
                  slot->info.name, what, (unsigned)f->mepc, (unsigned)f->mtval);
    espix_um_exit_sig(t, sig);
}

void espix_um_resume(RvExcFrame *u, uint32_t ms, espix_um_thread_t *t) __attribute__((noreturn));
#if CONFIG_ESPIX_UM_AMO_TRAP
static bool amo_emulate(espix_um_thread_t *t, RvExcFrame *u);
#endif

/*
 * An exception from U-mode, as task code with interrupts on: it may block, sleep, take the FPU or end the process.
 * espix_um_trap (umode_entry.S) left the exception at once, with the user's registers at u and the stack below them.
 */
IRAM_ATTR void espix_um_kentry(RvExcFrame *u, uint32_t cause, espix_um_thread_t *t)
{
    const uint32_t code = cause & 0xfff;
    const uint32_t need = (code == 12) ? PTE_X : (code == 13) ? PTE_R : (code == 15) ? PTE_W : 0;

    sp_mon(esp_cpu_get_core_id(), true);
    if (code == 12 && u->mepc == UM_KICK_PC && t->diverted) {
        t->diverted = false;
        u->mepc     = t->sig_pc;
        espix_um_sig_deliver(t, u);
    } else if (code == 8 && u->a7 == ESPIX_SYS_SIGRETURN) {
        espix_um_sig_return(t, u);
    } else if (code == 8) {
        t->sys_nr = u->a7;
        t->sys_a0 = u->a0;
        u->mepc += 4;
        u->a0 = (uint32_t)espix_um_syscall(t, u);
        t->sys_intr = (int32_t)u->a0 == -EINTR;
#if CONFIG_ESPIX_UM_AMO_TRAP
    } else if (code == 2 && amo_emulate(t, u)) {
#endif
    } else if (need == 0 || !espix_um_fault(t->proc->as, u->mtval, need)) {
        fault(t, u, code);
    }
    espix_um_resume(u, (uint32_t)u + RV_STK_FRMSZ, t);
}

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version, e_entry, e_phoff, e_shoff, e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} um_ehdr_t;

typedef struct {
    uint32_t p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align;
} um_phdr_t;

typedef struct {
    uint32_t sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link, sh_info, sh_addralign, sh_entsize;
} um_shdr_t;

#define ET_EXEC_       2
#define EM_RISCV_      243
#define PT_LOAD_       1
#define SHT_PROGBITS_  1
#define SHF_CODE_      0x6      /* SHF_ALLOC | SHF_EXECINSTR */

/* An AMO .w, the funct5 values of the ones the kernel does, and what replaces them: csrrw x0, cycle, x0. */
#define AMO_MASK   0x707fu
#define AMO_W      0x202fu
#define AMO_OPS    0x11111113u      /* add, swap, xor, or, and, min, max, minu, maxu: bit = funct5 */
#define UNIMP      0xc0001073u

bool espix_um_is_exec(const uint8_t *image, size_t size)
{
    const um_ehdr_t *eh = (const um_ehdr_t *)image;
    return size >= sizeof(*eh) && memcmp(eh->e_ident, "\x7f" "ELF", 4) == 0 &&
           eh->e_ident[4] == 1 && eh->e_type == ET_EXEC_ && eh->e_machine == EM_RISCV_;
}

bool espix_um_file_is_exec(const char *path)
{
    um_ehdr_t eh;
    FILE *f = fopen(path, "rb");
    const bool ok = f != NULL && fread(&eh, 1, sizeof(eh), f) == sizeof(eh) &&
                    espix_um_is_exec((const uint8_t *)&eh, sizeof(eh));
    if (f != NULL) {
        fclose(f);
    }
    return ok;
}

#if CONFIG_ESPIX_UM_AMO_TRAP
static int amo_cmp(const void *a, const void *b)
{
    const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

/* Each AMO in the image's code, or every one if table is NULL; recorded and replaced by UNIMP otherwise. */
static uint32_t amo_scan(uint8_t *image, size_t size, uint32_t *table)
{
    const um_ehdr_t *eh = (const um_ehdr_t *)image;
    if (eh->e_shoff == 0 || eh->e_shentsize != sizeof(um_shdr_t) ||
        eh->e_shoff + (size_t)eh->e_shnum * sizeof(um_shdr_t) > size) {
        return 0;
    }
    uint32_t n = 0;
    for (int i = 0; i < eh->e_shnum; i++) {
        um_shdr_t sh;
        memcpy(&sh, image + eh->e_shoff + i * sizeof(sh), sizeof(sh));
        if (sh.sh_type != SHT_PROGBITS_ || (sh.sh_flags & SHF_CODE_) != SHF_CODE_ ||
            sh.sh_offset > size || sh.sh_size > size - sh.sh_offset) {
            continue;
        }
        for (uint32_t off = 0; off + 2 <= sh.sh_size;) {
            uint8_t *const p = image + sh.sh_offset + off;
            if ((p[0] & 3) != 3) {
                off += 2;
                continue;
            }
            if (off + 4 > sh.sh_size) {
                break;
            }
            const uint32_t insn = p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
            if ((insn & AMO_MASK) == AMO_W && ((AMO_OPS >> (insn >> 27)) & 1)) {
                if (table != NULL) {
                    table[2 * n]     = sh.sh_addr + off;
                    table[2 * n + 1] = insn;
                    memcpy(p, &(uint32_t){ UNIMP }, 4);
                }
                n++;
            }
            off += 4;
        }
    }
    return n;
}

/* The image's AMOs become UNIMP, for amo_emulate() to do as LR/SC (docs/GOTCHAS.md). */
static bool amo_rewrite(espix_um_proc_t *p, uint8_t *image, size_t size)
{
    const uint32_t n = amo_scan(image, size, NULL);
    if (n == 0) {
        return true;
    }
    p->amos = malloc(n * 2 * sizeof(uint32_t));
    if (p->amos == NULL) {
        return false;
    }
    p->amo_count = amo_scan(image, size, p->amos);
    qsort(p->amos, p->amo_count, 2 * sizeof(uint32_t), amo_cmp);
    return true;
}

/* An AMO the loader replaced, done on the user's word as LR/SC with the old value into rd. */
static bool amo_emulate(espix_um_thread_t *t, RvExcFrame *u)
{
    espix_um_proc_t *const p = t->proc;
    const uint32_t *const e = bsearch(&u->mepc, p->amos, p->amo_count, 2 * sizeof(uint32_t), amo_cmp);
    if (e == NULL) {
        return false;
    }
    const uint32_t insn = e[1];
    uint32_t *const x = (uint32_t *)u;      /* x1-x31 at their numbers; slot 0 is mepc */
    const uint32_t rd = (insn >> 7) & 31, rs1 = (insn >> 15) & 31, rs2 = (insn >> 20) & 31;
    const uint32_t va = (rs1 != 0) ? x[rs1] : 0, src = (rs2 != 0) ? x[rs2] : 0;
    volatile uint32_t *const k = (va & 3) ? NULL : espix_um_user_pin(p->as, va, PTE_W);
    if (k == NULL) {
        u->mtval = va;
        fault(t, u, (va & 3) ? 6 : 15);
        return true;
    }
    uint32_t old = *k, new;
    do {
        switch (insn >> 27) {
        case 0x00: new = old + src; break;
        case 0x04: new = old ^ src; break;
        case 0x08: new = old | src; break;
        case 0x0c: new = old & src; break;
        case 0x10: new = ((int32_t)old < (int32_t)src) ? old : src; break;
        case 0x14: new = ((int32_t)old > (int32_t)src) ? old : src; break;
        case 0x18: new = (old < src) ? old : src; break;
        case 0x1c: new = (old > src) ? old : src; break;
        default:   new = src; break;      /* 0x01: swap */
        }
    } while (!__atomic_compare_exchange_n(k, &old, new, true, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED));
    espix_um_user_unpin(p->as);
    if (rd != 0) {
        x[rd] = old;
    }
    u->mepc += 4;
    return true;
}
#endif

/*
 * An image's read-only segment, shared by the processes running that image: found by its address,
 * size and CRC, and checked byte for byte. It lives while a process maps it, and a process gives it
 * up before its table goes, so the frames it names are always mapped by someone.
 */
struct espix_um_text {
    struct espix_um_text *next;
    uint32_t              vaddr, filesz, crc, npages;
    int                   users;
    void                 *frames[];
};

static struct espix_um_text *s_texts;

/* Read-only, no zeroed tail, and no page shared with another segment. */
static bool text_shareable(const uint8_t *image, const um_ehdr_t *eh, const um_phdr_t *ph)
{
    if ((ph->p_flags & 2) || ph->p_filesz == 0 || ph->p_filesz != ph->p_memsz) {
        return false;
    }
    const uint32_t lo = ph->p_vaddr & ~(PG - 1), hi = (ph->p_vaddr + ph->p_memsz + PG - 1) & ~(PG - 1);
    for (int i = 0; i < eh->e_phnum; i++) {
        const um_phdr_t *q = (const um_phdr_t *)(image + eh->e_phoff) + i;
        if (q != ph && q->p_type == PT_LOAD_ && q->p_memsz > 0 &&
            (q->p_vaddr & ~(PG - 1)) < hi && q->p_vaddr + q->p_memsz > lo) {
            return false;
        }
    }
    return true;
}

static void text_put(struct espix_um_text *t)
{
    if (t == NULL) {
        return;
    }
    xSemaphoreTake(s_texts_lock, portMAX_DELAY);
    const bool last = --t->users == 0;
    for (struct espix_um_text **pp = &s_texts; last && *pp != NULL; pp = &(*pp)->next) {
        if (*pp == t) {
            *pp = t->next;
            break;
        }
    }
    xSemaphoreGive(s_texts_lock);
    if (last) {
        for (uint32_t i = 0; i < t->npages; i++) {
            espix_um_frame_text(t->frames[i], false);
        }
        free(t);
    }
}

/* The cached copy of ph's bytes, its users counted up, or NULL. */
static struct espix_um_text *text_find(const um_phdr_t *ph, const uint8_t *bytes, uint32_t crc)
{
    xSemaphoreTake(s_texts_lock, portMAX_DELAY);
    struct espix_um_text *t = s_texts;
    while (t != NULL && !(t->vaddr == ph->p_vaddr && t->filesz == ph->p_filesz && t->crc == crc)) {
        t = t->next;
    }
    if (t != NULL) {
        t->users++;
    }
    xSemaphoreGive(s_texts_lock);
    const uint32_t lo = ph->p_vaddr & ~(PG - 1);
    for (uint32_t i = 0; t != NULL && i < t->npages; i++) {
        const uint32_t va = lo + i * PG;
        const uint32_t from = MAX(va, ph->p_vaddr), to = MIN(va + PG, ph->p_vaddr + ph->p_filesz);
        if (memcmp((const uint8_t *)t->frames[i] + (from - va), bytes + (from - ph->p_vaddr), to - from) != 0) {
            text_put(t);
            t = NULL;
        }
    }
    return t;
}

static struct espix_um_text *text_add(const um_phdr_t *ph, uint32_t crc, void *const *frames, uint32_t npages)
{
    struct espix_um_text *t = malloc(sizeof(*t) + npages * sizeof(void *));
    if (t == NULL) {
        return NULL;
    }
    *t = (struct espix_um_text){ .vaddr = ph->p_vaddr, .filesz = ph->p_filesz, .crc = crc,
                                 .npages = npages, .users = 1 };
    memcpy(t->frames, frames, npages * sizeof(void *));
    for (uint32_t i = 0; i < npages; i++) {
        espix_um_frame_text(t->frames[i], true);
    }
    xSemaphoreTake(s_texts_lock, portMAX_DELAY);
    t->next = s_texts;
    s_texts = t;
    xSemaphoreGive(s_texts_lock);
    return t;
}

static const char *load_segments(espix_um_proc_t *p, const uint8_t *image, size_t size, uint32_t *entry)
{
    espix_um_as_t *const as = p->as;
    const um_ehdr_t *eh = (const um_ehdr_t *)image;
    if (eh->e_phentsize != sizeof(um_phdr_t) ||
        eh->e_phoff + (size_t)eh->e_phnum * sizeof(um_phdr_t) > size) {
        return "bad program headers";
    }
    uint32_t image_end = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        const um_phdr_t *ph = (const um_phdr_t *)(image + eh->e_phoff) + i;
        if (ph->p_type != PT_LOAD_ || ph->p_memsz == 0) {
            continue;
        }
        if (ph->p_vaddr < UVA_MIN || ph->p_memsz > UVA_MAX - ph->p_vaddr ||
            ph->p_filesz > ph->p_memsz || ph->p_offset + ph->p_filesz > size) {
            return "a segment outside the user range";
        }
        const uint32_t flags = ((ph->p_flags & 4) ? PTE_R : 0) | ((ph->p_flags & 2) ? PTE_W : 0) |
                               ((ph->p_flags & 1) ? PTE_X : 0);
        const uint32_t end = ph->p_vaddr + ph->p_memsz;
        const uint32_t file_end = (ph->p_vaddr + ph->p_filesz + PG - 1) & ~(PG - 1);
        if (!espix_um_region_add(as, ph->p_vaddr, end, flags)) {
            return "overlapping segments";
        }
        image_end = MAX(image_end, end);

        const uint32_t lo = ph->p_vaddr & ~(PG - 1), npages = (file_end - lo) / PG;
        const bool share = p->ntext < ESPIX_UM_TEXTS && text_shareable(image, eh, ph);
        const uint32_t crc = share ? esp_rom_crc32_le(0, image + ph->p_offset, ph->p_filesz) : 0;
        struct espix_um_text *t = share ? text_find(ph, image + ph->p_offset, crc) : NULL;
        if (t != NULL) {
            p->text[p->ntext++] = t;
            for (uint32_t i = 0; i < npages; i++) {
                if (!espix_um_map_frame(as, lo + i * PG, t->frames[i], flags)) {
                    return "out of memory for the image";
                }
            }
            continue;
        }
        void **frames = share ? malloc(npages * sizeof(void *)) : NULL;
        for (uint32_t va = lo; va < MIN(end, file_end); va += PG) {
            uint8_t *frame = espix_um_map_new(as, va, flags);
            if (frame == NULL) {
                free(frames);
                return "out of memory for the image";
            }
            if (frames != NULL) {
                frames[(va - lo) / PG] = frame;
            }
            const uint32_t lo = MAX(va, ph->p_vaddr);
            const uint32_t hi = MIN(va + PG, ph->p_vaddr + ph->p_filesz);
            if (hi > lo) {
                memcpy(frame + (lo - va), image + ph->p_offset + (lo - ph->p_vaddr), hi - lo);
            }
            if (flags & PTE_X) {
                /* Fetched through the I-cache, which refills from PSRAM and may hold the frame's old lines. */
                esp_cache_msync(frame, PG, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
                esp_cache_msync(frame, PG, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_INST);
            }
        }
        if (frames != NULL && (t = text_add(ph, crc, frames, npages)) != NULL) {
            p->text[p->ntext++] = t;
        }
        free(frames);
    }
    if (eh->e_entry < UVA_MIN || eh->e_entry >= UVA_MAX) {
        return "an entry point outside the image";
    }
    espix_um_brk_init(as, image_end);
    *entry = eh->e_entry;
    return NULL;
}

static bool put_user(espix_um_as_t *as, uint32_t va, const void *src, size_t n)
{
    const uint8_t *p = src;
    while (n > 0) {
        uint8_t *k = espix_um_user_pin(as, va, PTE_W);
        if (k == NULL) {
            return false;
        }
        const size_t c = MIN(n, PG - (va & (PG - 1)));
        memcpy(k, p, c);
        espix_um_user_unpin(as);
        va += c;
        p  += c;
        n  -= c;
    }
    return true;
}

/* The user stack: argv and environ strings at the top, then their pointer arrays. */
static const char *build_stack(espix_um_as_t *as, char **argv, int argc, char **envp,
                               uint32_t *usp, uint32_t *uargv, uint32_t *uenvp)
{
    /* The page below the reservation stays unmapped: a guard. */
    if (!espix_um_region_add(as, USTACK_TOP - USTACK_MAX, USTACK_TOP, PTE_R | PTE_W)) {
        return "too many segments";
    }
    int envc = 0;
    while (envp != NULL && envp[envc] != NULL) {
        envc++;
    }
    const int n = argc + envc;
    uint32_t *ptrs = calloc((size_t)n + 2, sizeof(uint32_t));
    if (ptrs == NULL) {
        return "out of memory";
    }
    uint32_t top = USTACK_TOP;
    for (int i = 0; i < n; i++) {
        const char *s = (i < argc) ? argv[i] : envp[i - argc];
        const size_t len = strlen(s) + 1;
        if (len > ARGS_MAX || top - len < USTACK_TOP - ARGS_MAX) {
            free(ptrs);
            return "arguments too long";
        }
        top -= len;
        if (!put_user(as, top, s, len)) {
            free(ptrs);
            return "out of memory for the stack";
        }
        ptrs[i < argc ? i : i + 1] = top;
    }
    /* argv[argc] and envp[envc] are the zeros calloc left. */
    top = (top - ((uint32_t)n + 2) * sizeof(uint32_t)) & ~15u;
    const bool put = put_user(as, top, ptrs, ((size_t)n + 2) * sizeof(uint32_t));
    free(ptrs);
    if (!put) {
        return "out of memory for the stack";
    }

    *uargv = top;
    *uenvp = top + ((uint32_t)argc + 1) * sizeof(uint32_t);
    *usp   = top;
    return NULL;
}

const char *espix_um_exec(espix_proc_slot_t *slot)
{
    espix_proc_app_t *const app = slot->app;
    const uint32_t asid = asid_alloc();
    if (asid == 0) {
        return "out of address-space ids";
    }

    /* After an exec the process keeps its thread, descriptors and signal state; only the image is new. */
    espix_um_as_t *as = espix_um_as_new(asid);
    espix_um_thread_t *t = app->um;
    espix_um_proc_t *p = (t != NULL) ? t->proc : NULL;
    if (t == NULL) {
        t = heap_caps_calloc(1, sizeof(*t), MALLOC_CAP_INTERNAL);
        p = calloc(1, sizeof(*p));
        SemaphoreHandle_t lock = xSemaphoreCreateMutex();
        if (t == NULL || p == NULL || lock == NULL) {
            free(t);
            free(p);
            if (lock != NULL) {
                vSemaphoreDelete(lock);
            }
            t = NULL;
        } else {
            p->lock    = lock;
            p->threads = 1;
            espix_um_fds_init(p);
            t->proc = p;
            t->slot = slot;
            t->tid  = slot->info.pid;
            app->um = t;
        }
    }
    if (as == NULL || t == NULL) {
        espix_um_as_free(as);
        asid_free(asid);
        return "out of memory for an address space";
    }
    p->as   = as;
    espix_um_as_count(as, &slot->um_pages);
    t->task = xTaskGetCurrentTaskHandle();
    t->satp = espix_um_as_satp(as);

    uint32_t entry = 0, usp = 0, uargv = 0, uenvp = 0;
#if CONFIG_ESPIX_UM_AMO_TRAP
    if (!amo_rewrite(p, app->image, slot->info.image_bytes)) {
        return "out of memory for the AMO table";
    }
    if (p->amo_count > 0) {
        espix_klog(ESPIX_KLOG_DEBUG, TAG, "pid %d: %u AMOs done in the kernel", (int)slot->info.pid,
                   (unsigned)p->amo_count);
    }
#endif
    const char *why = load_segments(p, app->image, slot->info.image_bytes, &entry);
    if (why == NULL) {
        why = build_stack(as, app->argv, app->argc, app->envp, &usp, &uargv, &uenvp);
    }
    if (why != NULL) {
        return why;
    }

    /* The file image has been copied into the address space. */
    free(app->image);
    app->image = NULL;

    asm volatile ("mv %0, tp" : "=r"(t->ktp));
    vTaskSetThreadLocalStoragePointer(NULL, ESPIX_TLS_UM_IDX, t);
    if (pvTaskGetThreadLocalStoragePointer(NULL, ESPIX_TLS_FSPRIV_IDX) != NULL) {
        vTaskSetThreadLocalStoragePointer(NULL, ESPIX_TLS_UM_IDX, NULL);
        return "refusing to enter U-mode with filesystem privilege held";
    }

    espix_klog(ESPIX_KLOG_DEBUG, TAG, "pid %d: %s in U-mode, entry 0x%08x, asid %u",
               (int)slot->info.pid, slot->info.name, (unsigned)entry, (unsigned)asid);
    espix_um_enter(entry, usp, (uint32_t)app->argc, uargv, uenvp, 0);
    __builtin_unreachable();
}

/* ------------------------------------------------------------------ */
/* fork, vfork and exec                                                */
/* ------------------------------------------------------------------ */

/* A vfork's parent waits on this until its child execs or ends. Under s_vfork_lock, which a waker holds across its notify. */
typedef struct espix_um_vfork {
    int          refs;
    bool         done;
    TaskHandle_t waiter;
} vfork_done_t;

static void vfork_put(vfork_done_t *v)
{
    xSemaphoreTake(s_vfork_lock, portMAX_DELAY);
    const bool last = --v->refs == 0;
    xSemaphoreGive(s_vfork_lock);
    if (last) {
        free(v);
    }
}

/* The child lets go of its parent's memory: the parent's vfork returns. */
static void vfork_release(espix_um_proc_t *p)
{
    vfork_done_t *const v = p->vfork;
    if (v == NULL) {
        return;
    }
    p->vfork = NULL;
    xSemaphoreTake(s_vfork_lock, portMAX_DELAY);
    v->done = true;
    if (v->waiter != NULL) {
        xTaskNotifyGiveIndexed(v->waiter, ESPIX_NOTIFY_SIG_IDX);
    }
    xSemaphoreGive(s_vfork_lock);
    vfork_put(v);
}

/* The image's part of a process: its texts, its table (unless another process still borrows it) and its AMOs. */
static void image_release(espix_um_proc_t *p, uint32_t *pages)
{
    for (int i = 0; i < p->ntext; i++) {
        text_put(p->text[i]);     /* before the table: a cached text's frames are always mapped */
    }
    p->ntext = 0;
    vfork_release(p);
    if (p->as != NULL) {
        const uint32_t asid = espix_um_as_asid(p->as);
        if (espix_um_as_put(p->as, pages)) {
            asid_free(asid);
        }
        p->as = NULL;
    }
    free(p->amos);
    p->amos      = NULL;
    p->amo_count = 0;
}

static void proc_free(espix_um_proc_t *p, uint32_t *pages)
{
    image_release(p, pages);
    vSemaphoreDelete(p->lock);
    free(p);
}

/* What a fork's child starts from: the parent's registers at its syscall, and its FPU. */
typedef struct espix_um_fork {
    RvExcFrame       frame;
    uint32_t         fpu[33];
    espix_um_proc_t *proc;
    uint32_t         sig_blocked;
    bool             vfork;
} um_fork_t;

static void fork_drop(um_fork_t *ctx)
{
    uint32_t none = 0;
    espix_um_fds_drop(ctx->proc);
    proc_free(ctx->proc, &none);
    free(ctx);
}

void espix_um_release(espix_proc_slot_t *slot)
{
    if (slot->app == NULL) {
        return;
    }
    if (slot->app->fork != NULL) {
        fork_drop(slot->app->fork);     /* a child that never ran */
        slot->app->fork = NULL;
    }
    if (slot->app->um == NULL) {
        return;
    }
    espix_um_thread_t *t = slot->app->um;
    proc_free(t->proc, &slot->um_pages);
    free(t);
    slot->app->um = NULL;
}

int32_t espix_um_fork(espix_um_thread_t *t, RvExcFrame *f, bool vfork)
{
    espix_um_proc_t *const p = t->proc;
    um_fork_t *const ctx = calloc(1, sizeof(*ctx));
    espix_um_proc_t *const cp = calloc(1, sizeof(*cp));
    SemaphoreHandle_t lock = xSemaphoreCreateMutex();
    vfork_done_t *const vd = vfork ? calloc(1, sizeof(*vd)) : NULL;
    if (ctx == NULL || cp == NULL || lock == NULL || (vfork && vd == NULL)) {
        free(ctx);
        free(cp);
        free(vd);
        if (lock != NULL) {
            vSemaphoreDelete(lock);
        }
        return -ENOMEM;
    }
    espix_um_fpu_save(ctx->fpu);
    ctx->frame       = *f;
    ctx->frame.a0    = 0;
    ctx->sig_blocked = t->sig_blocked;
    ctx->vfork       = vfork;
    ctx->proc        = cp;
    cp->lock    = lock;
    cp->threads = 1;

    uint32_t none = 0;
    int32_t r = espix_um_fds_fork(p, cp);
    if (r == 0 && p->amo_count > 0) {
        cp->amos = malloc(p->amo_count * 2 * sizeof(uint32_t));
        if (cp->amos != NULL) {
            memcpy(cp->amos, p->amos, p->amo_count * 2 * sizeof(uint32_t));
            cp->amo_count = p->amo_count;
        } else {
            r = -ENOMEM;
        }
    }
    if (r == 0 && vfork) {
        cp->as = espix_um_as_get(p->as);
    } else if (r == 0) {
        const uint32_t asid = asid_alloc();
        cp->as = (asid != 0) ? espix_um_as_fork(p->as, asid) : NULL;
        if (cp->as == NULL) {
            if (asid != 0) {
                asid_free(asid);
            }
            r = (asid != 0) ? -ENOMEM : -EAGAIN;
        }
    }
    if (r == 0 && !vfork) {
        xSemaphoreTake(s_texts_lock, portMAX_DELAY);
        for (int i = 0; i < p->ntext; i++) {
            p->text[i]->users++;
            cp->text[i] = p->text[i];
        }
        cp->ntext = p->ntext;
        xSemaphoreGive(s_texts_lock);
    }
    xSemaphoreTake(p->lock, portMAX_DELAY);
    memcpy(cp->act, p->act, sizeof(cp->act));
    xSemaphoreGive(p->lock);

    espix_pid_t pid = 0;
    if (r == 0) {
        if (vfork) {
            vd->refs  = 2;
            cp->vfork = vd;
        }
        r = espix_proc_fork(t->slot, ctx, &pid);
    }
    if (r != 0) {
        cp->vfork = NULL;
        espix_um_fds_drop(cp);
        proc_free(cp, &none);
        free(ctx);
        free(vd);
        return r;
    }
    if (!vfork) {
        return (int32_t)pid;
    }

    /* Until the child execs or ends: it runs on this thread's stack. Only an end of the process comes first. */
    espix_proc_slot_t *const slot = t->slot;
    xSemaphoreTake(s_vfork_lock, portMAX_DELAY);
    vd->waiter = xTaskGetCurrentTaskHandle();
    xSemaphoreGive(s_vfork_lock);
    for (;;) {
        xSemaphoreTake(s_vfork_lock, portMAX_DELAY);
        const bool done = vd->done;
        if (done || slot->stop_requested || p->exiting) {
            vd->waiter = NULL;
        }
        xSemaphoreGive(s_vfork_lock);
        if (done) {
            break;
        }
        if (slot->stop_requested || p->exiting) {
            vfork_put(vd);
            espix_um_exit_sig(t, SIGKILL);
        }
        (void)ulTaskNotifyTakeIndexed(ESPIX_NOTIFY_SIG_IDX, pdTRUE, pdMS_TO_TICKS(1000));
    }
    vfork_put(vd);
    return (int32_t)pid;
}

const char *espix_um_fork_run(espix_proc_slot_t *slot)
{
    um_fork_t *const ctx = slot->app->fork;
    espix_um_thread_t *const t = heap_caps_calloc(1, sizeof(*t), MALLOC_CAP_INTERNAL);
    if (t == NULL) {
        return "out of memory for a thread";
    }
    espix_um_proc_t *const p = ctx->proc;
    t->proc        = p;
    t->slot        = slot;
    t->tid         = slot->info.pid;
    t->task        = xTaskGetCurrentTaskHandle();
    t->satp        = espix_um_as_satp(p->as);
    t->sig_blocked = ctx->sig_blocked;
    slot->app->um   = t;
    slot->app->fork = NULL;
    if (!ctx->vfork) {
        espix_um_as_count(p->as, &slot->um_pages);
    }

    /* The parent's FPU registers are this thread's, restored as it first enters U. */
    RvCoprocSaveArea *const sa = pxPortGetCoprocArea(t->task, true, FPU_COPROC_IDX);
    memcpy(sa->sa_coprocs[FPU_COPROC_IDX], ctx->fpu, sizeof(ctx->fpu));
    sa->sa_enable |= 1u << FPU_COPROC_IDX;

    /* The frame at the kernel stack top for later traps, two scratch words above it. */
    uint32_t area[RV_STK_FRMSZ / 4 + 8] __attribute__((aligned(16)));
    memcpy(area, &ctx->frame, sizeof(ctx->frame));
    free(ctx);

    asm volatile ("mv %0, tp" : "=r"(t->ktp));
    vTaskSetThreadLocalStoragePointer(NULL, ESPIX_TLS_UM_IDX, t);
    if (pvTaskGetThreadLocalStoragePointer(NULL, ESPIX_TLS_FSPRIV_IDX) != NULL) {
        vTaskSetThreadLocalStoragePointer(NULL, ESPIX_TLS_UM_IDX, NULL);
        return "refusing to enter U-mode with filesystem privilege held";
    }
    espix_um_resume((RvExcFrame *)area, (uint32_t)area + RV_STK_FRMSZ, t);
}

int32_t espix_um_exec_prepare(espix_um_thread_t *t, const char *path, int argc, char **argv, char **envp)
{
    char abs[ESPIX_PATH_MAX];
    struct stat st;
    if (espix_fs_resolve(espix_proc_cwd(), path, abs, sizeof(abs)) != ESP_OK) {
        return -ENAMETOOLONG;
    }
    if (stat(abs, &st) != 0) {
        return -errno;
    }
    if (!S_ISREG(st.st_mode) || !(st.st_mode & S_IXUSR)) {
        return -EACCES;
    }
    /* An M-mode image needs a task of its own size: not yet from U-mode. */
    if (!espix_um_file_is_exec(abs)) {
        return -ENOEXEC;
    }
    espix_proc_slot_t *const slot = t->slot;
    if (slot->stop_requested || t->proc->exiting) {
        return -EINTR;
    }
    return espix_proc_exec_commit(slot, abs, argc, argv, envp);
}

void espix_um_exec_go(espix_um_thread_t *t)
{
    /* No way back: every other thread ends, and the main one loads the image in proc_task(). */
    exit_group(t, 0, 0, true);
}

void espix_um_exec_reset(espix_proc_slot_t *slot)
{
    espix_um_thread_t *const t = slot->app->um;
    espix_um_proc_t *const p = t->proc;
    espix_um_objs_release(p);
    /* A vfork's child leaves its parent's mappings alone. */
    if (p->vfork == NULL) {
        espix_um_unmap_files(p->as);
    }
    image_release(p, &slot->um_pages);
    espix_um_fds_cloexec(p);
    espix_um_sig_exec(p);
    p->exiting     = false;
    p->exec        = false;
    p->exit_status = 0;
    p->exit_sig    = 0;
    p->threads     = 1;
    t->diverted    = false;
    t->gone        = false;
    t->suspended   = false;
    t->sys_intr    = false;
    t->clear_tid   = 0;
    t->stack       = 0;
    t->exit_jmp    = NULL;
    vTaskSetThreadLocalStoragePointer(NULL, ESPIX_TLS_UM_IDX, NULL);
    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);
    slot->stop_requested = false;
    slot->stop_sig       = 0;
    slot->loading        = true;
    xSemaphoreGive(g_espix_proc_lock);
}

#endif /* CONFIG_ESPIX_UMODE */

