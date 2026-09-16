/*
 * FAT, mounted under espix's VFS.
 *
 * Why this file exists
 * --------------------
 * Stage 2 of the USB work: a stick enumerates, and now it mounts, without ever
 * leaving espix's namespace. Why FatFs cannot simply be registered is in
 * tools/patch-fatfs.py -- in short, an ESP-IDF FatFs mount always ends in
 * esp_vfs_register_fs(), so the filesystem would be reachable at that prefix
 * with espix's permission check skipped, and the ops that would let espix drive
 * it directly are file-scope static.
 *
 * What that costs is translation, and it is all this file is: one shim per op,
 * because IDF's ops were written for a caller that let esp_vfs strip the mount
 * prefix and for a context esp_vfs_fat_register() made. A shim strips the prefix
 * from the paths that carry one and hands IDF its own context back.
 *
 * Three policies here are deliberate, and two of them differ from IDF's:
 *
 *   * Nothing here formats. esp_vfs_fat_*_mount() will f_mkfs() a volume that
 *     does not look like FAT, when asked to. A stick somebody plugged in to read
 *     must never come back empty, so a failure to mount is reported as one --
 *     with the FRESULT named, since "no FAT filesystem" and "drive not ready"
 *     are very different news.
 *   * The block device belongs to the caller. This never releases the handle it
 *     is given: a whole disk and a partition view are both mountable, and only
 *     the caller knows which it made.
 *   * Unmounting is refused while anything is still open on the mount; see
 *     espix_vfs_del_mount(). Then it releases in dependencies' order: the volume
 *     (FatFs), then the diskio, then the context, because FatFs is the only
 *     holder of the diskio and the context is the only holder of FatFs.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"

#include "esp_vfs_ops.h"

#include "diskio_bdl.h"
#include "diskio_impl.h"
#include "vfs_fat_internal.h"

#include "espix_fs.h"
#include "espix_kernel.h"

#include "espix_fs_priv.h"

#define TAG "fatfs"

/* Open files per volume. Five is FatFs's own default for FIL capacity. */
#define FAT_MAX_FILES 5

/*
 * One mount. Handed to espix_vfs_add_mount() as the ops' context, so a shim gets
 * its own record back as `ctx` and needs no lookup -- and so the prefix cannot
 * be confused with another mount's.
 */
typedef struct {
    void                   *fat_ctx;   /* vfs_fat_ctx_t; opaque outside IDF */
    const esp_vfs_fs_ops_t *ops;       /* IDF's tables, driven through */
    FATFS                  *fs;
    BYTE                    pdrv;
    char                    drive[3];  /* "0:" */
    char                    prefix[ESPIX_FS_PREFIX_MAX];
    size_t                  len;
    bool                    used;
} fat_mount_t;

static fat_mount_t s_mounts[ESPIX_FS_MAX_MOUNTS];

/* Reserving a slot touches no I/O, so the same handful-of-instructions rule as
 * vfs.c applies: a critical section, not a mutex. */
static portMUX_TYPE s_fat_lock = portMUX_INITIALIZER_UNLOCKED;

static fat_mount_t *slot_reserve(const char *prefix)
{
    const size_t len = strlen(prefix);

    portENTER_CRITICAL(&s_fat_lock);
    for (size_t i = 0; i < ESPIX_FS_MAX_MOUNTS; i++) {
        if (s_mounts[i].used) {
            continue;
        }
        s_mounts[i].used = true;
        s_mounts[i].len  = len;
        strlcpy(s_mounts[i].prefix, prefix, sizeof(s_mounts[i].prefix));
        portEXIT_CRITICAL(&s_fat_lock);
        return &s_mounts[i];
    }
    portEXIT_CRITICAL(&s_fat_lock);
    return NULL;
}

static void slot_release(fat_mount_t *m)
{
    portENTER_CRITICAL(&s_fat_lock);
    m->used = false;
    m->len  = 0;
    m->prefix[0] = '\0';
    portEXIT_CRITICAL(&s_fat_lock);
}

static fat_mount_t *slot_find(const char *prefix)
{
    fat_mount_t *found = NULL;

    portENTER_CRITICAL(&s_fat_lock);
    for (size_t i = 0; i < ESPIX_FS_MAX_MOUNTS; i++) {
        if (s_mounts[i].used && strcmp(s_mounts[i].prefix, prefix) == 0) {
            found = &s_mounts[i];
            break;
        }
    }
    portEXIT_CRITICAL(&s_fat_lock);
    return found;
}

