/* umode: the S31 user-mode / Sv32 spike (R-P8.6); results in docs/ROADMAP.md. */
#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32S31

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_private/hw_stack_guard.h"
#include "riscv/csr.h"
#include "riscv/csr_clic.h"
#include "riscv/rvruntime-frames.h"
#include "soc/reg_base.h"
#include "soc/cpu_apm_reg.h"
#include "soc/soc.h"

#include "espix_cmds_priv.h"
#include "espix_task.h"

#define PG        4096u
#define DATA_OFF  0x800u        /* the data word shares the code's physical page */
#define MAGIC_A   0x5AA5F00Du
#define MAGIC_B   0xB0B0CAFEu
#define REPS      32

#define VA_CODE   0x00400000u
#define VA_DATA   0x00401000u   /* same page as VA_CODE, writable, not executable */
#define VA_NOA    0x00402000u   /* A clear */
#define VA_NOD    0x00403000u   /* A set, D clear */
#define VA_KERN   0x00404000u   /* no U bit */
#define VA_HOLE   0x00405000u   /* not mapped */
#define VA_SWEEP  0x00500000u   /* SWEEP_MAX aliases of the same page */
#define SWEEP_MAX 256

typedef struct {
    uint32_t ra, sp, s[12], mstatus, mtvec, mscratch;
    uint32_t t_enter, t_trap, mcause, mepc, mtval, ua[4];
} umode_ctx_t;

_Static_assert(offsetof(umode_ctx_t, mstatus) == 56, "umode_probe.S CTX_MSTATUS");
_Static_assert(offsetof(umode_ctx_t, t_enter) == 68, "umode_probe.S CTX_T_ENTER");
_Static_assert(offsetof(umode_ctx_t, ua) == 88, "umode_probe.S CTX_UA0");

uint32_t umode_enter(umode_ctx_t *c, uint32_t pc, uint32_t sp,
                     uint32_t a0, uint32_t a1, uint32_t a2);
uint32_t umode_mprv_load(umode_ctx_t *c, uint32_t va);

extern const char umode_text_start[], umode_text_end[];
extern const char u_ecall[], u_load2[], u_store[], u_fault_load[], u_jump[],
                  u_csr[], u_sweep[], u_spin[];

#define UPC(sym) (VA_CODE + (uint32_t)((sym) - umode_text_start))

/* LP RAM survives a watchdog reset: where a run that never came back stopped. */
typedef struct {
    uint32_t magic, step, pc, mtvec_rb, traps, mcause, mepc, inside;
    uint32_t thresh, intstatus;     /* mintthresh, mintstatus at the last trap */
} umode_crumb_t;

#define CRUMB_RUNNING 0x554d4f44u
#define CRUMB_DONE    0x444f4e45u

RTC_NOINIT_ATTR umode_crumb_t umode_crumb;
_Static_assert(offsetof(umode_crumb_t, mtvec_rb) == 12 && offsetof(umode_crumb_t, mepc) == 24 &&
               offsetof(umode_crumb_t, intstatus) == 36,
               "umode_probe.S CRUMB_*");

enum {
    ST_SETUP, ST_SATP, ST_MPP, ST_MEDELEG, ST_PMP, ST_TRAPS, ST_TLB, ST_AD,
    ST_MPRV, ST_MEGA, ST_ASID, ST_PSRAM, ST_MPRV_HOLE, ST_MPRV_PERM, ST_SPIN, ST_COUNT
};

static const char *const s_step_names[ST_COUNT] = {
    "setup", "csrs: satp write", "csrs: mstatus.MPP", "csrs: medeleg", "csrs: pmpcfg",
    "traps", "tlb", "ad", "mprv", "mega", "asid", "psram", "mprvhole", "mprvperm", "spin",
};

static void crumb(uint32_t step)
{
    umode_crumb.step = step;
    umode_crumb.pc   = 0;
}

typedef struct {
    uint32_t         *root, *l2;
    uint8_t          *page;         /* code at 0, data word at DATA_OFF */
    uint8_t          *page_b;       /* PSRAM, the other address space's data */
    umode_ctx_t      *c;            /* on the probe's stack: umode_trap's sp stays in bounds */
    char             *out;
    volatile size_t   out_len;
    size_t            out_cap;
    espix_task_exit_t exit;
    uint32_t          stages;       /* bit per ST_* to run */
} umode_t;

static void say(umode_t *u, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const size_t len = u->out_len;
    const int n = vsnprintf(u->out + len, u->out_cap - len, fmt, ap);
    va_end(ap);
    if (n > 0) {
        u->out_len = len + ((size_t)n < u->out_cap - len ? (size_t)n : u->out_cap - len - 1);
    }
}

static uint32_t pte(const void *pa, uint32_t flags)
{
    return (((uint32_t)pa >> 12) << 10) | flags;
}

static void map(uint32_t *root, uint32_t *l2, uint32_t va, const void *pa, uint32_t flags)
{
    root[va >> 22] = pte(l2, PTE_V);
    l2[(va >> 12) & 0x3ff] = pte(pa, flags);
}

static uint32_t satp_of(const uint32_t *root, uint32_t asid)
{
    return 0x80000000u | (asid << 22) | ((uint32_t)root >> 12);
}

static const char *cause_name(uint32_t mcause)
{
    switch (mcause & 0xfff) {
    case 1:  return "instruction access fault";
    case 2:  return "illegal instruction";
    case 5:  return "load access fault";
    case 7:  return "store access fault";
    case 8:  return "ecall from U";
    case 12: return "instruction page fault";
    case 13: return "load page fault";
    case 15: return "store page fault";
    default: return "?";
    }
}

