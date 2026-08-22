/*
 * espix built-in commands.
 *
 * Commands are plain functions registered into the espix_shell registry. They
 * take the session they were invoked from, so they must write with
 * espix_printf()/espix_puts() rather than printf() — otherwise their output
 * goes to the console instead of the session (or the file) that asked for it.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register every built-in. Call once, after the filesystem is mounted. */
void espix_cmds_register_all(void);

#ifdef __cplusplus
}
#endif
