#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "espix_proc.h"

/*
 * espix_svc -- what runs unattended.
 *
 * A unit is a program espix starts at boot and keeps running, independent of
 * any login. A background job dies with its session by design, and the one
 * thing a daemon needs is to outlive the shell that would otherwise have
 * started it.
 *
 * Units come from /etc/units, one per line:
 *
 *     name   restart     command [args...]
 *     blinkd always      /bin/blinkd --period 500
 *     once   once        /bin/once
 *     poll   every 6h    upgrade --check
 *
 * restart is always (start it again when it exits), once (run it, then leave
 * it stopped), or every <interval> with s, m, h or d. A scheduled unit is
 * oneshot: the interval is the schedule, and a failed run retries sooner a
 * few times rather than waiting out a whole interval.
 *
 * A command starting with / is a program -- the only form that needs no PATH,
 * which a supervisor does not have. Anything else is a builtin of the shell,
 * run on a task of its own under the unit's session, which is what lets
 * "upgrade --check" be a unit without anything becoming an ELF.
 */

#define ESPIX_SVC_NAME_MAX 32

typedef struct {
    char        name[ESPIX_SVC_NAME_MAX];
    espix_pid_t pid;             /* ESPIX_PID_NONE when not running */
    bool        always;          /* restart policy */
    bool        enabled;         /* wants to run */
    bool        running;         /* a builtin task, which has no pid */
    uint32_t    every_s;         /* 0 unless scheduled */
    uint32_t    restarts;
    bool        stopping;        /* a builtin has been asked to stop */
    bool        ran;             /* has started at least once */
    int         last_code;       /* what it exited with, once it has */
} espix_svc_info_t;

/* Read the units file and start the supervisor. Safe to call twice. */
esp_err_t espix_svc_init(void);

/* For the service command. The lock is taken inside each call. */
int       espix_svc_count(void);
bool      espix_svc_info(int index, espix_svc_info_t *out);
esp_err_t espix_svc_start(const char *name);
esp_err_t espix_svc_stop(const char *name);

/*
 * For a unit that runs on a task rather than as a process: true once its stop
 * has been asked for, so the loop it runs can finish and let the supervisor
 * see it end. False outside a unit, so the same command runs by hand.
 */
bool      espix_svc_stopping(void);
esp_err_t espix_svc_reload(void);

/*
 * The shutdown path: ask every unit to stop, and wait for them to go, up to
 * deadline_us. Returns how many were still running when it gave up.
 *
 * Disabling comes first, under the supervisor's lock, so that a unit which
 * exits promptly is not started again before the phase is over. The wait then
 * ends as soon as the last unit is gone: a builtin that has set its done flag
 * counts as gone even though the supervisor has not collected it yet, so the
 * one-second tick is not what a shutdown costs.
 */
int espix_svc_quiesce(int64_t deadline_us);

/*
 * True when the last boot left a core dump, so units are being held stopped.
 * A fault on the previous boot is the one signal that says "do not start
 * unattended work again yet": one bad unit otherwise becomes a boot loop.
 */
bool      espix_svc_safe_mode(void);
