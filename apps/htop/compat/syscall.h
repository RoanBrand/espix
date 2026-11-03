/*
 * htop reaches for two raw Linux syscalls: capget (to tell whether a non-root
 * process carries permitted capabilities) and ioprio_get/ioprio_set (the IO
 * priority column). espix has no raw-syscall escape hatch -- every entry is a
 * named ESPIX_SYS_* call -- so the numbers are named here and syscall() says
 * ENOSYS. htop's own code then reports "no capabilities" and an IO priority
 * it cannot read, which is true. See ../README.md.
 */
#pragma once

#include <errno.h>

#define SYS_capget      0x1000
#define SYS_ioprio_get  0x1001
#define SYS_ioprio_set  0x1002

static inline long syscall(long number, ...)
{
    (void)number;
    errno = ENOSYS;
    return -1;
}
