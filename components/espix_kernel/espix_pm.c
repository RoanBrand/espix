/*
 * Power management requests: who needs what, counted, and for how long.
 *
 * See espix_pm.h for why this exists. The implementation is deliberately dull --
 * a small table of holders per need, a count, and a callback when a count
 * crosses zero. Everything interesting is in the caller and in the applier that
 * owns the hardware, which is the point: neither has to know the other.
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "espix_kernel.h"
#include "espix_pm.h"

#define TAG "pm"

/*
 * Room for every session plus the components that want one. A full table is a
 * programming error rather than a runtime condition, and is reported as one:
 * silently dropping a request would leave a caller believing it holds
 * something, which is the failure mode this whole file exists to avoid.
 */
#define PM_HOLDERS 16

typedef struct {
    void    *task;
    unsigned count;
} pm_holder_t;

static pm_holder_t       s_holders[ESPIX_PM_NEED_COUNT][PM_HOLDERS];
static unsigned          s_total[ESPIX_PM_NEED_COUNT];
static espix_pm_apply_fn s_apply;
static portMUX_TYPE      s_lock = portMUX_INITIALIZER_UNLOCKED;

/* Called with the lock held. Returns the caller's row, or NULL when full. */
static pm_holder_t *holder_get(espix_pm_need_t need, void *task, bool create)
{
    for (int i = 0; i < PM_HOLDERS; i++) {
        if (s_holders[need][i].task == task) {
            return &s_holders[need][i];
        }
    }
    if (!create) {
        return NULL;
    }
    for (int i = 0; i < PM_HOLDERS; i++) {
        if (s_holders[need][i].task == NULL) {
            s_holders[need][i].task = task;
            return &s_holders[need][i];
        }
    }
    return NULL;
}

static bool need_valid(espix_pm_need_t need)
{
    return need >= 0 && need < ESPIX_PM_NEED_COUNT;
}

esp_err_t espix_pm_acquire(espix_pm_need_t need)
{
    if (!need_valid(need)) {
        return ESP_ERR_INVALID_ARG;
    }
    void *self    = (void *)xTaskGetCurrentTaskHandle();
    bool  crossed = false;
    bool  full    = false;

    portENTER_CRITICAL(&s_lock);
    pm_holder_t *h = holder_get(need, self, true);
    if (h == NULL) {
        full = true;
    } else {
        h->count++;
        if (s_total[need]++ == 0) {
            crossed = true;
        }
    }
    portEXIT_CRITICAL(&s_lock);

    if (full) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "no slot for need %d", (int)need);
        return ESP_ERR_NO_MEM;
    }
    /*
     * Outside the lock: the applier talks to the network, and calling it with a
     * spinlock held would be a good way to find out what that costs.
     */
    if (crossed && s_apply != NULL) {
        s_apply(need, true);
    }
    return ESP_OK;
}

esp_err_t espix_pm_release(espix_pm_need_t need)
{
    if (!need_valid(need)) {
        return ESP_ERR_INVALID_ARG;
    }
    void *self    = (void *)xTaskGetCurrentTaskHandle();
    bool  crossed = false;

    portENTER_CRITICAL(&s_lock);
    pm_holder_t *h = holder_get(need, self, false);
    if (h != NULL) {
        if (--h->count == 0) {
            h->task = NULL;
        }
        if (s_total[need] > 0 && --s_total[need] == 0) {
            crossed = true;
        }
    }
    portEXIT_CRITICAL(&s_lock);

    /* Releasing what was never held is a caller bug, but not one worth a
     * panic: saying so and carrying on leaves the count honest. */
    if (h == NULL) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "release of need %d not held by this task",
                   (int)need);
    }
    if (crossed && s_apply != NULL) {
        s_apply(need, false);
    }
    return ESP_OK;
}

void espix_pm_task_gone(void *task)
{
    if (task == NULL) {
        return;
    }
    for (int n = 0; n < ESPIX_PM_NEED_COUNT; n++) {
        bool crossed = false;
        portENTER_CRITICAL(&s_lock);
        for (int i = 0; i < PM_HOLDERS; i++) {
            if (s_holders[n][i].task == task) {
                s_holders[n][i].task  = NULL;
                s_holders[n][i].count = 0;
                crossed = true;
            }
        }
        if (crossed) {
            /* Recompute rather than subtract: the row is the truth, and a
             * count that has drifted is worse than one that took a scan. */
            unsigned total = 0;
            for (int i = 0; i < PM_HOLDERS; i++) {
                total += s_holders[n][i].count;
            }
            crossed     = (s_total[n] != 0) && (total == 0);
            s_total[n]  = total;
        }
        portEXIT_CRITICAL(&s_lock);

        if (crossed && s_apply != NULL) {
            espix_klog(ESPIX_KLOG_INFO, TAG, "need %d released by a dying task",
                       n);
            s_apply((espix_pm_need_t)n, false);
        }
    }
}

size_t espix_pm_holds(espix_pm_need_t need)
{
    if (!need_valid(need)) {
        return 0;
    }
    portENTER_CRITICAL(&s_lock);
    const unsigned n = s_total[need];
    portEXIT_CRITICAL(&s_lock);
    return n;
}

void espix_pm_set_applier(espix_pm_apply_fn fn)
{
    s_apply = fn;
}
