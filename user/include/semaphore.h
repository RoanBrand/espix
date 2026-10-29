/* POSIX unnamed semaphores for U-mode apps, which picolibc lacks; libespix builds them on futexes. */
#pragma once

#include <limits.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SEM_VALUE_MAX INT_MAX

typedef struct {
    volatile int value;
    volatile int waiters;
} sem_t;

int sem_init(sem_t *s, int pshared, unsigned value);
int sem_destroy(sem_t *s);
int sem_wait(sem_t *s);
int sem_trywait(sem_t *s);
int sem_timedwait(sem_t *s, const struct timespec *abs);
int sem_post(sem_t *s);
int sem_getvalue(sem_t *s, int *value);

#ifdef __cplusplus
}
#endif
