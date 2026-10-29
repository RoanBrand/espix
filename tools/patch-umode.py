#!/usr/bin/env python3
"""
Teach IDF's RISC-V trap entries about U-mode, for espix's user-mode apps.

docs/USER-MODE.md has the design. Every vector that can interrupt a U-mode app
switches to the task's kernel stack on the way in, back to the user's on the way
out, and reloads the kernel's gp and tp, which hold the app's values in U-mode.

mscratch is the kernel stack top while the hart is in U-mode and 0 in M-mode.
The entry tells the two apart by swapping mscratch with t0, never with sp: the
hardware stack guard checks every write to sp, and a transient zero trips it.
Only the U-mode paths move sp, and the guard is off while an app runs. The top
two words above the kernel stack top are scratch for the user's sp and t0.

Three entries, because the CLIC vectors to three handlers:

  _panic_handler      exceptions (an app's ecall and faults go to espix_um_trap)
  _interrupt_handler  every ordinary interrupt, and FreeRTOS's context switch
  esp_ipc_isr_handler the cross-core IPC interrupt

Kernel code never runs with an app's satp: the entries write it to bare, and
espix_um_return() loads the thread's on the way back.

The interrupt frame grows from 128 bytes to RV_STK_FRMSZ so it can record which
privilege the task was interrupted in: IDF restores the *interrupted* task's
mstatus after a switch, which is wrong once one task is in U-mode and the next in
M-mode.

Everything is inside #if CONFIG_ESPIX_UMODE, so a build without it, or another
project on this IDF tree, compiles exactly as before.
"""

import os
import sys
from pathlib import Path

EXPECTED_IDF = "v6.1"
MARK = "CONFIG_ESPIX_UMODE"

VECTORS = "components/riscv/vectors.S"
IPC = "components/esp_system/port/arch/riscv/esp_ipc_isr_handler.S"

MACROS = """#if CONFIG_ESPIX_UMODE
/* espix user mode: mscratch is the kernel stack top in U-mode, 0 in M-mode. */
    .equ ESPIX_UM_FRAME_USER, 0x1800        /* interrupt frame flag; exception frames hold mstatus */
    .equ ESPIX_UM_MCAUSE_TO_USER, (1 << 27) /* CLIC mcause: MPIE=1, MPP=U, level 0 */

/* From U-mode: onto the kernel stack, the user's sp and t0 parked above its top. From M-mode: nothing. */
.macro espix_um_entry
    csrrw   t0, mscratch, t0
    bnez    t0, 91f
    csrrw   t0, mscratch, t0
    j       92f
91:
    sw      sp, 0(t0)
    csrr    sp, mscratch
    sw      sp, 4(t0)
    mv      sp, t0
    lw      t0, 4(sp)
    csrw    mscratch, sp
92:
.endm

/* After the frame is saved: the user's sp into it, the kernel's gp and tp back. Clobbers t0. */
.macro espix_um_entry_fix flag=0
    csrrw   t0, mscratch, zero
.if \\flag
    sw      zero, RV_STK_MSTATUS(sp)
.endif
    beqz    t0, 98f
    csrw    satp, zero
    lw      t0, 0(t0)
    sw      t0, RV_STK_SP(sp)
.if \\flag
    li      t0, ESPIX_UM_FRAME_USER
    sw      t0, RV_STK_MSTATUS(sp)
.endif
    .option push
    .option norelax
    la      gp, __global_pointer$
    .option pop
    csrr    t0, mhartid
    slli    t0, t0, 2
    la      tp, espix_um_ktp
    add     tp, tp, t0
    lw      tp, 0(tp)
98:
.endm

/* Back to U-mode from a frame at the kernel stack top; mscratch holds the user's sp. */
.macro espix_um_exit_to_user
    lw      gp, RV_STK_GP(sp)
    restore_general_regs RV_STK_FRMSZ
    csrrw   sp, mscratch, sp
    mret
.endm
#endif /* CONFIG_ESPIX_UMODE */

"""

