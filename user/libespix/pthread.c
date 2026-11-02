/*
 * POSIX threads for U-mode apps. A thread is the kernel's (THREAD_CREATE); the
 * rest is built here on its futexes, as musl builds it on Linux's: joins,
 * mutexes, condition variables, rwlocks, barriers, spinlocks, the locks
 * picolibc's stdio and malloc take, and semaphores (docs/USER-MODE.md).
 */
#include <errno.h>
#include <limits.h>
#include <picotls.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/lock.h>
#include <time.h>
#include <unistd.h>

#include "sys.h"

/* LR/SC, never an AMO: a U-mode AMO can lose an update against the other core (docs/KNOWN-ISSUES.md). */
static inline uint32_t xchg(volatile void *p, uint32_t v)
{
    uint32_t old, fail;
    __asm__ volatile ("1: lr.w.aqrl %0, (%2)\n"
                      "   sc.w.rl   %1, %3, (%2)\n"
                      "   bnez      %1, 1b"
                      : "=&r"(old), "=&r"(fail) : "r"(p), "r"(v) : "memory");
    return old;
}

static inline int fetch_add(volatile void *p, int d)
{
    int old, sum, fail;
    __asm__ volatile ("1: lr.w.aqrl %0, (%3)\n"
                      "   add       %1, %0, %4\n"
                      "   sc.w.rl   %2, %1, (%3)\n"
                      "   bnez      %2, 1b"
                      : "=&r"(old), "=&r"(sum), "=&r"(fail) : "r"(p), "r"(d) : "memory");
    return old;
}

#define KEYS_MAX  32
#define DTOR_ROUNDS 4
#define UNSET     0xFFFFFFFFu   /* the static initializers' value */

enum { JOINABLE, DETACHED, EXITING };

/* A thread: its join word, result and keys, its TLS block after it. */
struct thread {
    volatile int    tid;    /* nonzero while it runs; the kernel zeroes it and wakes joiners at the end */
    int             state;
    void         *(*fn)(void *);
    void           *arg;
    void           *ret;
    void           *keys[KEYS_MAX];
    struct thread  *next;   /* a detached thread that has ended, until the kernel lets go of it */
};

static struct thread s_main;
static __thread struct thread *t_self;
static int s_live = 1;              /* threads running, the main one included */
static struct thread *s_ended;

static struct thread *self(void)
{
    struct thread *const t = t_self;
    return (t != NULL) ? t : &s_main;
}

/* ------------------------------------------------------------------ futexes */

static int futex_wait(volatile int *w, int val, const struct timespec *rel)
{
    struct espix_timespec ts = { 0 };
    if (rel != NULL) {
        ts.sec  = rel->tv_sec;
        ts.nsec = (int32_t)rel->tv_nsec;
    }
    return (int)espix_sys6(ESPIX_SYS_FUTEX, (long)w, ESPIX_FUTEX_WAIT, val, rel ? (long)&ts : 0, 0, 0);
}

static void futex_wake(volatile int *w, int n)
{
    (void)espix_sys3(ESPIX_SYS_FUTEX, w, ESPIX_FUTEX_WAKE, n);
}

/* What is left until abs on clk; false once it has passed. */
static bool until(clockid_t clk, const struct timespec *abs, struct timespec *rel)
{
    struct timespec now;
    if (clock_gettime(clk, &now) != 0) {
        return false;
    }
    rel->tv_sec  = abs->tv_sec - now.tv_sec;
    rel->tv_nsec = abs->tv_nsec - now.tv_nsec;
    if (rel->tv_nsec < 0) {
        rel->tv_nsec += 1000000000L;
        rel->tv_sec--;
    }
    return rel->tv_sec > 0 || (rel->tv_sec == 0 && rel->tv_nsec > 0);
}

