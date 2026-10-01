/*
 * exit(), _Exit(), _exit(), abort() and assert() -- the ways a C program ends
 * itself, and none of them may take the board down.
 *
 * Why this file exists
 * --------------------
 * The firmware's \`exit()\` is newlib's, and on this target it ends at IDF's
 * \`_exit()\`, which is literally \`abort()\` (esp_libc/src/syscalls.c). That panics
 * and reboots. So an app calling the most ordinary function in C reset the
 * device. abi_libc.c also published \`abort\` and \`_Exit\` directly, so those reset
 * it too, and \`assert()\` got there a third way: a failing assert calls the
 * *firmware's* \`__assert_func\`, which calls the firmware's \`abort\` at its own link
 * time -- intercepting the app's \`abort\` symbol cannot change that.
 *
 * So all five names are claimed here.
 *
 * Why a resolver and not a table
 * ------------------------------
 * elf_find_sym_default() searches the loader's own libc table *first*, and it
 * already answers for \`exit\`. A table registered with esp_elf_register_symbol()
 * is consulted after that and could never shadow it. The resolver runs before
 * all of it -- the same reason sleep() and getenv() are here. See abi_resolver.c.
 *
 * Where "end the process" goes
 * ----------------------------
 * espix_proc_exit() longjmps back to proc_task(), which is where a normal return
 * from app_main() lands, so the ordinary teardown runs: streams restored and
 * closed, the ELF released, the slot marked EXITED with the status, the task
 * deleted. It is deliberately not vTaskDelete() from here -- deleting the task
 * at this point is precisely what used to leak the image and lose the status.
 *
 * atexit() is still unpublished, so no handler runs on the way out. That is
 * unchanged, and noted in abi_libc.c.
 */

#include <stdio.h>
#include <stdlib.h>

#include "espix_kernel.h"
#include "espix_proc_priv.h"

#define TAG "abi"

/*
 * What a shell reports for a process killed by SIGABRT: 128 + 6. exec.c already
 * answers 126 for "found it, could not run it" and 127 for "no such file", so
 * this is the one number in that family not already spoken for.
 */
#define ESPIX_ABORT_STATUS 134

static void espix_abi_exit(int status) __attribute__((noreturn));
static void espix_abi_Exit(int status) __attribute__((noreturn));
static void espix_abi_abort(void) __attribute__((noreturn));
static void
espix_abi_assert_func(const char *file, int line, const char *func,
                      const char *failedexpr) __attribute__((noreturn));

/* exit(): the buffered streams are flushed first, as C requires. They are the
 * app's own -- funopen() objects over the session -- so this is what puts the
 * last line of output in front of whoever ran the program. */
static void espix_abi_exit(int status)
{
    fflush(stdout);
    fflush(stderr);
    espix_proc_exit(status);
}

/* _Exit()/_exit(): the same, without the flush. C says _Exit does not flush and
 * runs no handlers, and an app that chose it chose that. */
static void espix_abi_Exit(int status)
{
    espix_proc_exit(status);
}

static void espix_abi_abort(void)
{
    fflush(stdout);
    fflush(stderr);
    espix_proc_exit(ESPIX_ABORT_STATUS);
}

/*
 * What an app's failed assert() calls.
 *
 * The wording is newlib's own, because a test that greps for the message should
 * not care which implementation printed it -- and the implementation this
 * replaces printed exactly this and then reset the machine.
 */
static void
espix_abi_assert_func(const char *file, int line, const char *func,
                      const char *failedexpr)
{
    fprintf(stderr,
            "assertion \"%s\" failed: file \"%s\", function: \"%s\", line %d\n",
            failedexpr != NULL ? failedexpr : "",
            file != NULL ? file : "?", func != NULL ? func : "?", line);
    fflush(stderr);
    espix_proc_exit(ESPIX_ABORT_STATUS);
}

static const abi_sym_t s_exit_syms[] = {
    /* exit() is answered by the loader's own libc table, so naming it here is
     * the whole reason this is a resolver rather than another table. */
    ABI_SYM("exit",          espix_abi_exit),
    ABI_SYM("_Exit",         espix_abi_Exit),
    /* The POSIX spelling. newlib's exit() calls it, and an app may call it too. */
    ABI_SYM("_exit",         espix_abi_Exit),
    ABI_SYM("abort",         espix_abi_abort),
    ABI_SYM("__assert_func", espix_abi_assert_func),
};

void espix_proc_abi_exit_register(void)
{
    espix_abi_resolver_add(s_exit_syms,
                           sizeof(s_exit_syms) / sizeof(s_exit_syms[0]));

    espix_klog(ESPIX_KLOG_DEBUG, TAG, "exit and abort published to apps");
}
