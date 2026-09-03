/*
 * espix root filesystem.
 *
 * LittleFS is registered as the VFS *fallback* (empty base path), which makes
 * it the real root: paths are /bin/hello, /etc/motd, /home/... rather than
 * /storage/bin/hello. ESP-IDF documents the empty-base-path case explicitly
 * ("a fallback VFS ... will handle paths which are not matched by any other
 * registered VFS"), so device VFSes such as /dev/uart keep working via
 * longest-prefix match.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Partition label the rootfs lives on — must match partitions.csv. */
#define ESPIX_FS_ROOT_PARTITION "storage"

typedef struct {
    size_t total_bytes;
    size_t used_bytes;
} espix_fs_info_t;

/*
 * Mount the rootfs, formatting it if it will not mount, then ensure the
 * standard directory skeleton exists and set the CWD to "/".
 */
esp_err_t espix_fs_mount_root(void);

esp_err_t espix_fs_stat_root(espix_fs_info_t *out);

bool espix_fs_is_mounted(void);

/*
 * Resolve `path` against `cwd` into `out` (absolute, no "." or ".." segments,
 * no trailing slash except for "/" itself). Used by every shell command that
 * takes a path, so relative paths behave the same everywhere.
 */
esp_err_t espix_fs_resolve(const char *cwd, const char *path,
                           char *out, size_t out_len);

/* Recursive delete, used by `rm -r`. */
esp_err_t espix_fs_rm_rf(const char *abs_path);

/* ------------------------------------------------------------------ */
/* File modes                                                          */
/* ------------------------------------------------------------------ */

/*
 * espix implements nine permission bits and not twelve.
 *
 * There are two identities -- `root` on the console and `esp` over SSH -- so it
 * is not that there is nobody to distinguish. What is missing is an *owner*:
 * no file records one, and setuid, setgid and sticky are all defined in terms
 * of the owner of the thing they are set on. Each would be a bit that could be
 * stored, displayed and never consulted.
 *
 * chmod refuses them by name rather than masking them away silently; see
 * chmod_parse().
 */
#define ESPIX_MODE_BITS 0777

/* Where chmod records what it changed. See mode.c for why this is an
 * exceptions list rather than a mode for every file. */
#define ESPIX_MODES_PATH "/etc/modes"

/* Ceiling on stored overrides. Reached only by someone chmod'ing sixty-odd
 * separate paths away from their defaults, which is not a device workload. */
#define ESPIX_MODE_OVERRIDES_MAX 64

/* Load /etc/modes. Called by espix_fs_mount_root(); safe to call twice. */
void espix_fs_mode_init(void);

/*
 * The mode of `abs_path`: a stored override if there is one, else the rule
 * (directories 0755, ELF files 0755, everything else 0644).
 *
 * `st` is an already-taken stat to save a second one -- pass NULL and this
 * takes its own. Returns 0 for a path that does not exist.
 */
mode_t espix_fs_mode(const char *abs_path, const struct stat *st);

/*
 * Whether `abs_path` may be executed: a regular file whose mode has S_IXUSR.
 *
 * This is what the shell gates on. It replaces sniffing the ELF magic at the
 * point of execution -- that test still exists, inside the rule, but it now
 * decides the *default* mode rather than the answer, so `chmod -x` can
 * overrule it.
 */
bool espix_fs_is_executable(const char *abs_path);

/*
 * Set the mode of an existing path and persist it.
 *
 * ESP_ERR_INVALID_ARG for a mode outside ESPIX_MODE_BITS, ESP_ERR_NOT_FOUND if
 * the path does not exist, ESP_ERR_NO_MEM if the override table is full.
 *
 * Setting a mode the rule already produces removes the override rather than
 * storing it, so this is also how a file returns to having no stored mode.
 */
esp_err_t espix_fs_chmod(const char *abs_path, mode_t mode);

/* Drop any override for a path that has been deleted, and follow one across a
 * rename. Called by rm/mv and by the SFTP server, which own every rename espix
 * can see -- an app calling rename() directly bypasses both. */
void espix_fs_mode_forget(const char *abs_path);
void espix_fs_mode_rename(const char *old_abs, const char *new_abs);

/* Render "-rwxr-xr-x" into `out`. Never emits s, S, t or T. */
void espix_fs_mode_str(mode_t mode, bool is_dir, char *out, size_t len);

/*
 * Read one `key=value` out of a config file under /etc.
 *
 * espix's config files are all the same shape — one `key=value` per line, `#`
 * to end-of-line for comments, leading whitespace ignored, first match wins —
 * because they are meant to be edited with `echo >>` and read with `cat`. This
 * lives here rather than in whichever component happened to need it first, so
 * /etc/wifi.conf, /etc/ntp.conf and whatever comes next all parse identically.
 *
 * Returns false if the file is absent or the key is not in it, which callers
 * are expected to treat as "unconfigured" rather than as an error.
 */
bool espix_fs_conf_get(const char *path, const char *key,
                       char *out, size_t len);

#ifdef __cplusplus
}
#endif