/* 0 free, 1 held, 2 held with waiters: Drepper's "Futexes Are Tricky", mutex 2. */
static int word_lock(volatile int *w, clockid_t clk, const struct timespec *abs)
{
    int c = 0;
    if (__atomic_compare_exchange_n(w, &c, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        return 0;
    }
    if (c != 2) {
        c = (int)xchg(w, 2);
    }
    while (c != 0) {
        struct timespec rel;
        if (abs != NULL && !until(clk, abs, &rel)) {
            return ETIMEDOUT;
        }
        futex_wait(w, 2, abs ? &rel : NULL);
        c = (int)xchg(w, 2);
    }
    return 0;
}

static bool word_trylock(volatile int *w)
{
    int c = 0;
    return __atomic_compare_exchange_n(w, &c, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

static void word_unlock(volatile int *w)
{
    if (xchg(w, 0) == 2) {
        futex_wake(w, 1);
    }
}

/* ------------------------------------------------------------------ picolibc's locks */

struct __lock {
    volatile int   word;
    struct thread *owner;
    int            count;
};

struct __lock __lock___libc_recursive_mutex;

void __retarget_lock_init(_LOCK_T *l)           { *l = calloc(1, sizeof(struct __lock)); }
void __retarget_lock_init_recursive(_LOCK_T *l) { *l = calloc(1, sizeof(struct __lock)); }
void __retarget_lock_close(_LOCK_T l)           { free(l); }
void __retarget_lock_close_recursive(_LOCK_T l) { free(l); }

void __retarget_lock_acquire(_LOCK_T l)
{
    if (l != NULL) {
        word_lock(&l->word, CLOCK_MONOTONIC, NULL);
    }
}

void __retarget_lock_release(_LOCK_T l)
{
    if (l != NULL) {
        word_unlock(&l->word);
    }
}

static void lock_recursive(struct __lock *l)
{
    struct thread *const me = self();
    if (l->owner == me) {
        l->count++;
        return;
    }
    word_lock(&l->word, CLOCK_MONOTONIC, NULL);
    l->owner = me;
    l->count = 1;
}

static void unlock_recursive(struct __lock *l)
{
    if (--l->count == 0) {
        l->owner = NULL;
        word_unlock(&l->word);
    }
}

void __retarget_lock_acquire_recursive(_LOCK_T l)
{
    if (l != NULL) {
        lock_recursive(l);
    }
}

void __retarget_lock_release_recursive(_LOCK_T l)
{
    if (l != NULL) {
        unlock_recursive(l);
    }
}

/* ------------------------------------------------------------------ objects behind handles */

/* The object a handle names, made on first use when a static initializer (or zero) left none. */
static void *object(volatile uint32_t *h, size_t size, void (*init)(void *))
{
    uint32_t v = __atomic_load_n(h, __ATOMIC_ACQUIRE);
    if (v != UNSET && v != 0) {
        return (void *)(uintptr_t)v;
    }
    void *const o = calloc(1, size);
    if (o == NULL) {
        return NULL;
    }
    if (init != NULL) {
        init(o);
    }
    if (!__atomic_compare_exchange_n(h, &v, (uint32_t)(uintptr_t)o, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        free(o);
        return (void *)(uintptr_t)v;
    }
    return o;
}

static void object_free(volatile uint32_t *h)
{
    const uint32_t v = xchg(h, UNSET);
    if (v != UNSET && v != 0) {
        free((void *)(uintptr_t)v);
    }
}

/* ------------------------------------------------------------------ threads */

/* Detached threads that ended, freed once the kernel has zeroed their tid. */
static void reap_ended(void)
{
    struct thread *t = (struct thread *)(uintptr_t)xchg(&s_ended, 0);
    while (t != NULL) {
        struct thread *const next = t->next;
        if (t->tid == 0) {
            free(t);
        } else {
            t->next = __atomic_load_n(&s_ended, __ATOMIC_ACQUIRE);
            while (!__atomic_compare_exchange_n(&s_ended, &t->next, t, false,
                                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            }
        }
        t = next;
    }
}

void __espix_thread_entry(void);

void __espix_thread_start(struct thread *t)
{
    t_self = t;
    pthread_exit(t->fn(t->arg));
}

int pthread_create(pthread_t *out, const pthread_attr_t *attr, void *(*fn)(void *), void *arg)
{
    reap_ended();
    const size_t align = (_tls_align() > 16) ? _tls_align() : 16;
    struct thread *const t = calloc(1, sizeof(*t) + align + _tls_size());
    if (t == NULL) {
        return EAGAIN;
    }
    void *const tls = (void *)(((uintptr_t)(t + 1) + align - 1) & ~(uintptr_t)(align - 1));
    _init_tls(tls);
    t->fn    = fn;
    t->arg   = arg;
    t->tid   = -1;      /* the thread writes its own before it runs */
    t->state = (attr != NULL && attr->detachstate == PTHREAD_CREATE_DETACHED) ? DETACHED : JOINABLE;
    const uintptr_t stack = (attr != NULL && attr->stackaddr != NULL)
                          ? (uintptr_t)attr->stackaddr + (size_t)attr->stacksize : 0;

    (void)fetch_add(&s_live, 1);
    const long r = espix_sys5(ESPIX_SYS_THREAD_CREATE, __espix_thread_entry, t, tls, stack, &t->tid);
    if (r < 0) {
        (void)fetch_add(&s_live, -1);
        free(t);
        return (int)-r;
    }
    *out = (pthread_t)(uintptr_t)t;
    return 0;
}

static void run_destructors(struct thread *t);

void pthread_exit(void *ret)
{
    struct thread *const t = self();
    t->ret = ret;
    run_destructors(t);
    /* The last thread out ends the process as exit(0) would, atexit handlers and all. */
    if (fetch_add(&s_live, -1) == 1) {
        exit(0);
    }
    if (t != &s_main &&
        xchg(&t->state, EXITING) == DETACHED) {
        t->next = __atomic_load_n(&s_ended, __ATOMIC_ACQUIRE);
        while (!__atomic_compare_exchange_n(&s_ended, &t->next, t, false,
                                            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        }
    }
    espix_sys0(ESPIX_SYS_THREAD_EXIT);
    for (;;) {
    }
}

static void join_wait(struct thread *t)
{
    for (int tid; (tid = t->tid) != 0;) {
        futex_wait(&t->tid, tid, NULL);
    }
}

int pthread_join(pthread_t th, void **ret)
{
    struct thread *const t = (struct thread *)(uintptr_t)th;
    if (t == self()) {
        return EDEADLK;
    }
    if (t == &s_main || t->state == DETACHED) {
        return EINVAL;
    }
    join_wait(t);
    if (ret != NULL) {
        *ret = t->ret;
    }
    free(t);
    return 0;
}

int pthread_detach(pthread_t th)
{
    struct thread *const t = (struct thread *)(uintptr_t)th;
    if (t == &s_main) {
        return EINVAL;
    }
    /* Already on its way out as a joinable thread: join it here instead. */
    if (xchg(&t->state, DETACHED) == EXITING) {
        join_wait(t);
        free(t);
    }
    return 0;
}

pthread_t pthread_self(void)              { return (pthread_t)(uintptr_t)self(); }
int pthread_equal(pthread_t a, pthread_t b) { return a == b; }
int sched_yield(void)                      { return (int)espix_sys0(ESPIX_SYS_SCHED_YIELD); }
void pthread_yield(void)                   { sched_yield(); }
pid_t gettid(void)                         { return (pid_t)espix_sys0(ESPIX_SYS_GETTID); }
int sched_getcpu(void)                     { return (int)espix_sys0(ESPIX_SYS_GETCPU); }
int pthread_setconcurrency(int level)      { (void)level; return 0; }
int pthread_getconcurrency(void)           { return 0; }

int pthread_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void))
{
    (void)prepare; (void)parent; (void)child;
    return 0;       /* no fork() yet (M8), so nothing to run */
}

int pthread_setname_np(pthread_t th, const char *name) { (void)th; (void)name; return 0; }

int pthread_getname_np(pthread_t th, char *buf, size_t len)
{
    (void)th;
    if (len > 0) {
        buf[0] = '\0';
    }
    return 0;
}

/* ------------------------------------------------------------------ attributes */

int pthread_attr_init(pthread_attr_t *a)
{
    *a = (pthread_attr_t){ .is_initialized = 1, .detachstate = PTHREAD_CREATE_JOINABLE };
    return 0;
}

int pthread_attr_destroy(pthread_attr_t *a) { a->is_initialized = 0; return 0; }

int pthread_attr_setdetachstate(pthread_attr_t *a, int s)
{
    if (s != PTHREAD_CREATE_JOINABLE && s != PTHREAD_CREATE_DETACHED) {
        return EINVAL;
    }
    a->detachstate = s;
    return 0;
}

int pthread_attr_getdetachstate(const pthread_attr_t *a, int *s) { *s = a->detachstate; return 0; }

/* A stack the kernel gives grows as touched up to 8 MB, so a size is only remembered. */
int pthread_attr_setstacksize(pthread_attr_t *a, size_t n)
{
    a->stacksize = (int)n;
    return 0;
}

int pthread_attr_getstacksize(const pthread_attr_t *a, size_t *n) { *n = (size_t)a->stacksize; return 0; }

int pthread_attr_setstack(pthread_attr_t *a, void *addr, size_t n)
{
    a->stackaddr = addr;
    a->stacksize = (int)n;
    return 0;
}

int pthread_attr_getstack(const pthread_attr_t *a, void **addr, size_t *n)
{
    *addr = a->stackaddr;
    *n    = (size_t)a->stacksize;
    return 0;
}

int pthread_attr_setguardsize(pthread_attr_t *a, size_t n)        { (void)a; (void)n; return 0; }
int pthread_attr_getguardsize(const pthread_attr_t *a, size_t *n) { (void)a; *n = 4096; return 0; }

/* ------------------------------------------------------------------ once and keys */

int pthread_once(pthread_once_t *o, void (*fn)(void))
{
    volatile int *const st = &o->init_executed;     /* 0 not run, 1 running, 2 done */
    int c = 0;
    if (__atomic_compare_exchange_n(st, &c, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE)) {
        fn();
        __atomic_store_n(st, 2, __ATOMIC_RELEASE);
        futex_wake(st, INT_MAX);
        return 0;
    }
    while ((c = __atomic_load_n(st, __ATOMIC_ACQUIRE)) != 2) {
        futex_wait(st, c, NULL);
    }
    return 0;
}

static struct __lock s_keys_lock;
static bool          s_key_used[KEYS_MAX];
static void        (*s_key_dtor[KEYS_MAX])(void *);

int pthread_key_create(pthread_key_t *k, void (*dtor)(void *))
{
    int r = EAGAIN;
    lock_recursive(&s_keys_lock);
    for (int i = 0; i < KEYS_MAX; i++) {
        if (!s_key_used[i]) {
            s_key_used[i] = true;
            s_key_dtor[i] = dtor;
            *k = (pthread_key_t)i;
            r  = 0;
            break;
        }
    }
    unlock_recursive(&s_keys_lock);
    return r;
}

int pthread_key_delete(pthread_key_t k)
{
    if (k >= KEYS_MAX) {
        return EINVAL;
    }
    lock_recursive(&s_keys_lock);
    s_key_used[k] = false;
    s_key_dtor[k] = NULL;
    unlock_recursive(&s_keys_lock);
    return 0;
}

int pthread_setspecific(pthread_key_t k, const void *v)
{
    if (k >= KEYS_MAX || !s_key_used[k]) {
        return EINVAL;
    }
    self()->keys[k] = (void *)v;
    return 0;
}

void *pthread_getspecific(pthread_key_t k)
{
    return (k < KEYS_MAX) ? self()->keys[k] : NULL;
}

static void run_destructors(struct thread *t)
{
    for (int round = 0; round < DTOR_ROUNDS; round++) {
        bool any = false;
        for (int i = 0; i < KEYS_MAX; i++) {
            void *const v = t->keys[i];
            void (*const dtor)(void *) = s_key_dtor[i];
            if (v != NULL && dtor != NULL) {
                t->keys[i] = NULL;
                dtor(v);
                any = true;
            }
        }
        if (!any) {
            break;
        }
    }
}

/* ------------------------------------------------------------------ mutexes */

struct mutex {
    struct __lock lk;
    int           type;
};

int pthread_mutexattr_init(pthread_mutexattr_t *a)
{
    *a = (pthread_mutexattr_t){ .is_initialized = 1, .type = PTHREAD_MUTEX_DEFAULT };
    return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t *a) { a->is_initialized = 0; return 0; }

int pthread_mutexattr_settype(pthread_mutexattr_t *a, int type)
{
    if (type < PTHREAD_MUTEX_NORMAL || type > PTHREAD_MUTEX_DEFAULT) {
        return EINVAL;
    }
    a->type      = type;
    a->recursive = (type == PTHREAD_MUTEX_RECURSIVE);
    return 0;
}

int pthread_mutexattr_gettype(const pthread_mutexattr_t *a, int *type) { *type = a->type; return 0; }

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a)
{
    struct mutex *const mx = calloc(1, sizeof(*mx));
    if (mx == NULL) {
        return ENOMEM;
    }
    mx->type = (a != NULL && a->type != PTHREAD_MUTEX_DEFAULT) ? a->type : PTHREAD_MUTEX_NORMAL;
    *m = (pthread_mutex_t)(uintptr_t)mx;
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *m)
{
    object_free(m);
    return 0;
}

static int mutex_lock(pthread_mutex_t *m, const struct timespec *abs)
{
    struct mutex *const mx = object(m, sizeof(struct mutex), NULL);
    if (mx == NULL) {
        return ENOMEM;
    }
    struct thread *const me = self();
    if (mx->lk.owner == me) {
        if (mx->type == PTHREAD_MUTEX_RECURSIVE) {
            mx->lk.count++;
            return 0;
        }
        if (mx->type == PTHREAD_MUTEX_ERRORCHECK) {
            return EDEADLK;
        }
    }
    const int r = word_lock(&mx->lk.word, CLOCK_REALTIME, abs);
    if (r == 0) {
        mx->lk.owner = me;
        mx->lk.count = 1;
    }
    return r;
}

int pthread_mutex_lock(pthread_mutex_t *m) { return mutex_lock(m, NULL); }

int pthread_mutex_timedlock(pthread_mutex_t *m, const struct timespec *abs)
{
    return mutex_lock(m, abs);
}

int pthread_mutex_trylock(pthread_mutex_t *m)
{
    struct mutex *const mx = object(m, sizeof(struct mutex), NULL);
    if (mx == NULL) {
        return ENOMEM;
    }
    struct thread *const me = self();
    if (mx->lk.owner == me && mx->type == PTHREAD_MUTEX_RECURSIVE) {
        mx->lk.count++;
        return 0;
    }
    if (!word_trylock(&mx->lk.word)) {
        return EBUSY;
    }
    mx->lk.owner = me;
    mx->lk.count = 1;
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *m)
{
    struct mutex *const mx = object(m, sizeof(struct mutex), NULL);
    if (mx == NULL) {
        return EINVAL;
    }
    if (mx->type != PTHREAD_MUTEX_NORMAL && mx->lk.owner != self()) {
        return EPERM;
    }
    if (mx->type == PTHREAD_MUTEX_RECURSIVE && --mx->lk.count > 0) {
        return 0;
    }
    mx->lk.owner = NULL;
    word_unlock(&mx->lk.word);
    return 0;
}

/* ------------------------------------------------------------------ condition variables */

struct cond {
    volatile int seq;
    clockid_t    clk;
};

static void cond_default(void *o)
{
    ((struct cond *)o)->clk = CLOCK_REALTIME;
}

int pthread_condattr_init(pthread_condattr_t *a)
{
    *a = (pthread_condattr_t){ .is_initialized = 1, .clock = CLOCK_REALTIME };
    return 0;
}

int pthread_condattr_destroy(pthread_condattr_t *a) { a->is_initialized = 0; return 0; }
int pthread_condattr_setclock(pthread_condattr_t *a, clockid_t c) { a->clock = (clock_t)c; return 0; }

int pthread_condattr_getclock(const pthread_condattr_t *a, clockid_t *c)
{
    *c = (clockid_t)a->clock;
    return 0;
}

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a)
{
    struct cond *const cv = calloc(1, sizeof(*cv));
    if (cv == NULL) {
        return ENOMEM;
    }
    cv->clk = (a != NULL) ? (clockid_t)a->clock : CLOCK_REALTIME;
    *c = (pthread_cond_t)(uintptr_t)cv;
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *c)
{
    object_free(c);
    return 0;
}

static int cond_wait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *abs)
{
    struct cond *const cv = object(c, sizeof(struct cond), cond_default);
    struct mutex *const mx = object(m, sizeof(struct mutex), NULL);
    if (cv == NULL || mx == NULL) {
        return ENOMEM;
    }
    const int seq   = cv->seq;
    const int count = mx->lk.count;
    mx->lk.owner = NULL;
    mx->lk.count = 0;
    word_unlock(&mx->lk.word);

    int r = 0;
    struct timespec rel;
    if (abs != NULL && !until(cv->clk, abs, &rel)) {
        r = ETIMEDOUT;
    } else if (futex_wait(&cv->seq, seq, abs ? &rel : NULL) == -ETIMEDOUT) {
        r = ETIMEDOUT;
    }

    word_lock(&mx->lk.word, CLOCK_MONOTONIC, NULL);
    mx->lk.owner = self();
    mx->lk.count = count;
    return r;
}

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) { return cond_wait(c, m, NULL); }

