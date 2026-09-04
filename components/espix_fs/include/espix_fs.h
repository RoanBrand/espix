/*
 * espix root filesystem.
 *
 * espix registers its *own* VFS as the fallback (empty base path), which makes
 * it the real root: paths are /bin/hello, /etc/motd, /home/... rather than
 * /storage/bin/hello. ESP-IDF documents the empty-base-path case explicitly
 * ("a fallback VFS ... will handle paths which are not matched by any other
 * registered VFS"), so device VFSes such as /dev/uart keep working via
 * longest-prefix match.
 *
 * LittleFS sits underneath, mounted but registered at no path at all, reached
 * through a pointer rather than a name -- see vfs.c. That is what puts espix on
 * the path of every file call in the system, an app's fopen() included, which
 * is where a permission check belongs and where it was previously impossible.
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
 * Files now have owners, so the reason has changed. It is no longer that
 * setuid, setgid and sticky have nothing to be defined against; it is that
 * espix does not act on them. There is no privilege for a setuid binary to
 * raise to that a process could not already reach, because a process's
 * credentials come from its session and nothing hands them out otherwise, and
 * nothing consults the sticky bit when deleting from a directory.
 *
 * So they would still be bits that could be stored, displayed and never
 * consulted, which is the thing worth refusing. chmod rejects them by name
 * rather than masking them away silently; see chmod_parse().
 */
#define ESPIX_MODE_BITS 0777

/*
 * Where a mode lives when it differs from the rule: a LittleFS user attribute
 * on the file itself, so it moves with a rename and dies with a delete without
 * espix doing anything about either.
 *
 * There is no registry of attribute types -- SPEC.md says as much -- so 0x70
 * is a local choice. The ESP port uses 't' (0x74) for mtime; do not collide.
 *
 * uid and gid were stored from the start and read by nothing, so that filling
 * them in later would cost no rewrite of anything already on disk. This is
 * later: they now carry a real owner, and espix_fs_access_check() compares
 * against them.
 */
#define ESPIX_FS_ATTR_POSIX 0x70

typedef struct __attribute__((packed)) {
    uint16_t mode;
    uint16_t uid;
    uint16_t gid;
} espix_fs_posix_attr_t;

/*
 * The mode of `abs_path`: the stored attribute if there is one, else the rule
 * (directories 0755, ELF files 0755, everything else 0644).
 *
 * `st` is an already-taken stat to save a second one -- pass NULL and this
 * takes its own. Returns 0 for a path that does not exist.
 */
mode_t espix_fs_mode(const char *abs_path, const struct stat *st);

/*
 * The owner of `abs_path`: the stored attribute if there is one, else the rule.
 *
 * `st` is an already-taken stat to save a second one, as for espix_fs_mode().
 * Either output pointer may be NULL. A path that does not exist, or a system
 * with no owner rule installed, answers root -- which is also what a zero-filled
 * attribute says, so the absence of information and the presence of uid 0 mean
 * the same thing on purpose.
 */
void espix_fs_owner(const char *abs_path, const struct stat *st,
                    uint16_t *uid, uint16_t *gid);

/*
 * Set the owner of an existing path and persist it.
 *
 * Pass ESPIX_FS_KEEP_ID for either field to leave it alone, which is what
 * `chown user` (no group) and `chgrp` need.
 *
 * As with espix_fs_chmod(), an owner the rule already produces removes the
 * attribute rather than storing one, so chowning a file back to where it
 * started leaves no trace.
 */
#define ESPIX_FS_KEEP_ID ((uint16_t)0xFFFF)

esp_err_t espix_fs_chown(const char *abs_path, uint16_t uid, uint16_t gid);

/*
 * Who owns a path that carries no stored attribute.
 *
 * espix_fs cannot answer this itself: it means reading /etc/passwd, and
 * espix_auth already depends on espix_fs, so the call has to go the other way.
 * espix_auth installs the rule once it has parsed the account file; until then,
 * and if nothing ever installs one, everything belongs to root.
 *
 * Return false to decline, which means root.
 */
typedef bool (*espix_fs_owner_rule_t)(const char *abs_path, uint16_t *uid,
                                      uint16_t *gid);

void espix_fs_set_owner_rule(espix_fs_owner_rule_t rule);

/*
 * Run a stretch of file operations as root, for a component that owns a file
 * the permission check would otherwise keep it out of.
 *
 * espix has no setuid, so there is no way for `passwd` -- a builtin, running
 * with the credentials of whoever typed it -- to rewrite /etc/passwd, and no way
 * for `ls -l` to read a name out of it when it is 0600 root. Unix solves that
 * by making the binary setuid-root. espix solves it by letting the one component
 * that understands the file reach it directly, and keeping everyone else out.
 *
 * Deliberately not a general capability: espix_auth is the only caller, the
 * scope is a few lines around a fopen(), and the pairing is per task, so one
 * session raising privilege cannot affect another. Nest freely; the depth is
 * counted. Anything added here should be able to say why it is not a hole.
 */
void espix_fs_priv_begin(void);
void espix_fs_priv_end(void);

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
 * the path does not exist, ESP_FAIL if the filesystem rejected the write.
 *
 * Setting a mode the rule already produces removes the attribute rather than
 * storing it, so this is also how a file returns to having no stored mode.
 */
esp_err_t espix_fs_chmod(const char *abs_path, mode_t mode);

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
