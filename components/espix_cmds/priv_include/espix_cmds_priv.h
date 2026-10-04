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
void espix_cmds_register_svc(void);
void espix_cmds_register_net(void);
void espix_cmds_register_vpn(void);
void espix_cmds_register_motd(void);
void espix_cmds_register_time(void);
void espix_cmds_register_blk(void);
void espix_cmds_register_usbhost(void);
void espix_cmds_register_ota(void);
void espix_cmds_register_hash(void);
void espix_cmds_register_bt(void);
void espix_cmds_register_play(void);
void espix_cmds_register_display(void);
void espix_cmds_register_text(void);

/*
 * The palette the login banner and `top` share, and the helper that colours a
 * "(NN%)" by how full it is: green while there is room, yellow past 75%, red
 * past 90%. Shared rather than duplicated so the two cannot drift about what
 * "nearly full" looks like.
 */
#define ANSI_WARN   "\033[33m"          /* yellow */
#define ANSI_OK     "\033[32m"          /* green */
#define ANSI_BAD    "\033[31m"          /* red */
#define ANSI_RESET  "\033[0m"

unsigned pct_of(uint64_t used, uint64_t total);

/* Append " (NN%)" to `out`, coloured. Added only when the whole sequence fits,
 * so a row truncated elsewhere cannot swallow the reset and bleed colour. */
void append_pct(char *out, size_t len, bool ansi, unsigned pct);

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

/*
 * A download's progress, as a line every tenth, for a session watching one.
 * Shared by 'fetch' and by the run path's first-launch download, so the two
 * report the same way; the implementation is in cmd_fetch.c.
 */
typedef struct {
    espix_session_t *s;
    unsigned         decile;    /* the tenth reported last */
    bool             noted;     /* an unknown-size transfer has been mentioned */
} espix_fetch_progress_t;

void espix_cmds_fetch_progress(void *ctx, const char *path,
                               size_t done, size_t total);

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