/*
 * What espix calls "/mnt/dir/file" is what FatFs calls "/dir/file": espix routes
 * by prefix and calls the driver directly, so the stripping esp_vfs would have
 * done never happened. The mount point itself is FatFs's root, not an empty
 * name.
 */
static const char *fat_relative(const fat_mount_t *m, const char *path)
{
    const char *rest = path + m->len;
    return (*rest == '\0') ? "/" : rest;
}

/* The FRESULTs worth telling apart in a message; the rest share one name. */
static const char *fresult_name(FRESULT fr)
{
    switch (fr) {
    case FR_OK:                  return "ok";
    case FR_DISK_ERR:            return "disk error";
    case FR_INT_ERR:             return "internal error";
    case FR_NOT_READY:           return "drive not ready";
    case FR_NO_FILE:             return "no such file";
    case FR_NO_PATH:             return "no such path";
    case FR_INVALID_NAME:        return "invalid name";
    case FR_DENIED:              return "denied";
    case FR_EXIST:               return "already exists";
    case FR_WRITE_PROTECTED:     return "write protected";
    case FR_NOT_ENABLED:         return "volume not mounted";
    case FR_NO_FILESYSTEM:       return "not a FAT filesystem";
    case FR_TOO_MANY_OPEN_FILES: return "too many open files";
    case FR_NOT_ENOUGH_CORE:     return "out of memory";
    case FR_LOCKED:              return "volume is locked";
    default:                     return "unhandled error";
    }
}
/* ------------------------------------------------------------------ */
/* The shims                                                           */
/* ------------------------------------------------------------------ */

/*
 * Paths carry the mount prefix and have to lose it; fds and directory handles
 * carry none, and only need IDF's own context back. The second kind is a
 * one-liner each, which is the honest price of driving IDF's tables directly --
 * see the top of this file.
 */

static int fat_open(void *ctx, const char *path, int flags, int mode)
{
    fat_mount_t *m = ctx;
    return m->ops->open_p(m->fat_ctx, fat_relative(m, path), flags, mode);
}

static int fat_stat(void *ctx, const char *path, struct stat *st)
{
    fat_mount_t *m = ctx;
    return m->ops->dir->stat_p(m->fat_ctx, fat_relative(m, path), st);
}

static int fat_unlink(void *ctx, const char *path)
{
    fat_mount_t *m = ctx;
    return m->ops->dir->unlink_p(m->fat_ctx, fat_relative(m, path));
}

static int fat_rename(void *ctx, const char *src, const char *dst)
{
    fat_mount_t *m = ctx;
    return m->ops->dir->rename_p(m->fat_ctx, fat_relative(m, src),
                                 fat_relative(m, dst));
}

static DIR *fat_opendir(void *ctx, const char *name)
{
    fat_mount_t *m = ctx;
    return m->ops->dir->opendir_p(m->fat_ctx, fat_relative(m, name));
}

static int fat_mkdir(void *ctx, const char *name, mode_t mode)
{
    fat_mount_t *m = ctx;
    return m->ops->dir->mkdir_p(m->fat_ctx, fat_relative(m, name), mode);
}

static int fat_rmdir(void *ctx, const char *name)
{
    fat_mount_t *m = ctx;
    return m->ops->dir->rmdir_p(m->fat_ctx, fat_relative(m, name));
}

static int fat_truncate(void *ctx, const char *path, off_t length)
{
    fat_mount_t *m = ctx;
    return m->ops->dir->truncate_p(m->fat_ctx, fat_relative(m, path), length);
}

static int fat_utime(void *ctx, const char *path, const struct utimbuf *times)
{
    fat_mount_t *m = ctx;
    return m->ops->dir->utime_p(m->fat_ctx, fat_relative(m, path), times);
}

static int fat_close(void *ctx, int fd)
{
    fat_mount_t *m = ctx;
    return m->ops->close_p(m->fat_ctx, fd);
}

static ssize_t fat_read(void *ctx, int fd, void *dst, size_t size)
{
    fat_mount_t *m = ctx;
    return m->ops->read_p(m->fat_ctx, fd, dst, size);
}

static ssize_t fat_write(void *ctx, int fd, const void *data, size_t size)
{
    fat_mount_t *m = ctx;
    return m->ops->write_p(m->fat_ctx, fd, data, size);
}