int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *abs)
{
    return cond_wait(c, m, abs);
}

static int cond_wake(pthread_cond_t *c, int n)
{
    struct cond *const cv = object(c, sizeof(struct cond), cond_default);
    if (cv == NULL) {
        return ENOMEM;
    }
    (void)fetch_add(&cv->seq, 1);
    futex_wake(&cv->seq, n);
    return 0;
}

int pthread_cond_signal(pthread_cond_t *c)    { return cond_wake(c, 1); }
int pthread_cond_broadcast(pthread_cond_t *c) { return cond_wake(c, INT_MAX); }

/* ------------------------------------------------------------------ rwlocks */

struct rwlock {
    volatile int lock;
    int          readers;
    bool         writer;
    volatile int seq;
};

int pthread_rwlockattr_init(pthread_rwlockattr_t *a)    { a->is_initialized = 1; return 0; }
int pthread_rwlockattr_destroy(pthread_rwlockattr_t *a) { a->is_initialized = 0; return 0; }

int pthread_rwlock_init(pthread_rwlock_t *rw, const pthread_rwlockattr_t *a)
{
    (void)a;
    struct rwlock *const l = calloc(1, sizeof(*l));
    if (l == NULL) {
        return ENOMEM;
    }
    *rw = (pthread_rwlock_t)(uintptr_t)l;
    return 0;
}

