/*
 * Signals for U-mode apps (docs/USER-MODE.md): each process's actions, each
 * thread's mask, and handlers run in U on a frame pushed onto the thread's own
 * stack. A return to U with something due is diverted into a trap
 * (espix_um_return_to()), so all of this runs as the thread's task code, where
 * it may block and touch user memory; never in an interrupt.
 */
#include "sdkconfig.h"

#if CONFIG_ESPIX_UMODE

#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "espix_kernel.h"
#include "espix_umode.h"

#define TAG "umode"

/* An action's handler word: SIG_DFL and SIG_IGN, or an address in the app. */
#define H_DFL 0u
#define H_IGN 1u

_Static_assert(offsetof(RvExcFrame, mepc) == 0 && offsetof(RvExcFrame, t6) == 31 * sizeof(uint32_t),
               "a frame's first 32 words are pc and x1-x31, as a signal frame's regs are");
_Static_assert(sizeof(struct espix_sigframe) % 4 == 0, "a signal frame is whole words");

/* The calling task's thread of slot's process, if it is one that still takes signals. */
static espix_um_thread_t *self_of(const espix_proc_slot_t *slot)
{
    espix_um_thread_t *const t = pvTaskGetThreadLocalStoragePointer(NULL, ESPIX_TLS_UM_IDX);
    return (t != NULL && t->slot == slot && !t->gone) ? t : NULL;
}

static bool does_nothing(uint32_t handler, int sig)
{
    return handler == H_IGN || (handler == H_DFL && espix_sig_default_ignores(sig));
}

/* Signals whose action is nothing, and those whose action is the end of the process. Read unlocked: words. */
static uint32_t ignored(const espix_um_proc_t *p)
{
    uint32_t m = 0;
    for (int sig = 1; sig < NSIG; sig++) {
        if (does_nothing(p->act[sig].handler, sig)) {
            m |= espix_sigbit(sig);
        }
    }
    return m;
}

static uint32_t terminating(const espix_um_proc_t *p)
{
    uint32_t m = 0;
    for (int sig = 1; sig < NSIG; sig++) {
        if (p->act[sig].handler == H_DFL && !espix_sig_default_ignores(sig) && !espix_sig_default_stops(sig)) {
            m |= espix_sigbit(sig);
        }
    }
    return m;
}

static int lowest(uint32_t m)
{
    return (m != 0) ? __builtin_ctz(m) : 0;
}

/*
 * A process's signal goes to one thread: the first that returns to U with it
 * unblocked, or a blocked call's, which claims it here so that only its wait
 * ends with EINTR. Signals that do nothing are left for the return to U.
 */
uint32_t espix_um_sig_claim(espix_proc_slot_t *slot)
{
    espix_um_thread_t *const t = self_of(slot);
    if (t == NULL) {
        return 0;
    }
    const uint32_t want = ~t->sig_blocked & ~ignored(t->proc);
    uint32_t got = t->sig_pending & want;
    if (got == 0 && (slot->sig_pending & want) != 0) {
        xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);
        got = espix_sigbit(lowest(slot->sig_pending & want));
        slot->sig_pending &= ~got;
        xSemaphoreGive(g_espix_proc_lock);
        t->sig_pending |= got;
    }
    return got;
}

int espix_um_termsig(const espix_proc_slot_t *slot)
{
    const espix_um_thread_t *const t = self_of(slot);
    const espix_um_proc_t *const p = ((const espix_um_thread_t *)slot->app->um)->proc;
    const uint32_t pending = slot->sig_pending | ((t != NULL) ? t->sig_pending : 0);
    const uint32_t blocked = (t != NULL) ? t->sig_blocked : 0;
    return lowest(pending & ~blocked & terminating(p));
}

/* The thread's own first, then the process's. */
static int take(espix_um_thread_t *t)
{
    const uint32_t mine = t->sig_pending & ~t->sig_blocked;
    if (mine != 0) {
        const int sig = lowest(mine);
        t->sig_pending &= ~espix_sigbit(sig);
        return sig;
    }
    espix_proc_slot_t *const slot = t->slot;
    if ((slot->sig_pending & ~t->sig_blocked) == 0) {
        return 0;
    }
    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);
    const int sig = lowest(slot->sig_pending & ~t->sig_blocked);
    slot->sig_pending &= ~espix_sigbit(sig);
    xSemaphoreGive(g_espix_proc_lock);
    return sig;
}

