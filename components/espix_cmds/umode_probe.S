/* umode spike: mret into U-mode, and umode_trap returns from umode_enter() on any exception. */
#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32S31

#define CTX_RA       0
#define CTX_SP       4
#define CTX_S0       8
#define CTX_MSTATUS  56
#define CTX_MTVEC    60
#define CTX_MSCRATCH 64
#define CTX_T_ENTER  68
#define CTX_T_TRAP   72
#define CTX_MCAUSE   76
#define CTX_MEPC     80
#define CTX_MTVAL    84
#define CTX_UA0      88

#define CRUMB_MTVEC_RB 12
#define CRUMB_TRAPS    16
#define CRUMB_MCAUSE   20
#define CRUMB_MEPC     24
#define CRUMB_THRESH   32
#define CRUMB_INTSTATUS 36

#define MSTATUS_MPP  0x1800
#define MSTATUS_MPRV 0x20000

.macro save_kernel ctx
    sw      ra,  CTX_RA(\ctx)
    sw      sp,  CTX_SP(\ctx)
    sw      s0,  CTX_S0+0(\ctx)
    sw      s1,  CTX_S0+4(\ctx)
    sw      s2,  CTX_S0+8(\ctx)
    sw      s3,  CTX_S0+12(\ctx)
    sw      s4,  CTX_S0+16(\ctx)
    sw      s5,  CTX_S0+20(\ctx)
    sw      s6,  CTX_S0+24(\ctx)
    sw      s7,  CTX_S0+28(\ctx)
    sw      s8,  CTX_S0+32(\ctx)
    sw      s9,  CTX_S0+36(\ctx)
    sw      s10, CTX_S0+40(\ctx)
    sw      s11, CTX_S0+44(\ctx)
    csrr    t0, mstatus
    sw      t0, CTX_MSTATUS(\ctx)
    csrr    t0, mtvec
    sw      t0, CTX_MTVEC(\ctx)
    csrr    t0, mscratch
    sw      t0, CTX_MSCRATCH(\ctx)
    /* Keep mtvec's mode bits (CLIC): only the base moves. */
    andi    t1, t0, 0x3f
    la      t0, umode_trap
    or      t0, t0, t1
    csrw    mtvec, t0
    csrw    mscratch, \ctx
    csrr    t0, mtvec
    la      t1, umode_crumb
    sw      t0, CRUMB_MTVEC_RB(t1)
.endm

.macro restore_kernel ctx
    lw      t0, CTX_MTVEC(\ctx)
    csrw    mtvec, t0
    lw      t0, CTX_MSCRATCH(\ctx)
    csrw    mscratch, t0
    lw      ra,  CTX_RA(\ctx)
    lw      sp,  CTX_SP(\ctx)
    lw      s0,  CTX_S0+0(\ctx)
    lw      s1,  CTX_S0+4(\ctx)
    lw      s2,  CTX_S0+8(\ctx)
    lw      s3,  CTX_S0+12(\ctx)
    lw      s4,  CTX_S0+16(\ctx)
    lw      s5,  CTX_S0+20(\ctx)
    lw      s6,  CTX_S0+24(\ctx)
    lw      s7,  CTX_S0+28(\ctx)
    lw      s8,  CTX_S0+32(\ctx)
    lw      s9,  CTX_S0+36(\ctx)
    lw      s10, CTX_S0+40(\ctx)
    lw      s11, CTX_S0+44(\ctx)
    /* Last: once MIE is back an interrupt may be taken, and sp must be the task's. */
    lw      t0, CTX_MSTATUS(\ctx)
    csrw    mstatus, t0
.endm

    .section .iram1.umode, "ax"

/* void umode_enter(umode_ctx_t *ctx, uint32_t pc, uint32_t sp, uint32_t a0, uint32_t a1, uint32_t a2) */
    .global umode_enter
    .type   umode_enter, @function
    .balign 4
umode_enter:
    save_kernel a0
    csrw    mepc, a1
    /* mcause=0: mret restores interrupt level 0, MPP=U, MPIE=0 (CLIC aliases). */
    csrw    mcause, zero
    li      t0, MSTATUS_MPP | MSTATUS_MPRV
    csrc    mstatus, t0
    mv      t2, a0
    mv      sp, a2
    mv      a0, a3
    mv      a1, a4
    mv      a2, a5
    csrr    t0, mcycle
    sw      t0, CTX_T_ENTER(t2)
    mret
    .size   umode_enter, . - umode_enter