int pthread_rwlock_destroy(pthread_rwlock_t *rw)
{
    object_free(rw);
    return 0;
}

static int rw_take(pthread_rwlock_t *rw, bool write, bool wait, const struct timespec *abs)
{
    struct rwlock *const l = object(rw, sizeof(struct rwlock), NULL);
    if (l == NULL) {
        return ENOMEM;
    }
    for (;;) {
        word_lock(&l->lock, CLOCK_MONOTONIC, NULL);
        if (!l->writer && (!write || l->readers == 0)) {
            if (write) {
                l->writer = true;
            } else {
                l->readers++;
            }
            word_unlock(&l->lock);
            return 0;
        }
        const int seq = l->seq;
        word_unlock(&l->lock);
        if (!wait) {
            return EBUSY;
        }
        struct timespec rel;
        if (abs != NULL && !until(CLOCK_REALTIME, abs, &rel)) {
            return ETIMEDOUT;
        }
        futex_wait(&l->seq, seq, abs ? &rel : NULL);
    }
}

int pthread_rwlock_rdlock(pthread_rwlock_t *rw)    { return rw_take(rw, false, true, NULL); }
int pthread_rwlock_wrlock(pthread_rwlock_t *rw)    { return rw_take(rw, true, true, NULL); }
int pthread_rwlock_tryrdlock(pthread_rwlock_t *rw) { return rw_take(rw, false, false, NULL); }
int pthread_rwlock_trywrlock(pthread_rwlock_t *rw) { return rw_take(rw, true, false, NULL); }

