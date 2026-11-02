/* picolibc's <signal.h>, and the thread call it lacks. */
#pragma once

#include_next <signal.h>

int pthread_sigmask(int how, const sigset_t *set, sigset_t *old);