/* sig's action, reset to SIG_DFL first if it asked to be. */
static struct espix_sigaction action_of(espix_um_proc_t *p, int sig)
{
    xSemaphoreTake(p->lock, portMAX_DELAY);
    const struct espix_sigaction act = p->act[sig];
    if (act.handler > H_IGN && (act.flags & SA_RESETHAND)) {
        p->act[sig].handler = H_DFL;
    }
    xSemaphoreGive(p->lock);
    return act;
}

/* The frame onto the thread's stack and u into the handler; false when the stack cannot take it. */
static bool push(espix_um_thread_t *t, RvExcFrame *u, int sig, const struct espix_sigaction *act,
                 const struct espix_siginfo *info)
{
    struct espix_sigframe f;
    f.info = *info;
    f.mask = t->suspended ? t->sig_saved : t->sig_blocked;
    memcpy(f.regs, u, sizeof(f.regs));
    espix_um_fpu_save(f.fregs);
    const uint32_t sp = (u->sp - sizeof(f)) & ~15u;
    if (espix_um_copyout(t->proc->as, sp, &f, sizeof(f)) != 0) {
        return false;
    }
    t->suspended   = false;
    t->sig_blocked = (t->sig_blocked | act->mask | ((act->flags & SA_NODEFER) ? 0 : espix_sigbit(sig))) &
                     ~ESPIX_SIG_UNCATCHABLE;
    u->sp   = sp;
    u->a0   = (uint32_t)sig;
    u->a1   = sp + offsetof(struct espix_sigframe, info);
    u->a2   = sp;
    u->ra   = act->restorer;
    u->mepc = act->handler;
    return true;
}

static void __attribute__((noreturn)) bad_frame(espix_um_thread_t *t, uint32_t sp)
{
    espix_klog(ESPIX_KLOG_WARN, TAG, "pid %d: no signal frame at sp 0x%08x", (int)t->slot->info.pid,
               (unsigned)sp);
    espix_um_exit_group(t, 128 + SIGSEGV);
}

/* What SA_RESTART restarts: not the sleeps and waits whose time would start again, as on Linux. */
static bool restartable(uint32_t nr)
{
    return nr != ESPIX_SYS_NANOSLEEP && nr != ESPIX_SYS_SIGSUSPEND && nr != ESPIX_SYS_FUTEX &&
           nr != ESPIX_SYS_POLL;
}

void espix_um_sig_deliver(espix_um_thread_t *t, RvExcFrame *u)
{
    espix_proc_slot_t *const slot = t->slot;
    espix_um_proc_t *const p = t->proc;
    const bool intr = t->sys_intr;
    t->sys_intr = false;
    for (;;) {
        if (slot->stop_requested || p->exiting) {
            espix_um_exit_group(t, 128 + SIGKILL);
        }
        if (slot->sig_stop_req) {
            (void)espix_sigcheck_mask();    /* parks until the stop is lifted */
            continue;
        }
        const int sig = take(t);
        if (sig == 0) {
            break;
        }
        const struct espix_sigaction act = action_of(p, sig);
        if (does_nothing(act.handler, sig)) {
            continue;
        }
        if (act.handler == H_DFL && espix_sig_default_stops(sig)) {
            (void)espix_proc_signal(slot->info.pid, SIGSTOP);
            continue;
        }
        if (act.handler == H_DFL) {
            espix_um_exit_group(t, 128 + sig);
        }
        if (intr && (act.flags & SA_RESTART) && restartable(t->sys_nr)) {
            u->mepc -= 4;
            u->a0 = t->sys_a0;
        }
        const struct espix_siginfo info = { .signo = sig, .code = SI_USER };
        if (!push(t, u, sig, &act, &info)) {
            bad_frame(t, u->sp);
        }
        return;
    }
    if (t->suspended) {
        t->suspended   = false;
        t->sig_blocked = t->sig_saved;
    }
}

void espix_um_sig_return(espix_um_thread_t *t, RvExcFrame *u)
{
    struct espix_sigframe f;
    if (espix_um_copyin(t->proc->as, &f, u->sp, sizeof(f)) != 0) {
        bad_frame(t, u->sp);
    }
    memcpy(u, f.regs, sizeof(f.regs));
    espix_um_fpu_restore(f.fregs);
    t->sig_blocked = f.mask & ~ESPIX_SIG_UNCATCHABLE;
}

