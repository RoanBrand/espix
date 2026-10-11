/*
 * The registry of app threads: which process each belongs to, its task and its
 * tid. Pure bookkeeping with no FreeRTOS in it, so the host unit tests compile
 * it as it is; locking is the caller's (abi_pthread.c holds the proc lock).
 *
 * An entry is reserved by pthread_create, before the thread exists, and given
 * its task by the thread itself -- so a full table is a refusal, never a thread
 * nobody can reach. Until then its task is NULL.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const void *slot;       /* NULL marks a free entry */
    void       *task;
    void       *reent;      /* its newlib reent, so a kill can take back stdio */
    int32_t     tid;
} espix_thread_t;

typedef struct {
    espix_thread_t *v;
    size_t          cap;
} espix_threads_t;

/* Reserve an entry for a thread about to start. False when the table is full. */
bool espix_threads_add(espix_threads_t *t, const void *slot, int32_t tid);

/* Give reserved entry `tid` its task and reent. False when it is gone -- its
 * process ended before the thread got this far. */
bool espix_threads_set_task(espix_threads_t *t, int32_t tid, void *task,
                            void *reent);

/* Forget entry `tid`, copying it to `out` when given. */
bool espix_threads_remove(espix_threads_t *t, int32_t tid, espix_thread_t *out);

/* The tid registered for `task`, or -1. */
int32_t espix_threads_tid_of(const espix_threads_t *t, const void *task);

/* Copy out up to `max` of `slot`'s threads, in table order; returns how many
 * the slot has, which may exceed `max`. */
size_t espix_threads_of(const espix_threads_t *t, const void *slot,
                        espix_thread_t *out, size_t max);

/* Remove the first of `slot`'s threads into `out`. False when it has none. */
bool espix_threads_take(espix_threads_t *t, const void *slot,
                        espix_thread_t *out);

#ifdef __cplusplus
}
#endif