VECTORS_EDITS = [
    ("macros",
     "    .global rtos_int_enter\n    .global rtos_int_exit\n",
     MACROS + "    .global rtos_int_enter\n    .global rtos_int_exit\n"),

    ("exception entry",
     "_panic_handler:\n"
     "    /* Allocate space on the stack and store general purpose registers */\n"
     "    save_general_regs RV_STK_FRMSZ\n",
     "_panic_handler:\n"
     "#if CONFIG_ESPIX_UMODE\n"
     "    espix_um_entry\n"
     "#endif\n"
     "    /* Allocate space on the stack and store general purpose registers */\n"
     "    save_general_regs RV_STK_FRMSZ\n"),

    ("exception frame",
     "    sw    t0, RV_STK_SP(sp)\n    save_xepc\n    save_xcsr\n",
     "    sw    t0, RV_STK_SP(sp)\n    save_xepc\n    save_xcsr\n"
     "#if CONFIG_ESPIX_UMODE\n"
     "    espix_um_entry_fix\n"
     "#endif\n"),

    ("exception dispatch",
     "_panic_handler_not_coproc:\n\n#endif /* ( SOC_CPU_COPROC_NUM > 0 ) */\n",
     "_panic_handler_not_coproc:\n\n#endif /* ( SOC_CPU_COPROC_NUM > 0 ) */\n"
     "#if CONFIG_ESPIX_UMODE\n"
     "    /* An exception from U-mode is the app's, never a panic. */\n"
     "    bltz  s0, 93f\n"
     "    lw    t0, RV_STK_MSTATUS(sp)\n"
     "    li    t1, ESPIX_UM_FRAME_USER\n"
     "    and   t0, t0, t1\n"
     "    bnez  t0, 93f\n"
     "    mv    a0, sp\n"
     "    mv    a1, s0\n"
     "    call  espix_um_trap\n"
     "    j     _return_from_exception\n"
     "93:\n"
     "#endif\n"),

    ("exception exit",
     "_return_from_exception:\n    mintthresh_csr_restore s5\n\n    restore_xepc\n",
     "_return_from_exception:\n    mintthresh_csr_restore s5\n\n"
     "#if CONFIG_ESPIX_UMODE\n"
     "    lw      t0, RV_STK_MSTATUS(sp)\n"
     "    li      t1, ESPIX_UM_FRAME_USER\n"
     "    and     t0, t0, t1\n"
     "    bnez    t0, 96f\n"
     "    csrci   mstatus, 0x8\n"
     "    mv      a0, sp\n"
     "    call    espix_um_return\n"
     "    li      t0, ESPIX_UM_MCAUSE_TO_USER\n"
     "    csrw    mcause, t0\n"
     "    lw      t0, RV_STK_SP(sp)\n"
     "    csrw    mscratch, t0\n"
     "    restore_xepc\n"
     "    espix_um_exit_to_user\n"
     "96:\n"
     "#endif\n"
     "    restore_xepc\n"),

    ("interrupt entry",
     "_tee_interrupt_handler:\n"
     "    /* Start by saving the general purpose registers and the PC value before\n"
     "     * the interrupt happened. */\n"
     "    save_general_regs\n",
     "_tee_interrupt_handler:\n"
     "    /* Start by saving the general purpose registers and the PC value before\n"
     "     * the interrupt happened. */\n"
     "#if CONFIG_ESPIX_UMODE\n"
     "    espix_um_entry\n"
     "    save_general_regs RV_STK_FRMSZ\n"
     "#else\n"
     "    save_general_regs\n"
     "#endif\n"),

    ("interrupt frame",
     "    addi    a0, sp, CONTEXT_SIZE /* restore sp with the value when interrupt happened */\n"
     "\n    /* Save SP former value */\n    sw      a0, RV_STK_SP(sp)\n",
     "#if CONFIG_ESPIX_UMODE\n"
     "    addi    a0, sp, RV_STK_FRMSZ\n"
     "#else\n"
     "    addi    a0, sp, CONTEXT_SIZE /* restore sp with the value when interrupt happened */\n"
     "#endif\n"
     "\n    /* Save SP former value */\n    sw      a0, RV_STK_SP(sp)\n"
     "#if CONFIG_ESPIX_UMODE\n"
     "    espix_um_entry_fix 1\n"
     "#endif\n"),

    ("interrupt resume",
     "    call    rtos_int_exit\n",
     "    call    rtos_int_exit\n"
     "#if CONFIG_ESPIX_UMODE\n"
     "    /* The task resumed may not be the one interrupted: its privilege is in its own frame. */\n"
     "    lw      t0, RV_STK_MSTATUS(sp)\n"
     "    li      t1, ESPIX_UM_FRAME_USER\n"
     "    or      a0, a0, t1\n"
     "    xor     a0, a0, t0\n"
     "    /* mcause keeps its own MPP on a CLIC, and mret honours it: it must agree with mstatus. */\n"
     "    slli    t2, t0, 17\n"
     "    li      t1, 0x30000000\n"
     "    or      s1, s1, t1\n"
     "    xor     s1, s1, t2\n"
     "    beqz    t0, 95f\n"
     "    ori     a0, a0, 0x80\n"
     "    /* espix_um_return() gave the thread the FPU; keep it on (FS=Initial or above). */\n"
     "    li      t1, 0x2000\n"
     "    or      a0, a0, t1\n"
     "    /* Back to U-mode: MPIE set, interrupt level 0. */\n"
     "    li      t1, ~(0xff << 16)\n"
     "    and     s1, s1, t1\n"
     "    li      t1, ESPIX_UM_MCAUSE_TO_USER\n"
     "    or      s1, s1, t1\n"
     "    mv      s2, a0\n"
     "    mv      a0, sp\n"
     "    call    espix_um_return\n"
     "    mv      a0, s2\n"
     "    lw      t0, RV_STK_SP(sp)\n"
     "    csrw    mscratch, t0\n"
     "    csrw    mcause, s1\n"
     "    csrw    mstatus, a0\n"
     "    mintthresh_csr_restore s5\n"
     "    restore_xepc\n"
     "    espix_um_exit_to_user\n"
     "95:\n"
     "#endif\n"),

    ("interrupt return",
     "    restore_xepc\n    restore_general_regs\n"
     "    /* exit, this will also re-enable the interrupts */\n    xret\n",
     "    restore_xepc\n"
     "#if CONFIG_ESPIX_UMODE\n"
     "    restore_general_regs RV_STK_FRMSZ\n"
     "#else\n"
     "    restore_general_regs\n"
     "#endif\n"
     "    /* exit, this will also re-enable the interrupts */\n    xret\n"),
]