bool espix_um_sig_fault(espix_um_thread_t *t, RvExcFrame *u, int sig)
{
    if (t->sig_blocked & espix_sigbit(sig)) {
        return false;
    }
    const struct espix_sigaction act = action_of(t->proc, sig);
    if (act.handler <= H_IGN) {
        return false;
    }
    const struct espix_siginfo info = { .signo = sig, .addr = u->mtval };
    return push(t, u, sig, &act, &info);
}

/* ------------------------------------------------------------------ */
/* Syscalls                                                            */
/* ------------------------------------------------------------------ */

int32_t espix_um_sigaction(espix_um_thread_t *t, int sig, uint32_t uact, uint32_t uold)
{
    espix_um_proc_t *const p = t->proc;
    const uint32_t bit = espix_sigbit(sig);
    if (bit == 0 || (uact != 0 && (bit & ESPIX_SIG_UNCATCHABLE))) {
        return -EINVAL;
    }
    struct espix_sigaction act;
    if (uact != 0) {
        const int r = espix_um_copyin(p->as, &act, uact, sizeof(act));
        if (r != 0) {
            return r;
        }
        act.mask &= ~ESPIX_SIG_UNCATCHABLE;
    }
    xSemaphoreTake(p->lock, portMAX_DELAY);
    const struct espix_sigaction old = p->act[sig];
    if (uact != 0) {
        p->act[sig] = act;
    }
    xSemaphoreGive(p->lock);
    /* A signal set to do nothing is discarded if pending, as POSIX asks. */
    if (uact != 0 && does_nothing(act.handler, sig)) {
        xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);
        t->slot->sig_pending &= ~bit;
        xSemaphoreGive(g_espix_proc_lock);
        t->sig_pending &= ~bit;
    }
    return (uold != 0) ? espix_um_copyout(p->as, uold, &old, sizeof(old)) : 0;
}

int32_t espix_um_sigprocmask(espix_um_thread_t *t, int how, uint32_t uset, uint32_t uold)
{
    const uint32_t old = t->sig_blocked;
    if (uset != 0) {
        uint32_t set;
        const int r = espix_um_copyin(t->proc->as, &set, uset, sizeof(set));
        if (r != 0) {
            return r;
        }
        set &= ~ESPIX_SIG_UNCATCHABLE;
        switch (how) {
        case SIG_BLOCK:   t->sig_blocked = old | set;  break;
        case SIG_UNBLOCK: t->sig_blocked = old & ~set; break;
        case SIG_SETMASK: t->sig_blocked = set;        break;
        default:          return -EINVAL;
        }
    }
    return (uold != 0) ? espix_um_copyout(t->proc->as, uold, &old, sizeof(old)) : 0;
}

int32_t espix_um_sigpending(espix_um_thread_t *t, uint32_t uout)
{
    const uint32_t m = (t->sig_pending | t->slot->sig_pending) & t->sig_blocked;
    return espix_um_copyout(t->proc->as, uout, &m, sizeof(m));
}

/* The mask comes back once a handler has run under it (push()), or on the return to U without one. */
int32_t espix_um_sigsuspend(espix_um_thread_t *t, uint32_t umask)
{
    uint32_t mask;
    const int r = espix_um_copyin(t->proc->as, &mask, umask, sizeof(mask));
    if (r != 0) {
        return r;
    }
    t->sig_saved   = t->sig_blocked;
    t->suspended   = true;
    t->sig_blocked = mask & ~ESPIX_SIG_UNCATCHABLE;
    while (espix_sigcheck_mask() == 0) {
        (void)ulTaskNotifyTakeIndexed(ESPIX_NOTIFY_SIG_IDX, pdTRUE, portMAX_DELAY);
    }
    return -EINTR;
}

int32_t espix_um_raise(espix_um_thread_t *t, int sig)
{
    if (espix_sigbit(sig) == 0) {
        return -EINVAL;
    }
    if (sig == SIGKILL) {
        espix_um_exit_group(t, 128 + SIGKILL);
    }
    if (sig == SIGSTOP) {
        return (espix_proc_signal(t->slot->info.pid, SIGSTOP) == ESP_OK) ? 0 : -ESRCH;
    }
    t->sig_pending |= espix_sigbit(sig);
    return 0;
}

#endif /* CONFIG_ESPIX_UMODE */
