/* Signals: actions, masks, the timer, and a handler's way back (docs/USER-MODE.md). */
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/time.h>
#include <unistd.h>

#include "sys.h"

_Static_assert(sizeof(sigset_t) == sizeof(uint32_t), "a mask is one word");
_Static_assert(sizeof(siginfo_t) == sizeof(struct espix_siginfo) &&
               offsetof(siginfo_t, si_addr) == offsetof(struct espix_siginfo, addr),
               "siginfo_t is the kernel's");
_Static_assert(ESPIX_SYS_SIGRETURN == 47, "the restorer's number");

/* Where a handler returns, with sp still at the frame the kernel pushed. */
__attribute__((naked)) static void restorer(void)
{
    __asm__ volatile ("li a7, 47\n\tecall");
}

int sigaction(int sig, const struct sigaction *act, struct sigaction *old)
{
    struct espix_sigaction k, o;
    if (act != NULL) {
        k = (struct espix_sigaction){ .handler  = (uint32_t)(uintptr_t)act->sa_handler,
                                      .flags    = (uint32_t)act->sa_flags,
                                      .mask     = (uint32_t)act->sa_mask,
                                      .restorer = (uint32_t)(uintptr_t)restorer };
    }
    const long r = espix_sys3(ESPIX_SYS_SIGACTION, sig, act ? &k : NULL, old ? &o : NULL);
    if (r < 0) {
        return (int)espix_ret(r);
    }
    if (old != NULL) {
        old->sa_handler = (void (*)(int))(uintptr_t)o.handler;
        old->sa_mask    = (sigset_t)o.mask;
        old->sa_flags   = (int)o.flags;
    }
    return 0;
}

/* BSD's and glibc's signal(): the handler stays, and an interrupted call restarts. */
void (*signal(int sig, void (*handler)(int)))(int)
{
    struct sigaction act = { .sa_handler = handler, .sa_mask = 0, .sa_flags = SA_RESTART }, old;
    return (sigaction(sig, &act, &old) == 0) ? old.sa_handler : SIG_ERR;
}

int raise(int sig)
{
    return (int)espix_ret(espix_sys1(ESPIX_SYS_RAISE, sig));
}

int killpg(pid_t pgrp, int sig)
{
    return kill(-pgrp, sig);
}

int sigprocmask(int how, const sigset_t *set, sigset_t *old)
{
    return (int)espix_ret(espix_sys3(ESPIX_SYS_SIGPROCMASK, how, set, old));
}

int pthread_sigmask(int how, const sigset_t *set, sigset_t *old)
{
    const long r = espix_sys3(ESPIX_SYS_SIGPROCMASK, how, set, old);
    return (r < 0) ? (int)-r : 0;
}

int sigpending(sigset_t *set)
{
    return (int)espix_ret(espix_sys1(ESPIX_SYS_SIGPENDING, set));
}

int sigsuspend(const sigset_t *mask)
{
    return (int)espix_ret(espix_sys1(ESPIX_SYS_SIGSUSPEND, mask));
}

int pause(void)
{
    sigset_t now;
    sigprocmask(SIG_BLOCK, NULL, &now);
    return sigsuspend(&now);
}

static struct espix_timespec ts_of(const struct timeval *tv)
{
    return (struct espix_timespec){ .sec = tv->tv_sec, .nsec = (int32_t)tv->tv_usec * 1000 };
}

static struct timeval tv_of(const struct espix_timespec *ts)
{
    return (struct timeval){ .tv_sec = (time_t)ts->sec, .tv_usec = ts->nsec / 1000 };
}

int setitimer(int which, const struct itimerval *nv, struct itimerval *ov)
{
    struct espix_itimerval k, o;
    if (nv != NULL) {
        k.interval = ts_of(&nv->it_interval);
        k.value    = ts_of(&nv->it_value);
    }
    const long r = espix_sys3(ESPIX_SYS_SETITIMER, which, nv ? &k : NULL, ov ? &o : NULL);
    if (r < 0) {
        return (int)espix_ret(r);
    }
    if (ov != NULL) {
        ov->it_interval = tv_of(&o.interval);
        ov->it_value    = tv_of(&o.value);
    }
    return 0;
}

int getitimer(int which, struct itimerval *v)
{
    return setitimer(which, NULL, v);
}

unsigned alarm(unsigned secs)
{
    struct itimerval nv = { .it_value = { .tv_sec = secs } }, ov;
    if (setitimer(ITIMER_REAL, &nv, &ov) != 0) {
        return 0;
    }
    return (unsigned)ov.it_value.tv_sec + (ov.it_value.tv_usec != 0);
}

/* espix's own calls an M-mode app makes. A U-mode app's handlers run as signals arrive, so none waits here. */
int espix_sigcheck(void)
{
    return 0;
}

int espix_term_size(int *cols, int *rows)
{
    return (int)espix_ret(espix_sys2(ESPIX_SYS_TERM_SIZE, cols, rows));
}
