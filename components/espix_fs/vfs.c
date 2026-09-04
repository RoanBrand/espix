/*
 * espix's VFS: the root of the namespace, stacked on littlefs.
 *
 * Why this exists at all
 * ----------------------
 * Every file call in the system -- from a shell command or from an app loaded
 * off the filesystem -- goes through ESP-IDF's VFS, which routes by path prefix
 * to whichever driver registered it. espix used to register joltwallet's
 * littlefs port at "" (the fallback, which is what makes a filesystem the root)
 * and live above it. That left espix with nowhere to stand: an app calls
 * fopen(), libc calls the VFS, the VFS calls the port, and no espix code runs
 * on the path at all.
 *
 * So permissions could not be enforced -- checking in `cat` alone is a boundary
 * you step around with `run`. This is exactly why NuttX enforces file
 * permissions and espix could not: NuttX owns its VFS.
 *
 * Linux and NuttX both check permissions in the *VFS*, not in filesystems --
 * inode_permission()/generic_permission() decide, while ext4 and btrfs carry no
 * permission checks and merely supply i_mode/i_uid/i_gid. So espix registers
 * this VFS as the root, and the filesystem sits underneath it.
 *
 * Stacked by pointer, not by path
 * -------------------------------
 * The obvious way to reach the layer below is to give it a base path --
 * mount littlefs at "/.lfs", and rewrite "/etc/motd" to "/.lfs/etc/motd". That
 * works, and it publishes a second name for the root: anything spelling
 * "/.lfs/..." addresses the filesystem with these checks skipped.
 *
 * A stackable filesystem does not route through the namespace; it holds a
 * pointer to the layer below. esp_littlefs_mount() (see
 * tools/patch-littlefs.py) mounts without registering a base path and hands
 * back the driver's ops and context, so littlefs is live and addressable by no
 * path at all. Three things fall out: no second name, no path translation --
 * the port wants a mount-relative path and espix's base is "", so paths pass
 * through unchanged -- and no DIR wrapper, because nothing re-enters the VFS.
 *
 * On DIR handles: esp_vfs_opendir() stamps dd_vfs_idx on whatever pointer comes
 * back, so the handle littlefs returns gets marked as belonging to this VFS.
 * That is harmless here precisely because forwarding is a direct call: the
 * port's readdir casts the pointer to its own type and never reads that field.
 * Routing through a path instead would have sent readdir back into this file,
 * forever.
 *
 * Every op degrades rather than crashing if the layer below lacks it: the port
 * populates truncate, ftruncate and utime behind Kconfig, and `access` and
 * `link` not at all. Checking the pointer at call time beats duplicating its
 * #ifdefs here and being wrong later.
 */

#include <errno.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_littlefs.h"
#include "esp_vfs.h"
#include "esp_vfs_ops.h"

#include "espix_fs.h"
#include "espix_kernel.h"

#include "espix_fs_priv.h"

#define TAG "fs"

/*
 * The layer below. A struct passed as the VFS context rather than file-scope
 * statics, because `mount` is on the roadmap and a second mount would otherwise
 * mean unpicking this.
 */
typedef struct {
    const esp_vfs_fs_ops_t  *ops;
    const esp_vfs_dir_ops_t *dir;
    void                    *ctx;
} lower_t;

static lower_t s_root;

/* Refuse rather than crash when the layer below does not implement something. */
#define NO_LOWER(expr) ((expr) == NULL)

static int enosys(void)
{
    errno = ENOSYS;
    return -1;
}

/* ------------------------------------------------------------------ */
/* File operations                                                     */
/* ------------------------------------------------------------------ */

static int vfs_open(void *ctx, const char *path, int flags, int mode)
{
    const lower_t *l = ctx;

    const int err = espix_fs_access_check(path, ESPIX_FS_ACCESS_OPEN, flags);
    if (err != 0) {
        errno = err;
        return -1;
    }
    if (NO_LOWER(l->ops->open_p)) {
        return enosys();
    }
    return l->ops->open_p(l->ctx, path, flags, mode);
}

static int vfs_close(void *ctx, int fd)
{
    const lower_t *l = ctx;
    return NO_LOWER(l->ops->close_p) ? enosys() : l->ops->close_p(l->ctx, fd);
}

static ssize_t vfs_read(void *ctx, int fd, void *dst, size_t size)
{
    const lower_t *l = ctx;
    return NO_LOWER(l->ops->read_p) ? enosys()
                                    : l->ops->read_p(l->ctx, fd, dst, size);
}

static ssize_t vfs_write(void *ctx, int fd, const void *data, size_t size)
{
    const lower_t *l = ctx;
    return NO_LOWER(l->ops->write_p) ? enosys()
                                     : l->ops->write_p(l->ctx, fd, data, size);
}

static ssize_t vfs_pread(void *ctx, int fd, void *dst, size_t size, off_t off)
{
    const lower_t *l = ctx;
    return NO_LOWER(l->ops->pread_p)
               ? enosys() : l->ops->pread_p(l->ctx, fd, dst, size, off);
}