IPC_EDITS = [
    ("include",
     '#include "soc/system_reg.h"\n',
     '#include "sdkconfig.h"\n#include "soc/system_reg.h"\n'),

    ("entry",
     "esp_ipc_isr_handler:\n",
     "esp_ipc_isr_handler:\n"
     "#if CONFIG_ESPIX_UMODE\n"
     "    /* From U-mode: onto the kernel stack with the kernel's gp, tp and table; mscratch is 1 until the exit. */\n"
     "    csrrw   t0, mscratch, t0\n"
     "    bnez    t0, 81f\n"
     "    csrrw   t0, mscratch, t0\n"
     "    j       82f\n"
     "81:\n"
     "    sw      sp, 0(t0)\n"
     "    csrr    sp, mscratch\n"
     "    sw      sp, 4(t0)\n"
     "    mv      sp, t0\n"
     "    addi    sp, sp, -16\n"
     "    sw      gp, 0(sp)\n"
     "    sw      tp, 4(sp)\n"
     "    csrrw   t0, satp, zero\n"
     "    sw      t0, 8(sp)\n"
     "    .option push\n"
     "    .option norelax\n"
     "    la      gp, __global_pointer$\n"
     "    .option pop\n"
     "    csrr    t0, mhartid\n"
     "    slli    t0, t0, 2\n"
     "    la      tp, espix_um_ktp\n"
     "    add     tp, tp, t0\n"
     "    lw      tp, 0(tp)\n"
     "    lw      t0, 20(sp)\n"
     "    csrwi   mscratch, 1\n"
     "82:\n"
     "#endif\n"),

    ("exit choice",
     "    /* Restore a0 - a7, t0 - t6, ra */\n",
     "#if CONFIG_ESPIX_UMODE\n"
     "    csrr    t0, mscratch\n"
     "    bnez    t0, 85f\n"
     "#endif\n"
     "    /* Restore a0 - a7, t0 - t6, ra */\n"),

    ("exit to user",
     "    addi    sp, sp, (CONTEXT_SIZE)\n\n    mret\n",
     "    addi    sp, sp, (CONTEXT_SIZE)\n\n    mret\n"
     "#if CONFIG_ESPIX_UMODE\n"
     "85:\n"
     "    /* The app's table back: the entry set satp bare for the kernel. */\n"
     "    addi    a0, sp, CONTEXT_SIZE\n"
     "    lw      a0, 8(a0)\n"
     "    csrw    satp, a0\n"
     "    lw      a0, 0(sp)\n"
     "    lw      a1, 4(sp)\n"
     "    lw      a2, 8(sp)\n"
     "    lw      a3, 12(sp)\n"
     "    lw      a4, 16(sp)\n"
     "    lw      a5, 20(sp)\n"
     "    lw      a6, 24(sp)\n"
     "    lw      a7, 28(sp)\n"
     "    lw      t0, 32(sp)\n"
     "    lw      t1, 36(sp)\n"
     "    lw      t2, 40(sp)\n"
     "    lw      t3, 44(sp)\n"
     "    lw      t4, 48(sp)\n"
     "    lw      t5, 52(sp)\n"
     "    lw      t6, 56(sp)\n"
     "    lw      ra, 60(sp)\n"
     "    addi    sp, sp, (CONTEXT_SIZE)\n"
     "    lw      gp, 0(sp)\n"
     "    lw      tp, 4(sp)\n"
     "    addi    sp, sp, 16\n"
     "    csrw    mscratch, sp\n"
     "    lw      sp, 0(sp)\n"
     "    mret\n"
     "#endif\n"),
]


