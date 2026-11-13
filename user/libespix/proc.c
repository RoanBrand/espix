/* Process identity, signals to processes, and the end of one. */
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/resource.h>
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

/* The supplementary groups: the kernel's 32-bit ids, in the app's gid_t. */
int getgroups(int size, gid_t list[])
{
    const int n = espix_ret(espix_sys2(ESPIX_SYS_GETGROUPS, 0, NULL));
    if (n <= 0 || size == 0) {
        return n;
    }
    if (size < n) {
        errno = EINVAL;
        return -1;
    }
    uint32_t ids[n];
    const int got = espix_ret(espix_sys2(ESPIX_SYS_GETGROUPS, n, ids));
    for (int i = 0; i < got; i++) {
        list[i] = (gid_t)ids[i];
    }
    return got;
}

pid_t getsid(pid_t pid)
{
    return (pid_t)espix_ret(espix_sys1(ESPIX_SYS_GETSID, pid));
}

int kill(pid_t pid, int sig)
{
    return espix_ret(espix_sys2(ESPIX_SYS_KILL, pid, sig));
}

static long mem_pages(int name)
{
    struct espix_meminfo m;
    if (espix_ret(espix_sys1(ESPIX_SYS_MEMINFO, &m)) < 0) {
        return -1;
    }
    return (name == _SC_PHYS_PAGES) ? (long)m.total : (long)m.free;
}

long sysconf(int name)
{
    switch (name) {
    case _SC_PHYS_PAGES:
    case _SC_AVPHYS_PAGES:
        return mem_pages(name);
    case _SC_OPEN_MAX:
        return ESPIX_OPEN_MAX;
    case _SC_PAGESIZE:
        return 4096;
    case _SC_NPROCESSORS_ONLN:
    case _SC_NPROCESSORS_CONF:
        return 2;   /* both chips' cores */
    case _SC_CLK_TCK:
        return ESPIX_USER_HZ;   /* /proc's clock, not the kernel's tick */
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

#if !ESPIX_NOMMU_APP
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
#endif

_Noreturn void _exit(int status)
{
    espix_sys1(ESPIX_SYS_EXIT, status);
    for (;;) {
    }
}

_Noreturn void _Exit(int status) __attribute__((weak, alias("_exit")));

/* Limits on a file: espix's paths and names, its pipes' atomic write, and no disabling character. */
long fpathconf(int fd, int name)
{
    (void)fd;
    switch (name) {
    case _PC_NAME_MAX:
        return 255;
    case _PC_PATH_MAX:
        return ESPIX_SYS_PATH_MAX;
    case _PC_PIPE_BUF:
        return 512;
    case _PC_LINK_MAX:
        return 1;
    case _PC_VDISABLE:
        return 0;
    default:
        errno = EINVAL;
        return -1;
    }
}

long pathconf(const char *path, int name)
{
    (void)path;
    return fpathconf(-1, name);
}

/* No Unix priority exists yet: a read is 0, as Linux gives with errno cleared, and a set is refused. */
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