/* int umode_mprv_load(ctx, va): M-mode load via the user mapping; value in ua[0], cycles in ua[1]. */
    .global umode_mprv_load
    .type   umode_mprv_load, @function
    .balign 4
umode_mprv_load:
    save_kernel a0
    sw      zero, CTX_MCAUSE(a0)
    li      t0, MSTATUS_MPP
    csrc    mstatus, t0
    li      t0, MSTATUS_MPRV
    csrr    t1, mcycle
    csrs    mstatus, t0
    lw      t2, 0(a1)
    csrc    mstatus, t0
    csrr    t3, mcycle
    sub     t3, t3, t1
    sw      t2, CTX_UA0(a0)
    sw      t3, CTX_UA0+4(a0)
    restore_kernel a0
    li      a0, 0
    ret
    .size   umode_mprv_load, . - umode_mprv_load

/* CLIC: exceptions go to mtvec's base, which must be 64-byte aligned. */
    .global umode_trap
    .balign 64
umode_trap:
    csrrw   sp, mscratch, sp
    sw      a0, CTX_UA0+0(sp)
    csrr    a0, mcycle
    sw      a0, CTX_T_TRAP(sp)
    sw      a1, CTX_UA0+4(sp)
    sw      a2, CTX_UA0+8(sp)
    sw      a3, CTX_UA0+12(sp)
    csrr    a0, mcause
    sw      a0, CTX_MCAUSE(sp)
    csrr    a0, mepc
    sw      a0, CTX_MEPC(sp)
    csrr    a0, mtval
    sw      a0, CTX_MTVAL(sp)
    la      a1, umode_crumb
    lw      a2, CRUMB_TRAPS(a1)
    addi    a2, a2, 1
    sw      a2, CRUMB_TRAPS(a1)
    csrr    a2, mcause
    sw      a2, CRUMB_MCAUSE(a1)
    csrr    a2, mepc
    sw      a2, CRUMB_MEPC(a1)
    csrr    a2, 0x347
    sw      a2, CRUMB_THRESH(a1)
    csrr    a2, 0xfb1
    sw      a2, CRUMB_INTSTATUS(a1)
    mv      a0, sp
    restore_kernel a0
    lw      a0, CTX_MCAUSE(a0)
    ret

/* The U-mode side: position independent, copied into the code page; each entry ends in ecall. */
    .section .rodata.umode_text, "a"
    .balign 4
    .global umode_text_start
umode_text_start:

    .global u_ecall
u_ecall:
    ecall

/* a0 = va: a0 = value, a1 = cycles for the first load, a2 = for the second. */
    .global u_load2
u_load2:
    rdcycle t0
    lw      t1, 0(a0)
    xor     t5, t1, t1
    rdcycle t2
    lw      t3, 0(a0)
    xor     t5, t3, t3
    rdcycle t4
    sub     a1, t2, t0
    sub     a2, t4, t2
    mv      a0, t1
    ecall

/* a0 = va, a1 = value. */
    .global u_store
u_store:
    sw      a1, 0(a0)
    ecall

/* a0 = va: the load is expected to fault; a3 = the cycle count just before it. */
    .global u_fault_load
u_fault_load:
    rdcycle a3
    lw      t1, 0(a0)
    ecall

/* a0 = va to jump to. */
    .global u_jump
u_jump:
    rdcycle a3
    jr      a0

/* An M-mode CSR from U-mode: must be an illegal instruction. */
    .global u_csr
u_csr:
    csrr    t0, mstatus
    ecall

/* a0 = base, a1 = count, a2 = stride: one warm pass, then a1 = cycles for the second. */
    .global u_sweep
u_sweep:
    mv      t0, a0
    mv      t1, a1
1:  lw      t2, 0(t0)
    add     t0, t0, a2
    addi    t1, t1, -1
    bnez    t1, 1b
    mv      t0, a0
    mv      t1, a1
    rdcycle t3
2:  lw      t2, 0(t0)
    add     t0, t0, a2
    addi    t1, t1, -1
    bnez    t1, 2b
    xor     t2, t2, t2
    rdcycle t4
    sub     a1, t4, t3
    ecall

/* a0 = cycles to spin in U-mode before the ecall; an interrupt may end it first. */
    .global u_spin
u_spin:
    rdcycle t0
1:  rdcycle t1
    sub     t2, t1, t0
    bltu    t2, a0, 1b
    ecall

    .global umode_text_end
umode_text_end:

#endif /* CONFIG_IDF_TARGET_ESP32S31 */
