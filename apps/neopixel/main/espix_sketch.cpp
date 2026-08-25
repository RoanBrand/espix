/*
 * The main() an espix sketch does not have to write. See espix_sketch.h.
 */

#include <setjmp.h>
#include <stdlib.h>

#include "espix_sketch.h"

/*
 * espix's cooperative stop, resolved at load time against the table the
 * firmware publishes. Declared here rather than in the header so a sketch never
 * has to see it.
 */
extern "C" bool espix_app_stopping(void);

/*
 * Where espixExit() returns to. longjmp() rather than a flag the loop checks,
 * so "does not return" is literally true: a sketch that calls espixExit()
 * halfway through loop() stops there, instead of running the rest of the
 * iteration and finding out later.
 *
 * setjmp and longjmp are in the ELF loader's own exported symbol table, so this
 * needs nothing added to espix.
 */
/*
 * Normally supplied by crtbegin.o, which -nostdlib never links. A file-scope
 * object with a destructor registers it as
 * __cxa_atexit(dtor, obj, &__dso_handle), so without this the link fails with
 * "undefined reference to `__dso_handle'" and, less helpfully, "final link
 * failed: bad value".
 *
 * Its value is never used: espix tears an app's image down wholesale, so the
 * __cxa_atexit it publishes accepts the registration and does nothing. What
 * matters is that the address exists.
 */
extern "C" void *__dso_handle;   /* declare with C linkage... */
void *__dso_handle = nullptr;    /* ...then define it */

static jmp_buf s_exit;
static int     s_status;

bool espixStopping()
{
    return espix_app_stopping();
}

void espixExit(int status)
{
    s_status = status;
    longjmp(s_exit, 1);

    /* longjmp() does not return, but the compiler wants the noreturn promise
     * kept even so. */
    abort();
}

/*
 * extern "C" is not optional. The app is linked with `-e app_main` and compiled
 * with `-Dmain=app_main`, so in C++ this entry point would otherwise be
 * name-mangled: the linker reports "cannot find entry symbol app_main" and
 * produces an ELF with no sections at all.
 *
 * argc and argv are accepted because espix passes them, and ignored because an
 * Arduino sketch has no notion of them. A sketch that wants arguments is better
 * off written as an ordinary espix app with its own main().
 */
/*
 * The global constructors, bounded by ctors.ld. Nothing runs these for us: the
 * ELF loader jumps straight to the entry point, and -nostartfiles means there is
 * no crt code to do it either.
 *
 * Walked back to front, which is the .ctors convention -- the linker emits the
 * list in reverse priority order, and crtbegin walks it the same way.
 */
extern void (*__espix_ctors_start[])();
extern void (*__espix_ctors_end[])();

static void run_global_constructors()
{
    void (**p)() = __espix_ctors_end;

    while (p-- != __espix_ctors_start) {
        if (*p != nullptr && *p != (void (*)())-1) {
            (*p)();
        }
    }
}

extern "C" int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (setjmp(s_exit) != 0) {
        return s_status;        /* arrived here from espixExit() */
    }

    run_global_constructors();

    setup();

    for (;;) {
        loop();
    }
}