int pthread_rwlock_timedrdlock(pthread_rwlock_t *rw, const struct timespec *abs)
{
    return rw_take(rw, false, true, abs);
}

int pthread_rwlock_timedwrlock(pthread_rwlock_t *rw, const struct timespec *abs)
{
    return rw_take(rw, true, true, abs);
}

int pthread_rwlock_unlock(pthread_rwlock_t *rw)
{
    struct rwlock *const l = object(rw, sizeof(struct rwlock), NULL);
    if (l == NULL) {
        return EINVAL;
    }
    word_lock(&l->lock, CLOCK_MONOTONIC, NULL);
    if (l->writer) {
        l->writer = false;
    } else if (l->readers > 0) {
        l->readers--;
    }
    l->seq++;
    word_unlock(&l->lock);
    futex_wake(&l->seq, INT_MAX);
    return 0;
}

/* ------------------------------------------------------------------ barriers and spinlocks */

struct barrier {
    volatile int lock;
    unsigned     count, waiting;
    volatile int seq;
};

int pthread_barrierattr_init(pthread_barrierattr_t *a)    { a->is_initialized = 1; return 0; }
int pthread_barrierattr_destroy(pthread_barrierattr_t *a) { a->is_initialized = 0; return 0; }

int pthread_barrier_init(pthread_barrier_t *b, const pthread_barrierattr_t *a, unsigned count)
{
    (void)a;
    if (count == 0) {
        return EINVAL;
    }
    struct barrier *const br = calloc(1, sizeof(*br));
    if (br == NULL) {
        return ENOMEM;
    }
    br->count = count;
    *b = (pthread_barrier_t)(uintptr_t)br;
    return 0;
}