static ssize_t fat_pread(void *ctx, int fd, void *dst, size_t size, off_t off)
{
    fat_mount_t *m = ctx;
    return m->ops->pread_p(m->fat_ctx, fd, dst, size, off);
}

static ssize_t fat_pwrite(void *ctx, int fd, const void *src, size_t size,
                          off_t off)
{
    fat_mount_t *m = ctx;
    return m->ops->pwrite_p(m->fat_ctx, fd, src, size, off);
}

static off_t fat_lseek(void *ctx, int fd, off_t off, int mode)
{
    fat_mount_t *m = ctx;
    return m->ops->lseek_p(m->fat_ctx, fd, off, mode);
}

static int fat_fstat(void *ctx, int fd, struct stat *st)
{
    fat_mount_t *m = ctx;
    return m->ops->fstat_p(m->fat_ctx, fd, st);
}

static int fat_fcntl(void *ctx, int fd, int cmd, int arg)
{
    fat_mount_t *m = ctx;
    return m->ops->fcntl_p(m->fat_ctx, fd, cmd, arg);
}

static int fat_fsync(void *ctx, int fd)
{
    fat_mount_t *m = ctx;
    return m->ops->fsync_p(m->fat_ctx, fd);
}

static int fat_ftruncate(void *ctx, int fd, off_t length)
{
    fat_mount_t *m = ctx;
    return m->ops->dir->ftruncate_p(m->fat_ctx, fd, length);
}

static struct dirent *fat_readdir(void *ctx, DIR *pdir)
{
    fat_mount_t *m = ctx;
    return m->ops->dir->readdir_p(m->fat_ctx, pdir);
}

static int fat_readdir_r(void *ctx, DIR *pdir, struct dirent *entry,
                         struct dirent **out)
{
    fat_mount_t *m = ctx;
    return m->ops->dir->readdir_r_p(m->fat_ctx, pdir, entry, out);
}

static long fat_telldir(void *ctx, DIR *pdir)
{
    fat_mount_t *m = ctx;
    return m->ops->dir->telldir_p(m->fat_ctx, pdir);
}

static void fat_seekdir(void *ctx, DIR *pdir, long offset)
{
    fat_mount_t *m = ctx;
    m->ops->dir->seekdir_p(m->fat_ctx, pdir, offset);
}

static int fat_closedir(void *ctx, DIR *pdir)
{
    fat_mount_t *m = ctx;
    return m->ops->dir->closedir_p(m->fat_ctx, pdir);
}

/*
 * espix's table. `link_p` and `access_p` stay NULL, mirroring the root: FatFs
 * has no links, and an `access` that suddenly worked would change behaviour
 * nothing asked to change. Everything else is a shim above.
 */
static const esp_vfs_dir_ops_t s_fat_dir = {
    .stat_p      = &fat_stat,
    .link_p      = NULL,
    .unlink_p    = &fat_unlink,
    .rename_p    = &fat_rename,
    .opendir_p   = &fat_opendir,
    .readdir_p   = &fat_readdir,
    .readdir_r_p = &fat_readdir_r,
    .telldir_p   = &fat_telldir,
    .seekdir_p   = &fat_seekdir,
    .closedir_p  = &fat_closedir,
    .mkdir_p     = &fat_mkdir,
    .rmdir_p     = &fat_rmdir,
    .access_p    = NULL,
    .truncate_p  = &fat_truncate,
    .ftruncate_p = &fat_ftruncate,
    .utime_p     = &fat_utime,
};

static const esp_vfs_fs_ops_t s_fat_ops = {
    .write_p  = &fat_write,
    .lseek_p  = &fat_lseek,
    .read_p   = &fat_read,
    .pread_p  = &fat_pread,
    .pwrite_p = &fat_pwrite,
    .open_p   = &fat_open,
    .close_p  = &fat_close,
    .fstat_p  = &fat_fstat,
    .fcntl_p  = &fat_fcntl,
    .fsync_p  = &fat_fsync,
    .dir      = &s_fat_dir,
};

/* ------------------------------------------------------------------ */
/* Public                                                              */
/* ------------------------------------------------------------------ */

