/*
 * The main() an espix sketch does not have to write. See espix_sketch.h.
 */

#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>

#include "espix_sketch.h"

/*
 * espix's cooperative stop, resolved at load time against the table the
 * firmware publishes. Declared here rather than in the header so a sketch never
 * has to see it.
 */
extern "C" bool espix_app_stopping(void);

/*
 * Arduino's real delay(), reached through the linker's --wrap. Defining our own
 * delay() is not an option: it shares a translation unit with micros() and
 * delayMicroseconds(), which Adafruit_NeoPixel uses, so that unit is linked and
 * a second definition would collide. Arduino uses --wrap in the same file for
 * the panic handler, so the mechanism is known to fit this build.
 */
extern "C" void __real_delay(uint32_t ms);

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

/*
 * A sketch need not define teardown(); this is what it gets if it does not.
 * Weak, so a real one silently replaces it.
 */
__attribute__((weak)) void teardown() {}

/*
 * Where espixExit() and a cancelled delay() return to. longjmp() rather than a
 * flag the loop checks, so "does not return" is literally true.
 *
 * setjmp and longjmp are in the ELF loader's own exported symbol table, so none
 * of this needs anything added to espix.
 */
static jmp_buf s_exit;
static int     s_status;
static bool    s_finishing;

/*
 * espix runs a 100Hz tick, so vTaskDelay cannot resolve finer than this and
 * slicing at one tick costs nothing in latency or wakeups.
 */
#define TICK_MS 10

static void finish(int status) __attribute__((noreturn));

static void finish(int status)
{
    /*
     * teardown() may itself call delay() -- fading an LED out over half a
     * second is exactly the kind of thing people write. Setting the flag
     * *before* calling it sends that delay down the plain path below, so
     * teardown can neither re-enter here nor run a second time.
     */
    if (!s_finishing) {
        s_finishing = true;
        teardown();
    }

    s_status = status;
    longjmp(s_exit, 1);

    abort();    /* unreachable; keeps the noreturn promise */
}

bool espixStopping()
{
    return espix_app_stopping();
}

void espixExit(int status)
{
    finish(status);
}

/*
 * delay(), made a cancellation point. Sleeps a tick at a time and checks
 * between slices, because espix's stop is a polled flag with no notification
 * behind it -- there is nothing to wait on, so this is the honest shape.
 */
extern "C" void __wrap_delay(uint32_t ms)
{
    if (s_finishing) {
        __real_delay(ms);       /* already tearing down: an ordinary delay */
        return;
    }

    while (ms >= TICK_MS) {
        __real_delay(TICK_MS);
        ms -= TICK_MS;

        if (espix_app_stopping()) {
            finish(0);          /* does not return */
        }
    }

    if (ms > 0) {
        __real_delay(ms);
    }
}

/*
 * The global constructors, bounded by ctors.ld. Nothing runs these for us: the
 * ELF loader jumps straight to the entry point, and -nostartfiles means there
 * is no crt code to do it either.
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

/*
 * extern "C" is not optional. The app is linked with `-e app_main` and compiled
 * with `-Dmain=app_main`, so in C++ this entry point would otherwise be
 * name-mangled: the linker reports "cannot find entry symbol app_main" and
 * produces an ELF with no sections at all.
 *
 * argc and argv are accepted because espix passes them, and ignored because an
 * Arduino sketch has no notion of them.
 */
extern "C" int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (setjmp(s_exit) != 0) {
        return s_status;        /* arrived here from finish() */
    }

    run_global_constructors();
    setup();

    for (;;) {
        loop();

        /*
         * A loop() that never calls delay() would otherwise be uninterruptible,
         * and would starve every other task besides -- a millis()-driven loop
         * is a common Arduino idiom. One flag read per iteration buys the
         * guarantee.
         */
        if (espix_app_stopping()) {
            finish(0);
        }
    }
}