int pthread_barrier_destroy(pthread_barrier_t *b)
{
    object_free(b);
    return 0;
}

int pthread_barrier_wait(pthread_barrier_t *b)
{
    struct barrier *const br = (struct barrier *)(uintptr_t)*b;
    word_lock(&br->lock, CLOCK_MONOTONIC, NULL);
    if (++br->waiting == br->count) {
        br->waiting = 0;
        br->seq++;
        word_unlock(&br->lock);
        futex_wake(&br->seq, INT_MAX);
        return PTHREAD_BARRIER_SERIAL_THREAD;
    }
    const int seq = br->seq;
    word_unlock(&br->lock);
    while (__atomic_load_n(&br->seq, __ATOMIC_ACQUIRE) == seq) {
        futex_wait(&br->seq, seq, NULL);
    }
    return 0;
}

int pthread_spin_init(pthread_spinlock_t *s, int pshared) { (void)pshared; *s = 0; return 0; }
int pthread_spin_destroy(pthread_spinlock_t *s)           { (void)s; return 0; }

int pthread_spin_lock(pthread_spinlock_t *s)
{
    while (xchg(s, 1) != 0) {
        sched_yield();      /* the holder may be on this core */
    }
    return 0;
}

int pthread_spin_trylock(pthread_spinlock_t *s)
{
    return (xchg(s, 1) == 0) ? 0 : EBUSY;
}