/* In-bounds stack for an IDF interrupt taken from U-mode; mintthresh does not mask them there. */
static uint32_t task_usp(void)
{
    uint32_t sp;
    asm volatile ("mv %0, sp" : "=r"(sp));
    return (sp - 512) & ~15u;
}

/* One excursion into U-mode, scheduler held while mtvec is ours; returns mcause. */
static uint32_t run(umode_t *u, uint32_t satp, bool flush, uint32_t pc,
                    uint32_t a0, uint32_t a1, uint32_t a2)
{
    vTaskSuspendAll();
    const uint32_t thr = RV_SWAP_CSR(MINTTHRESH_CSR, 0xff);
    esp_hw_stack_guard_monitor_stop();
    const uint32_t old_satp = RV_SWAP_CSR(satp, satp);
    /* With S-mode present, U reads the counters only if both enable them. */
    const uint32_t old_cen  = RV_SWAP_CSR(mcounteren, 0x7);
    const uint32_t old_scen = RV_SWAP_CSR(scounteren, 0x7);
    if (flush) {
        asm volatile ("sfence.vma" ::: "memory");
    }
    umode_crumb.pc = pc;
    umode_crumb.inside = 1;
    const uint32_t cause = umode_enter(u->c, pc, task_usp(), a0, a1, a2);
    umode_crumb.inside = 0;
    RV_WRITE_CSR(scounteren, old_scen);
    RV_WRITE_CSR(mcounteren, old_cen);
    RV_WRITE_CSR(satp, old_satp);
    esp_hw_stack_guard_monitor_start();
    RV_WRITE_CSR(MINTTHRESH_CSR, thr);
    xTaskResumeAll();
    return cause;
}

static uint32_t run_mprv(umode_t *u, uint32_t satp, uint32_t va)
{
    vTaskSuspendAll();
    const uint32_t thr = RV_SWAP_CSR(MINTTHRESH_CSR, 0xff);
    const uint32_t old_satp = RV_SWAP_CSR(satp, satp);
    asm volatile ("sfence.vma" ::: "memory");
    umode_crumb.pc = va;
    umode_crumb.inside = 2;
    const uint32_t cause = umode_mprv_load(u->c, va);
    umode_crumb.inside = 0;
    RV_WRITE_CSR(satp, old_satp);
    RV_WRITE_CSR(MINTTHRESH_CSR, thr);
    xTaskResumeAll();
    return cause;
}

