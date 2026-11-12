/* A U-mode thread's TLS block, set up as picolibc's own would (user/uapp.ld names its parts), and errno in it. */
#include "tls.h"
#include <stdint.h>
#include <string.h>

extern char __tdata_source[], __tdata_size[], __tbss_size[], __tbss_offset[];

void _init_tls(void *tls)
{
    memcpy(tls, __tdata_source, (uintptr_t)__tdata_size);
    memset((char *)tls + (uintptr_t)__tbss_offset, 0, (uintptr_t)__tbss_size);
}

void _set_tls(void *tls)
{
    __asm__ volatile("mv tp, %0" : : "r"(tls));
}

static __thread int t_errno;

/* errno, one per thread: the app's code, its libc and libc.so's all reach it through this call. */
int *__errno(void)
{
    return &t_errno;
}
