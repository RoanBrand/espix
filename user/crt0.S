/* U-mode app entry: the kernel passes argc, argv and envp in a0-a2 and the stack in sp. */
    .section .text.init.enter, "ax"
    .global _start
    .type   _start, @function
_start:
    .option push
    .option norelax
    la      gp, __global_pointer$
    .option pop
    call    __espix_start
1:  j       1b
    .size   _start, . - _start
