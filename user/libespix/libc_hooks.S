/*
 * The calls /lib/libc.so's shared code makes into the app: one fixed slot each, at
 * the app's first text address (user/uapp.ld), so one image serves every app.
 * tools/build-libc.sh links libc.so against these addresses. Appending a slot is
 * compatible; moving one is not.
 */
    .section .text.espix_hooks, "ax", @progbits
    .option push
    .option norvc
    .option norelax

    .globl __espix_hooks
__espix_hooks:

    .macro hook name
    .globl __espix_hook_\name
__espix_hook_\name:
    tail \name
    .endm

    hook __errno
    hook __retarget_lock_init
    hook __retarget_lock_init_recursive
    hook __retarget_lock_close
    hook __retarget_lock_close_recursive
    hook __retarget_lock_acquire
    hook __retarget_lock_acquire_recursive
    hook __retarget_lock_release
    hook __retarget_lock_release_recursive

    /* Not a call: the address libc.so passes for its libc-wide lock (pthread.c maps it to the app's). */
    .globl __espix_hook___lock___libc_recursive_mutex
__espix_hook___lock___libc_recursive_mutex:
    .word 0

    .option pop
