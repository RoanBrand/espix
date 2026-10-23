/*
 * An app's stdin, stdout and stderr: one set per process, as on any POSIX system.
 *
 * picolibc has one `stdout` for the whole image -- a plain global, not per task
 * -- and on a chip with no MMU every process shares that image. So the kernel's
 * stdout stays the console and an app gets its own, the way a Linux process gets
 * its own copy of libc's data: the app's references to `stdin`, `stdout` and
 * `stderr` are resolved, at load, to the three FILE pointers in *its* process
 * slot (espix_proc_slot_t.std). They are ordinary variables there. All of the
 * process's threads see the same three, and an app that assigns `stdout` or
 * freopen()s it changes its own and nobody else's.
 *
 * The calls that use a standard stream without naming it -- printf, puts,
 * putchar, getchar, perror, scanf -- are libc's, and libc's would use its own
 * global. So they are answered here, by the resolver, with the same call on the
 * calling process's stream. Everything that takes a FILE * is libc's own and
 * needs nothing: the app passes it its stream.
 *
 * Which process a symbol is resolved for is espix_proc_self() of the task doing
 * the relocation: the process itself for a dlopen(), and exec.c's relocation
 * helper -- which carries the slot it is relocating for -- at load.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "espix_proc_priv.h"

#if !CONFIG_LIBC_PICOLIBC
#error "espix's process model is built on picolibc, IDF's default libc. Delete sdkconfig.<target> so it is regenerated from sdkconfig.defaults."
#endif

FILE *espix_proc_stdio(int fd)
{
    espix_proc_slot_t *const self = espix_proc_self();

    if (self != NULL && self->std[fd] != NULL) {
        return self->std[fd];
    }
    return (fd == 0) ? stdin : (fd == 1) ? stdout : stderr;
}

uintptr_t espix_abi_stdio_var(const char *name)
{
    static const char *const k_names[3] = { "stdin", "stdout", "stderr" };

    for (int fd = 0; fd < 3; fd++) {
        if (strcmp(name, k_names[fd]) == 0) {
            espix_proc_slot_t *const self = espix_proc_self();
            return (self != NULL) ? (uintptr_t)&self->std[fd] : 0;
        }
    }
    return 0;
}

static int abi_vprintf(const char *fmt, va_list ap)
{
    return vfprintf(espix_proc_stdio(1), fmt, ap);
}

static int abi_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const int n = abi_vprintf(fmt, ap);
    va_end(ap);
    return n;
}

/* Under the stream's lock, so the newline cannot be separated from its line by
 * another thread's output. */
static int abi_puts(const char *s)
{
    FILE *const f = espix_proc_stdio(1);

    flockfile(f);
    const int rc = (fputs(s, f) == EOF || fputc('\n', f) == EOF) ? EOF : 0;
    funlockfile(f);
    return rc;
}

static int abi_putchar(int c)
{
    return fputc(c, espix_proc_stdio(1));
}

static int abi_getchar(void)
{
    return fgetc(espix_proc_stdio(0));
}

static void abi_perror(const char *s)
{
    const int err = errno;
    FILE *const f = espix_proc_stdio(2);

    if (s != NULL && *s != '\0') {
        fprintf(f, "%s: %s\n", s, strerror(err));
    } else {
        fprintf(f, "%s\n", strerror(err));
    }
}

static int abi_vscanf(const char *fmt, va_list ap)
{
    return vfscanf(espix_proc_stdio(0), fmt, ap);
}

static int abi_scanf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const int n = abi_vscanf(fmt, ap);
    va_end(ap);
    return n;
}

static const abi_sym_t s_stdio_syms[] = {
    ABI_SYM("printf",  abi_printf),
    ABI_SYM("vprintf", abi_vprintf),
    ABI_SYM("puts",    abi_puts),
    ABI_SYM("putchar", abi_putchar),
    ABI_SYM("getchar", abi_getchar),
    ABI_SYM("perror",  abi_perror),
    ABI_SYM("scanf",   abi_scanf),
    ABI_SYM("vscanf",  abi_vscanf),
};

void espix_proc_abi_stdio_register(void)
{
    espix_abi_resolver_add(s_stdio_syms,
                           sizeof(s_stdio_syms) / sizeof(s_stdio_syms[0]));
}