static ssize_t vfs_pwrite(void *ctx, int fd, const void *src, size_t size,
                          off_t off)
{
    const lower_t *l = ctx;
    return NO_LOWER(l->ops->pwrite_p)
               ? enosys() : l->ops->pwrite_p(l->ctx, fd, src, size, off);
}

static off_t vfs_lseek(void *ctx, int fd, off_t size, int mode)
{
    const lower_t *l = ctx;
    return NO_LOWER(l->ops->lseek_p) ? enosys()
                                     : l->ops->lseek_p(l->ctx, fd, size, mode);
}

static int vfs_fstat(void *ctx, int fd, struct stat *st)
{
    const lower_t *l = ctx;
    return NO_LOWER(l->ops->fstat_p) ? enosys()
                                     : l->ops->fstat_p(l->ctx, fd, st);
}

static int vfs_fsync(void *ctx, int fd)
{
    const lower_t *l = ctx;
    return NO_LOWER(l->ops->fsync_p) ? enosys() : l->ops->fsync_p(l->ctx, fd);
}

static int vfs_fcntl(void *ctx, int fd, int cmd, int arg)
{
    const lower_t *l = ctx;
    return NO_LOWER(l->ops->fcntl_p) ? enosys()
                                     : l->ops->fcntl_p(l->ctx, fd, cmd, arg);
}

/* ------------------------------------------------------------------ */
/* Directory and path operations                                       */
/* ------------------------------------------------------------------ */

/*
 * Not gated. POSIX does not require read permission to stat a file -- it
 * requires execute on each parent directory, which is a path traversal check
 * espix does not do. Gating it would be stricter than Unix rather than closer
 * to it, and `ls -l` would start failing on directories you can list.
 */
static int vfs_stat(void *ctx, const char *path, struct stat *st)
{
    const lower_t *l = ctx;
    return NO_LOWER(l->dir->stat_p) ? enosys()
                                    : l->dir->stat_p(l->ctx, path, st);
}

static int vfs_unlink(void *ctx, const char *path)
{
    const lower_t *l = ctx;

    const int err = espix_fs_access_check(path, ESPIX_FS_ACCESS_UNLINK, 0);
    if (err != 0) {
        errno = err;
        return -1;
    }
    return NO_LOWER(l->dir->unlink_p) ? enosys()
                                      : l->dir->unlink_p(l->ctx, path);
}

static int vfs_rename(void *ctx, const char *src, const char *dst)
{
    const lower_t *l = ctx;

    /* Both ends: a rename removes a name here and creates one there. */
    int err = espix_fs_access_check(src, ESPIX_FS_ACCESS_RENAME, 0);
    if (err == 0) {
        err = espix_fs_access_check(dst, ESPIX_FS_ACCESS_RENAME, 0);
    }
    if (err != 0) {
        errno = err;
        return -1;
    }
    return NO_LOWER(l->dir->rename_p) ? enosys()
                                      : l->dir->rename_p(l->ctx, src, dst);
}

static DIR *vfs_opendir(void *ctx, const char *name)
{
    const lower_t *l = ctx;

    const int err = espix_fs_access_check(name, ESPIX_FS_ACCESS_OPENDIR, 0);
    if (err != 0) {
        errno = err;
        return NULL;
    }
    if (NO_LOWER(l->dir->opendir_p)) {
        errno = ENOSYS;
        return NULL;
    }
    return l->dir->opendir_p(l->ctx, name);
}

static struct dirent *vfs_readdir(void *ctx, DIR *pdir)
{
    const lower_t *l = ctx;

    if (NO_LOWER(l->dir->readdir_p)) {
        errno = ENOSYS;
        return NULL;
    }
    return l->dir->readdir_p(l->ctx, pdir);
}

static int vfs_readdir_r(void *ctx, DIR *pdir, struct dirent *entry,
                         struct dirent **out)
{
    const lower_t *l = ctx;
    return NO_LOWER(l->dir->readdir_r_p)
               ? enosys() : l->dir->readdir_r_p(l->ctx, pdir, entry, out);
}

static long vfs_telldir(void *ctx, DIR *pdir)
{
    const lower_t *l = ctx;
    return NO_LOWER(l->dir->telldir_p) ? enosys()
                                       : l->dir->telldir_p(l->ctx, pdir);
}

static void vfs_seekdir(void *ctx, DIR *pdir, long offset)
{
    const lower_t *l = ctx;

    if (!NO_LOWER(l->dir->seekdir_p)) {
        l->dir->seekdir_p(l->ctx, pdir, offset);
    }
}

static int vfs_closedir(void *ctx, DIR *pdir)
{
    const lower_t *l = ctx;
    return NO_LOWER(l->dir->closedir_p) ? enosys()
                                        : l->dir->closedir_p(l->ctx, pdir);
}

