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

#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_private/startup_internal.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/assist_debug_hal.h"
#include "hal/assist_debug_ll.h"
#include "riscv/csr.h"
#include "riscv/encoding.h"
#include "riscv/rvruntime-frames.h"
#include "soc/cpu_apm_reg.h"

#include "espix_kernel.h"
#include "espix_shell.h"
#include "espix_umode.h"

#define TAG "umode"

#define PG          4096u
#define UVA_MIN     0x00010000u
#define UVA_MAX     0x70000000u
#define USTACK_TOP  0x80000000u
#define USTACK_MAX  (8u * 1024u * 1024u)    /* reserved, mapped as touched; Linux's ulimit -s */
#define ARGS_MAX    (64u * 1024u)
#define UM_KICK_PC  0x00000ffcu                 /* below the image, never mapped */

/* Each core's current U-mode thread's kernel tp; the patched vectors load it on entry from U. */
uint32_t espix_um_ktp[portNUM_PROCESSORS];

static uint32_t s_boot_mscratch[portNUM_PROCESSORS];


/* ASIDs: one per live process; a freed one is flushed on each core before its next use there. */
#define ASIDS 512
static uint32_t    s_asid_used[ASIDS / 32] = { 1 };
static uint32_t    s_asid_stale[portNUM_PROCESSORS][ASIDS / 32];
static portMUX_TYPE s_asid_lock = portMUX_INITIALIZER_UNLOCKED;

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
        s_asid_stale[c][asid / 32] |= 1u << (asid % 32);
    }
    taskEXIT_CRITICAL(&s_asid_lock);
}

void espix_um_enter(uint32_t pc, uint32_t usp, uint32_t a0, uint32_t a1, uint32_t a2);
void espix_um_return(void *frame);
void espix_um_trap(RvExcFrame *frame, uint32_t cause);

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
    /* cpu_apm region 0 (flash and PSRAM) is TEE-only, and U-mode is REE: the page tables isolate instead. */
    REG_SET_BIT(CPU_APM_REGION0_ATTR_REG, 0x0777);
    espix_klog(ESPIX_KLOG_INFO, TAG, "user mode on; mscratch at boot 0x%08x 0x%08x",
               (unsigned)s_boot_mscratch[0], (unsigned)s_boot_mscratch[portNUM_PROCESSORS - 1]);
}

extern StaticTask_t *port_uxCoprocOwner[portNUM_PROCESSORS][SOC_CPU_COPROC_NUM];
RvCoprocSaveArea *pxPortGetCoprocArea(StaticTask_t *task, bool allocate, int coproc);
RvCoprocSaveArea *pxPortUpdateCoprocOwner(int coreid, int coproc, StaticTask_t *owner);
void vPortTaskPinToCore(StaticTask_t *task, int coreid);

static IRAM_ATTR void fpu_save(void *area)
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

static IRAM_ATTR void fpu_restore(const void *area)
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
static IRAM_ATTR void fpu_take(uint32_t core)
{
    StaticTask_t *const self = (StaticTask_t *)xTaskGetCurrentTaskHandle();
    RV_SET_CSR(mstatus, 1u << 13);
    if (port_uxCoprocOwner[core][FPU_COPROC_IDX] == self) {
        return;
    }
    RvCoprocSaveArea *const former = pxPortUpdateCoprocOwner(core, FPU_COPROC_IDX, self);
    if (former != NULL && former != (void *)-1) {
        fpu_save(former->sa_coprocs[FPU_COPROC_IDX]);
    }
    vPortTaskPinToCore(self, core);
    RvCoprocSaveArea *const sa = pxPortGetCoprocArea(self, false, FPU_COPROC_IDX);
    fpu_restore((sa->sa_enable & (1u << FPU_COPROC_IDX)) ? sa->sa_coprocs[FPU_COPROC_IDX] : s_fpu_zero);
    sa->sa_enable |= 1u << FPU_COPROC_IDX;
}

