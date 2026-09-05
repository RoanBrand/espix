/* Internal to the espix_fs component: the seam between espix's VFS and the
 * access policy it consults. */
#pragma once

#include <stdbool.h>

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
 * May the caller change `abs_path`'s mode or owner?
 *
 * chmod and chown do not go through the VFS -- they are espix_fs calls that
 * write a littlefs attribute directly -- so espix_fs_access_check() never sees
 * them and cannot be what protects them. Without this, any user could take
 * ownership of any file, which makes every other check pointless.
 *
 * Returns 0 to allow, or EPERM. Changing the owner is root's alone, the way
 * chown(2) restricts it; changing the mode needs only to be the owner.
 */
int espix_fs_admin_check(const char *abs_path, bool changing_owner);

/*
 * Give a just-created path the caller's ownership.
 *
 * The ownership rule covers the ordinary case -- files under a home belong to
 * whoever lives there -- and this covers the rest: a file created in a
 * directory somebody has chmod'd or chowned into general use would otherwise be
 * born belonging to root, and its own creator could not then write it.
 *
 * A no-op for espix itself and cheap when the rule already agrees, because
 * espix_fs_chown() stores nothing it would derive anyway.
 */
void espix_fs_claim(const char *abs_path);

/*
 * Is the calling task inside espix_fs_priv_begin()/end()?
 *
 * The public pair raises privilege; this asks. Both the permission check and
 * the process-root test have to answer "yes, allow" for a raised task, and
 * this is what keeps the two from disagreeing about what raised means.
 */
bool espix_fs_priv_active(void);

/*
 * Publish espix's VFS as the root, forwarding to the filesystem described by
 * `lower_ops` and `lower_ctx` (as returned by esp_littlefs_mount()).
 */
esp_err_t espix_vfs_register_root(const esp_vfs_fs_ops_t *lower_ops,
                                  void *lower_ctx);

#ifdef __cplusplus
}
#endif
