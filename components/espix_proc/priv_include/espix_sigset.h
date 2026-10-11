/*
 * Signal-set and sleep arithmetic, with no FreeRTOS or IDF in it, so the host
 * unit tests (test_apps/host) can compile it as it is.
 */
#pragma once

#include <signal.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bit for `sig`, or 0 if it is not a signal. Not sigaddset(): that macro is
 * `1 << sig` on a signed int, and SIGUSR2 is 31. */
static inline uint32_t espix_sigbit(int sig)
{
    return (sig > 0 && sig < NSIG) ? ((uint32_t)1u << sig) : 0u;
}

/* Signals that cannot be caught, blocked or ignored, as POSIX requires. */
#define ESPIX_SIG_UNCATCHABLE (espix_sigbit(SIGKILL) | espix_sigbit(SIGSTOP))

/*
 * Not a signal: bit 0, which no signal uses because they start at 1. Set in the
 * mask espix_sigcheck_mask() returns to mean "this process has been asked to
 * terminate", so one call answers both questions a blocking call needs to ask.
 */
#define ESPIX_SIG_STOPPING ((uint32_t)1u << 0)

/*
 * Ticks for a duration, saturating one short of `forever` rather than wrapping:
 * sleep(UINT_MAX) is silly but must not become a short nap, and `forever`
 * itself (portMAX_DELAY) means something else to a wait.
 */
static inline uint32_t espix_ticks_from_us(uint64_t us, uint32_t hz,
                                           uint32_t forever)
{
    const uint64_t t = (us * hz) / 1000000ULL;

    return (t >= (uint64_t)forever) ? (forever - 1) : (uint32_t)t;
}

/* What an interrupted sleep() reports: whole seconds, rounded up, because 0
 * would read as a sleep that ran to completion. */
static inline unsigned espix_sleep_secs_left(uint32_t ms_left)
{
    return (unsigned)(((uint64_t)ms_left + 999u) / 1000u);
}

#ifdef __cplusplus
}
#endif