/* On every return to U-mode: the thread's table, the kernel tp for the next entry, no stack guard. */
IRAM_ATTR void espix_um_return(void *frame)
{
    espix_um_thread_t *t = pvTaskGetThreadLocalStoragePointer(NULL, ESPIX_TLS_UM_IDX);
    /* A fatal signal or a kill: the return lands on an unmapped pc instead, and the trap ends the process.
     * The first entry has no frame (espix_um_enter); its next return will see the signal. */
    if (frame != NULL && t->kick_sig == 0) {
        if (t->slot->stop_requested) {
            t->kick_sig = SIGKILL;
        } else if ((t->slot->sig_pending & ~t->slot->sig_blocked) != 0) {
            t->kick_sig = espix_proc_termsig(t->slot);
        }
        if (t->kick_sig != 0) {
            ((RvExcFrame *)frame)->mepc = UM_KICK_PC;
        }
    }
    const uint32_t core = esp_cpu_get_core_id();
    espix_um_ktp[core] = t->ktp;
    RV_WRITE_CSR(satp, t->satp);
    const uint32_t asid = (t->satp & SATP32_ASID) >> 22;
    if (s_asid_stale[core][asid / 32] & (1u << (asid % 32))) {
        s_asid_stale[core][asid / 32] &= ~(1u << (asid % 32));
        asm volatile ("sfence.vma zero, %0" :: "r"(asid) : "memory");
    }
    fpu_take(core);
    assist_debug_hal_sp_mon_disable(core);
}

/* The most kernel stack any U-mode task has used, to size CONFIG_ESPIX_UM_KSTACK from. */
static uint32_t s_kstack_most;