static int cmp_u32(const void *a, const void *b)
{
    const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static void stats(umode_t *u, const char *what, uint32_t *v, int n)
{
    qsort(v, n, sizeof(v[0]), cmp_u32);
    say(u, "  %-34s min %4u  median %4u  max %4u cycles\n",
        what, (unsigned)v[0], (unsigned)v[n / 2], (unsigned)v[n - 1]);
}

/* Expect a trap with this exception code; say what happened either way. */
static bool expect(umode_t *u, const char *what, uint32_t cause, uint32_t want)
{
    const bool ok = (cause & 0xfff) == want;
    say(u, "  %-34s %s (mcause 0x%08x, mtval 0x%08x)%s\n", what, cause_name(cause),
        (unsigned)cause, (unsigned)u->c->mtval, ok ? "" : "  <-- unexpected");
    return ok;
}

static void probe_csrs(umode_t *u)
{
    const uint32_t misa = RV_READ_CSR(misa);
    say(u, "csrs\n  misa 0x%08x: S %s, U %s\n", (unsigned)misa,
        (misa & (1u << 18)) ? "yes" : "no", (misa & (1u << 20)) ? "yes" : "no");

    crumb(ST_SATP);
    const uint32_t thr = RV_SWAP_CSR(MINTTHRESH_CSR, 0xff);
    const uint32_t want = 0x80000000u | 0x7fc00000u | ((uint32_t)u->root >> 12);
    const uint32_t old_satp = RV_SWAP_CSR(satp, want);
    const uint32_t rb = RV_SWAP_CSR(satp, old_satp);
    crumb(ST_MPP);
    const uint32_t old_ms = RV_READ_CSR(mstatus);
    RV_WRITE_CSR(mstatus, (old_ms & ~0x1800u) | 0x0800u);
    const uint32_t mpp = (RV_READ_CSR(mstatus) >> 11) & 3;
    RV_WRITE_CSR(mstatus, old_ms);
    crumb(ST_MEDELEG);
    const uint32_t old_ed = RV_SWAP_CSR(medeleg, 0xffffffffu);
    const uint32_t medeleg = RV_SWAP_CSR(medeleg, old_ed);
    RV_WRITE_CSR(MINTTHRESH_CSR, thr);

    say(u, "  satp wrote 0x%08x read 0x%08x: mode %s, %d ASID bits, ppn %s\n",
        (unsigned)want, (unsigned)rb, (rb >> 31) ? "Sv32" : "bare",
        __builtin_popcount((rb >> 22) & 0x1ff),
        (rb & 0x3fffff) == (want & 0x3fffff) ? "kept" : "changed");
    say(u, "  mstatus.MPP=S %s; medeleg writable 0x%08x\n",
        mpp == 1 ? "holds" : "does not hold", (unsigned)medeleg);
    crumb(ST_PMP);
    /* Locked entries bind S/U with the same R/W/X; unmatched addresses deny them. */
    say(u, "  pmpcfg0-3 0x%08x 0x%08x 0x%08x 0x%08x\n",
        (unsigned)RV_READ_CSR(pmpcfg0), (unsigned)RV_READ_CSR(pmpcfg1),
        (unsigned)RV_READ_CSR(pmpcfg2), (unsigned)RV_READ_CSR(pmpcfg3));
}

static void probe_traps(umode_t *u, uint32_t satp)
{
    uint32_t enter[REPS], whole[REPS];
    uint32_t cause = 0;

    say(u, "round trip (mret into U, ecall back)\n");
    for (int i = 0; i < REPS; i++) {
        const uint32_t t0 = RV_READ_CSR(mcycle);
        cause = run(u, satp, i == 0, UPC(u_ecall), 0, 0, 0);
        whole[i] = RV_READ_CSR(mcycle) - t0;
        enter[i] = u->c->t_trap - u->c->t_enter;
        if ((cause & 0xfff) != 8) {
            expect(u, "ecall", cause, 8);
            return;
        }
    }
    stats(u, "mret to trap entry", enter, REPS);
    stats(u, "whole excursion, C to C", whole, REPS);

    say(u, "isolation\n");
    expect(u, "load from an unmapped page", run(u, satp, true, UPC(u_fault_load), VA_HOLE, 0, 0), 13);
    expect(u, "load from a page without U", run(u, satp, true, UPC(u_fault_load), VA_KERN, 0, 0), 13);
    expect(u, "store to the code page (R-X)", run(u, satp, true, UPC(u_store), VA_CODE, 1, 0), 15);
    expect(u, "jump into the data page (RW-)", run(u, satp, true, UPC(u_jump), VA_DATA + DATA_OFF, 0, 0), 12);
    expect(u, "jump to an unmapped page", run(u, satp, true, UPC(u_jump), VA_HOLE, 0, 0), 12);
    expect(u, "read mstatus from U", run(u, satp, true, UPC(u_csr), 0, 0, 0), 2);
    expect(u, "load from kernel memory by PA", run(u, satp, true, UPC(u_fault_load), (uint32_t)u->root, 0, 0), 13);

    uint32_t lat[REPS];
    for (int i = 0; i < REPS; i++) {
        run(u, satp, false, UPC(u_fault_load), VA_HOLE, 0, 0);
        lat[i] = u->c->t_trap - u->c->ua[3];
    }
    stats(u, "page fault, U load to M handler", lat, REPS);
}

static void probe_tlb(umode_t *u, uint32_t satp)
{
    uint32_t miss[REPS], hit[REPS], warm[REPS];

    say(u, "loads (data in internal SRAM, tables in internal SRAM)\n");
    for (int i = 0; i < REPS; i++) {
        const uint32_t cause = run(u, satp, true, UPC(u_load2), VA_DATA + DATA_OFF, 0, 0);
        if ((cause & 0xfff) != 8 || u->c->ua[0] != MAGIC_A) {
            expect(u, "load", cause, 8);
            say(u, "  read 0x%08x, wanted 0x%08x\n", (unsigned)u->c->ua[0], MAGIC_A);
            return;
        }
        miss[i] = u->c->ua[1];
        hit[i]  = u->c->ua[2];
        run(u, satp, false, UPC(u_load2), VA_DATA + DATA_OFF, 0, 0);
        warm[i] = u->c->ua[1];
    }
    say(u, "  value read through the mapping: 0x%08x (ok)\n", (unsigned)u->c->ua[0]);
    stats(u, "first load after sfence.vma", miss, REPS);
    stats(u, "same load again", hit, REPS);
    stats(u, "first load, TLB kept from last run", warm, REPS);

    say(u, "TLB reach (K loads, one per page; minus K loads to one page)\n");
    static const int ks[] = { 1, 4, 8, 10, 12, 16, 32, 64, 96, 128, 144, 160, 192, 224, 256 };
    for (size_t ki = 0; ki < sizeof(ks) / sizeof(ks[0]); ki++) {
        const int k = ks[ki];
        uint32_t best = UINT32_MAX, base = UINT32_MAX;
        for (int r = 0; r < 5; r++) {
            run(u, satp, true, UPC(u_sweep), VA_SWEEP, k, PG);
            best = u->c->ua[1] < best ? u->c->ua[1] : best;
            run(u, satp, true, UPC(u_sweep), VA_SWEEP, k, 0);
            base = u->c->ua[1] < base ? u->c->ua[1] : base;
        }
        const int extra = (int)best - (int)base;
        say(u, "  %3d pages: %6u cycles, %3d.%d extra per load\n", k, (unsigned)best,
            extra / k, (extra % k) * 10 / k);
    }
}

static void probe_ad(umode_t *u, uint32_t satp)
{
    uint32_t *noa = &u->l2[(VA_NOA >> 12) & 0x3ff];
    uint32_t *nod = &u->l2[(VA_NOD >> 12) & 0x3ff];

    say(u, "A/D bits\n");
    uint32_t cause = run(u, satp, true, UPC(u_load2), VA_NOA + DATA_OFF, 0, 0);
    if ((cause & 0xfff) == 8) {
        say(u, "  load from a page with A clear: allowed, PTE A now %s (hardware update)\n",
            (*noa & PTE_A) ? "set" : "still clear");
    } else {
        expect(u, "load from a page with A clear", cause, 13);
        say(u, "  -> A is software's: the kernel sets it on the fault (Svade)\n");
    }
    cause = run(u, satp, true, UPC(u_store), VA_NOD + DATA_OFF + 4, 0x1234, 0);
    if ((cause & 0xfff) == 8) {
        say(u, "  store to a page with D clear: allowed, PTE D now %s (hardware update)\n",
            (*nod & PTE_D) ? "set" : "still clear");
    } else {
        expect(u, "store to a page with D clear", cause, 15);
        say(u, "  -> D is software's too\n");
    }
}

static void probe_asid(umode_t *u, uint32_t *root_b, uint32_t *l2_b)
{
    const uint32_t sa1 = satp_of(u->root, 1), sb2 = satp_of(root_b, 2), sb1 = satp_of(root_b, 1);
    const uint32_t va = VA_DATA + DATA_OFF;

    say(u, "ASIDs (VA 0x%08x is 0x%08x in space A, 0x%08x in space B; B's page is PSRAM, cpu_apm opened to REE)\n",
        (unsigned)va, MAGIC_A, MAGIC_B);
    volatile uint32_t *apm_attr = (volatile uint32_t *)CPU_APM_REGION0_ATTR_REG;
    const uint32_t apm_old = *apm_attr;
    *apm_attr = apm_old | 0x0666;
    run(u, sa1, true, UPC(u_load2), va, 0, 0);
    say(u, "  A, ASID 1, after sfence:          0x%08x\n", (unsigned)u->c->ua[0]);
    uint32_t cause = run(u, sb2, false, UPC(u_load2), va, 0, 0);
    if ((cause & 0xfff) != 8) {
        expect(u, "B, ASID 2", cause, 8);
    }
    say(u, "  B, ASID 2, no sfence:             0x%08x %s\n", (unsigned)u->c->ua[0],
        u->c->ua[0] == MAGIC_B ? "(own translation)" : "(STALE: A's entry used)");
    run(u, sa1, false, UPC(u_load2), va, 0, 0);
    say(u, "  A, ASID 1 again, no sfence:       0x%08x, first load %u cycles, second %u\n",
        (unsigned)u->c->ua[0], (unsigned)u->c->ua[1], (unsigned)u->c->ua[2]);
    run(u, sb1, false, UPC(u_load2), va, 0, 0);
    say(u, "  B under A's ASID 1, no sfence:    0x%08x %s\n", (unsigned)u->c->ua[0],
        u->c->ua[0] == MAGIC_A ? "(stale, as the spec allows)" : "(satp write flushed)");

    /* GrieferPig saw an ASID-qualified sfence.vma leave stale entries. */
    uint32_t *pte_b = &l2_b[(VA_DATA >> 12) & 0x3ff];
    const uint32_t keep = *pte_b;
    run(u, sb2, true, UPC(u_load2), va, 0, 0);
    run(u, sa1, false, UPC(u_load2), va, 0, 0);
    *pte_b = pte(u->page, PTE_V | PTE_R | PTE_W | PTE_U | PTE_A | PTE_D);
    {
        const uint32_t thr = RV_SWAP_CSR(MINTTHRESH_CSR, 0xff);
        asm volatile ("sfence.vma zero, %0" :: "r"(2u) : "memory");
        RV_WRITE_CSR(MINTTHRESH_CSR, thr);
    }
    run(u, sb2, false, UPC(u_load2), va, 0, 0);
    say(u, "  B's PTE moved to A's page, sfence.vma x0,asid2: B reads 0x%08x %s\n",
        (unsigned)u->c->ua[0], u->c->ua[0] == MAGIC_A ? "(flushed)" : "(STALE after the ASID flush)");
    run(u, sa1, false, UPC(u_load2), va, 0, 0);
    say(u, "  ... and A (ASID 1) first load %u cycles: %s\n", (unsigned)u->c->ua[1],
        u->c->ua[1] <= 12 ? "kept, the flush was ASID 2's only" : "refetched");
    *pte_b = keep;
    *apm_attr = apm_old;

    uint32_t sw[REPS], swf[REPS];
    const uint32_t thr = RV_SWAP_CSR(MINTTHRESH_CSR, 0xff);
    const uint32_t old = RV_READ_CSR(satp);
    for (int i = 0; i < REPS; i++) {
        uint32_t t0 = RV_READ_CSR(mcycle);
        RV_WRITE_CSR(satp, (i & 1) ? sa1 : sb2);
        sw[i] = RV_READ_CSR(mcycle) - t0;
        t0 = RV_READ_CSR(mcycle);
        RV_WRITE_CSR(satp, (i & 1) ? sb2 : sa1);
        asm volatile ("sfence.vma" ::: "memory");
        swf[i] = RV_READ_CSR(mcycle) - t0;
    }
    RV_WRITE_CSR(satp, old);
    RV_WRITE_CSR(MINTTHRESH_CSR, thr);
    stats(u, "satp write", sw, REPS);
    stats(u, "satp write + sfence.vma", swf, REPS);
}

static void probe_mprv(umode_t *u, uint32_t satp)
{
    say(u, "M-mode reading a user pointer (mstatus.MPRV, MPP=U)\n");
    uint32_t cyc[REPS];
    for (int i = 0; i < REPS; i++) {
        const uint32_t cause = run_mprv(u, satp, VA_DATA + DATA_OFF);
        if (cause != 0) {
            expect(u, "MPRV load", cause, 0);
            return;
        }
        cyc[i] = u->c->ua[1];
    }
    say(u, "  value 0x%08x %s\n", (unsigned)u->c->ua[0], u->c->ua[0] == MAGIC_A ? "(ok)" : "(WRONG)");
    stats(u, "MPRV load incl. TLB miss", cyc, REPS);
}

/* Opt-in: 2 ms in U-mode; interrupts arrive, IDF takes them, U resumes. */
static void probe_spin(umode_t *u, uint32_t satp)
{
    say(u, "2 ms spinning in U-mode, mintthresh 0xff (interrupts taken from U are IDF's, then U resumes)\n");
    for (int i = 0; i < 4; i++) {
        const uint32_t t0 = RV_READ_CSR(mcycle);
        const uint32_t cause = run(u, satp, false, UPC(u_spin), 640000, 0, 0);
        const uint32_t took = RV_READ_CSR(mcycle) - t0;
        say(u, "  run %d: %s after %u cycles, mcause 0x%08x mepc 0x%08x, at the trap mintthresh 0x%02x mintstatus 0x%08x\n",
            i, (cause & 0x80000000u) ? "INTERRUPT" : cause_name(cause), (unsigned)took,
            (unsigned)cause, (unsigned)u->c->mepc, (unsigned)umode_crumb.thresh,
            (unsigned)umode_crumb.intstatus);
    }
}

/* Opt-in: on rev v0.0 a faulting MPRV load stopped the hart without a trap. */
static void probe_mprv_fault(umode_t *u, uint32_t satp, uint32_t va, const char *what)
{
    say(u, "MPRV load that must fault: %s (a hang here is the finding)\n", what);
    expect(u, what, run_mprv(u, satp, va), 13);
}

/* Read-only: the PMP and PMA layout, and what each APM has latched. */
#define PMP_ADDR(n) RV_READ_CSR(pmpaddr##n)
#define PMA_CFG(n)  RV_READ_CSR(0xBC0 + n)
#define PMA_ADDR(n) RV_READ_CSR(0xBD0 + n)

static void dump_protection(umode_t *u)
{
    const uint32_t pa[16] = {
        PMP_ADDR(0), PMP_ADDR(1), PMP_ADDR(2), PMP_ADDR(3), PMP_ADDR(4), PMP_ADDR(5),
        PMP_ADDR(6), PMP_ADDR(7), PMP_ADDR(8), PMP_ADDR(9), PMP_ADDR(10), PMP_ADDR(11),
        PMP_ADDR(12), PMP_ADDR(13), PMP_ADDR(14), PMP_ADDR(15),
    };
    const uint32_t cfg[4] = { RV_READ_CSR(pmpcfg0), RV_READ_CSR(pmpcfg1),
                              RV_READ_CSR(pmpcfg2), RV_READ_CSR(pmpcfg3) };
    say(u, "  pmp (cfg addr<<2):");
    for (int n = 0; n < 16; n++) {
        say(u, " %d:%02x/%08x", n, (unsigned)((cfg[n / 4] >> ((n % 4) * 8)) & 0xff),
            (unsigned)(pa[n] << 2));
    }
    const uint32_t pc[16] = {
        PMA_CFG(0), PMA_CFG(1), PMA_CFG(2), PMA_CFG(3), PMA_CFG(4), PMA_CFG(5), PMA_CFG(6),
        PMA_CFG(7), PMA_CFG(8), PMA_CFG(9), PMA_CFG(10), PMA_CFG(11), PMA_CFG(12),
        PMA_CFG(13), PMA_CFG(14), PMA_CFG(15),
    };
    const uint32_t pd[16] = {
        PMA_ADDR(0), PMA_ADDR(1), PMA_ADDR(2), PMA_ADDR(3), PMA_ADDR(4), PMA_ADDR(5),
        PMA_ADDR(6), PMA_ADDR(7), PMA_ADDR(8), PMA_ADDR(9), PMA_ADDR(10), PMA_ADDR(11),
        PMA_ADDR(12), PMA_ADDR(13), PMA_ADDR(14), PMA_ADDR(15),
    };
    say(u, "\n  pma (cfg addr<<2):");
    for (int n = 0; n < 16; n++) {
        say(u, " %d:%08x/%08x", n, (unsigned)pc[n], (unsigned)(pd[n] << 2));
    }
    say(u, "\n");
    static const struct { const char *name; uint32_t base; int masters; } apm[] = {
        { "cpu_apm", DR_REG_CPU_APM_BASE, 4 }, { "hp_apm", DR_REG_HP_APM_BASE, 7 },
        { "hp_mem_apm", DR_REG_HP_MEM_APM_BASE, 6 },
    };
    for (size_t a = 0; a < sizeof(apm) / sizeof(apm[0]); a++) {
        const volatile uint32_t *r = (const volatile uint32_t *)apm[a].base;
        say(u, "  %s: filter_en %08x func_ctrl %08x region0 %08x-%08x attr %08x\n",
            apm[a].name, (unsigned)r[0], (unsigned)r[0xc4 / 4], (unsigned)r[1],
            (unsigned)r[2], (unsigned)r[3]);
        for (int m = 0; m < apm[a].masters; m++) {
            const uint32_t st = r[(0xc8 + m * 0x10) / 4];
            if (st != 0) {
                say(u, "    M%d status %08x info0 %08x info1 %08x\n", m, (unsigned)st,
                    (unsigned)r[(0xd0 + m * 0x10) / 4], (unsigned)r[(0xd4 + m * 0x10) / 4]);
            }
        }
    }
}

static uint32_t megapage_load(umode_t *u, uint32_t satp, uint32_t va, uint32_t *miss, uint32_t *hit)
{
    for (int i = 0; i < REPS; i++) {
        const uint32_t cause = run(u, satp, true, UPC(u_load2), va, 0, 0);
        if ((cause & 0xfff) != 8) {
            return cause;
        }
        miss[i] = u->c->ua[1];
        hit[i]  = u->c->ua[2];
    }
    return 8;
}

static void probe_megapage(umode_t *u, uint32_t satp)
{
    say(u, "4 MiB megapage onto PSRAM\n");
    const uint32_t va = (uint32_t)u->page_b;
    const uint32_t vpn1 = va >> 22;
    if (u->root[vpn1] != 0) {
        say(u, "  skipped: VPN1 0x%03x in use\n", (unsigned)vpn1);
        return;
    }
    u->root[vpn1] = pte((void *)(va & ~0x3fffffu), PTE_V | PTE_R | PTE_W | PTE_U | PTE_A | PTE_D);
    uint32_t miss[REPS], hit[REPS];
    /* cpu_apm region 0 is flash+PSRAM, TEE (M-mode) only; U-mode is REE. */
    volatile uint32_t *attr = (volatile uint32_t *)CPU_APM_REGION0_ATTR_REG;
    say(u, "  cpu_apm region0 (flash+PSRAM) attr 0x%04x\n", (unsigned)*attr);
    if (*attr & 0x0fff) {
        say(u, "  REE access left open by an earlier run (a reset keeps APM state); closing it\n");
        *attr &= ~0x0fffu;
    }
    uint32_t cause = megapage_load(u, satp, va, miss, hit);
    expect(u, "as IDF leaves the protection", cause, 8);
    if ((cause & 0xfff) != 8) {
        const uint32_t old = *attr;
        say(u, "  opening R/W to REE for this test\n");
        *attr = old | 0x0666;
        cause = megapage_load(u, satp, va, miss, hit);
        *attr = old;
        for (int m = 0; m < 4; m++) {
            REG_WRITE(CPU_APM_M0_STATUS_CLR_REG + m * 0x10, CPU_APM_M0_EXCEPTION_STATUS_CLR);
        }
        if (!expect(u, "with REE R/W in cpu_apm", cause, 8)) {
            dump_protection(u);
            u->root[vpn1] = 0;
            return;
        }
    }
    say(u, "  value 0x%08x %s\n", (unsigned)u->c->ua[0], u->c->ua[0] == MAGIC_B ? "(ok)" : "(WRONG)");
    stats(u, "first load after sfence.vma", miss, REPS);
    stats(u, "same load again", hit, REPS);
    u->root[vpn1] = 0;
}

/* The same address space with both tables in PSRAM, behind the cache. */
static void probe_psram_tables(umode_t *u)
{
    say(u, "page tables in PSRAM\n");
    uint32_t *root = heap_caps_aligned_calloc(PG, 1, PG, MALLOC_CAP_SPIRAM);
    uint32_t *l2   = heap_caps_aligned_calloc(PG, 1, PG, MALLOC_CAP_SPIRAM);
    if (root == NULL || l2 == NULL) {
        say(u, "  skipped: no PSRAM\n");
        goto out;
    }
    /* Memory holds zeros (invalid PTEs); the real ones then sit only in the cache. */
    esp_cache_msync(root, PG, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    esp_cache_msync(l2, PG, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    map(root, l2, VA_CODE, u->page, PTE_V | PTE_R | PTE_X | PTE_U | PTE_A);
    map(root, l2, VA_DATA, u->page, PTE_V | PTE_R | PTE_W | PTE_U | PTE_A | PTE_D);
    const uint32_t satp = satp_of(root, 3);
    const volatile uint32_t *alias = (const volatile uint32_t *)
        ((uint32_t)&root[VA_DATA >> 22] + SOC_NON_CACHEABLE_OFFSET_PSRAM);
    say(u, "  root PTE: 0x%08x in the cache, 0x%08x in PSRAM (uncached alias)\n",
        (unsigned)root[VA_DATA >> 22], (unsigned)*alias);

    uint32_t cause = run(u, satp, true, UPC(u_load2), VA_DATA + DATA_OFF, 0, 0);
    if ((cause & 0xfff) == 8) {
        say(u, "  PTEs written, not written back: walk sees them (the walker reads through the cache)\n");
    } else {
        expect(u, "PTEs written, not written back", cause, 8);
        say(u, "  -> the walker reads PSRAM below the cache; PTE writes need a writeback\n");
    }
    esp_cache_msync(root, PG, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    esp_cache_msync(l2, PG, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    cause = run(u, satp, true, UPC(u_load2), VA_DATA + DATA_OFF, 0, 0);
    if (!expect(u, "after esp_cache_msync", cause, 8)) {
        goto out;
    }
    uint32_t miss[REPS];
    for (int i = 0; i < REPS; i++) {
        run(u, satp, true, UPC(u_load2), VA_DATA + DATA_OFF, 0, 0);
        miss[i] = u->c->ua[1];
    }
    say(u, "  value 0x%08x %s\n", (unsigned)u->c->ua[0], u->c->ua[0] == MAGIC_A ? "(ok)" : "(WRONG)");
    stats(u, "first load after sfence.vma", miss, REPS);
out:
    heap_caps_free(root);
    heap_caps_free(l2);
}

static void probe_task(void *arg)
{
    umode_t *u = arg;
    uint32_t *root_b = NULL, *l2_b = NULL;
    umode_ctx_t ctx = { 0 };
    u->c = &ctx;

    say(u, "core %d, internal free %u\n", xPortGetCoreID(),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    umode_crumb = (umode_crumb_t){ .magic = CRUMB_RUNNING, .step = ST_SETUP };

    const size_t text = (size_t)(umode_text_end - umode_text_start);
    memcpy(u->page, umode_text_start, text);
    *(volatile uint32_t *)(u->page + DATA_OFF) = MAGIC_A;
    *(volatile uint32_t *)u->page_b = MAGIC_B;
    esp_cache_msync(u->page_b, 64, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    asm volatile ("fence.i" ::: "memory");

    map(u->root, u->l2, VA_CODE, u->page, PTE_V | PTE_R | PTE_X | PTE_U | PTE_A);
    map(u->root, u->l2, VA_DATA, u->page, PTE_V | PTE_R | PTE_W | PTE_U | PTE_A | PTE_D);
    map(u->root, u->l2, VA_NOA,  u->page, PTE_V | PTE_R | PTE_W | PTE_U);
    map(u->root, u->l2, VA_NOD,  u->page, PTE_V | PTE_R | PTE_W | PTE_U | PTE_A);
    map(u->root, u->l2, VA_KERN, u->page, PTE_V | PTE_R | PTE_W | PTE_A | PTE_D);
    for (uint32_t i = 0; i < SWEEP_MAX; i++) {
        map(u->root, u->l2, VA_SWEEP + i * PG, u->page, PTE_V | PTE_R | PTE_U | PTE_A);
    }
    const uint32_t satp = satp_of(u->root, 1);

#define WANT(st) (u->stages & (1u << (st)))
    if (WANT(ST_SATP)) {
        probe_csrs(u);
    }
    if (WANT(ST_TRAPS)) {
        crumb(ST_TRAPS);
        probe_traps(u, satp);
    }
    if (WANT(ST_TLB)) {
        crumb(ST_TLB);
        probe_tlb(u, satp);
    }
    if (WANT(ST_AD)) {
        crumb(ST_AD);
        probe_ad(u, satp);
    }
    if (WANT(ST_MPRV)) {
        crumb(ST_MPRV);
        probe_mprv(u, satp);
    }
    if (WANT(ST_SPIN)) {
        crumb(ST_SPIN);
        probe_spin(u, satp);
    }
    if (WANT(ST_MPRV_HOLE)) {
        crumb(ST_MPRV_HOLE);
        probe_mprv_fault(u, satp, VA_HOLE, "unmapped VA");
    }
    if (WANT(ST_MPRV_PERM)) {
        crumb(ST_MPRV_PERM);
        probe_mprv_fault(u, satp, VA_KERN, "mapped, no U bit");
    }
    if (WANT(ST_MEGA)) {
        crumb(ST_MEGA);
        probe_megapage(u, satp);
    }
    if (!WANT(ST_ASID)) {
        goto psram;
    }
    crumb(ST_ASID);

    root_b = heap_caps_aligned_calloc(PG, 1, PG, MALLOC_CAP_INTERNAL);
    l2_b   = heap_caps_aligned_calloc(PG, 1, PG, MALLOC_CAP_INTERNAL);
    if (root_b != NULL && l2_b != NULL) {
        map(root_b, l2_b, VA_CODE, u->page, PTE_V | PTE_R | PTE_X | PTE_U | PTE_A);
        map(root_b, l2_b, VA_DATA, u->page_b - DATA_OFF, PTE_V | PTE_R | PTE_W | PTE_U | PTE_A | PTE_D);
        probe_asid(u, root_b, l2_b);
    } else {
        say(u, "ASIDs: skipped, no internal RAM for a second table\n");
    }
    heap_caps_free(root_b);
    heap_caps_free(l2_b);

psram:
    if (WANT(ST_PSRAM)) {
        crumb(ST_PSRAM);
        probe_psram_tables(u);
    }
    umode_crumb.magic = CRUMB_DONE;

    espix_task_exited(&u->exit);
}

static void report_last(espix_session_t *s)
{
    const umode_crumb_t c = umode_crumb;
    if (c.magic != CRUMB_RUNNING) {
        espix_printf(s, "umode: the last run %s\n",
                     c.magic == CRUMB_DONE ? "finished" : "left no record");
        return;
    }
    espix_printf(s, "umode: the last run did not come back\n"
                 "  step %s, %s 0x%08x\n"
                 "  mtvec read back 0x%08x, traps taken %u, last mcause 0x%08x mepc 0x%08x\n",
                 c.step < ST_COUNT ? s_step_names[c.step] : "?",
                 c.inside == 1 ? "inside U-mode, entry" : c.inside == 2 ? "inside an MPRV load of" : "between excursions, last entry",
                 (unsigned)c.pc, (unsigned)c.mtvec_rb, (unsigned)c.traps,
                 (unsigned)c.mcause, (unsigned)c.mepc);
}

static uint32_t parse_stages(int argc, char **argv)
{
    static const struct { const char *name; uint32_t bits; } names[] = {
        { "csrs",  1u << ST_SATP }, { "traps", 1u << ST_TRAPS }, { "tlb",  1u << ST_TLB },
        { "ad",    1u << ST_AD },   { "mprv",  1u << ST_MPRV },  { "mega", 1u << ST_MEGA },
        { "asid",  1u << ST_ASID }, { "psram", 1u << ST_PSRAM },
        { "mprvhole", 1u << ST_MPRV_HOLE }, { "mprvperm", 1u << ST_MPRV_PERM },
        { "spin", 1u << ST_SPIN },
    };
    if (argc < 2) {
        return UINT32_MAX & ~((1u << ST_MPRV_HOLE) | (1u << ST_MPRV_PERM) | (1u << ST_SPIN));
    }
    uint32_t bits = 0;
    for (int i = 1; i < argc; i++) {
        size_t j = 0;
        while (j < sizeof(names) / sizeof(names[0]) && strcmp(argv[i], names[j].name) != 0) {
            j++;
        }
        if (j == sizeof(names) / sizeof(names[0])) {
            return 0;
        }
        bits |= names[j].bits;
    }
    return bits;
}

typedef struct {
    espix_task_exit_t exit;
    int               kind;     /* flw, fsw, c.flw, c.fsw */
    bool              trapped;
    float             in, out;
} flw_probe_t;

extern StaticTask_t *port_uxCoprocOwner[portNUM_PROCESSORS][SOC_CPU_COPROC_NUM];

static const char *const s_flw_kinds[] = { "flw", "fsw", "c.flw", "c.fsw" };

/* A fresh task's first FPU instruction is a load or store, with the FPU off: IDF's lazy trap must take it. */
static void flw_task(void *arg)
{
    flw_probe_t *p = arg;
    register float *in asm("a0")  = &p->in;
    register float *out asm("a1") = &p->out;
    RV_CLEAR_CSR(mstatus, 3u << 13);
    switch (p->kind) {
    case 0: asm volatile (".option push\n.option norvc\nflw fa0, 0(a0)\nfsw fa0, 0(a1)\n.option pop" :: "r"(in), "r"(out) : "fa0", "memory"); break;
    case 1: asm volatile (".option push\n.option norvc\nfsw fa0, 0(a1)\n.option pop" :: "r"(out) : "memory"); break;
    case 2: asm volatile (".option push\n.option rvc\nc.flw fa0, 0(a0)\n.option pop\nfsw fa0, 0(a1)" :: "r"(in), "r"(out) : "fa0", "memory"); break;
    default: asm volatile (".option push\n.option rvc\nc.fsw fa0, 0(a1)\n.option pop" :: "r"(out) : "memory"); break;
    }
    /* Only IDF's lazy trap makes a task the FPU's owner. */
    p->trapped = port_uxCoprocOwner[xPortGetCoreID()][FPU_COPROC_IDX] == (StaticTask_t *)xTaskGetCurrentTaskHandle();
    espix_task_exited(&p->exit);
}

static int probe_flw(espix_session_t *s, int core)
{
    for (int kind = 0; kind < 4; kind++) {
        flw_probe_t p = { .kind = kind, .in = 2.5f };
        if (!espix_task_exit_init(&p.exit)) {
            return 1;
        }
        espix_printf(s, "umode: %s on core %d as a task's first FPU instruction... ", s_flw_kinds[kind], core);
        if (espix_kthread_start(&(const espix_kthread_t){
                .name = "umode-flw", .fn = flw_task, .arg = &p, .stack = 4096,
                .priority = 5, .core = core, .internal_stack = true, .thread = true },
                &p.exit.task) != ESP_OK) {
            vSemaphoreDelete(p.exit.done);
            return 1;
        }
        espix_task_exit_wait(&p.exit, 0);
        vSemaphoreDelete(p.exit.done);
        espix_printf(s, "%s%s\n", p.trapped ? "taken by the lazy FPU trap" : "ran without the trap",
                     ((kind == 0 || kind == 2) && p.out != 2.5f) ? ", but the value is wrong" : "");
    }
    return 0;
}

static int cmd_umode(espix_session_t *s, int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "last") == 0) {
        report_last(s);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "flw") == 0) {
        return probe_flw(s, (argc > 2) ? atoi(argv[2]) : 0);
    }
    const uint32_t stages = parse_stages(argc, argv);
    if (stages == 0) {
        espix_eprintf(s, "usage: umode [last | flw | csrs traps tlb ad mprv mega asid psram spin mprvhole mprvperm]\n");
        return 2;
    }
    if (umode_crumb.magic == CRUMB_RUNNING) {
        report_last(s);
    }

    umode_t *u = heap_caps_calloc(1, sizeof(*u), MALLOC_CAP_INTERNAL);
    if (u == NULL) {
        espix_eprintf(s, "umode: out of memory\n");
        return 1;
    }
    u->stages = stages;
    u->out_cap = 8192;
    u->out     = heap_caps_calloc(1, u->out_cap, MALLOC_CAP_SPIRAM);
    u->root    = heap_caps_aligned_calloc(PG, 1, PG, MALLOC_CAP_INTERNAL);
    u->l2      = heap_caps_aligned_calloc(PG, 1, PG, MALLOC_CAP_INTERNAL);
    u->page    = heap_caps_aligned_calloc(PG, 1, PG, MALLOC_CAP_INTERNAL);
    /* DATA_OFF into its own page, so space B maps it at the same offset as A. */
    uint8_t *b = heap_caps_aligned_calloc(PG, 1, PG, MALLOC_CAP_SPIRAM);
    u->page_b  = b != NULL ? b + DATA_OFF : NULL;

    int rc = 1;
    if (u->out == NULL || u->root == NULL || u->l2 == NULL || u->page == NULL || b == NULL) {
        espix_eprintf(s, "umode: out of memory for the tables\n");
        goto out;
    }
    if (!espix_task_exit_init(&u->exit)) {
        espix_eprintf(s, "umode: cannot create the exit notification\n");
        goto out;
    }
    if (espix_kthread_start(&(const espix_kthread_t){
            .name = "umode", .fn = probe_task, .arg = u, .stack = 8192,
            .priority = 5, .core = 1, .internal_stack = true, .thread = true },
            &u->exit.task) != ESP_OK) {
        espix_eprintf(s, "umode: cannot start the probe\n");
        goto out;
    }

    size_t shown = 0;
    bool   done  = false;
    while (!done) {
        done = espix_task_exit_wait(&u->exit, 50);
        const size_t len = u->out_len;
        if (len > shown) {
            espix_session_write(s, u->out + shown, len - shown, false);
            shown = len;
        }
    }
    rc = 0;
out:
    if (u->exit.done != NULL) {
        vSemaphoreDelete(u->exit.done);
    }
    heap_caps_free(u->out);
    heap_caps_free(u->root);
    heap_caps_free(u->l2);
    heap_caps_free(u->page);
    heap_caps_free(b);
    heap_caps_free(u);
    return rc;
}

static espix_cmd_t s_umode_cmds[] = {
    { .name = "umode", .fn = cmd_umode,
      .help = "S31 user-mode / Sv32 MMU spike: costs and behaviour", .usage = "umode [last | flw | csrs traps tlb ad mprv mega asid psram spin mprvhole mprvperm]" },
};

void espix_cmds_register_umode(void)
{
    espix_cmds_register_table(s_umode_cmds,
                              sizeof(s_umode_cmds) / sizeof(s_umode_cmds[0]));
}

#endif /* CONFIG_IDF_TARGET_ESP32S31 */