def die(msg):
    print(f"patch-umode: {msg}", file=sys.stderr)
    sys.exit(1)


def idf_root(argv):
    if "--idf-path" in argv:
        i = argv.index("--idf-path")
        if i + 1 < len(argv):
            return Path(argv[i + 1])
    env = os.environ.get("IDF_PATH")
    if not env:
        die("no IDF path: pass --idf-path, or set IDF_PATH")
    return Path(env)


def check_version(idf):
    version = idf / "version.txt"
    if version.is_file() and version.read_text(encoding="utf-8").strip() != EXPECTED_IDF:
        die(f"ESP-IDF is not {EXPECTED_IDF}; check the anchors in tools/patch-umode.py first")


def patch(idf, rel, edits):
    target = idf / rel
    if not target.is_file():
        die(f"{target} does not exist; is the IDF path right?")
    text = target.read_text(encoding="utf-8")
    if MARK in text:
        return
    check_version(idf)
    for name, old, new in edits:
        if text.count(old) != 1:
            die(f"{rel}: expected one '{name}' anchor, found {text.count(old)}; re-read the file")
        text = text.replace(old, new)
    target.write_text(text, encoding="utf-8")
    print(f"patch-umode: {rel}: U-mode entry and exit")


def main():
    idf = idf_root(sys.argv)
    patch(idf, VECTORS, VECTORS_EDITS)
    patch(idf, IPC, IPC_EDITS)
    return 0


if __name__ == "__main__":
    sys.exit(main())
