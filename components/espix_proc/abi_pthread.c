/*
 * An app's own threads, and the one thing they need that a FreeRTOS task does
 * not have: an espix process.
 *
 * R-P1.2 gave an app private memory -- regions attached to the process slot --
 * and the allocator finds that slot with espix_proc_self(), which keys on the
 * task that entered app_main(). A thread made with pthread_create() is a
 * different task, so it found no slot and its malloc() went to the global heap:
 * live, unowned, and not reclaimed at exit. That is the leak R-P1.2 exists to
 * remove, just smaller, and this is the seam that closes it.
 *
 * The free path did not need this. R-P1.2 made free() find the region by
 * address, because a free that misses its region and reaches the global heap is
 * corruption rather than a leak -- so the dangerous half was fixed first and
 * this is the remaining half.
 *
 * Why the resolver: pthread_create has to be espix's, not libc's, so that the
 * thread it makes belongs to the process. The resolver is searched before every
 * table; the rest of the pthread surface is in one (see abi_libc.c). Same seam
 * malloc, sleep and exit use.
 *
 * Why a trampoline rather than recording the handle after the call: IDF creates
 * the task inside pthread_create and it can start running before that call
 * returns, so a mapping installed afterwards would leave a window in which the
 * new thread allocates and finds no slot. The trampoline makes recording the
 * thread's first act.
 *
 * Why thread-local storage rather than a table keyed on task handles: a handle
 * is the address of a TCB, and FreeRTOS hands a dead task's TCB to the next task
 * created, so a stale entry can name the wrong task. A pointer read out of the
 * current task's own TLS cannot be another task's at all.
 */

#include <errno.h>
#include <semaphore.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"

#include "pthread.h"

#include "espix_kernel.h"
#include "espix_proc_priv.h"
#include "proc_threads.h"

#define TAG "abi"

/*
 * The threads an app has made, so that its process can signal them and end
 * them.
 *
 * A thread outlives its process otherwise. Every other resource an app holds is
 * released by espix_proc_release_resources() -- its memory, its descriptors,
 * its audio streams, its power-management holds -- and a thread is the one that
 * was left out: measured, after Doom exited, as a pthread still at 55% of a core
 * with a pointer to a stream the reaper had already closed for it.
 *
 * Reserved by pthread_create, so a full table is EAGAIN rather than a thread
 * nobody can reach, and given its task by the thread in the trampoline. Every
 * change is under the proc lock, and that is the guarantee the signal path
 * leans on: while the lock is held, a registered task has not yet left, so it
 * is safe to notify. See proc_threads.h.
 */
static espix_thread_t  s_thread_v[CONFIG_ESPIX_PTHREAD_MAX];
static espix_threads_t s_threads = { s_thread_v, CONFIG_ESPIX_PTHREAD_MAX };

/* The calling thread leaves the books, and its CPU time joins its process's. */
static void espix_pthread_unregister(espix_proc_slot_t *slot)
{
    const TaskHandle_t self = xTaskGetCurrentTaskHandle();

    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);
    const int32_t tid = espix_threads_tid_of(&s_threads, self);
    if (tid >= 0 && espix_threads_remove(&s_threads, tid, NULL) && slot != NULL) {
        TaskStatus_t st;
        vTaskGetInfo(self, &st, pdFALSE, eRunning);
        slot->cpu_threads_done += st.ulRunTimeCounter;
    }
    xSemaphoreGive(g_espix_proc_lock);
}

void espix_pthread_wake_locked(espix_proc_slot_t *slot)
{
    espix_thread_t th[CONFIG_ESPIX_PTHREAD_MAX];
    const size_t n = espix_threads_of(&s_threads, slot, th,
                                      CONFIG_ESPIX_PTHREAD_MAX);
    const TaskHandle_t self = xTaskGetCurrentTaskHandle();

    for (size_t i = 0; i < n && i < CONFIG_ESPIX_PTHREAD_MAX; i++) {
        if (th[i].task != NULL && th[i].task != self) {
            (void)xTaskNotifyGiveIndexed((TaskHandle_t)th[i].task,
                                         ESPIX_NOTIFY_SIG_IDX);
        }
    }
}

