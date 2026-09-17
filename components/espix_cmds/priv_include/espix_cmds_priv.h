/* Internal to the espix_cmds component. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "espix_shell.h"

#ifdef __cplusplus
extern "C" {
#endif

void espix_cmds_register_env(void);
void espix_cmds_register_fs(void);
void espix_cmds_register_sys(void);
void espix_cmds_register_run(void);
void espix_cmds_register_net(void);
void espix_cmds_register_motd(void);
void espix_cmds_register_time(void);
void espix_cmds_register_blk(void);
void espix_cmds_register_usbhost(void);

/* Resolves a non-builtin command name to a program in /bin or by path. */
void espix_cmds_register_exec_fallback(void);

/* The greeting every session opens with; also the `motd` command. */
void espix_cmds_print_greeting(espix_session_t *s);

/*
 * Resolve argv[i] against the session's cwd, reporting to the session and
 * returning false if the path does not fit.
 */
bool espix_cmd_path(espix_session_t *s, const char *arg,
                    char *out, size_t out_len);

/* A byte count as a size column: plain, or -h for "1.4K"/"21K"/"28.7G"/"3.1T". */
void espix_cmd_size(char *out, size_t len, uint64_t bytes, bool human);

/* Register a NULL-terminated array of commands, logging any duplicates. */
void espix_cmds_register_table(espix_cmd_t *table, size_t count);

/*
 * The device and filesystem type a mounted path came from, for `df`.
 *
 * `df` walks espix_fs_mount_at(), which yields mount points only; these are how
 * a row gets the rest. `source` is written as "/dev/sda2" -- the node dev.c
 * registers, and the name `mount`/`umount` take. Both return false when nothing
 * is mounted at `path`.
 */
bool espix_blk_mount_source(const char *path, char *out, size_t out_len);
bool espix_blk_mount_type(const char *path, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
