/* vfork: no stack of its own, which the child, returning on its parent's stack, would overwrite. */
    .section .text.vfork, "ax"
    .global vfork
    .type   vfork, @function
vfork:
    li      a7, 2093            /* ESPIX_SYS_VFORK; spawn.c checks it */
    ecall
    bltz    a0, 1f
    ret
1:  tail    __espix_errno_ret
    .size   vfork, . - vfork