size_t espix_pthread_list_locked(const espix_proc_slot_t *slot,
                                 espix_pid_t *tids, TaskHandle_t *tasks,
                                 size_t max)
{
    espix_thread_t th[CONFIG_ESPIX_PTHREAD_MAX];
    const size_t n = espix_threads_of(&s_threads, slot, th,
                                      CONFIG_ESPIX_PTHREAD_MAX);
    size_t out = 0;

    for (size_t i = 0; i < n && i < CONFIG_ESPIX_PTHREAD_MAX && out < max; i++) {
        if (th[i].task != NULL) {
            tids[out]  = (espix_pid_t)th[i].tid;
            tasks[out] = (TaskHandle_t)th[i].task;
            out++;
        }
    }
    return out;
}

bool espix_proc_thread_of_task(TaskHandle_t task, espix_pid_t *pid,
                               espix_pid_t *tid)
{
    bool found = false;

    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);
    const int32_t t = espix_threads_tid_of(&s_threads, task);
    if (t >= 0) {
        for (size_t i = 0; i < s_threads.cap; i++) {
            if (s_threads.v[i].tid == t && s_threads.v[i].slot != NULL) {
                const espix_proc_slot_t *const slot = s_threads.v[i].slot;
                *pid  = slot->info.pid;
                *tid  = (espix_pid_t)t;
                found = true;
                break;
            }
        }
    }
    xSemaphoreGive(g_espix_proc_lock);
    return found;
}

uint64_t espix_proc_cpu_threads_done(espix_pid_t pid)
{
    uint64_t done = 0;

    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);
    const espix_proc_slot_t *const slot = espix_proc_find(pid);
    if (slot != NULL) {
        done = slot->cpu_threads_done;
    }
    xSemaphoreGive(g_espix_proc_lock);
    return done;
}

/*
 * End any thread this process left running. Called from
 * espix_proc_release_resources(), which runs on both the clean exit and the
 * kill path.
 *
 * vTaskDelete() from another task is what FreeRTOS offers. The process's
 * streams are its slot's, not the thread's, so deleting one closes nothing.
 */
void espix_pthread_reap(espix_proc_slot_t *slot)
{
    if (slot == NULL) {
        return;
    }
    for (;;) {
        espix_thread_t victim;

        /* Suspended under the lock, as proc_force_kill() does: a thread on its
         * way out takes this lock to unregister, and one deleted holding it
         * would wedge every process. */
        xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);
        const bool got = espix_threads_take(&s_threads, slot, &victim);
        if (got && victim.task != NULL) {
            vTaskSuspend((TaskHandle_t)victim.task);
        }
        xSemaphoreGive(g_espix_proc_lock);

        if (!got) {
            return;
        }
        /* A reservation whose thread never started finds its entry gone and
         * returns without running anything. */
        if (victim.task != NULL) {
            espix_klog(ESPIX_KLOG_WARN, TAG,
                       "ending thread %d, which its process left behind",
                       (int)victim.tid);
            if (victim.caps) {
                vTaskDeleteWithCaps((TaskHandle_t)victim.task);
            } else {
                vTaskDelete((TaskHandle_t)victim.task);
            }
        }
    }
}

/*
 * What the trampoline needs to know, handed to it as its argument and freed by
 * it before the app's routine is called -- so there is exactly one owner from
 * the moment pthread_create accepts the thread.
 */
typedef struct {
    void              *(*fn)(void *);
    void               *arg;
    espix_proc_slot_t  *slot;
    espix_pid_t         tid;
} espix_pthread_start_t;

