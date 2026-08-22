/* Internal to the espix_cmds component. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "espix_shell.h"

#ifdef __cplusplus
extern "C" {
#endif

void espix_cmds_register_fs(void);
void espix_cmds_register_sys(void);
void espix_cmds_register_run(void);

/*
 * Resolve argv[i] against the session's cwd, reporting to the session and
 * returning false if the path does not fit.
 */
bool espix_cmd_path(espix_session_t *s, const char *arg,
                    char *out, size_t out_len);

/* Register a NULL-terminated array of commands, logging any duplicates. */
void espix_cmds_register_table(espix_cmd_t *table, size_t count);

#ifdef __cplusplus
}
#endif
