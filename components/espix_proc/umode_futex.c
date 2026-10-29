/*
 * Futexes for U-mode threads: wait while a word holds a value, and wake those
 * waiting on it, private to a process as Linux's private futexes are. libespix
 * builds pthread mutexes, condition variables and joins on them
 * (docs/USER-MODE.md).
 */
#include "sdkconfig.h"

#if CONFIG_ESPIX_UMODE

#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "espix_task.h"
#include "espix_umode.h"

/* On the waiting thread's kernel stack, on its process's list while it waits. */
struct espix_um_waiter {
    espix_um_waiter_t *next;
    uint32_t           uaddr;
    TaskHandle_t       task;
    bool               woken;
};

/*
 * Queued and the word read under the process's lock, as Linux's futex_wait
 * reads it under the hash bucket's: a waker either changed the word before
 * this read, or finds this waiter. The fences are the ordering: IDF's locks
 * are a bare LR/SC with no barrier.
 */
int32_t espix_um_futex_wait(espix_um_proc_t *p, uint32_t uaddr, uint32_t val, int64_t timeout_us)
{
    if (uaddr & 3) {
        return -EINVAL;
    }
    espix_um_waiter_t w = { .uaddr = uaddr, .task = xTaskGetCurrentTaskHandle() };
    xSemaphoreTake(p->lock, portMAX_DELAY);
    w.next           = p->futex_waiters;
    p->futex_waiters = &w;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint32_t now;
    int32_t r = espix_um_copyin(p->as, &now, uaddr, sizeof(now));
    if (r == 0 && now != val) {
        r = -EAGAIN;
    }
    if (r != 0) {
        p->futex_waiters = w.next;      /* still first: the lock was held throughout */
    }
    xSemaphoreGive(p->lock);
    if (r != 0) {
        return r;
    }
    const TickType_t deadline = (timeout_us >= 0)
        ? xTaskGetTickCount() + (TickType_t)((timeout_us + portTICK_PERIOD_MS * 1000 - 1) / (portTICK_PERIOD_MS * 1000))
        : 0;
    while (r == 0 && !__atomic_load_n(&w.woken, __ATOMIC_ACQUIRE)) {
        if (espix_sigcheck_mask() != 0) {
            r = -EINTR;
            break;
        }
        TickType_t wait = portMAX_DELAY;
        if (timeout_us >= 0) {
            const int32_t left = (int32_t)(deadline - xTaskGetTickCount());
            if (left <= 0) {
                r = -ETIMEDOUT;
                break;
            }
            wait = (TickType_t)left;
        }
        (void)ulTaskNotifyTakeIndexed(ESPIX_NOTIFY_SIG_IDX, pdTRUE, wait);
    }

    /* Under the lock, so a waker that found us has finished notifying before we go. */
    xSemaphoreTake(p->lock, portMAX_DELAY);
    if (w.woken) {
        r = 0;      /* a wake that raced the timeout or a signal still counts */
    } else {
        for (espix_um_waiter_t **pp = &p->futex_waiters; *pp != NULL; pp = &(*pp)->next) {
            if (*pp == &w) {
                *pp = w.next;
                break;
            }
        }
    }
    xSemaphoreGive(p->lock);
    return r;
}

int32_t espix_um_futex_wake(espix_um_proc_t *p, uint32_t uaddr, int32_t n)
{
    int32_t woke = 0;
    /* The word's new value is seen before the waiters are: Linux's smp_mb() in futex_wake. */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    xSemaphoreTake(p->lock, portMAX_DELAY);
    for (espix_um_waiter_t **pp = &p->futex_waiters; *pp != NULL && woke < n;) {
        espix_um_waiter_t *const w = *pp;
        if (w->uaddr != uaddr) {
            pp = &w->next;
            continue;
        }
        *pp = w->next;
        __atomic_store_n(&w->woken, true, __ATOMIC_RELEASE);
        xTaskNotifyGiveIndexed(w->task, ESPIX_NOTIFY_SIG_IDX);
        woke++;
    }
    xSemaphoreGive(p->lock);
    return woke;
}

#endif /* CONFIG_ESPIX_UMODE */
