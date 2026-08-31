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
