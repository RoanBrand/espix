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
 *     # name   restart  command [args...]
 *     blinkd   always   /bin/blinkd --period 500
 *
 * restart is always (start it again when it exits) or once (run it, then
 * leave it stopped). The program must be an absolute path: a supervisor has
 * no PATH, and resolving one here would be a second, invisible PATH.
 */

#define ESPIX_SVC_NAME_MAX 32

typedef struct {
    char        name[ESPIX_SVC_NAME_MAX];
    espix_pid_t pid;             /* ESPIX_PID_NONE when not running */
    bool        always;          /* restart policy */
    bool        enabled;         /* wants to run */
    uint32_t    restarts;
} espix_svc_info_t;

/* Read the units file and start the supervisor. Safe to call twice. */
esp_err_t espix_svc_init(void);

/* For the service command. The lock is taken inside each call. */
int       espix_svc_count(void);
bool      espix_svc_info(int index, espix_svc_info_t *out);
esp_err_t espix_svc_start(const char *name);
esp_err_t espix_svc_stop(const char *name);
esp_err_t espix_svc_reload(void);
