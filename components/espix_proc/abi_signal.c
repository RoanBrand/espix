/*
 * POSIX signals for loadable apps.
 *
 * Two jobs. The first is the signal API itself -- signal(), sigaction(),
 * kill(), raise(), getpid(), pause(), sigprocmask(), sigpending(). None of that
 * exists on this platform: ESP-IDF ships the signal *vocabulary* and no
 * machinery at all. <signal.h> declares the lot, the toolchain defines almost
 * none of it (libc's signal.o holds a single unused variable), kill() resolves
 * to a stub returning ENOSYS, and raise() resolves to one that calls abort().
 * IDF's pthread has no pthread_kill, and its pthread_sigmask is a no-op that
 * returns success while doing nothing. FreeRTOS-Plus-POSIX, vendored as
 * components/rt, is the message-queue slice only; upstream never implemented
 * signals either. So this is written from scratch, and because that namespace
 * is unclaimed it can be written under the real names rather than as an
 * espix_signal_* dialect: an app registers a handler the ordinary Unix way.
 *
 * The second job is the delivery points. A handler here runs in the app's own
 * task, synchronously, at a point where the app has called into espix -- and
 * then returns, and execution carries on where it was. That last part is what
 * makes it feel like Unix. What it is not is asynchronous: a real kernel
 * interrupts the thread at an arbitrary instruction and builds a signal frame
 * on its stack, and doing that here would mean rewriting a FreeRTOS task's
 * saved program counter on windowed-register Xtensa.
 *
 * The honest limit of that trade: a pure compute loop which never calls into
 * espix has no delivery point and never sees a signal. espix_sigcheck() is
 * exported for exactly that app, and SIGKILL is the answer when it is somebody
 * else's binary.
 *
 * Why a resolver and not just another symbol table: elf_find_sym_default()
 * searches the loader's own libc table first, and that table already answers
 * for sleep() and usleep(). A table registered with esp_elf_register_symbol()
 * is consulted after it, so it cannot shadow them. elf_set_symbol_resolver() is
 * the loader's documented hook for "symbol interception and hooking", which is
 * precisely this, and it costs no fork of the component.
 *
 * Note that every function here is prefixed and mapped to its POSIX name by
 * that resolver, rather than being *named* signal() and kill(). The names
 * espix could safely define are not the same set as the names apps need:
 * sleep() and usleep() are real functions in the firmware already, and getpid()
 * and raise() are force-linked by esp_libc with -u, so defining any of those
 * would be a duplicate symbol. Prefixing all of them keeps one rule instead of
 * four exceptions.
 */

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_elf.h"
#include "private/elf_symbol.h"

#include "espix_kernel.h"
#include "espix_proc_priv.h"

#define TAG "abi"

/* Below this, a sleep is shorter than the interruption is worth: one tick is
 * 10ms at CONFIG_FREERTOS_HZ=100, and IDF's usleep busy-waits under a tick
 * rather than yielding, which is behaviour worth keeping rather than
 * reimplementing. */
#define SHORT_SLEEP_US 20000

static TickType_t ticks_from_us(uint64_t us)
{
    const uint64_t t = (us * configTICK_RATE_HZ) / 1000000ULL;

    /* Saturate rather than wrap: sleep(UINT_MAX) is silly but must not become
     * a short nap. portMAX_DELAY itself means "forever" to vTaskDelay. */
    return (t >= (uint64_t)portMAX_DELAY) ? (portMAX_DELAY - 1) : (TickType_t)t;
}

/* ------------------------------------------------------------------ handlers */

/*
 * The handler table is allocated on first use and lives on the process slot.
 * Only the process itself ever writes it, from its own task, so no lock is
 * needed here; the table is freed with the slot's other resources, which
 * happens only after the task is gone.
 *
 * SIG_DFL is 0 and SIG_IGN is 1, so a calloc'd table already reads as "every
 * signal at its default".
 */
