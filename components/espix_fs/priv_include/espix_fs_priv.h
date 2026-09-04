/* Internal to the espix_fs component: the seam between espix's VFS and the
 * access policy it consults. */
#pragma once

#include "esp_err.h"
#include "esp_vfs_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Operations espix's VFS asks about before forwarding them.
 *
 * `stat` is absent on purpose: POSIX gates it on execute permission for each
 * parent directory, not read permission on the file, and espix does no path
 * traversal checks. Gating it would be stricter than Unix rather than closer.
 */
typedef enum {
    ESPIX_FS_ACCESS_OPEN,
    ESPIX_FS_ACCESS_OPENDIR,
    ESPIX_FS_ACCESS_UNLINK,
    ESPIX_FS_ACCESS_RENAME,
    ESPIX_FS_ACCESS_MKDIR,
    ESPIX_FS_ACCESS_RMDIR,
    ESPIX_FS_ACCESS_TRUNCATE,
} espix_fs_access_t;

/*
 * May the caller do `op` to `abs_path`? Returns 0 to allow, or the errno to
 * refuse with (EACCES, EPERM).
 *
 * `flags` carries the open flags for ESPIX_FS_ACCESS_OPEN and is 0 otherwise.
 */
int espix_fs_access_check(const char *abs_path, espix_fs_access_t op, int flags);

/*
 * Publish espix's VFS as the root, forwarding to the filesystem described by
 * `lower_ops` and `lower_ctx` (as returned by esp_littlefs_mount()).
 */
esp_err_t espix_vfs_register_root(const esp_vfs_fs_ops_t *lower_ops,
                                  void *lower_ctx);

#ifdef __cplusplus
}
#endif
