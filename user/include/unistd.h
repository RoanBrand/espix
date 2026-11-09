/* picolibc's <unistd.h>, plus gettid(), which glibc declares there too. */
#pragma once

#include_next <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

pid_t gettid(void);

#if defined(ESPIX_NOMMU_APP) && ESPIX_NOMMU_APP
#include <setjmp.h>

/*
 * vfork() without an MMU: the parent's resume point is a setjmp in the caller's own frame, which
 * the child then runs on, as Linux's child does on its parent's stack; the parent resumes there
 * with the child's pid once the child execs or exits (docs/USER-MODE.md).
 */
jmp_buf *__espix_vfork_prepare(void);
void     __espix_vfork_enter(jmp_buf *jb);

#define vfork() __extension__ ({                                    \
    jmp_buf *__espix_vjb = __espix_vfork_prepare();                 \
    pid_t __espix_vpid = -1;                                        \
    if (__espix_vjb != NULL) {                                      \
        __espix_vpid = (pid_t)setjmp(*__espix_vjb);                 \
        if (__espix_vpid == 0) {                                    \
            __espix_vfork_enter(__espix_vjb);                       \
        }                                                           \
    }                                                               \
    __espix_vpid;                                                   \
})
#endif

#ifdef __cplusplus
}
#endif
