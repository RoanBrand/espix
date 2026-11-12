/* A POSIX app without an address space (the S3): its entry and vfork. Its libc is its own, over /lib/libc.so. */
#if ESPIX_NOMMU_APP

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include "sys.h"

char **environ;

int main(int argc, char **argv, char **envp);
void espix_app_fini(void);

/* exit()'s destructors: the kernel ran the constructors, and knows the app's .fini_array. */
void __libc_fini_array(void)
{
    espix_app_fini();
}

int __espix_main(int argc, char **argv, char **envp)
{
    environ = envp;
    /* The first syscall makes the process's descriptor table, which its stdio writes to. */
    (void)espix_sys0(ESPIX_SYS_GETPID);
    exit(main(argc, argv, envp));
}

/* vfork(): the child's state and the parent's resume point, then the child on this task (unistd.h). */
jmp_buf *__espix_vfork_prepare(void)
{
    const long r = espix_sys1(ESPIX_SYS_VFORK, 0);
    if (r < 0 && r > -4096) {
        errno = (int)-r;
        return NULL;
    }
    return (jmp_buf *)r;
}

void __espix_vfork_enter(jmp_buf *jb)
{
    (void)espix_sys1(ESPIX_SYS_VFORK, (long)(uintptr_t)jb);
}

#endif /* ESPIX_NOMMU_APP */
