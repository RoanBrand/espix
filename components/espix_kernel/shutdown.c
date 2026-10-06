/*
 * The shutdown sequence: what espix does before it stops being espix.
 *
 * Linux's shutdown has a great deal to do, and nearly all of it is about state
 * that outlives the kernel -- open files, a dirty page cache, mounted volumes.
 * A reset here is total: tasks, sockets, drivers and RAM all go with it. What
 * survives is the storage that stays powered across the reset (a USB volume),
 * the peers on the network, and whatever hardware an app has switched on. Those
 * are the three things this sequence exists for.
 *
 * The order is the phase enum in espix_kernel.h rather than a priority number,
 * because the phases *are* the design: a subsystem says which one it belongs to
 * and cannot get the order wrong. Inside a phase, handlers run in the order they
 * were added.
 *
 * It lives in the kernel because it is a kernel facility -- reboot(2) is not a
 * service -- and it depends on nothing above it. Each subsystem registers what
 * it has to do; espix_main.c, which already knows all of them, does the wiring.
 *
 * The sequence runs on a task of its own rather than the caller's, for two
 * reasons. It may be a session, the desktop's event path or a unit's task, and
 * none of those stacks is the sequence's to spend. And a task that is neither a
 * process nor a unit is one the sequence cannot stop by accident -- the UNITS
 * and APPS phases would otherwise be aiming at the thing running them.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "espix_kernel.h"

static const char *TAG = "espix:shutdown";

/* Handlers per phase. Four phases, few subsystems each; a full table would be a
 * compile-time constant nobody could exceed without noticing. */
#define SHUTDOWN_PER_PHASE 8

/* Internal RAM, because deep sleep needs an internal stack pointer; see
 * shutdown_start(). */
#define SHUTDOWN_STACK    6144
#define SHUTDOWN_PRIORITY 5

/* Not shutdown_handler_t: IDF's esp_system.h already owns that name, for a
 * void(void) handler of its own. */
typedef struct {
    espix_shutdown_fn fn;
    const char       *name;
} shutdown_entry_t;

static shutdown_entry_t s_handlers[ESPIX_SHUTDOWN_PHASE_COUNT][SHUTDOWN_PER_PHASE];
static int                s_count[ESPIX_SHUTDOWN_PHASE_COUNT];

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_started;

typedef enum {
    SHUTDOWN_REBOOT = 0,
    SHUTDOWN_POWEROFF,
} shutdown_mode_t;

static shutdown_mode_t s_mode;
static unsigned        s_minutes;   /* poweroff only; 0 is the sleep-path test */

esp_err_t espix_shutdown_add(espix_shutdown_phase_t phase, espix_shutdown_fn fn,
                             const char *name)
{
    if (fn == NULL || (unsigned)phase >= ESPIX_SHUTDOWN_PHASE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_started) {
        /* Nothing joins a shutdown already under way: its phase may have run. */
        return ESP_ERR_INVALID_STATE;
    }
    if (s_count[phase] >= SHUTDOWN_PER_PHASE) {
        return ESP_ERR_NO_MEM;
    }

    s_handlers[phase][s_count[phase]].fn   = fn;
    s_handlers[phase][s_count[phase]].name = (name != NULL) ? name : "?";
    s_count[phase]++;
    return ESP_OK;
}

bool espix_shutdown_started(void)
{
    return s_started;
}

uint32_t espix_shutdown_remaining_ms(int64_t deadline_us)
{
    const int64_t left_us = deadline_us - esp_timer_get_time();

    if (left_us <= 0) {
        return 1;               /* see the note in the header: zero means "no limit" */
    }

    const uint32_t ms = (uint32_t)(left_us / 1000);
    return (ms == 0) ? 1 : ms;
}

