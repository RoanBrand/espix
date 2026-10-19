/*
 * espix user-mode syscall numbers, shared by the kernel and libespix-u.
 *
 * A number in a7, arguments in a0-a5, the result in a0: negative is -errno.
 * See docs/USER-MODE.md.
 */
#pragma once

#define ESPIX_SYS_EXIT   1   /* (status): ends the process */
#define ESPIX_SYS_WRITE  2   /* (fd, buf, len) */
