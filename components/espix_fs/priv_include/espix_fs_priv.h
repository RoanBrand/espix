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
 * May the calling process name `abs_path` at all?
 *
 * True unless the caller is a process with a root and the path falls outside
 * it. Raised tasks are always true -- espix reaching a file of its own is not
 * the confined process asking.
 *
 * Two callers, and they are not redundant. The VFS asks in resolve(), which
 * covers every path operation that goes through the VFS; espix_fs_admin_check()
 * asks because chmod and chown do not, for the same reason that function exists
 * at all. `abs_path` must already be resolved and normalised, so that ".."
 * cannot be used to spell a way out.
 */
bool espix_fs_root_permits(const char *abs_path);

/*
 * A mount prefix's maximum length, including the NUL. Internal: callers pass
 * mount points, they do not size anything with this.
 */
#define ESPIX_FS_PREFIX_MAX  32

/*
 * Publish espix's VFS as the root, forwarding to the filesystem described by
 * `lower_ops` and `lower_ctx` (as returned by esp_littlefs_mount()).
 */
esp_err_t espix_vfs_register_root(const esp_vfs_fs_ops_t *lower_ops,
                                  void *lower_ctx);

/*
 * Where a mount's modes and owners come from. One precedence, applied in this
 * order, and the reason there is only one:
 *
 *   LOWER   the filesystem keeps them -- ext, in its inodes. What its own stat
 *           returns is the answer, and nothing above it may override that: a
 *           volume carrying its own permissions is the one case where espix has
 *           nothing to add.
 *   ESPIX   espix keeps them for a filesystem that keeps none -- littlefs, in a
 *           user attribute. Stored, so a write has somewhere to go and chmod and
 *           chown work.
 *   NONE    neither, so the mount answers for what it holds: the uid and gid of
 *           whoever mounted it, and the rule for anything still unanswered (FAT).
 */
typedef enum {
    ESPIX_FS_META_NONE = 0,
    ESPIX_FS_META_ESPIX,
    ESPIX_FS_META_LOWER,
} espix_fs_meta_t;

/*
 * How a mounted filesystem is asked to change a mode and an owner, for one that
 * keeps them itself -- ext, whose inodes carry both. NULL for everything else:
 * FAT keeps neither, and the rootfs keeps espix's own attributes, which mode.c
 * writes directly.
 *
 * Both fields are always passed, not only the one the caller changed: whatever
 * they did not change has already been filled in from what the filesystem
 * reported, so an implementation may set both without clobbering either.
 * Returns 0, or -1 with errno set, like a VFS op.
 */
typedef struct {
    int  (*setattr)(void *ctx, const char *abs_path, mode_t mode,
                    uint16_t uid, uint16_t gid);
    void  *ctx;
} espix_fs_meta_ops_t;

/*
 * A second filesystem at `prefix` ("/mnt"), reached through espix's VFS so the
 * permission check applies there too -- which is the whole reason it is a table
 * in here rather than another esp_vfs registration. The caller owns the
 * filesystem: this only routes to it.
 *
 * `metadata` says where that filesystem's modes and owners come from, which is
 * what decides whether chmod and chown have anywhere to write, and `meta` is how
 * to write them there -- NULL for a filesystem that has no such thing.
 *
 * espix_vfs_del_mount() answers ESP_ERR_INVALID_STATE while a file or directory
 * is still open on the mount: a lower filesystem's fd cannot be revoked, so the
 * caller has to close up first.
 */
esp_err_t espix_vfs_add_mount(const char *prefix, const esp_vfs_fs_ops_t *ops,
                              void *ctx, espix_fs_meta_t metadata,
                              const espix_fs_meta_ops_t *meta,
                              uint16_t owner_uid, uint16_t owner_gid);
esp_err_t espix_vfs_del_mount(const char *prefix);

/* Where this path's modes and owners come from. See espix_fs_meta_t. */
espix_fs_meta_t espix_vfs_metadata(const char *abs_path);

/* How to change them, or NULL where the filesystem has no such thing. */
const espix_fs_meta_ops_t *espix_vfs_meta_ops(const char *abs_path);

/*
 * A mount whose owner the rule decides: the root, and any filesystem that
 * carries ownership itself. The same value as ESPIX_FS_KEEP_ID because it means
 * the same thing -- leave this to something else.
 */
#define ESPIX_FS_OWNER_RULE ((uint16_t)0xFFFF)

/*
 * The owner of the mount a path belongs to, when that mount keeps no ownership
 * of its own -- the uid and gid of whoever mounted it, which is the shape Linux
 * gives a removable volume with uid= and gid=. False means the rule answers, as
 * it does for the rootfs and everything under it.
 */
bool espix_vfs_mount_owner(const char *abs_path, uint16_t *uid, uint16_t *gid);

/* True once espix_fs_mount_dead() has marked a mount's device gone. fat.c asks
 * before syncing a volume on unmount, which is the write that would otherwise go
 * through a released block device. */
bool espix_vfs_mount_dead(const char *path);


/* ------------------------------------------------------------------ */
/* Device nodes -- see dev.c                                           */
/* ------------------------------------------------------------------ */

/*
 * Device fds occupy the top of the range esp_vfs can carry. Its `local_fd_t` is
 * a uint8_t on every target but Linux, so anything above 255 is silently
 * truncated -- which is why this is 240 and not something conspicuous like
 * 0x1000. vfs_open() refuses a lower-filesystem fd that reaches the base, so
 * the reservation is enforced rather than assumed.
 */
#define ESPIX_DEV_FD_BASE   240
#define ESPIX_DEV_FD_COUNT  16

void        espix_dev_init(void);

/* NULL when the path is not a device. The handle is opaque to vfs.c. */
const void *espix_dev_lookup(const char *abs_path);
void        espix_dev_stat(const void *handle, struct stat *st);
int         espix_dev_open(const void *handle, int flags);

/*
 * The virtual /dev directory. Its entries are the device table, and nothing
 * in it reaches littlefs, so whatever an older image left there is invisible.
 */
bool        espix_dev_isdir(const char *abs_path);
bool        espix_dev_underdev(const char *abs_path);
bool        espix_dev_mode(const char *abs_path, mode_t *out);
void        espix_dev_dir_stat(struct stat *st);

DIR        *espix_dev_opendir(void);
struct dirent *espix_dev_readdir(DIR *pdir);
int         espix_dev_readdir_r(DIR *pdir, struct dirent *entry,
                                struct dirent **out);
long        espix_dev_telldir(DIR *pdir);
void        espix_dev_seekdir(DIR *pdir, long offset);
int         espix_dev_closedir(DIR *pdir);

/* True for a DIR that espix_dev_opendir() handed out, so vfs.c can route. */
bool        espix_dev_dirp(DIR *pdir);

static inline bool espix_dev_fd(int fd)
{
    return fd >= ESPIX_DEV_FD_BASE;
}

int     espix_dev_close(int fd);
ssize_t espix_dev_read(int fd, void *dst, size_t size);
ssize_t espix_dev_pread(int fd, void *dst, size_t size, off_t off);
ssize_t espix_dev_write(int fd, const void *data, size_t size);
off_t   espix_dev_lseek(int fd, off_t off, int whence);
int     espix_dev_fstat(int fd, struct stat *st);
int     espix_dev_fsync(int fd);

#ifdef __cplusplus
}
#endif