static void (**handler_table(espix_proc_slot_t *slot))(int)
{
    if (slot->sig_handlers == NULL) {
        slot->sig_handlers = calloc(NSIG, sizeof(*slot->sig_handlers));
    }
    return slot->sig_handlers;
}

static bool catchable(int sig)
{
    const uint32_t bit = espix_sigbit(sig);

    return bit != 0 && (bit & ESPIX_SIG_UNCATCHABLE) == 0;
}

static void (*espix_abi_signal(int sig, void (*handler)(int)))(int)
{
    espix_proc_slot_t *slot = espix_proc_self();

    if (slot == NULL || !catchable(sig)) {
        errno = EINVAL;
        return SIG_ERR;
    }

    void (**tab)(int) = handler_table(slot);
    if (tab == NULL) {
        errno = ENOMEM;
        return SIG_ERR;
    }

    void (*prev)(int) = tab[sig];
    tab[sig] = handler;
    return prev;
}

/*
 * sa_mask and sa_flags are accepted and ignored. There is no signal frame to
 * apply a mask around and no restartable syscall for SA_RESTART to restart, and
 * SA_SIGINFO would need a siginfo_t espix has nothing to put in. Ignoring them
 * lets ordinary code that fills in a struct sigaction work; pretending to
 * honour them would be worse than either.
 */
static int espix_abi_sigaction(int sig, const struct sigaction *act,
                               struct sigaction *old)
{
    espix_proc_slot_t *slot = espix_proc_self();

    if (slot == NULL || !catchable(sig)) {
        errno = EINVAL;
        return -1;
    }

    void (**tab)(int) = handler_table(slot);
    if (tab == NULL) {
        errno = ENOMEM;
        return -1;
    }

    if (old != NULL) {
        memset(old, 0, sizeof(*old));
        old->sa_handler = tab[sig];
    }
    if (act != NULL) {
        tab[sig] = act->sa_handler;
    }
    return 0;
}

/* -------------------------------------------------------------------- sending */

static int errno_from_esp(esp_err_t err)
{
    switch (err) {
    case ESP_ERR_INVALID_ARG:   return EINVAL;
    case ESP_ERR_NO_MEM:        return ENOMEM;
    case ESP_ERR_NOT_FOUND:
    case ESP_ERR_INVALID_STATE: return ESRCH;   /* gone, or already finished */
    default:                    return EINVAL;
    }
}

static int espix_abi_kill(pid_t pid, int sig)
{
    /*
     * kill(pid, 0) is POSIX's "does this process exist" probe and must not
     * deliver anything. espix_proc_signal() would reject signal 0 as invalid,
     * which is the right answer to a different question.
     */
    if (sig == 0) {
        xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);
        espix_proc_slot_t *slot = espix_proc_find((espix_pid_t)pid);
        const bool alive = slot != NULL &&
                           !espix_proc_state_is_finished(slot->info.state);
        xSemaphoreGive(g_espix_proc_lock);

        if (!alive) {
            errno = ESRCH;
            return -1;
        }
        return 0;
    }

    const esp_err_t err = espix_proc_signal((espix_pid_t)pid, sig);
    if (err != ESP_OK) {
        errno = errno_from_esp(err);
        return -1;
    }
    return 0;
}

static pid_t espix_abi_getpid(void)
{
    espix_proc_slot_t *slot = espix_proc_self();

    /* Real getpid() cannot fail. A kernel task asking is not a process, and 0
     * is the least misleading thing to say. */
    return (slot != NULL) ? (pid_t)slot->info.pid : 0;
}

static int espix_abi_raise(int sig)
{
    espix_proc_slot_t *slot = espix_proc_self();

    if (slot == NULL) {
        errno = EPERM;
        return -1;
    }

    const int rc = espix_abi_kill((pid_t)slot->info.pid, sig);

    /* POSIX requires the signal be delivered before raise() returns. Nothing
     * else would: signalling yourself cannot wake you, since you are running. */
    if (rc == 0) {
        (void)espix_sigcheck_mask();
    }
    return rc;
}

/* --------------------------------------------------------------------- masks */

