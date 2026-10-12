#include "proc_threads.h"

static espix_thread_t *find_tid(const espix_threads_t *t, int32_t tid)
{
    for (size_t i = 0; i < t->cap; i++) {
        if (t->v[i].slot != NULL && t->v[i].tid == tid) {
            return &t->v[i];
        }
    }
    return NULL;
}

bool espix_threads_add(espix_threads_t *t, const void *slot, int32_t tid)
{
    if (slot == NULL || find_tid(t, tid) != NULL) {
        return false;
    }
    for (size_t i = 0; i < t->cap; i++) {
        if (t->v[i].slot == NULL) {
            t->v[i] = (espix_thread_t){ .slot = slot, .tid = tid };
            return true;
        }
    }
    return false;
}

bool espix_threads_set_task(espix_threads_t *t, int32_t tid, void *task,
                            void *reent)
{
    espix_thread_t *const e = find_tid(t, tid);

    if (e == NULL) {
        return false;
    }
    e->task  = task;
    e->reent = reent;
    return true;
}

bool espix_threads_set_caps(espix_threads_t *t, int32_t tid)
{
    espix_thread_t *const e = find_tid(t, tid);

    if (e == NULL) {
        return false;
    }
    e->caps = true;
    return true;
}

bool espix_threads_remove(espix_threads_t *t, int32_t tid, espix_thread_t *out)
{
    espix_thread_t *const e = find_tid(t, tid);

    if (e == NULL) {
        return false;
    }
    if (out != NULL) {
        *out = *e;
    }
    *e = (espix_thread_t){ 0 };
    return true;
}

int32_t espix_threads_tid_of(const espix_threads_t *t, const void *task)
{
    if (task == NULL) {
        return -1;
    }
    for (size_t i = 0; i < t->cap; i++) {
        if (t->v[i].slot != NULL && t->v[i].task == task) {
            return t->v[i].tid;
        }
    }
    return -1;
}

size_t espix_threads_of(const espix_threads_t *t, const void *slot,
                        espix_thread_t *out, size_t max)
{
    size_t n = 0;

    for (size_t i = 0; i < t->cap; i++) {
        if (slot != NULL && t->v[i].slot == slot) {
            if (n < max && out != NULL) {
                out[n] = t->v[i];
            }
            n++;
        }
    }
    return n;
}

bool espix_threads_take(espix_threads_t *t, const void *slot,
                        espix_thread_t *out)
{
    for (size_t i = 0; i < t->cap; i++) {
        if (slot != NULL && t->v[i].slot == slot) {
            if (out != NULL) {
                *out = t->v[i];
            }
            t->v[i] = (espix_thread_t){ 0 };
            return true;
        }
    }
    return false;
}
