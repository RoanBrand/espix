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
 *     name   restart   [options]   command [args...]
 *     nfsd   always                nfsd
 *     poll   every 6h  success=100 upgrade --check
 *     desk   once      requires=vnc desktop start
 *
 * restart is always, once, or every <interval> (s, m, h, d); a failed
 * scheduled run retries sooner a few times. success= lists exit statuses
 * besides 0 that mean it worked; requires= names units that must be up first.
 *
 * A command starting with / is a program; anything else is a shell builtin,
 * run as a DAEMON process under the unit's session.
 */

#define ESPIX_SVC_NAME_MAX 32

typedef struct {
    char        name[ESPIX_SVC_NAME_MAX];
    espix_pid_t pid;             /* ESPIX_PID_NONE when not running */
    bool        always;          /* restart policy */
    bool        enabled;         /* wants to run */
    uint32_t    every_s;         /* 0 unless scheduled */
    uint32_t    restarts;
    bool        stopping;        /* asked to stop, and not gone yet */
    bool        ran;             /* has started at least once */
    int         last_code;       /* what it exited with, once it has */
    bool        last_ok;         /* and whether that counts as success */
    bool        waiting;         /* due, but something it requires is not up */
} espix_svc_info_t;

/* Read the units file and start the supervisor. Safe to call twice. */
esp_err_t espix_svc_init(void);

/* For the service command. The lock is taken inside each call. */
int       espix_svc_count(void);
bool      espix_svc_info(int index, espix_svc_info_t *out);
esp_err_t espix_svc_start(const char *name);
esp_err_t espix_svc_stop(const char *name);

esp_err_t espix_svc_reload(void);

/*
 * True when the last boot left a core dump, so units are being held stopped.
 * A fault on the previous boot is the one signal that says "do not start
 * unattended work again yet": one bad unit otherwise becomes a boot loop.
 */
bool      espix_svc_safe_mode(void);
