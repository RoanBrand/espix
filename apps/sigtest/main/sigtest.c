/*
 * espix example app: signals.
 *
 * This is NOT part of the espix firmware. It is cross-compiled on the host into
 * a standalone ELF, copied onto the device's filesystem, and executed at
 * runtime by `run /bin/sigtest`.
 *
 * Almost all of it is ordinary POSIX, which is the point: signal(), getpid()
 * and sleep() mean here what they mean anywhere, and espix resolves them at
 * load time against what the firmware publishes. A handler runs in this task,
 * synchronously, at the point the app next calls into espix -- and then
 * returns, and execution carries on at the next line. Sending SIGUSR1 during
 * the sleep below demonstrates exactly that: the count goes up and the sleep
 * resumes.
 *
 * What is not POSIX is espix_sigcheck(), used only by the `spin` mode. POSIX
 * delivers signals asynchronously and espix delivers them when a process calls
 * in, so a loop that blocks on nothing has to say when that is.
 *
 * Modes:
 *   sigtest            handle SIGTERM/SIGINT/SIGHUP and SIGUSR1, sleeping
 *   sigtest ignore     ignore them, to exercise `kill -9` and the Ctrl-C escalation
 *   sigtest spin       a compute loop with no blocking call at all
 */

#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Set by the handler, read by the loop. sig_atomic_t because that is what a
 * handler may touch, and only flags are set here: the printing happens back in
 * the loop, which is the idiom worth copying. */
static volatile sig_atomic_t s_stop;
static volatile sig_atomic_t s_usr1;
static volatile sig_atomic_t s_last;

/* espix's delivery point. Only the `spin` mode needs it. */
extern bool espix_sigcheck(void);

static void on_signal(int sig)
{
    s_last = sig;

    if (sig == SIGUSR1) {
        s_usr1++;               /* handled and survivable: keep going */
        return;
    }
    s_stop = 1;
}

static const char *signame(int sig)
{
    switch (sig) {
    case SIGTERM: return "SIGTERM";
    case SIGINT:  return "SIGINT";
    case SIGHUP:  return "SIGHUP";
    case SIGUSR1: return "SIGUSR1";
    case SIGUSR2: return "SIGUSR2";
    default:      return "a signal";
    }
}

static void cleanup(void)
{
    /* Where an app would put its hardware back. Reaching this line at all is
     * the thing being tested: a hard kill never does. */
    printf("sigtest: cleaning up\n");
    fflush(stdout);
}

static int run_spin(void)
{
    printf("sigtest: pid %d, spinning with no blocking call\n", (int)getpid());
    fflush(stdout);

    unsigned long long n = 0;

    for (;;) {
        /* Some work with no call into espix in it. */
        for (int i = 0; i < 200000; i++) {
            n += (unsigned long long)i;
        }

        /* Without this the loop above would be uninterruptible: nothing else
         * here gives espix a chance to run a handler. */
        if (espix_sigcheck() || s_stop) {
            printf("sigtest: stopping after %llu\n", n);
            cleanup();
            return 0;
        }
    }
}

static int run_ignore(void)
{
    signal(SIGTERM, SIG_IGN);
    signal(SIGINT,  SIG_IGN);
    signal(SIGHUP,  SIG_IGN);

    printf("sigtest: pid %d, ignoring SIGTERM/SIGINT/SIGHUP\n", (int)getpid());
    printf("sigtest: `kill -9 %d` or three Ctrl-Cs to end this\n", (int)getpid());
    fflush(stdout);

    /*
     * Nothing here ends on its own -- that is the point of this mode. SIGKILL,
     * or the shell's third Ctrl-C, is the only way out. The return below is
     * unreachable and exists because -Werror=return-type asks for it.
     */
    for (;;) {
        (void)sleep(60);
    }

    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "spin") == 0) {
        return run_spin();
    }
    if (argc > 1 && strcmp(argv[1], "ignore") == 0) {
        return run_ignore();
    }

    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);
    signal(SIGHUP,  on_signal);
    signal(SIGUSR1, on_signal);

    printf("sigtest: pid %d, handlers installed\n", (int)getpid());
    printf("sigtest: sleeping; try `kill -USR1 %d` then `kill %d`\n",
           (int)getpid(), (int)getpid());
    fflush(stdout);

    for (;;) {
        /*
         * A long sleep on purpose. Nothing should ever wait 60 seconds here:
         * espix wakes a sleeping process when it signals it, so `left` below
         * reports how much of the sleep was cut short.
         */
        const unsigned left = sleep(60);

        if (s_stop) {
            printf("sigtest: %s, %u seconds of sleep left\n",
                   signame((int)s_last), left);
            cleanup();
            return 0;
        }

        if (s_usr1) {
            /* The handler already ran, returned, and the sleep returned after
             * it. Carrying on from here is the whole demonstration. */
            printf("sigtest: SIGUSR1 x%d, %u seconds left; carrying on\n",
                   (int)s_usr1, left);
            fflush(stdout);
            s_usr1 = 0;
            continue;
        }

        printf("sigtest: slept the full 60s, nothing arrived\n");
        fflush(stdout);
    }
}
