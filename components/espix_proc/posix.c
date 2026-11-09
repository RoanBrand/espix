/*
 * A POSIX app without an address space (an M-mode app built by espix-cc): its descriptor
 * table and the rest of a U-mode process's syscall state, made on its first syscall. The
 * syscalls are the ones a U-mode app makes (umode_sys.c), reached by a direct call to
 * espix_posix_syscall(); its threads and signals are an M-mode app's, through the ABI.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "espix_posix.h"

/* The state and its stdio: stdin, stdout and stderr as streams over descriptors 0-2, so dup2 onto them redirects. */
typedef struct {
    espix_um_thread_t t;
    FILE             *std[3];
} posix_t;

static int std_io(void *cookie, char *buf, int len, bool out)
{
    espix_um_thread_t *const t = espix_posix_thread_of(espix_proc_self());
    if (t == NULL || len <= 0) {
        return 0;
    }
    const int32_t r = espix_posix_rw(t, (int)(intptr_t)cookie - 1, buf, (size_t)len, out);
    return (r < 0) ? (errno = (int)-r, -1) : (int)r;
}

static int std_read(void *cookie, char *buf, int len)
{
    return std_io(cookie, buf, len, false);
}

static int std_write(void *cookie, const char *buf, int len)
{
    return std_io(cookie, (char *)buf, len, true);
}

/* The process's streams become the routed ones; the session's stay behind fds 0-2 as their STD entries. */
static void std_route(posix_t *x, espix_proc_slot_t *slot)
{
    for (int fd = 0; fd < 3; fd++) {
        FILE *const f = funopen((void *)(intptr_t)(fd + 1), fd == 0 ? std_read : NULL, fd > 0 ? std_write : NULL,
                                NULL, NULL);
        if (f == NULL) {
            continue;
        }
        setvbuf(f, NULL, (fd == 1) ? _IOLBF : _IONBF, (fd == 1) ? 128 : 0);
        x->std[fd]     = f;
        slot->std[fd]  = f;
    }
}

espix_um_thread_t *espix_posix_thread_of(espix_proc_slot_t *slot)
{
    if (slot == NULL || slot->app == NULL) {
        return NULL;
    }
#if CONFIG_ESPIX_UMODE
    if (slot->app->um != NULL) {
        return slot->app->um;
    }
#endif
    return slot->app->posix;
}

espix_um_thread_t *espix_posix_self(void)
{
    espix_proc_slot_t *const slot = espix_proc_self();
    if (slot == NULL || slot->app == NULL || slot->info.kind != ESPIX_PROC_APP) {
        return NULL;
    }
    espix_um_thread_t *t = __atomic_load_n((espix_um_thread_t **)&slot->app->posix, __ATOMIC_ACQUIRE);
    if (t != NULL) {
        return t;
    }
    posix_t *const x = calloc(1, sizeof(*x));
    espix_um_proc_t *const p = calloc(1, sizeof(*p));
    SemaphoreHandle_t lock = xSemaphoreCreateMutex();
    if (x == NULL || p == NULL || lock == NULL) {
        free(x);
        free(p);
        if (lock != NULL) {
            vSemaphoreDelete(lock);
        }
        return NULL;
    }
    t = &x->t;
    p->lock    = lock;
    p->threads = 1;
    espix_um_fds_init(p);
    t->proc = p;
    t->slot = slot;
    t->tid  = slot->info.pid;
    /* Two threads' first calls at once: one state wins, the other is given back. */
    espix_um_thread_t *none = NULL;
    if (!__atomic_compare_exchange_n((espix_um_thread_t **)&slot->app->posix, &none, t, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        vSemaphoreDelete(lock);
        free(p);
        free(x);
        return none;
    }
    std_route(x, slot);
    return t;
}

void espix_posix_release(espix_proc_slot_t *slot)
{
    espix_um_thread_t *const t = (slot->app != NULL) ? slot->app->posix : NULL;
    if (t == NULL) {
        return;
    }
    posix_t *const x = (posix_t *)t;
    /* Flushed already by an end the process made itself. A killed one's task may have died holding a
     * stream's lock, and a close would wait on it for ever: those are left, as a kill leaves its others. */
    for (int fd = 0; fd < 3; fd++) {
        if (x->std[fd] != NULL) {
            if (slot->std[fd] == x->std[fd]) {
                slot->std[fd] = (fd == 0) ? stdin : (fd == 1) ? stdout : stderr;
            }
            if (slot->exiting) {
                fclose(x->std[fd]);
            }
        }
    }
    slot->app->posix = NULL;
    vSemaphoreDelete(t->proc->lock);
    free(t->proc);
    free(x);
}

#if !CONFIG_ESPIX_UMODE
/* Surfaces and streams are U-mode's handles; an M-mode app's are its own to close. */
void espix_um_objs_release(espix_um_proc_t *p)
{
    (void)p;
}
#endif
