/*
 * The calling process's identity, for the kernel and the syscall layer. espix
 * has one credential per process and no setuid execution, so uid and euid are
 * the same number.
 */

#include <sys/types.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "espix_proc.h"
#include "espix_proc_priv.h"

/* A task that is not a process: never 0, which would say "you are root". */
#define IDENT_UNKNOWN 65534

static uint16_t ident_uid(void)
{
    uint16_t uid = 0;
    return espix_proc_cred_of_task(xTaskGetCurrentTaskHandle(), &uid, NULL, NULL, NULL) ? uid : IDENT_UNKNOWN;
}

static uint16_t ident_gid(void)
{
    uint16_t gid = 0;
    return espix_proc_cred_of_task(xTaskGetCurrentTaskHandle(), NULL, &gid, NULL, NULL) ? gid : IDENT_UNKNOWN;
}

uid_t getuid(void)  { return (uid_t)ident_uid(); }
uid_t geteuid(void) { return (uid_t)ident_uid(); }
gid_t getgid(void)  { return (gid_t)ident_gid(); }
gid_t getegid(void) { return (gid_t)ident_gid(); }