esp_err_t espix_fs_mount_fat(const char *path, esp_blockdev_handle_t dev)
{
    if (path == NULL || dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Ahead of the driver: a device that reads as empty is a different complaint
     * from a volume that is not FAT, and worth saying so before mounting. */
    if (dev->geometry.disk_size == 0) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "%s: the device reports no size",
                   path);
        return ESP_ERR_INVALID_ARG;
    }

    fat_mount_t *m = slot_reserve(path);
    if (m == NULL) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "%s: no free mount slot", path);
        return ESP_ERR_NO_MEM;
    }

    BYTE pdrv = 0xFF;
    esp_err_t err = ff_diskio_get_drive(&pdrv);
    if (err != ESP_OK) {
        /*
         * Every FatFs volume is in use. That has to leave here as something
         * other than what the diskio layer returned: ESP_ERR_NOT_FOUND is also
         * what "this is not a FAT filesystem" means by the time a caller sees
         * it, and blaming the filesystem for a volume limit is the kind of
         * message that costs somebody an hour. IDF's own vfs_fat_bdl.c makes the
         * same translation, to the same code.
         */
        espix_klog(ESPIX_KLOG_WARN, TAG, "%s: no free volume (FF_VOLUMES is %d)",
                   path, FF_VOLUMES);
        err = ESP_ERR_NO_MEM;
        goto fail_slot;
    }
    err = ff_diskio_register_bdl(pdrv, dev);
    if (err != ESP_OK) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "%s: cannot bind the device: %s",
                   path, esp_err_to_name(err));
        goto fail_slot;
    }
    m->pdrv = pdrv;
    /* Built the way IDF builds it: a drive number is one digit because FatFs
     * numbers volumes from "0:", and ff_diskio_get_drive() fails rather than
     * handing out a bigger one. */
    m->drive[0] = (char)('0' + pdrv);
    m->drive[1] = ':';
    m->drive[2] = '\0';

    /*
     * base_path is bookkeeping: the patched context never strips it, because
     * this file already has by the time IDF sees a path. It is the mount point
     * so that a log line from IDF names the right volume.
     */
    const esp_vfs_fat_conf_t conf = {
        .base_path = (char *)path,
        .fat_drive = m->drive,
        .max_files = FAT_MAX_FILES,
    };
    err = esp_vfs_fat_ctx_create(&conf, &m->fs, &m->fat_ctx);
    if (err != ESP_OK) {
        goto fail_diskio;
    }

    /* opt = 1: mount now and answer through the return value, which is how a
     * volume that is not FAT gets named instead of formatted. */
    const FRESULT fr = f_mount(m->fs, m->drive, 1);
    if (fr != FR_OK) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "%s: %s", path, fresult_name(fr));
        err = (fr == FR_NO_FILESYSTEM || fr == FR_NO_FILE)
                  ? ESP_ERR_NOT_FOUND : ESP_FAIL;
        goto fail_ctx;
    }

    m->ops = esp_vfs_fat_get_ops();

    /* Published last: until this returns, no path can reach the mount. */
    err = espix_vfs_add_mount(path, &s_fat_ops, m, false);
    if (err != ESP_OK) {
        f_mount(NULL, m->drive, 0);
        goto fail_ctx;
    }

    espix_klog(ESPIX_KLOG_INFO, TAG, "mounted %s at %s", m->drive, path);
    return ESP_OK;

fail_ctx:
    esp_vfs_fat_ctx_free(m->fat_ctx);
    m->fat_ctx = NULL;
    m->fs      = NULL;
fail_diskio:
    ff_diskio_unregister(m->pdrv);
fail_slot:
    slot_release(m);
    return err;
}

esp_err_t espix_fs_unmount_fat(const char *path)
{
    if (path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    fat_mount_t *m = slot_find(path);
    if (m == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    /*
     * Removed from the namespace first, and refused while a file or directory
     * is still open on it -- the volume does not come out from under a reader.
     * See espix_vfs_del_mount().
     */
    const esp_err_t err = espix_vfs_del_mount(path);
    if (err != ESP_OK) {
        return err;
    }

    const FRESULT fr = f_mount(NULL, m->drive, 0);
    if (fr != FR_OK) {
        /* Reported, not retried: the mount is gone from espix's table already,
         * and putting it back would leave a volume nothing routes to. */
        espix_klog(ESPIX_KLOG_WARN, TAG, "%s: unmounting the volume: %s",
                   path, fresult_name(fr));
    }

    ff_diskio_unregister(m->pdrv);
    esp_vfs_fat_ctx_free(m->fat_ctx);
    m->fat_ctx = NULL;
    m->fs      = NULL;
    m->ops     = NULL;
    slot_release(m);

    espix_klog(ESPIX_KLOG_INFO, TAG, "unmounted %s", path);
    return ESP_OK;
}