static void *espix_pthread_trampoline(void *arg)
{
    espix_pthread_start_t *const st = arg;

    void *(*const fn)(void *) = st->fn;
    void *const user_arg      = st->arg;
    espix_proc_slot_t *const slot = st->slot;
    const espix_pid_t tid     = st->tid;
    free(st);

    /* Into the process's books before anything else, so a signal can reach it
     * and the process's end can end it. Gone already means the process ended
     * while this thread was being made: there is nothing to run for. */
    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);
    const bool live = espix_threads_set_task(&s_threads, tid,
                                             xTaskGetCurrentTaskHandle());
    xSemaphoreGive(g_espix_proc_lock);
    if (!live) {
        return NULL;
    }

    /* Before the app's routine can allocate. */
    vTaskSetThreadLocalStoragePointer(NULL, ESPIX_TLS_PROC_IDX, slot);

    void *const ret = fn(user_arg);

    /* Last, so a slot pointer does not outlive the thread. */
    vTaskSetThreadLocalStoragePointer(NULL, ESPIX_TLS_PROC_IDX, NULL);
    espix_pthread_unregister(slot);
    return ret;
}

static int espix_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                                void *(*start_routine)(void *), void *arg)
{
    espix_proc_slot_t *const slot =
        (start_routine != NULL) ? espix_proc_self() : NULL;

    /* Not a process -- espix's own code, or a thread making a thread -- so there
     * is no arena to attach to and the real call is the whole job. */
    if (slot == NULL) {
        return pthread_create(thread, attr, start_routine, arg);
    }

    espix_pthread_start_t *const st = malloc(sizeof(*st));
    if (st == NULL) {
        return ENOMEM;
    }
    st->fn   = start_routine;
    st->arg  = arg;
    st->slot = slot;

    /* The tid comes from the pid space, so it names exactly one thing. */
    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);
    st->tid = espix_proc_next_pid();
    const bool room = espix_threads_add(&s_threads, slot, st->tid);
    xSemaphoreGive(g_espix_proc_lock);
    if (!room) {
        free(st);
        return EAGAIN;
    }

    const espix_pid_t tid = st->tid;
    const int rc = pthread_create(thread, attr, espix_pthread_trampoline, st);
    if (rc != 0) {
        free(st);       /* the trampoline will never run to free it */
        xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);
        (void)espix_threads_remove(&s_threads, tid, NULL);
        xSemaphoreGive(g_espix_proc_lock);
    }
    return rc;
}

/*
 * espix's own thread of a process: sshd:rx under its connection, audio's mixer
 * under audio. Booked like an app's, so ps lists it and a signal wakes it, but
 * with no stdio to take or give back.
 */
typedef struct {
    espix_kthread_fn_t fn;
    void              *arg;
    espix_proc_slot_t *slot;
    espix_pid_t        tid;
} kthread_start_t;

static void kthread_trampoline(void *arg)
{
    const kthread_start_t st = *(kthread_start_t *)arg;
    free(arg);

    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);
    const bool live = espix_threads_set_task(&s_threads, st.tid,
                                             xTaskGetCurrentTaskHandle());
    xSemaphoreGive(g_espix_proc_lock);

    if (live) {
        vTaskSetThreadLocalStoragePointer(NULL, ESPIX_TLS_PROC_IDX, st.slot);
        st.fn(st.arg);
        vTaskSetThreadLocalStoragePointer(NULL, ESPIX_TLS_PROC_IDX, NULL);
        espix_pthread_unregister(st.slot);
    }
    vTaskDeleteWithCaps(NULL);
}