static void shutdown_task(void *arg)
{
    (void)arg;

    /*
     * One budget for the whole sequence, taken before anything runs. A handler
     * that waits gives up when the system's time is gone, not its own -- so a
     * slow phase cannot push the act past what the caller was promised.
     */
    const int64_t deadline_us = esp_timer_get_time() + ESPIX_SHUTDOWN_GRACE_US;

    espix_klog(ESPIX_KLOG_WARN, TAG, "%s: going down",
               (s_mode == SHUTDOWN_REBOOT) ? "reboot" : "poweroff");

    /*
     * Each step at INFO, the way systemd says "Stopping X" on the way down: a
     * shutdown is rare, and a person watching the console should see what is
     * about to happen rather than only what happened to log. DEBUG was the
     * first version and it made the sequence almost invisible -- the console
     * echoes INFO by default, so the only lines that arrived were the ones the
     * handlers themselves wrote, with no statement of what was being done.
     *
     * The name is the handler's, which reads on its own; the phase it was
     * registered in is the ordering and nothing else.
     */
    for (unsigned p = 0; p < ESPIX_SHUTDOWN_PHASE_COUNT; p++) {
        for (int i = 0; i < s_count[p]; i++) {
            espix_klog(ESPIX_KLOG_INFO, TAG, "%s", s_handlers[p][i].name);
            s_handlers[p][i].fn(deadline_us);
        }
    }

    espix_klog(ESPIX_KLOG_INFO, TAG, "ready");

    /* What was written is only worth anything if it reaches the console before
     * the reset does; the flusher is a task, so give it a moment. */
    (void)espix_klog_drained_wait(200);

    if (s_mode == SHUTDOWN_REBOOT) {
        esp_restart();
    }

    /*
     * Deep sleep is the only "off" this chip has: it cannot cut its own power,
     * so the chip drops to microamps and everything but the RTC domain goes.
     * The timer is what brings it back -- deliberately, because a device nobody
     * remembers to power-cycle is a device that is gone.
     *
     * Zero is the test value rather than "stay off": it wakes after a second,
     * which exercises the whole sleep path without a ten-minute wait.
     */
    uint64_t us = (uint64_t)s_minutes * 60ULL * 1000000ULL;
    if (s_minutes == 0) {
        us = 1000000;
    }

    const esp_err_t err = esp_sleep_enable_timer_wakeup(us);
    if (err != ESP_OK) {
        /*
         * Waking by itself is the part that was asked for, so a timer that will
         * not arm becomes a reboot rather than the indefinite off that staying
         * asleep with no wake source would be.
         */
        espix_klog(ESPIX_KLOG_ERROR, TAG,
                   "wake timer refused (%s); rebooting instead of staying off",
                   esp_err_to_name(err));
        (void)espix_klog_drained_wait(200);
        esp_restart();
    }

    esp_deep_sleep_start();
}

/*
 * Take the one claim. False means somebody else is already leaving, in which
 * case the caller has nothing left to do but wait for the reset -- which is
 * what the commands do, and why this does not have to.
 */
static bool shutdown_claim(shutdown_mode_t mode, unsigned minutes)
{
    bool claimed = false;

    portENTER_CRITICAL(&s_mux);
    if (!s_started) {
        s_started = true;
        s_mode    = mode;
        s_minutes = minutes;
        claimed   = true;
    }
    portEXIT_CRITICAL(&s_mux);

    return claimed;
}

static void shutdown_start(void)
{
    TaskHandle_t task = NULL;

    /*
     * Internal RAM, deliberately, and not the PSRAM the rest of the system
     * prefers for stacks.
     *
     * Deep sleep powers the PSRAM down on the way in, and ESP-IDF refuses to do
     * that with the stack pointer sitting in it: esp_sleep_isolate_digital_gpio()
     * asserts esp_ptr_internal(esp_cpu_get_sp()) on this chip. A shutdown task
     * with a PSRAM stack therefore panics at the last step of a power-off --
     * measured, not theorised: the first poweroff left a coredump in the task
     * named below and rebooted from the fault handler.
     *
     * Six kilobytes out of internal is affordable at this point in the system's
     * life, and it is the only moment anything is asked to give any up.
     */
    if (xTaskCreate(shutdown_task, "espix:shutdown", SHUTDOWN_STACK, NULL,
                    SHUTDOWN_PRIORITY, &task) != pdPASS) {
        /*
         * No stack for it: do it here, which is the thing the task exists to
         * avoid -- but a shutdown that does not happen is worse. A power-off
         * cannot be trusted to this stack either, since the caller's may be
         * PSRAM; turning it into a reboot keeps the promise that matters (the
         * device comes back) rather than panicking into safe mode.
         */
        espix_klog(ESPIX_KLOG_ERROR, TAG,
                   "no stack for the shutdown task; running it on the caller's");
        if (s_mode == SHUTDOWN_POWEROFF) {
            espix_klog(ESPIX_KLOG_ERROR, TAG,
                       "poweroff needs an internal stack; rebooting instead");
            s_mode = SHUTDOWN_REBOOT;
        }
        shutdown_task(NULL);
    }
}

void espix_shutdown_reboot(void)
{
    if (shutdown_claim(SHUTDOWN_REBOOT, 0)) {
        shutdown_start();
    }
}

void espix_shutdown_poweroff(unsigned minutes)
{
    if (shutdown_claim(SHUTDOWN_POWEROFF, minutes)) {
        shutdown_start();
    }
}