static int espix_abi_sigprocmask(int how, const sigset_t *set, sigset_t *old)
{
    espix_proc_slot_t *slot = espix_proc_self();

    if (slot == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (old != NULL) {
        *old = (sigset_t)slot->sig_blocked;
    }
    if (set == NULL) {
        return 0;
    }

    /* SIGKILL and SIGSTOP cannot be blocked, and POSIX says to ignore the
     * attempt rather than fail it. */
    const uint32_t v = (uint32_t)*set & ~ESPIX_SIG_UNCATCHABLE;

    switch (how) {
    case SIG_BLOCK:   slot->sig_blocked |= v;  break;
    case SIG_UNBLOCK: slot->sig_blocked &= ~v; break;
    case SIG_SETMASK: slot->sig_blocked  = v;  break;
    default:
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int espix_abi_sigpending(sigset_t *set)
{
    espix_proc_slot_t *slot = espix_proc_self();

    if (slot == NULL || set == NULL) {
        errno = EINVAL;
        return -1;
    }
    *set = (sigset_t)slot->sig_pending;
    return 0;
}

/* ------------------------------------------------------- interruptible sleeps */

/*
 * The point of interposing these.
 *
 * espix_proc_signal() calls xTaskAbortDelay() on the target, which cuts a
 * vTaskDelay() short. Without that a process sitting in sleep(60) would not
 * reach a delivery point for a minute -- long past the grace period, so it
 * would be deleted mid-sleep with its handler never run and its cleanup never
 * done. Waking it is what makes a handler useful to an app that sleeps, which
 * is most of them.
 *
 * The return values follow POSIX: sleep() reports the seconds it did not sleep,
 * nanosleep() fails with EINTR and fills in the remainder. An app whose handler
 * only sets a flag depends on that -- if the sleep silently resumed, the flag
 * would go unread until it expired.
 */
static unsigned espix_abi_sleep(unsigned seconds)
{
    const TickType_t deadline =
        xTaskGetTickCount() + ticks_from_us((uint64_t)seconds * 1000000ULL);

    for (;;) {
        if (espix_sigcheck_mask() != 0) {
            const int32_t left = (int32_t)(deadline - xTaskGetTickCount());

            if (left <= 0) {
                return 0;
            }
            /* Round up: POSIX wants whole seconds, and reporting 0 from an
             * interrupted sleep would look like it ran to completion. */
            return (unsigned)((pdTICKS_TO_MS((TickType_t)left) + 999) / 1000);
        }

        const int32_t left = (int32_t)(deadline - xTaskGetTickCount());
        if (left <= 0) {
            return 0;
        }
        vTaskDelay((TickType_t)left);
    }
}

static int espix_abi_usleep(useconds_t usec)
{
    if (usec < SHORT_SLEEP_US) {
        (void)espix_sigcheck_mask();
        return usleep(usec);            /* the firmware's own, sub-tick aware */
    }

    const TickType_t deadline = xTaskGetTickCount() + ticks_from_us(usec);

    for (;;) {
        if (espix_sigcheck_mask() != 0) {
            errno = EINTR;
            return -1;
        }

        const int32_t left = (int32_t)(deadline - xTaskGetTickCount());
        if (left <= 0) {
            return 0;
        }
        vTaskDelay((TickType_t)left);
    }
}

static int espix_abi_nanosleep(const struct timespec *req, struct timespec *rem)
{
    if (req == NULL || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L ||
        req->tv_sec < 0) {
        errno = EINVAL;
        return -1;
    }

    const uint64_t us = (uint64_t)req->tv_sec * 1000000ULL +
                        (uint64_t)req->tv_nsec / 1000ULL;

    /*
     * Short enough not to be worth interrupting -- and delegated to usleep()
     * rather than to nanosleep(), because ESP-IDF does not implement
     * nanosleep() either. It is declared in <time.h> and defined nowhere, so
     * calling it here is a link error and an app calling it would have failed
     * to load. Publishing this is therefore how apps get nanosleep() at all.
     */
    if (us < SHORT_SLEEP_US) {
        (void)espix_sigcheck_mask();
        if (rem != NULL) {
            rem->tv_sec  = 0;
            rem->tv_nsec = 0;
        }
        return usleep((useconds_t)us);
    }

    const TickType_t deadline = xTaskGetTickCount() + ticks_from_us(us);

    for (;;) {
        if (espix_sigcheck_mask() != 0) {
            const int32_t left = (int32_t)(deadline - xTaskGetTickCount());

            if (rem != NULL) {
                const uint32_t ms = (left > 0)
                                        ? pdTICKS_TO_MS((TickType_t)left) : 0;
                rem->tv_sec  = (time_t)(ms / 1000u);
                rem->tv_nsec = (long)(ms % 1000u) * 1000000L;
            }
            errno = EINTR;
            return -1;
        }

        const int32_t left = (int32_t)(deadline - xTaskGetTickCount());
        if (left <= 0) {
            if (rem != NULL) {
                rem->tv_sec  = 0;
                rem->tv_nsec = 0;
            }
            return 0;
        }
        vTaskDelay((TickType_t)left);
    }
}

static int espix_abi_pause(void)
{
    for (;;) {
        if (espix_sigcheck_mask() != 0) {
            errno = EINTR;
            return -1;         /* pause() has no success case */
        }

        /* Woken by xTaskAbortDelay() when a signal arrives. portMAX_DELAY - 1
         * is roughly 497 days at a 100Hz tick, so the loop is the mechanism and
         * not a timeout. */
        vTaskDelay(portMAX_DELAY - 1);
    }
}

/* ------------------------------------------------------------------- resolver */

typedef struct {
    const char *name;
    uintptr_t   addr;
} abi_sym_t;

#define ABI_SYM(posix_name, fn) { (posix_name), (uintptr_t)(void *)(fn) }

static const abi_sym_t s_signal_syms[] = {
    /* Dispositions. */
    ABI_SYM("signal",      espix_abi_signal),
    ABI_SYM("sigaction",   espix_abi_sigaction),

    /* Sending. getpid() and raise() are here because an app cannot reach the
     * firmware's own -- those resolve to ENOSYS and abort() respectively. */
    ABI_SYM("kill",        espix_abi_kill),
    ABI_SYM("raise",       espix_abi_raise),
    ABI_SYM("getpid",      espix_abi_getpid),

    /* Masks. sigemptyset and friends are macros in <signal.h>, so an app gets
     * them at compile time and there is nothing to publish. */
    ABI_SYM("sigprocmask", espix_abi_sigprocmask),
    ABI_SYM("sigpending",  espix_abi_sigpending),

    /*
     * The delivery points. These shadow the loader's own libc entries, which is
     * the entire reason this is a resolver: an app's sleep() has to be one that
     * a signal can cut short.
     */
    ABI_SYM("sleep",       espix_abi_sleep),
    ABI_SYM("usleep",      espix_abi_usleep),
    ABI_SYM("nanosleep",   espix_abi_nanosleep),
    ABI_SYM("pause",       espix_abi_pause),

    /* For an app that blocks on nothing and would otherwise never be reachable.
     * espix_app_stopping() is published by abi_cxx.cpp and is the older, simpler
     * form of the same question. */
    ABI_SYM("espix_sigcheck", espix_sigcheck),
};

static uintptr_t espix_symbol_resolver(const char *sym_name)
{
    if (sym_name != NULL) {
        for (size_t i = 0; i < sizeof(s_signal_syms) / sizeof(s_signal_syms[0]);
             i++) {
            if (strcmp(sym_name, s_signal_syms[i].name) == 0) {
                return s_signal_syms[i].addr;
            }
        }
    }

    /* Everything else, including the whole libc and IDF surface, unchanged. */
    return elf_find_sym_default(sym_name);
}

void espix_proc_abi_signal_register(void)
{
    elf_set_symbol_resolver(espix_symbol_resolver);

    espix_klog(ESPIX_KLOG_DEBUG, TAG, "signal syscalls published to apps");
}
