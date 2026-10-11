/*
 * Power management: what a component needs from hardware it does not own.
 *
 * The problem is Linux's PM QoS and Android's WifiLock, and the shape is copied
 * from them on purpose. The component knows what it needs -- a link that does
 * not sleep, because a drag is a round trip per motion -- the network knows
 * what that costs, and neither has to know the other. `wifi ps off` was the
 * manual version; vnc grew a private copy of it, and Bluetooth audio grew
 * another, in the wrong component; this is the one all three should use.
 *
 * The vocabulary is the quality, not the mechanism. A caller asks for what it
 * needs; espix decides what that means today and may mean something else later.
 * An API that names WIFI_PS_NONE would be a UI for ESP-IDF's API rather than a
 * policy for espix's, and would have to change every time the answer did -- as
 * this one did, when the audio answer turned out to be the opposite of the
 * interactive one.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    /*
     * A round trip per exchange: vnc motion, an ssh keystroke. The radio must
     * not sleep, because a dropped beacon interval is paid on every one of
     * them. Measured on this board: 76 ms mean ping with the station's default
     * sleep, 4.9 ms with the radio awake.
     */
    ESPIX_PM_LATENCY = 0,

    /*
     * Audio over the shared antenna: Bluetooth must not lose its turn to a
     * WiFi radio that is awake by default. Note this is the *opposite* ask --
     * here WiFi is told to sleep as deeply as it can, because an SBC frame torn
     * apart by a timeslice is an audible gap. Measured by ear: with WiFi
     * associated the tone pulsed, with WiFi disconnected it was continuous,
     * and keeping the radio awake (the interactive answer) did not help.
     */
    ESPIX_PM_AUDIO_AIR,

    ESPIX_PM_NEED_COUNT,
} espix_pm_need_t;

/*
 * Held by the calling task, and counted: two callers are two holds, and the
 * last release is what puts the hardware back. A request is not a name that a
 * crashed session can leave behind -- a task that dies has its holds dropped by
 * the reaper, which is the whole difference between this and the classic
 * WifiLock leak of a radio left awake for good.
 */
esp_err_t espix_pm_acquire(espix_pm_need_t need);
esp_err_t espix_pm_release(espix_pm_need_t need);

/* Told when a task dies, so a hold cannot outlive the task that took it. */
void espix_pm_task_gone(void *task);

/* Outstanding holds, for a UI or a test. */
size_t espix_pm_holds(espix_pm_need_t need);

/*
 * What the subsystem that owns the hardware installs. It is called with the new
 * state when a count crosses zero -- never for a state already in effect -- so
 * an applier can be a plain set-and-restore, and a component that takes and
 * gives many times costs one apply.
 */
typedef void (*espix_pm_apply_fn)(espix_pm_need_t need, bool active);
void espix_pm_set_applier(espix_pm_apply_fn fn);

#ifdef __cplusplus
}
#endif
