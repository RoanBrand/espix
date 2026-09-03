/*
 * The main() an espix sketch does not have to write. See espix_sketch.h.
 */

#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include "espix_sketch.h"

/*
 * espix's delivery point, resolved at load time against the table the firmware
 * publishes. Declared here rather than in the header so a sketch never has to
 * see it.
 *
 * Everything else this shim needs is ordinary POSIX -- signal() and usleep() --
 * which espix publishes under their real names. This one has no POSIX
 * equivalent, because POSIX delivers signals asynchronously and espix delivers
 * them when the process calls in. A loop() that blocks on nothing has to say
 * when that is.
 */
extern "C" bool espix_sigcheck(void);

/*
 * Arduino's real delay(), reached through the linker's --wrap. Defining our own
 * delay() is not an option: it shares a translation unit with micros() and
 * delayMicroseconds(), which Adafruit_NeoPixel uses, so that unit is linked and
 * a second definition would collide. Arduino uses --wrap in the same file for
 * the panic handler, so the mechanism is known to fit this build.
 *
 * Only used once teardown() is running; see __wrap_delay().
 */
extern "C" void __real_delay(uint32_t ms);

/*
 * Set by the handler, read by the loop. sig_atomic_t because that is what a
 * signal handler is allowed to touch -- and here it is not merely a formality:
 * the handler really does run in the middle of whatever the sketch was doing.
 */
static volatile sig_atomic_t s_stop;

/*
 * Every signal whose default action would end the process, caught so the sketch
 * ends *tidily* instead: teardown() runs, the LED goes off, and main() returns
 * a status. Without a handler the default action still stops the app, but at
 * whatever point it next calls in rather than through finish().
 *
 * SIGKILL is deliberately absent. It cannot be caught, which is the point of
 * it: `kill -9` on a sketch that has wedged itself must not be something the
 * sketch can decline.
 */
static void on_stop(int sig)
{
    (void)sig;
    s_stop = 1;
}

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
    /*
     * The flag, not a fresh delivery point: by the time a sketch asks, the
     * handler has already run at whatever call brought the signal in. Asking
     * again would let a SIGSTOP park the sketch inside a query, which reads
     * badly for a function that just answers a question.
     */
    return s_stop != 0;
}

void espixExit(int status)
{
    finish(status);
}

/*
 * delay(), made a cancellation point.
 *
 * usleep() rather than Arduino's delay(): espix publishes an interruptible one,
 * so a signal arriving mid-delay both wakes the sleep and runs the handler on
 * the way out. Arduino's delay() is neither -- it would sleep through the
 * signal, and on a long delay the process would still be asleep when the grace
 * period ran out and be deleted with teardown() never run.
 *
 * Still sliced, for the case where the signal was *not* a stop: a handler that
 * only counts something returns, and the loop carries on to the next slice
 * rather than letting delay() return early. One slice is one 100Hz tick, so
 * this costs nothing that a plain delay would not.
 */
extern "C" void __wrap_delay(uint32_t ms)
{
    if (s_finishing) {
        __real_delay(ms);       /* already tearing down: an ordinary delay */
        return;
    }

    while (ms >= TICK_MS) {
        (void)usleep(TICK_MS * 1000u);
        ms -= TICK_MS;

        if (s_stop) {
            finish(0);          /* does not return */
        }
    }

    if (ms > 0) {
        (void)usleep(ms * 1000u);

        if (s_stop) {
            finish(0);
        }
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

    /*
     * Installed before setup(), because setup() is where a sketch lights an LED
     * or claims a peripheral, and a stop arriving during it should still reach
     * teardown().
     */
    signal(SIGTERM, on_stop);       /* `kill` */
    signal(SIGINT,  on_stop);       /* Ctrl-C */
    signal(SIGHUP,  on_stop);       /* the session that started it went away */

    run_global_constructors();
    setup();

    for (;;) {
        loop();

        /*
         * A loop() that never calls delay() would otherwise be uninterruptible,
         * and would starve every other task besides -- a millis()-driven loop
         * is a common Arduino idiom. This is the delivery point for that case:
         * nothing else here calls into espix, so without it the handler above
         * would never get the chance to run.
         */
        if (espix_sigcheck() || s_stop) {
            finish(0);
        }
    }
}
