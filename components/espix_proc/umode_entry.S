/* The first entry into U-mode; every later one is the patched IDF exit (tools/patch-umode.py). */
#include "sdkconfig.h"

#if CONFIG_ESPIX_UMODE

#include "riscv/rvruntime-frames.h"

    .section .iram1.umode_entry, "ax"

/* void espix_um_enter(uint32_t pc, uint32_t usp, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t tp): does not return */
    .global espix_um_enter
    .type   espix_um_enter, @function
    .balign 4
espix_um_enter:
    csrci   mstatus, 0x8
    mv      s0, a0
    mv      s1, a1
    mv      s2, a2
    mv      s3, a3
    mv      s4, a4
    mv      s5, a5
    li      a0, 0
    call    espix_um_return
    /* CLIC mcause: MPIE=1, MPP=U, interrupt level 0. */
    li      t0, (1 << 27)
    csrw    mcause, t0
    csrw    mepc, s0
    /* The kernel stack top for traps from U-mode, two scratch words above it. */
    addi    t0, sp, -16
    csrw    mscratch, t0
    mv      sp, s1
    mv      a0, s2
    mv      a1, s3
    mv      a2, s4
    li      ra, 0
    li      gp, 0
    mv      tp, s5
    li      t0, 0
    li      t1, 0
    li      t2, 0
    li      s0, 0
    li      s1, 0
    li      a3, 0
    li      a4, 0
    li      a5, 0
    li      a6, 0
    li      a7, 0
    li      s2, 0
    li      s3, 0
    li      s4, 0
    li      s5, 0
    li      s6, 0
    li      s7, 0
    li      s8, 0
    li      s9, 0
    li      s10, 0
    li      s11, 0
    li      t3, 0
    li      t4, 0
    li      t5, 0
    li      t6, 0
    mret
    .size   espix_um_enter, . - espix_um_enter

/*
 * An exception from U-mode, jumped to by the patched vector: a0 is the user's saved registers, at the
 * kernel stack top, a1 the cause, interrupts off. Nothing here may be preempted: this core locks up
 * when an exception is taken while an earlier one is unretired, even another task's. So the exception
 * is left at once, into espix_um_kentry(a0, a1, thread) as task code, its stack below the registers.
 */
    .global espix_um_trap
    .type   espix_um_trap, @function
    .balign 4
espix_um_trap:
    csrr    t0, mhartid
    slli    t0, t0, 2
    la      t1, espix_um_cur
    add     t1, t1, t0
    lw      a2, 0(t1)
    la      t0, espix_um_kentry
    csrw    mepc, t0
    /* CLIC mcause: MPP=M, MPIE=1, interrupt level 0. */
    li      t0, (3 << 28) | (1 << 27)
    csrw    mcause, t0
    li      ra, 0
    mret
    .size   espix_um_trap, . - espix_um_trap

/* void espix_um_resume(RvExcFrame *u, uint32_t ms, espix_um_thread_t *t): back to U-mode with u's registers; ms is mscratch for the next trap */
    .global espix_um_resume
    .type   espix_um_resume, @function
    .balign 4
espix_um_resume:
    csrci   mstatus, 0x8
    mv      s0, a0
    mv      s1, a1
    mv      a1, a2
    call    espix_um_return_to
    /* CLIC mcause: MPIE=1, MPP=U, interrupt level 0. */
    li      t0, (1 << 27)
    csrw    mcause, t0
    lw      t0, RV_STK_MEPC(s0)
    csrw    mepc, t0
    csrw    mscratch, s1
    mv      sp, s0
    lw      ra, RV_STK_RA(sp)
    lw      gp, RV_STK_GP(sp)
    lw      tp, RV_STK_TP(sp)
    lw      t0, RV_STK_T0(sp)
    lw      t1, RV_STK_T1(sp)
    lw      t2, RV_STK_T2(sp)
    lw      s0, RV_STK_S0(sp)
    lw      s1, RV_STK_S1(sp)
    lw      a0, RV_STK_A0(sp)
    lw      a1, RV_STK_A1(sp)
    lw      a2, RV_STK_A2(sp)
    lw      a3, RV_STK_A3(sp)
    lw      a4, RV_STK_A4(sp)
    lw      a5, RV_STK_A5(sp)
    lw      a6, RV_STK_A6(sp)
    lw      a7, RV_STK_A7(sp)
    lw      s2, RV_STK_S2(sp)
    lw      s3, RV_STK_S3(sp)
    lw      s4, RV_STK_S4(sp)
    lw      s5, RV_STK_S5(sp)
    lw      s6, RV_STK_S6(sp)
    lw      s7, RV_STK_S7(sp)
    lw      s8, RV_STK_S8(sp)
    lw      s9, RV_STK_S9(sp)
    lw      s10, RV_STK_S10(sp)
    lw      s11, RV_STK_S11(sp)
    lw      t3, RV_STK_T3(sp)
    lw      t4, RV_STK_T4(sp)
    lw      t5, RV_STK_T5(sp)
    lw      t6, RV_STK_T6(sp)
    lw      sp, RV_STK_SP(sp)
    mret
    .size   espix_um_resume, . - espix_um_resume

#endif /* CONFIG_ESPIX_UMODE */
