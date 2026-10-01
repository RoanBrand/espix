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
#include <semaphore.h>
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

/*
 * Give this thread the process's stdio, and take it back again before the
 * thread is reclaimed.
 *
 * An app's streams are funopen() objects over its session, and they live in the
 * reent of the task that entered app_main() -- espix swaps them in there and
 * records that reent in slot->reent. A *new* thread gets a fresh reent whose
 * stdout is the console's, so without this its printf goes to the UART and
 * never reaches the user.
 *
 * Taking them back is not tidiness. FreeRTOS deletes the task by running
 * _reclaim_reent() on its reent, and that fcloses every stream in it that is
 * not the global one -- so a thread that exits still holding the process's
 * stdout makes the *thread's* death close the *process's* stream, from the
 * deleting context and possibly mid-write. That is not hypothetical: it showed
 * up as a spinlock assert inside _puts_r the first time this shipped. The
 * process's own exit path has the same guard; see espix_proc_detach_streams().
 *
 * _REENT_STD* and _GLOBAL_REENT are newlib's, and are what that function uses
 * rather than the raw fields, because they are what survives _REENT_SMALL.
 */
static void espix_pthread_take_streams(espix_proc_slot_t *slot)
{
    struct _reent *const pr = (slot != NULL) ? slot->reent : NULL;
    if (pr == NULL) {
        return;
    }

    _REENT_STDIN(_REENT)  = _REENT_STDIN(pr);
    _REENT_STDOUT(_REENT) = _REENT_STDOUT(pr);
    _REENT_STDERR(_REENT) = _REENT_STDERR(pr);
}

static void espix_pthread_drop_streams(void)
{
    _REENT_STDIN(_REENT)  = _REENT_STDIN(_GLOBAL_REENT);
    _REENT_STDOUT(_REENT) = _REENT_STDOUT(_GLOBAL_REENT);
    _REENT_STDERR(_REENT) = _REENT_STDERR(_GLOBAL_REENT);
}

static void *espix_pthread_trampoline(void *arg)
{
    espix_pthread_start_t *const st = arg;

    void *(*const fn)(void *) = st->fn;
    void *const user_arg      = st->arg;
    espix_proc_slot_t *const slot = st->slot;
    free(st);

    /* The thread's first act, before the app's routine can allocate. */
    vTaskSetThreadLocalStoragePointer(NULL, ESPIX_TLS_PROC_IDX, slot);

    espix_pthread_take_streams(slot);

    void *const ret = fn(user_arg);

    /* Before the thread is deleted, or its death closes the process's streams;
     * see the note on the pair above. */
    espix_pthread_drop_streams();

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

/*
 * pthread_exit() unwinds without returning through the trampoline's tail, so
 * the streams have to be taken back here instead -- otherwise a thread that
 * ends this way leaves the process's stdout in its reent for _reclaim_reent()
 * to close. It is answered below by the loader, so this has to be a resolver
 * entry rather than a table one, and it is the only pthread name espix
 * shadows.
 */
static void espix_pthread_exit(void *retval)
{
    espix_pthread_drop_streams();
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
 * Not here: the six the loader already answers for (a duplicate would be a dead
 * entry, and check-abi says so), and pthread_cancel, whose IDF implementation
 * is a stub -- exporting one would advertise a guarantee that does not exist.
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