static int vfs_mkdir(void *ctx, const char *name, mode_t mode)
{
    const lower_t *l = ctx;

    const int err = espix_fs_access_check(name, ESPIX_FS_ACCESS_MKDIR, 0);
    if (err != 0) {
        errno = err;
        return -1;
    }
    return NO_LOWER(l->dir->mkdir_p) ? enosys()
                                     : l->dir->mkdir_p(l->ctx, name, mode);
}

static int vfs_rmdir(void *ctx, const char *name)
{
    const lower_t *l = ctx;

    const int err = espix_fs_access_check(name, ESPIX_FS_ACCESS_RMDIR, 0);
    if (err != 0) {
        errno = err;
        return -1;
    }
    return NO_LOWER(l->dir->rmdir_p) ? enosys() : l->dir->rmdir_p(l->ctx, name);
}

static int vfs_truncate(void *ctx, const char *path, off_t length)
{
    const lower_t *l = ctx;

    const int err = espix_fs_access_check(path, ESPIX_FS_ACCESS_TRUNCATE, 0);
    if (err != 0) {
        errno = err;
        return -1;
    }
    return NO_LOWER(l->dir->truncate_p)
               ? enosys() : l->dir->truncate_p(l->ctx, path, length);
}

static int vfs_ftruncate(void *ctx, int fd, off_t length)
{
    const lower_t *l = ctx;
    return NO_LOWER(l->dir->ftruncate_p)
               ? enosys() : l->dir->ftruncate_p(l->ctx, fd, length);
}

static int vfs_utime(void *ctx, const char *path, const struct utimbuf *times)
{
    const lower_t *l = ctx;
    return NO_LOWER(l->dir->utime_p) ? enosys()
                                     : l->dir->utime_p(l->ctx, path, times);
}

/* ------------------------------------------------------------------ */
/* The tables                                                          */
/* ------------------------------------------------------------------ */

/*
 * `link_p` and `access_p` stay NULL, mirroring the port. Symlinks are a
 * documented no (LittleFS has no link type). `access` could now be answered
 * from the mode espix already knows -- but the port never provided it, so
 * anything relying on it failing today (the SFTP server's O_EXCL check, for
 * one) would change behaviour. That is an improvement to make deliberately,
 * not a side effect of moving layers.
 *
 * No termios or select: this is a filesystem, not a device.
 */
static const esp_vfs_dir_ops_t s_espix_vfs_dir = {
    .stat_p      = &vfs_stat,
    .link_p      = NULL,
    .unlink_p    = &vfs_unlink,
    .rename_p    = &vfs_rename,
    .opendir_p   = &vfs_opendir,
    .readdir_p   = &vfs_readdir,
    .readdir_r_p = &vfs_readdir_r,
    .telldir_p   = &vfs_telldir,
    .seekdir_p   = &vfs_seekdir,
    .closedir_p  = &vfs_closedir,
    .mkdir_p     = &vfs_mkdir,
    .rmdir_p     = &vfs_rmdir,
    .access_p    = NULL,
    .truncate_p  = &vfs_truncate,
    .ftruncate_p = &vfs_ftruncate,
    .utime_p     = &vfs_utime,
};

static const esp_vfs_fs_ops_t s_espix_vfs = {
    .write_p  = &vfs_write,
    .lseek_p  = &vfs_lseek,
    .read_p   = &vfs_read,
    .pread_p  = &vfs_pread,
    .pwrite_p = &vfs_pwrite,
    .open_p   = &vfs_open,
    .close_p  = &vfs_close,
    .fstat_p  = &vfs_fstat,
    .fcntl_p  = &vfs_fcntl,
    .fsync_p  = &vfs_fsync,
    .dir      = &s_espix_vfs_dir,
};

esp_err_t espix_vfs_register_root(const esp_vfs_fs_ops_t *lower_ops,
                                  void *lower_ctx)
{
    if (lower_ops == NULL || lower_ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (lower_ops->dir == NULL) {
        espix_klog(ESPIX_KLOG_ERROR, TAG,
                   "the filesystem below has no directory operations");
        return ESP_ERR_INVALID_ARG;
    }

    s_root.ops = lower_ops;
    s_root.dir = lower_ops->dir;
    s_root.ctx = lower_ctx;

    /*
     * "" is the fallback: any path no longer prefix claims. That is what makes
     * this the root, and only one VFS can hold it -- there is no "/" entry to
     * share, since esp_vfs_register_fs() requires a prefix of two characters or
     * more. Device VFSes such as /dev/uart keep working by being longer.
     *
     * STATIC because the tables above are `const` and outlive any call;
     * CONTEXT_PTR because the ops take the lower layer as their first argument.
     */
    const esp_err_t err = esp_vfs_register_fs(
        "", &s_espix_vfs, ESP_VFS_FLAG_CONTEXT_PTR | ESP_VFS_FLAG_STATIC,
        &s_root);

    if (err != ESP_OK) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "cannot register the root VFS: %s",
                   esp_err_to_name(err));
    }
    return err;
}
