/* Process identity, signals to processes, and the end of one. */
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <unistd.h>

#include "sys.h"

pid_t getpid(void)  { return (pid_t)espix_sys0(ESPIX_SYS_GETPID); }
pid_t getppid(void) { return (pid_t)espix_sys0(ESPIX_SYS_GETPPID); }
uid_t getuid(void)  { return (uid_t)espix_sys0(ESPIX_SYS_GETUID); }
uid_t geteuid(void) { return (uid_t)espix_sys0(ESPIX_SYS_GETEUID); }
gid_t getgid(void)  { return (gid_t)espix_sys0(ESPIX_SYS_GETGID); }
gid_t getegid(void) { return (gid_t)espix_sys0(ESPIX_SYS_GETEGID); }
pid_t getpgrp(void) { return (pid_t)espix_sys0(ESPIX_SYS_GETPGRP); }

pid_t getsid(pid_t pid)
{
    return (pid_t)espix_ret(espix_sys1(ESPIX_SYS_GETSID, pid));
}

int kill(pid_t pid, int sig)
{
    return espix_ret(espix_sys2(ESPIX_SYS_KILL, pid, sig));
}

long sysconf(int name)
{
    switch (name) {
    case _SC_OPEN_MAX:
        return ESPIX_OPEN_MAX;
    case _SC_PAGESIZE:
        return 4096;
    case _SC_CLK_TCK:
        /* The clock /proc's jiffies count in, not FreeRTOS's tick: htop and
         * every other Linux reader divides by this to turn a stat field into
         * seconds.  /proc generates at the same rate. */
        return ESPIX_USER_HZ;
    case _SC_NPROCESSORS_ONLN:
    case _SC_NPROCESSORS_CONF:
        return 2;   /* the S31's cores, the one U-mode target */
    case _SC_HOST_NAME_MAX:
        return 64;
    case _SC_GETPW_R_SIZE_MAX:
        return 512;
    case _SC_GETGR_R_SIZE_MAX:
        return 256;
    default:
        errno = EINVAL;
        return -1;
    }
}

static uintptr_t s_brk;

int brk(void *addr)
{
    s_brk = (uintptr_t)espix_sys1(ESPIX_SYS_BRK, addr);
    if (s_brk != (uintptr_t)addr) {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

/* The heap grows by brk, a page mapped only when malloc's caller touches it. */
void *sbrk(ptrdiff_t incr)
{
    if (s_brk == 0) {
        s_brk = (uintptr_t)espix_sys1(ESPIX_SYS_BRK, 0);
    }
    const uintptr_t old = s_brk;
    if (incr != 0 && brk((void *)(old + incr)) != 0) {
        return (void *)-1;
    }
    return (void *)old;
}

_Noreturn void _exit(int status)
{
    espix_sys1(ESPIX_SYS_EXIT, status);
    for (;;) {
    }
}

/* No Unix priority exists to read or set yet. Reporting 0 is what Linux does
 * for a process it cannot find (with errno cleared), and a set fails with
 * EPERM: htop's NICE column is truthful, and an attempt to change it says so. */
int getpriority(int which, id_t who)
{
    (void)which;
    (void)who;
    errno = 0;
    return 0;
}

int setpriority(int which, id_t who, int prio)
{
    (void)which;
    (void)who;
    (void)prio;
    errno = EPERM;
    return -1;
}
