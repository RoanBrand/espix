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
extern "C" int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (setjmp(s_exit) != 0) {
        return s_status;        /* arrived here from espixExit() */
    }

    setup();

    for (;;) {
        loop();
    }
}