esp_err_t espix_proc_thread_start(espix_proc_slot_t *slot,
                                  const espix_kthread_t *k, TaskHandle_t *out)
{
    kthread_start_t *const st = malloc(sizeof(*st));
    if (st == NULL) {
        return ESP_ERR_NO_MEM;
    }
    *st = (kthread_start_t){ .fn = k->fn, .arg = k->arg, .slot = slot };

    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);
    st->tid = espix_proc_next_pid();
    const bool room = espix_threads_add(&s_threads, slot, st->tid) &&
                      espix_threads_set_caps(&s_threads, st->tid);
    xSemaphoreGive(g_espix_proc_lock);
    if (!room) {
        free(st);
        return ESP_ERR_NO_MEM;
    }

    const espix_pid_t tid = st->tid;
    BaseType_t        ok  = pdFAIL;
    if (!k->internal_stack) {
        ok = xTaskCreatePinnedToCoreWithCaps(kthread_trampoline, k->name,
                                             k->stack, st, k->priority, out,
                                             k->core, ESPIX_STACK_CAPS);
    }
    if (ok != pdPASS) {
        ok = xTaskCreatePinnedToCoreWithCaps(kthread_trampoline, k->name,
                                             k->stack, st, k->priority, out,
                                             k->core,
                                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (ok != pdPASS) {
        free(st);
        xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);
        (void)espix_threads_remove(&s_threads, tid, NULL);
        xSemaphoreGive(g_espix_proc_lock);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/*
 * pthread_exit() unwinds without returning through the trampoline's tail, so
 * the thread's books have to be closed here instead. It is answered below by
 * the loader, so this has to be a resolver entry rather than a table one, and
 * it is the only pthread name espix shadows.
 */
static void espix_pthread_exit(void *retval);

void espix_pthread_exit_self(void)
{
    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);
    const bool registered =
        espix_threads_tid_of(&s_threads, xTaskGetCurrentTaskHandle()) >= 0;
    xSemaphoreGive(g_espix_proc_lock);

    if (registered) {
        espix_pthread_exit(NULL);
    }
}

static void espix_pthread_exit(void *retval)
{
    espix_proc_slot_t *const slot = espix_proc_self();

    /* Unwinding this way skips the trampoline's tail, so the process's books
     * have to be cleared here or the entry names a task that is gone. */
    vTaskSetThreadLocalStoragePointer(NULL, ESPIX_TLS_PROC_IDX, NULL);
    espix_pthread_unregister(slot);
    pthread_exit(retval);
}

static const abi_sym_t s_pthread_syms[] = {
    ABI_SYM("pthread_create", espix_pthread_create),
    ABI_SYM("pthread_exit",   espix_pthread_exit),
};

/*
 * The rest of the thread surface: everything an app needs to *coordinate*
 * threads, which until now it could not do at all.
 *
 * The loader answers for exactly six names -- create, join, detach, exit, and
 * two attribute calls (esp_elf_symbol.c) -- so an app could start a thread and
 * wait for it and nothing else. Calling pthread_mutex_lock did not misbehave,
 * it failed to *load*: the name resolved nowhere. That makes threads a
 * curiosity rather than something an app can build on, which is why these are
 * published here rather than left for later.
 *
 * Referencing them is also what keeps them in the image. IDF force-links each
 * pthread module with a -u on its pthread_include_*_impl marker, but the build
 * garbage-collects unreferenced sections, so the marker survives and the
 * functions do not. A name in this table is a reference.
 *
 * pthread_create and pthread_exit are not here: they need the resolver, above.
 * pthread_cancel is not either, because its IDF implementation is a stub --
 * exporting one would advertise a guarantee that does not exist.
 */
static const struct esp_elfsym s_pthread_syms_sync[] = {
    /* Identity. */
    ESP_ELFSYM_EXPORT(pthread_self),
    ESP_ELFSYM_EXPORT(pthread_equal),

    /* Attributes beyond the two the loader answers for. */
    ESP_ELFSYM_EXPORT(pthread_attr_destroy),
    ESP_ELFSYM_EXPORT(pthread_attr_setdetachstate),
    ESP_ELFSYM_EXPORT(pthread_attr_getdetachstate),
    ESP_ELFSYM_EXPORT(pthread_attr_getstacksize),

    /* The four the loader's table used to answer for (R-P3.1). */
    ESP_ELFSYM_EXPORT(pthread_attr_init),
    ESP_ELFSYM_EXPORT(pthread_attr_setstacksize),
    ESP_ELFSYM_EXPORT(pthread_detach),
    ESP_ELFSYM_EXPORT(pthread_join),

    /* Mutual exclusion. */
    ESP_ELFSYM_EXPORT(pthread_mutex_init),
    ESP_ELFSYM_EXPORT(pthread_mutex_destroy),
    ESP_ELFSYM_EXPORT(pthread_mutex_lock),
    ESP_ELFSYM_EXPORT(pthread_mutex_trylock),
    ESP_ELFSYM_EXPORT(pthread_mutex_timedlock),
    ESP_ELFSYM_EXPORT(pthread_mutex_unlock),
    ESP_ELFSYM_EXPORT(pthread_mutexattr_init),
    ESP_ELFSYM_EXPORT(pthread_mutexattr_destroy),
    ESP_ELFSYM_EXPORT(pthread_mutexattr_settype),
    ESP_ELFSYM_EXPORT(pthread_mutexattr_gettype),

    /* Condition variables. */
    ESP_ELFSYM_EXPORT(pthread_cond_init),
    ESP_ELFSYM_EXPORT(pthread_cond_destroy),
    ESP_ELFSYM_EXPORT(pthread_cond_wait),
    ESP_ELFSYM_EXPORT(pthread_cond_timedwait),
    ESP_ELFSYM_EXPORT(pthread_cond_signal),
    ESP_ELFSYM_EXPORT(pthread_cond_broadcast),
    ESP_ELFSYM_EXPORT(pthread_condattr_init),
    ESP_ELFSYM_EXPORT(pthread_condattr_destroy),
    ESP_ELFSYM_EXPORT(pthread_condattr_setclock),
    ESP_ELFSYM_EXPORT(pthread_condattr_getclock),

    /* Read/write locks. */
    ESP_ELFSYM_EXPORT(pthread_rwlock_init),
    ESP_ELFSYM_EXPORT(pthread_rwlock_destroy),
    ESP_ELFSYM_EXPORT(pthread_rwlock_rdlock),
    ESP_ELFSYM_EXPORT(pthread_rwlock_wrlock),
    ESP_ELFSYM_EXPORT(pthread_rwlock_tryrdlock),
    ESP_ELFSYM_EXPORT(pthread_rwlock_trywrlock),
    ESP_ELFSYM_EXPORT(pthread_rwlock_unlock),

    /* Thread-specific data, which a thread now otherwise has no way to keep. */
    ESP_ELFSYM_EXPORT(pthread_key_create),
    ESP_ELFSYM_EXPORT(pthread_key_delete),
    ESP_ELFSYM_EXPORT(pthread_setspecific),
    ESP_ELFSYM_EXPORT(pthread_getspecific),

    /* One-time initialisation, and POSIX semaphores -- the simplest primitive
     * an app reaches for, and the one that needs no attributes at all. */
    ESP_ELFSYM_EXPORT(pthread_once),
    ESP_ELFSYM_EXPORT(sem_init),
    ESP_ELFSYM_EXPORT(sem_destroy),
    ESP_ELFSYM_EXPORT(sem_wait),
    ESP_ELFSYM_EXPORT(sem_trywait),
    ESP_ELFSYM_EXPORT(sem_timedwait),
    ESP_ELFSYM_EXPORT(sem_post),
    ESP_ELFSYM_EXPORT(sem_getvalue),

    ESP_ELFSYM_END
};

void espix_proc_abi_pthread_register(void)
{
    espix_abi_resolver_add(s_pthread_syms,
                           sizeof(s_pthread_syms) / sizeof(s_pthread_syms[0]));

    if (esp_elf_register_symbol(s_pthread_syms_sync) != 0) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "could not publish the thread surface");
        return;
    }

    espix_klog(ESPIX_KLOG_DEBUG, TAG,
               "pthread_create published, so an app's threads are its process's, "
               "and the primitives to coordinate them");
}