/* Where an app's exit or fault ends: plain task context on the kernel stack, as an M-mode app's exit() is. */
void espix_um_exited(int status)
{
    const uint32_t used = CONFIG_ESPIX_UM_KSTACK - uxTaskGetStackHighWaterMark(NULL);
    if (used > s_kstack_most) {
        s_kstack_most = used;
        espix_klog(ESPIX_KLOG_INFO, TAG, "kernel stack: %u of %u bytes, the most yet",
                   (unsigned)used, (unsigned)CONFIG_ESPIX_UM_KSTACK);
    }
    espix_proc_exit(status);
    __builtin_unreachable();
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

static void __attribute__((noreturn)) fault(const RvExcFrame *f, uint32_t code)
{
    espix_proc_slot_t *const slot = espix_proc_self();
    const int sig = fault_signal(code);
    const char *what = (sig == SIGILL) ? "Illegal instruction" : (sig == SIGBUS) ? "Bus error"
                     : (sig == SIGTRAP) ? "Trace/breakpoint trap" : "Segmentation fault";

    espix_klog(ESPIX_KLOG_WARN, TAG, "pid %d: %s at pc 0x%08x, address 0x%08x (mcause %u)",
               slot ? (int)slot->info.pid : -1, what, (unsigned)f->mepc,
               (unsigned)f->mtval, (unsigned)code);
    if (slot != NULL) {
        espix_eprintf(slot->info.session, "%s: %s (pc 0x%08x, address 0x%08x)\n",
                      slot->info.name, what, (unsigned)f->mepc, (unsigned)f->mtval);
    }
    espix_um_exited(128 + sig);
}

void espix_um_kentry(RvExcFrame *u, uint32_t cause);
void espix_um_resume(RvExcFrame *u, uint32_t ms) __attribute__((noreturn));

/*
 * An exception from U-mode, on the thread's kernel stack with interrupts off.
 *
 * Nothing here may be preempted: this core locks up when an exception is taken
 * while an earlier one is unretired, even another task's. So the user's
 * registers go to the slot above mscratch and the exception is left at once,
 * into espix_um_kentry() as ordinary task code.
 */
void espix_um_trap(RvExcFrame *f, uint32_t cause)
{
    extern char __global_pointer$[];
    const espix_um_thread_t *const t = pvTaskGetThreadLocalStoragePointer(NULL, ESPIX_TLS_UM_IDX);
    if (t == NULL) {
        abort();
    }
    RvExcFrame *const u = (RvExcFrame *)((uint8_t *)f + RV_STK_FRMSZ + 16);
    memcpy(u, f, sizeof(*u));
    f->mepc    = (uint32_t)espix_um_kentry;
    f->a0      = (uint32_t)u;
    f->a1      = cause;
    f->ra      = 0;
    f->gp      = (uint32_t)__global_pointer$;
    f->tp      = t->ktp;
    f->mstatus |= MSTATUS_MPP;
    /* CLIC mcause: MPP=M, MPIE=1, level 0; mret takes them from here. */
    RV_WRITE_CSR(mcause, (3u << 28) | (1u << 27));
}

/* The exception's work, in task context with interrupts on: it may block, sleep, take the FPU or end the process. */
void espix_um_kentry(RvExcFrame *u, uint32_t cause)
{
    espix_um_thread_t *const t = pvTaskGetThreadLocalStoragePointer(NULL, ESPIX_TLS_UM_IDX);
    const uint32_t code = cause & 0xfff;
    const uint32_t need = (code == 12) ? PTE_X : (code == 13) ? PTE_R : (code == 15) ? PTE_W : 0;

    assist_debug_hal_sp_mon_enable(esp_cpu_get_core_id());
    if (code == 12 && u->mepc == UM_KICK_PC && t->kick_sig != 0) {
        espix_um_exited(128 + t->kick_sig);
    } else if (code == 8) {
        u->mepc += 4;
        u->a0 = (uint32_t)espix_um_syscall(t, u);
    } else if (need == 0 || !espix_um_fault(t->proc->as, u->mtval, need)) {
        fault(u, code);
    }
    espix_um_resume(u, (uint32_t)u - 16);
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

#define ET_EXEC_  2
#define EM_RISCV_ 243
#define PT_LOAD_  1

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

static const char *load_segments(espix_um_as_t *as, const uint8_t *image, size_t size, uint32_t *entry)
{
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
        if (end > file_end && !espix_um_region_add(as, file_end, end, flags)) {
            return "too many segments";
        }
        image_end = MAX(image_end, end);
        for (uint32_t va = ph->p_vaddr & ~(PG - 1); va < MIN(end, file_end); va += PG) {
            uint8_t *frame = espix_um_map_new(as, va, flags);
            if (frame == NULL) {
                return "out of memory for the image";
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
        uint8_t *k = espix_um_user_ptr(as, va, PTE_W);
        if (k == NULL) {
            return false;
        }
        const size_t c = MIN(n, PG - (va & (PG - 1)));
        memcpy(k, p, c);
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

    espix_um_as_t *as = espix_um_as_new(asid);
    espix_um_thread_t *t = heap_caps_calloc(1, sizeof(*t), MALLOC_CAP_INTERNAL);
    espix_um_proc_t *p = calloc(1, sizeof(*p));
    if (as == NULL || t == NULL || p == NULL) {
        espix_um_as_free(as);
        free(t);
        free(p);
        asid_free(asid);
        return "out of memory for an address space";
    }
    p->as   = as;
    espix_um_fds_init(p);
    t->proc = p;
    t->slot = slot;
    t->satp = espix_um_as_satp(as);
    app->um = t;

    uint32_t entry = 0, usp = 0, uargv = 0, uenvp = 0;
    const char *why = load_segments(as, app->image, slot->info.image_bytes, &entry);
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
    espix_um_enter(entry, usp, (uint32_t)app->argc, uargv, uenvp);
    __builtin_unreachable();
}

void espix_um_release(espix_proc_slot_t *slot)
{
    if (slot->app == NULL || slot->app->um == NULL) {
        return;
    }
    espix_um_thread_t *t = slot->app->um;
    asid_free((t->satp & SATP32_ASID) >> 22);
    espix_um_as_free(t->proc->as);
    free(t->proc);
    free(t);
    slot->app->um = NULL;
}

#endif /* CONFIG_ESPIX_UMODE */
