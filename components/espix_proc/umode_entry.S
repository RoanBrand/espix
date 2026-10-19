/* The first entry into U-mode; every later one is the patched IDF exit (tools/patch-umode.py). */
#include "sdkconfig.h"

#if CONFIG_ESPIX_UMODE

    .section .iram1.umode_entry, "ax"

/* void espix_um_enter(uint32_t pc, uint32_t usp, uint32_t a0, uint32_t a1, uint32_t a2): does not return */
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
    li      a0, 0
    call    espix_um_return
    /* CLIC mcause: MPIE=1, MPP=U, interrupt level 0. */
    li      t0, (1 << 27)
    csrw    mcause, t0
    csrw    mepc, s0
    /* The kernel stack top for traps from U-mode, with two scratch words above it. */
    addi    t0, sp, -16
    csrw    mscratch, t0
    mv      sp, s1
    mv      a0, s2
    mv      a1, s3
    mv      a2, s4
    li      ra, 0
    li      gp, 0
    li      tp, 0
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

#endif /* CONFIG_ESPIX_UMODE */
