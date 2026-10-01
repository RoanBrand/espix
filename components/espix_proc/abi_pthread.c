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
 * Why the resolver: espix does not own the pthread surface (see abi_libc.c);
 * pthread_create is answered below, and only the resolver is searched first.
 * Same seam malloc, sleep and exit already use.
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
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pthread.h"

#include "espix_kernel.h"
#include "espix_proc_priv.h"

#define TAG "abi"

/*
 * What the trampoline needs to know, handed to it as its argument and freed by
 * it before the app's routine is called -- so there is exactly one owner from
 * the moment pthread_create accepts the thread.
 */
typedef struct {
    void              *(*fn)(void *);
    void               *arg;
    espix_proc_slot_t  *slot;
} espix_pthread_start_t;

static void *espix_pthread_trampoline(void *arg)
{
    espix_pthread_start_t *const st = arg;

    void *(*const fn)(void *) = st->fn;
    void *const user_arg      = st->arg;
    espix_proc_slot_t *const slot = st->slot;
    free(st);

    /* The thread's first act, before the app's routine can allocate. */
    vTaskSetThreadLocalStoragePointer(NULL, ESPIX_TLS_PROC_IDX, slot);

    void *const ret = fn(user_arg);

    /* And its last, so a slot pointer does not outlive the thread. A thread that
     * ends through pthread_exit() skips this, which leaves a pointer to a slot
     * that espix_proc_release_resources() will have cleared the state of -- the
     * caller checks that before trusting it. */
    vTaskSetThreadLocalStoragePointer(NULL, ESPIX_TLS_PROC_IDX, NULL);
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

    const int rc = pthread_create(thread, attr, espix_pthread_trampoline, st);
    if (rc != 0) {
        free(st);       /* the trampoline will never run to free it */
    }
    return rc;
}

static const abi_sym_t s_pthread_syms[] = {
    ABI_SYM("pthread_create", espix_pthread_create),
};

void espix_proc_abi_pthread_register(void)
{
    espix_abi_resolver_add(s_pthread_syms,
                           sizeof(s_pthread_syms) / sizeof(s_pthread_syms[0]));

    espix_klog(ESPIX_KLOG_DEBUG, TAG,
               "pthread_create published, so an app's threads are its process's");
}