int pthread_spin_unlock(pthread_spinlock_t *s)
{
    __atomic_store_n(s, 0, __ATOMIC_RELEASE);
    return 0;
}

/* ------------------------------------------------------------------ semaphores */

int sem_init(sem_t *s, int pshared, unsigned value)
{
    if (pshared != 0) {
        errno = ENOSYS;
        return -1;
    }
    if (value > SEM_VALUE_MAX) {
        errno = EINVAL;
        return -1;
    }
    s->value = (int)value;
    return 0;
}

int sem_destroy(sem_t *s) { (void)s; return 0; }

int sem_trywait(sem_t *s)
{
    int v = __atomic_load_n(&s->value, __ATOMIC_ACQUIRE);
    while (v > 0) {
        if (__atomic_compare_exchange_n(&s->value, &v, v - 1, false, __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE)) {
            return 0;
        }
    }
    errno = EAGAIN;
    return -1;
}

static int sem_take(sem_t *s, const struct timespec *abs)
{
    for (;;) {
        if (sem_trywait(s) == 0) {
            return 0;
        }
        struct timespec rel;
        if (abs != NULL && !until(CLOCK_REALTIME, abs, &rel)) {
            errno = ETIMEDOUT;
            return -1;
        }
        (void)fetch_add(&s->waiters, 1);
        const int r = futex_wait(&s->value, 0, abs ? &rel : NULL);
        (void)fetch_add(&s->waiters, -1);
        if (r == -EINTR) {
            errno = EINTR;
            return -1;
        }
    }
}

int sem_wait(sem_t *s)                                   { return sem_take(s, NULL); }
int sem_timedwait(sem_t *s, const struct timespec *abs) { return sem_take(s, abs); }

int sem_post(sem_t *s)
{
    if (fetch_add(&s->value, 1) >= SEM_VALUE_MAX) {
        (void)fetch_add(&s->value, -1);
        errno = EOVERFLOW;
        return -1;
    }
    if (__atomic_load_n(&s->waiters, __ATOMIC_SEQ_CST) > 0) {
        futex_wake(&s->value, 1);
    }
    return 0;
}

int sem_getvalue(sem_t *s, int *v)
{
    *v = __atomic_load_n(&s->value, __ATOMIC_ACQUIRE);
    return 0;
}
