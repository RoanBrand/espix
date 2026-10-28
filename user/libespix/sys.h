/* libespix's ecall and the -errno convention (docs/USER-MODE.md). */
#pragma once

#include <errno.h>

#include "espix_syscall.h"

static inline long espix_sys6(long n, long a, long b, long c, long d, long e, long f)
{
    register long a0 __asm__("a0") = a;
    register long a1 __asm__("a1") = b;
    register long a2 __asm__("a2") = c;
    register long a3 __asm__("a3") = d;
    register long a4 __asm__("a4") = e;
    register long a5 __asm__("a5") = f;
    register long a7 __asm__("a7") = n;
    __asm__ volatile ("ecall"
                      : "+r"(a0)
                      : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a7)
                      : "memory");
    return a0;
}

#define espix_sys0(n)             espix_sys6((n), 0, 0, 0, 0, 0, 0)
#define espix_sys1(n, a)          espix_sys6((n), (long)(a), 0, 0, 0, 0, 0)
#define espix_sys2(n, a, b)       espix_sys6((n), (long)(a), (long)(b), 0, 0, 0, 0)
#define espix_sys3(n, a, b, c)    espix_sys6((n), (long)(a), (long)(b), (long)(c), 0, 0, 0)
#define espix_sys5(n, a, b, c, d, e) \
    espix_sys6((n), (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), 0)

static inline long espix_ret(long r)
{
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    return r;
}
