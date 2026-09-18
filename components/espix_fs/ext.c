/*
 * ext2/3/4, mounted under espix's VFS.
 *
 * The third filesystem driver, after LittleFS (the root) and FatFs (fat.c), and
 * deliberately the same shape as fat.c: IDF's VFS is not used, because a
 * filesystem registered at a prefix is reachable there with espix's permission
 * check skipped. See the top of fat.c and components/espix_fs/vfs.c.
 *
 * The library is lwext4, through the vendored esp_lwext4 component in
 * components/esp_lwext4. What that component gives this file, and what it does
 * not, is the shape of the code below:
 *
 *   * it supplies the block-device adapter (lwext4_port_bdl_*), written against
 *     esp_blockdev -- the same interface espix_fs_partition_view() hands out, so
 *     the lower half needs no translation at all;
 *   * it supplies the allocator and the generated configuration;
 *   * it does NOT mount. Its own VFS adapter registers a mount with IDF, which
 *     is the thing this file exists to avoid, so mounting is done here with
 *     lwext4's ext4_device_register()/ext4_mount();
 *   * it does NOT give a file-descriptor table, unlike FatFs's glue. The caller
 *     owns the ext4_file, so s_handles below is espix's own -- the genuinely new
 *     piece compared to fat.c, and why open/close are more than a passthrough.
 *
 * Read-only unless the caller asks otherwise, and the default is deliberate: an
 * ext volume mounted here is usually somebody's only copy of something, and the
 * writes go through the port's experimental extent implementation
 * (components/esp_lwext4/doc/CAVEATS.md). ext4_mount() takes read_only as an
 * argument, every mutating op below consults the same flag, and the adapter is
 * built read-only or writable to match -- so the three agree by construction
 * rather than by discipline. A writable mount additionally needs a journal, which
 * espix starts (lwext4's ext4_mount() does not) and which is also where journal
 * recovery happens; a volume without one is refused rather than quietly mounted
 * read-only. docs/ROADMAP.md has the caveats this rests on.
 *
 * Locking is one recursive mutex for every mount, because lwext4's own lock
 * hooks (struct ext4_lock) take no context argument -- so a per-mount lock
 * cannot be expressed through them, and nesting within one operation means a
 * plain mutex would deadlock.
 */

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_vfs_ops.h"

#include "ext4.h"
#include "ext4_crc32.h"
#include "ext4_inode.h"
#include "lwext4_port_bdl.h"

#include "espix_fs.h"
#include "espix_kernel.h"

#include "espix_fs_priv.h"

#define TAG "ext4"

/*
 * Open handles, files and directories together. Eight rather than FatFs's five:
 * a directory walk holds one open per level, so a deep tree spends several on a
 * single listing. Bounded rather than grown because a handle is small and a
 * filesystem that can allocate without limit is a way to run the heap out from
 * a path that returns no error.
 */
#define EXT_MAX_HANDLES 8

/*
 * lwext4's own limits, per CONFIG_EXT4_MOUNTPOINTS_COUNT and
 * CONFIG_EXT4_BLOCKDEVS_COUNT -- both 2 in the vendored component. Two is also
 * what CONFIG_FATFS_VOLUME_COUNT gives the FAT side, so the two drivers together
 * fit ESPIX_FS_MAX_MOUNTS.
 */
#define EXT_MAX_MOUNTS 2

typedef struct {
    bool               used;
    /*
     * What the mount was asked for. Kept per mount so the mutating ops can answer
     * EROFS themselves: a write to a read-only mount is a wrong request rather
     * than a filesystem that failed, and refusing it here keeps lwext4 from doing
     * the work of finding that out.
     */
    bool               read_only;
    lwext4_port_bdl_t *adapter;
    char               dev[16];                   /* registered device name */
    char               mp[16];                    /* lwext4 mount point */
    char               prefix[ESPIX_FS_PREFIX_MAX];
    size_t             len;
} ext_mount_t;

typedef struct {
    bool          used;
    bool          is_dir;
    /*
     * The mount this was opened on, for its read-only flag and for the path
     * ext4_cache_flush() wants. A mount cannot be released while a handle is open
     * on it -- espix_fs_unmount_ext() refuses that -- so this cannot dangle.
     */
    ext_mount_t  *m;
    /* The path as lwext4 spells it, for the calls that take one. */
    char          path[192];
    ext4_file     file;
    ext4_dir      dir;
    /* Read once at open. See ext_fstat(). */
    struct stat   st;
    /* What readdir() hands back, owned by the handle so it outlives the call. */
    struct dirent cur;
    /* Entry count, for telldir/seekdir. lwext4 offers no position to hand out. */
    long          offset;
} ext_handle_t;

static ext_mount_t  s_mounts[EXT_MAX_MOUNTS];
static ext_handle_t s_handles[EXT_MAX_HANDLES];

/* See the locking note at the top of the file. NULL until the first mount. */
static SemaphoreHandle_t s_lock;

/* lwext4 reaches these through ext4_mount_setup_locks(). No context argument,
 * which is why there is one lock and not one per mount. */
static void ext_lock(void)
{
    if (s_lock != NULL) {
        xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    }
}

static void ext_unlock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGiveRecursive(s_lock);
    }
}

static const struct ext4_lock s_ext_locks = {
    .lock   = ext_lock,
    .unlock = ext_unlock,
};

/* ------------------------------------------------------------------ */
/* Slots                                                               */
/* ------------------------------------------------------------------ */

/* Reserving a slot touches no I/O, so the same critical-section rule as fat.c
 * applies rather than a mutex. */
static portMUX_TYPE s_slot_lock = portMUX_INITIALIZER_UNLOCKED;

static ext_mount_t *mount_reserve(const char *prefix)
{
    const size_t len = strlen(prefix);

    portENTER_CRITICAL(&s_slot_lock);
    for (size_t i = 0; i < EXT_MAX_MOUNTS; i++) {
        if (s_mounts[i].used) {
            continue;
        }
        s_mounts[i].used = true;
        s_mounts[i].len  = len;
        strlcpy(s_mounts[i].prefix, prefix, sizeof(s_mounts[i].prefix));
        portEXIT_CRITICAL(&s_slot_lock);
        return &s_mounts[i];
    }
    portEXIT_CRITICAL(&s_slot_lock);
    return NULL;
}

static void mount_release(ext_mount_t *m)
{
    portENTER_CRITICAL(&s_slot_lock);
    m->used      = false;
    m->len       = 0;
    m->prefix[0] = '\0';
    portEXIT_CRITICAL(&s_slot_lock);
}

/* The mount at `prefix`, or NULL. A whole-string comparison: a mount point is
 * named by the caller and hidden for as long as it lasts, so there is no
 * longest-prefix question to answer here -- espix's VFS has already decided
 * which mount a path belongs to by the time a driver sees one. */
static ext_mount_t *mount_find(const char *prefix)
{
    ext_mount_t *found = NULL;

    portENTER_CRITICAL(&s_slot_lock);
    for (size_t i = 0; i < EXT_MAX_MOUNTS; i++) {
        if (s_mounts[i].used && strcmp(s_mounts[i].prefix, prefix) == 0) {
            found = &s_mounts[i];
            break;
        }
    }
    portEXIT_CRITICAL(&s_slot_lock);
    return found;
}

static ext_handle_t *handle_alloc(bool is_dir)
{
    ext_handle_t *h = NULL;

    portENTER_CRITICAL(&s_slot_lock);
    for (size_t i = 0; i < EXT_MAX_HANDLES; i++) {
        if (!s_handles[i].used) {
            memset(&s_handles[i], 0, sizeof(s_handles[i]));
            s_handles[i].used   = true;
            s_handles[i].is_dir = is_dir;
            h = &s_handles[i];
            break;
        }
    }
    portEXIT_CRITICAL(&s_slot_lock);
    return h;
}

static void handle_free(ext_handle_t *h)
{
    if (h == NULL) {
        return;
    }

    portENTER_CRITICAL(&s_slot_lock);
    h->used = false;
    portEXIT_CRITICAL(&s_slot_lock);
}

/* The handle an fd names, or NULL. The fd is an index into s_handles, which is
 * what open() returns and espix's VFS keeps for us. */
static ext_handle_t *handle_of(int fd)
{
    if (fd < 0 || fd >= EXT_MAX_HANDLES) {
        return NULL;
    }
    return s_handles[fd].used ? &s_handles[fd] : NULL;
}

static int handle_index(const ext_handle_t *h)
{
    return (int)(h - s_handles);
}

/* The DIR pointer espix's VFS maps back to us. DIR is opaque and nothing here
 * reads through it, so the handle's own address serves. */
static ext_handle_t *handle_of_dir(DIR *pdir)
{
    ext_handle_t *h = (ext_handle_t *)pdir;

    if (h == NULL || h < s_handles || h >= s_handles + EXT_MAX_HANDLES) {
        return NULL;
    }
    return h->used ? h : NULL;
}

/* ------------------------------------------------------------------ */
/* Paths                                                               */
/* ------------------------------------------------------------------ */

/*
 * What espix calls "/mnt/file" is what lwext4 calls "/ext0/file": espix routes
 * by prefix and calls the driver directly, so the stripping esp_vfs would have
 * done never happened, and lwext4 wants its own mount point in front.
 *
 * That mount point carries a trailing slash -- ext4_mount() refuses one without
 * (ext4.c:380, ENOTSUP) -- and lwext4 builds paths by concatenation, so the
 * caller's leading '/' is dropped rather than repeated: "/ext0/" plus
 * "etc/hosts". An empty rest is the mount root, which is the mount point as it
 * stands.
 *
 * False when the result does not fit, which is reachable -- prefix plus a long
 * path can exceed any single buffer -- and is reported as ENAMETOOLONG rather
 * than truncated, because a truncated path names a different file.
 */
static bool ext_path(const ext_mount_t *m, const char *path,
                     char *out, size_t out_len)
{
    const char *rest = path + m->len;

    if (*rest == '/') {
        rest++;
    }

    const int n = snprintf(out, out_len, "%s%s", m->mp, rest);
    return n > 0 && (size_t)n < out_len;
}

/*
 * The mount a path belongs to, and the path as lwext4 spells it. One pass,
 * because a caller needs both and the two must agree.
 */
typedef struct {
    ext_mount_t *m;
    char         path[192];
} ext_route_t;

static int ext_resolve(const char *path, ext_route_t *r)
{
    if (path == NULL) {
        return EINVAL;
    }

    for (size_t i = 0; i < EXT_MAX_MOUNTS; i++) {
        if (s_mounts[i].used &&
            strncmp(path, s_mounts[i].prefix, s_mounts[i].len) == 0) {
            r->m = &s_mounts[i];
            return ext_path(&s_mounts[i], path, r->path, sizeof(r->path))
                       ? 0 : ENAMETOOLONG;
        }
    }
    return ENOENT;
}

/* ------------------------------------------------------------------ */
/* stat                                                                */
/* ------------------------------------------------------------------ */

/*
 * One inode read answers everything stat needs, which is worth saying: lwext4's
 * public API has no fstat and no stat, and the alternative -- open, size, close
 * -- would be three operations and a handle for a question about a path.
 *
 * st_size is the inode's own size, and off_t is 64 bits (cmake/offt64.h), so a
 * file over 4 GiB reports its real size. It did not while off_t was a 32-bit
 * `long`: the filesystem could express the size and the type could not, which is
 * why `ls -l` showed the low half of it.
 */
static int stat_locked(const char *path, struct stat *st)
{
    struct ext4_inode   inode;
    struct ext4_sblock *sb  = NULL;
    uint32_t            ino = 0;
    int                 err;

    memset(st, 0, sizeof(*st));

    err = ext4_raw_inode_fill(path, &ino, &inode);
    if (err != EOK) {
        return err;
    }
    err = ext4_get_sblock(path, &sb);
    if (err != EOK) {
        return err;
    }

    st->st_ino   = ino;
    /* Carries the type bits as well as the permissions: ext4_inode_get_mode()
     * does not mask, and its type values are the S_IFMT ones. */
    st->st_mode  = ext4_inode_get_mode(sb, &inode);
    st->st_nlink = ext4_inode_get_links_cnt(&inode);
    st->st_uid   = ext4_inode_get_uid(&inode);
    st->st_gid   = ext4_inode_get_gid(&inode);
    st->st_size  = (off_t)ext4_inode_get_size(sb, &inode);
    st->st_atime = ext4_inode_get_access_time(&inode);
    st->st_mtime = ext4_inode_get_modif_time(&inode);
    st->st_ctime = ext4_inode_get_change_inode_time(&inode);
    st->st_blksize = 512;

    return 0;
}

/* ------------------------------------------------------------------ */
/* Files                                                               */
/* ------------------------------------------------------------------ */

/* A mutating op on a read-only mount. Every one of them lands here rather than
 * being left NULL, so espix's VFS never has to decide what a missing op means,
 * and the errno is the one a write-protected device gives. */
static int refusero(void)
{
    errno = EROFS;
    return -1;
}

static int ext_open(void *ctx, const char *path, int flags, int mode)
{
    (void)ctx;

    ext_route_t r;
    const int   err = ext_resolve(path, &r);
    if (err != 0) {
        errno = err;
        return -1;
    }

    /*
     * A write on a read-only mount is refused here rather than by lwext4, which
     * would answer the same way: the request is wrong, not the filesystem. Doing
     * it first also keeps a handle from being taken for an open that was never
     * going to succeed.
     */
    const bool mutates = ((flags & O_ACCMODE) != O_RDONLY) ||
                         ((flags & (O_CREAT | O_TRUNC)) != 0);
    if (mutates && r.m->read_only) {
        return refusero();
    }

    ext_handle_t *h = handle_alloc(false);
    if (h == NULL) {
        errno = EMFILE;
        return -1;
    }
    h->m = r.m;
    snprintf(h->path, sizeof(h->path), "%s", r.path);

    /*
     * The caller's flags, masked to the ones ext4_fopen2() takes -- access mode,
     * create, exclusive, truncate, append. The same bits, so this is a
     * translation rather than a mapping; espix's VFS passes its own flags
     * through, and they can carry more than these.
     */
    const int wanted = flags & (O_ACCMODE | O_CREAT | O_EXCL | O_TRUNC | O_APPEND);

    /*
     * Whether the file is already there, asked *before* the open rather than after
     * it: after the open it exists either way, and that difference is what says
     * whether the caller's mode should be applied. Asked after, the answer is
     * always yes and the mode is never set -- which is how the first version of
     * this left every new file at 0666.
     */
    bool existed = false;

    if ((flags & O_CREAT) != 0) {
        uint32_t ignored = 0;

        ext_lock();
        existed = (ext4_mode_get(r.path, &ignored) == EOK);
        ext_unlock();
    }

    ext_lock();
    const int r2 = ext4_fopen2(&h->file, r.path, wanted);

    /*
     * A create gets the caller's mode through the usual 022 -- espix has no umask
     * setting yet (see the mount options in docs/ROADMAP.md), and the alternative
     * is worse than a hardcoded one: lwext4's create leaves the new inode at 0666,
     * so every file written to a stick would be world-writable.
     */
    int r3 = r2;

    if (r2 == EOK && (flags & O_CREAT) != 0 && !existed) {
        r3 = ext4_mode_set(r.path, (uint32_t)(mode & 0777 & ~022));
    }

    /* Read now rather than at fstat(): the inode is already to hand, and an
     * fstat that could fail is one more way for close to be reached by a path
     * that has no error to report. */
    if (r3 == EOK) {
        r3 = stat_locked(r.path, &h->st);
    }
    if (r3 != EOK && r2 == EOK) {
        ext4_fclose(&h->file);
    }
    ext_unlock();

    if (r3 != EOK) {
        handle_free(h);
        errno = r3;
        return -1;
    }

    return handle_index(h);
}

static int ext_close(void *ctx, int fd)
{
    (void)ctx;

    ext_handle_t *h = handle_of(fd);
    if (h == NULL) {
        errno = EBADF;
        return -1;
    }

    ext_lock();
    const int err = ext4_fclose(&h->file);
    ext_unlock();

    handle_free(h);

    if (err != EOK) {
        errno = err;
        return -1;
    }
    return 0;
}

static ssize_t ext_read(void *ctx, int fd, void *dst, size_t size)
{
    (void)ctx;

    ext_handle_t *h = handle_of(fd);
    if (h == NULL) {
        errno = EBADF;
        return -1;
    }
    if (h->is_dir) {
        errno = EISDIR;
        return -1;
    }

    size_t    rcnt = 0;
    ext_lock();
    const int err = ext4_fread(&h->file, dst, size, &rcnt);
    ext_unlock();

    if (err != EOK) {
        errno = err;
        return -1;
    }
    /* A short count is an answer, not a failure: lwext4 stops at end of file and
     * reports what it read, the way read(2) does. */
    return (ssize_t)rcnt;
}

/*
 * Read at an offset without moving the file position, which lwext4 has no call
 * for. Save, seek, read, restore -- under one lock hold, so another reader
 * cannot observe the position in between. The restore matters as much as the
 * seek: POSIX leaves the position alone across a pread, and espix's own
 * espix_dev_pread sets the same standard.
 */
static ssize_t ext_pread(void *ctx, int fd, void *dst, size_t size, off_t off)
{
    (void)ctx;

    ext_handle_t *h = handle_of(fd);
    if (h == NULL) {
        errno = EBADF;
        return -1;
    }
    if (h->is_dir) {
        errno = EISDIR;
        return -1;
    }
    if (off < 0) {
        errno = EINVAL;
        return -1;
    }

    size_t  rcnt = 0;
    ssize_t out  = -1;

    ext_lock();
    const uint64_t saved = ext4_ftell(&h->file);

    if (ext4_fseek(&h->file, (int64_t)off, SEEK_SET) != EOK) {
        errno = EINVAL;
    } else {
        const int err = ext4_fread(&h->file, dst, size, &rcnt);
        if (err != EOK) {
            errno = err;
        } else {
            out = (ssize_t)rcnt;
        }
    }

    /* Restored even on failure: the position is the caller's, and an error is
     * not a licence to move it. */
    ext4_fseek(&h->file, (int64_t)saved, SEEK_SET);
    ext_unlock();

    return out;
}

static off_t ext_lseek(void *ctx, int fd, off_t off, int whence)
{
    (void)ctx;

    ext_handle_t *h = handle_of(fd);
    if (h == NULL) {
        errno = EBADF;
        return -1;
    }
    if (h->is_dir) {
        errno = EISDIR;
        return -1;
    }
    /* Past either end is legal in POSIX. What is not expressible is an offset
     * beyond off_t, which the caller could not have named either. */
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) {
        errno = EINVAL;
        return -1;
    }

    ext_lock();
    const int      err = ext4_fseek(&h->file, (int64_t)off, (uint32_t)whence);
    const uint64_t pos = ext4_ftell(&h->file);
    ext_unlock();

    if (err != EOK) {
        errno = EINVAL;
        return -1;
    }
    return (off_t)pos;
}

static int ext_fstat(void *ctx, int fd, struct stat *st)
{
    (void)ctx;

    ext_handle_t *h = handle_of(fd);
    if (h == NULL || st == NULL) {
        errno = EBADF;
        return -1;
    }
    if (h->is_dir) {
        errno = EISDIR;
        return -1;
    }

    /*
     * Read the inode now rather than answering from the snapshot the open took: a
     * writable mount means the size and the times can have changed since, and a
     * cached answer that outlives a write is worse than the cost of one lookup.
     */
    ext_lock();
    const int rc = stat_locked(h->path, st);
    ext_unlock();

    if (rc != EOK) {
        errno = rc;
        return -1;
    }
    h->st = *st;
    return 0;
}

static ssize_t ext_write(void *ctx, int fd, const void *data, size_t size)
{
    (void)ctx;

    ext_handle_t *h = handle_of(fd);
    if (h == NULL) {
        errno = EBADF;
        return -1;
    }
    if (h->is_dir) {
        errno = EISDIR;
        return -1;
    }
    if (h->m->read_only) {
        return refusero();
    }

    size_t    wcnt = 0;
    ext_lock();
    const int err = ext4_fwrite(&h->file, data, size, &wcnt);
    ext_unlock();

    if (err != EOK) {
        errno = err;
        return -1;
    }
    /* A short count is an answer, not a failure: lwext4 stops where it can no
     * longer write and reports what it did, the way write(2) does. */
    return (ssize_t)wcnt;
}

/*
 * Write at an offset without moving the file position -- the mirror of ext_pread,
 * and for the same reason: lwext4 has no call for it. Save, seek, write, restore,
 * under one lock hold so another writer cannot observe the position in between.
 */
static ssize_t ext_pwrite(void *ctx, int fd, const void *src, size_t size, off_t off)
{
    (void)ctx;

    ext_handle_t *h = handle_of(fd);
    if (h == NULL) {
        errno = EBADF;
        return -1;
    }
    if (h->is_dir) {
        errno = EISDIR;
        return -1;
    }
    if (off < 0) {
        errno = EINVAL;
        return -1;
    }
    if (h->m->read_only) {
        return refusero();
    }

    size_t  wcnt = 0;
    ssize_t out  = -1;

    ext_lock();
    const uint64_t saved = ext4_ftell(&h->file);

    if (ext4_fseek(&h->file, (int64_t)off, SEEK_SET) != EOK) {
        errno = EINVAL;
    } else {
        const int err = ext4_fwrite(&h->file, src, size, &wcnt);
        if (err != EOK) {
            errno = err;
        } else {
            out = (ssize_t)wcnt;
        }
    }

    /* Restored even on failure: the position is the caller's. */
    ext4_fseek(&h->file, (int64_t)saved, SEEK_SET);
    ext_unlock();

    return out;
}

/*
 * Put what was written on the medium, on demand.
 *
 * lwext4 keeps a block cache, so a write is not durable when write() returns. The
 * adapter is configured with sync_after_write, which means each write is already
 * handed to the device rather than sitting in the adapter as well; what is left is
 * the cache above it, which ext4_cache_flush() writes out and which the port names
 * as the explicit durability checkpoint. A read-only mount has nothing to flush.
 */
static int ext_fsync(void *ctx, int fd)
{
    (void)ctx;

    ext_handle_t *h = handle_of(fd);
    if (h == NULL) {
        errno = EBADF;
        return -1;
    }
    if (h->m->read_only) {
        return 0;
    }

    ext_lock();
    const int err = ext4_cache_flush(h->m->mp);
    ext_unlock();

    if (err != EOK) {
        errno = err;
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Directories                                                         */
/* ------------------------------------------------------------------ */

static int ext_stat(void *ctx, const char *path, struct stat *st)
{
    (void)ctx;

    if (st == NULL) {
        errno = EINVAL;
        return -1;
    }

    ext_route_t r;
    const int   err = ext_resolve(path, &r);
    if (err != 0) {
        errno = err;
        return -1;
    }

    ext_lock();
    const int r2 = stat_locked(r.path, st);
    ext_unlock();

    if (r2 != EOK) {
        errno = r2;
        return -1;
    }
    return 0;
}

/* lwext4 reports a type as EXT4_DE_*, a different enum from DT_*, so this maps
 * rather than casts. Anything unrecognised is DT_UNKNOWN, which is a legal
 * answer and asks the caller to stat. */
static unsigned char ext_dtype(uint8_t inode_type)
{
    switch (inode_type) {
    case EXT4_DE_REG_FILE: return DT_REG;
    case EXT4_DE_DIR:      return DT_DIR;
    case EXT4_DE_CHRDEV:   return DT_CHR;
    case EXT4_DE_BLKDEV:   return DT_BLK;
    case EXT4_DE_FIFO:     return DT_FIFO;
    case EXT4_DE_SOCK:     return DT_SOCK;
    case EXT4_DE_SYMLINK:  return DT_LNK;
    default:               return DT_UNKNOWN;
    }
}

static void ext_fill_dirent(struct dirent *out, const ext4_direntry *de)
{
    memset(out, 0, sizeof(*out));

    out->d_ino  = de->inode;
    out->d_type = ext_dtype(de->inode_type);

    /* ext4 names are at most 255 bytes and d_name is 256, so the clamp cannot
     * bite; it is here so that it stays that way. */
    size_t n = de->name_length;
    if (n > sizeof(out->d_name) - 1) {
        n = sizeof(out->d_name) - 1;
    }
    memcpy(out->d_name, de->name, n);
    out->d_name[n] = '\0';
}

/*
 * One entry, or NULL at the end of the directory.
 *
 * lwext4 cannot say whether it stopped because it ran out of entries or because
 * reading one failed -- ext4_dir_entry_next() returns NULL for both, with no
 * error to check -- so a listing that fails part-way is reported as a shorter
 * listing rather than as an error. Recorded in docs/KNOWN-ISSUES.md; the cure is
 * a change to lwext4, not to this file.
 */
static int ext_readdir_r(void *ctx, DIR *pdir, struct dirent *entry,
                         struct dirent **out)
{
    (void)ctx;

    ext_handle_t *h = handle_of_dir(pdir);
    if (h == NULL || entry == NULL || out == NULL) {
        errno = EBADF;
        return -1;
    }

    ext_lock();
    const ext4_direntry *de = ext4_dir_entry_next(&h->dir);
    if (de != NULL) {
        /* Copied out, not handed back: lwext4 filled the buffer inside the
         * descriptor, and the next call overwrites it. */
        ext_fill_dirent(entry, de);
        h->offset++;
    }
    ext_unlock();

    *out = (de != NULL) ? entry : NULL;
    return 0;
}

static struct dirent *ext_readdir(void *ctx, DIR *pdir)
{
    ext_handle_t *h = handle_of_dir(pdir);
    if (h == NULL) {
        errno = EBADF;
        return NULL;
    }

    struct dirent *out = NULL;
    if (ext_readdir_r(ctx, pdir, &h->cur, &out) != 0) {
        return NULL;
    }
    return out;
}

static DIR *ext_opendir(void *ctx, const char *name)
{
    (void)ctx;

    ext_route_t r;
    const int   err = ext_resolve(name, &r);
    if (err != 0) {
        errno = err;
        return NULL;
    }

    ext_handle_t *h = handle_alloc(true);
    if (h == NULL) {
        errno = EMFILE;
        return NULL;
    }

    ext_lock();
    const int r2 = ext4_dir_open(&h->dir, r.path);
    ext_unlock();

    if (r2 != EOK) {
        handle_free(h);
        errno = r2;
        return NULL;
    }

    /* DIR is opaque, and espix's VFS only compares the pointer and hands it
     * back, so the handle's own address is what a DIR* is here. */
    return (DIR *)h;
}

static int ext_closedir(void *ctx, DIR *pdir)
{
    (void)ctx;

    ext_handle_t *h = handle_of_dir(pdir);
    if (h == NULL) {
        errno = EBADF;
        return -1;
    }

    ext_lock();
    const int err = ext4_dir_close(&h->dir);
    ext_unlock();

    handle_free(h);

    if (err != EOK) {
        errno = err;
        return -1;
    }
    return 0;
}

static long ext_telldir(void *ctx, DIR *pdir)
{
    (void)ctx;

    ext_handle_t *h = handle_of_dir(pdir);
    if (h == NULL) {
        errno = EBADF;
        return -1;
    }
    return h->offset;
}

/*
 * lwext4 hands out no position to seek to, so what telldir() returns is a count
 * espix keeps, and seeking is a walk. Forward is a walk; backward is a rewind
 * and then a walk. The vendored port does the same, for the same reason.
 */
static void ext_seekdir(void *ctx, DIR *pdir, long offset)
{
    (void)ctx;

    ext_handle_t *h = handle_of_dir(pdir);
    if (h == NULL || offset < 0) {
        errno = EINVAL;
        return;
    }

    ext_lock();
    if (offset < h->offset) {
        ext4_dir_entry_rewind(&h->dir);
        h->offset = 0;
    }
    while (h->offset < offset) {
        if (ext4_dir_entry_next(&h->dir) == NULL) {
            break;
        }
        h->offset++;
    }
    ext_unlock();
}

/* The rest of the namespace. On a read-only mount each of these answers EROFS
 * and not ENOSYS: the operation is understood and the mount is the reason, which
 * is what a caller needs to tell apart and what leaving the op NULL would lose. */

static int ext_mkdir(void *ctx, const char *name, mode_t mode)
{
    (void)ctx;

    ext_route_t r;
    const int   err = ext_resolve(name, &r);
    if (err != 0) {
        errno = err;
        return -1;
    }
    if (r.m->read_only) {
        return refusero();
    }

    ext_lock();
    const int rc = ext4_dir_mk(r.path);

    /* lwext4's mkdir takes no mode, and an inode with no permission bits is a
     * directory nobody can enter -- the same reason ext_open() sets a mode after
     * creating a file. */
    const int mrc = (rc == EOK)
                        ? ext4_mode_set(r.path, (uint32_t)(mode & 0777 & ~022))
                        : rc;
    ext_unlock();

    if (mrc != EOK) {
        errno = mrc;
        return -1;
    }
    return 0;
}

static int ext_rmdir(void *ctx, const char *name)
{
    (void)ctx;

    ext_route_t r;
    const int   err = ext_resolve(name, &r);
    if (err != 0) {
        errno = err;
        return -1;
    }
    if (r.m->read_only) {
        return refusero();
    }

    ext_lock();
    const int rc = ext4_dir_rm(r.path);
    ext_unlock();

    if (rc != EOK) {
        errno = rc;
        return -1;
    }
    return 0;
}

static int ext_unlink(void *ctx, const char *path)
{
    (void)ctx;

    ext_route_t r;
    const int   err = ext_resolve(path, &r);
    if (err != 0) {
        errno = err;
        return -1;
    }
    if (r.m->read_only) {
        return refusero();
    }

    ext_lock();
    const int rc = ext4_fremove(r.path);
    ext_unlock();

    if (rc != EOK) {
        errno = rc;
        return -1;
    }
    return 0;
}

static int ext_rename(void *ctx, const char *src, const char *dst)
{
    (void)ctx;

    ext_route_t a;
    ext_route_t b;

    const int err = ext_resolve(src, &a);
    if (err != 0) {
        errno = err;
        return -1;
    }
    const int err2 = ext_resolve(dst, &b);
    if (err2 != 0) {
        errno = err2;
        return -1;
    }

    /* lwext4 has no rename across mounts, and a caller who asked for one wants
     * EXDEV: that is the answer POSIX names for it, and it is the one mv(1)
     * falls back from by copying. */
    if (a.m != b.m) {
        errno = EXDEV;
        return -1;
    }
    if (a.m->read_only) {
        return refusero();
    }

    ext_lock();
    const int rc = ext4_frename(a.path, b.path);
    ext_unlock();

    if (rc != EOK) {
        errno = rc;
        return -1;
    }
    return 0;
}

static int ext_truncate(void *ctx, const char *path, off_t length)
{
    (void)ctx;

    if (length < 0) {
        errno = EINVAL;
        return -1;
    }

    ext_route_t r;
    const int   err = ext_resolve(path, &r);
    if (err != 0) {
        errno = err;
        return -1;
    }
    if (r.m->read_only) {
        return refusero();
    }

    /*
     * Through a handle, because ext4_ftruncate() takes a file rather than a path
     * and lwext4 offers no path-shaped equivalent. Opened read-write on purpose:
     * a read-only handle is a request lwext4 refuses, and finding that out would
     * cost the same open anyway.
     */
    ext4_file f;
    int       rc;

    ext_lock();
    rc = ext4_fopen2(&f, r.path, O_RDWR);
    if (rc == EOK) {
        rc = ext4_ftruncate(&f, (uint64_t)length);

        const int crc = ext4_fclose(&f);
        if (rc == EOK) {
            rc = crc;
        }
    }
    ext_unlock();

    if (rc != EOK) {
        errno = rc;
        return -1;
    }
    return 0;
}

static int ext_ftruncate(void *ctx, int fd, off_t length)
{
    (void)ctx;

    ext_handle_t *h = handle_of(fd);
    if (h == NULL) {
        errno = EBADF;
        return -1;
    }
    if (h->is_dir) {
        errno = EISDIR;
        return -1;
    }
    if (length < 0) {
        errno = EINVAL;
        return -1;
    }
    if (h->m->read_only) {
        return refusero();
    }

    ext_lock();
    const int rc = ext4_ftruncate(&h->file, (uint64_t)length);

    /* The snapshot the handle keeps is now wrong; stat_locked() is the way back. */
    const int src = (rc == EOK) ? stat_locked(h->path, &h->st) : rc;
    ext_unlock();

    if (src != EOK) {
        errno = src;
        return -1;
    }
    return 0;
}

static int ext_utime(void *ctx, const char *path, const struct utimbuf *times)
{
    (void)ctx;

    if (times == NULL) {
        errno = EINVAL;
        return -1;
    }

    ext_route_t r;
    const int   err = ext_resolve(path, &r);
    if (err != 0) {
        errno = err;
        return -1;
    }
    if (r.m->read_only) {
        return refusero();
    }

    /*
     * lwext4 sets the three times through separate calls, each taking a path.
     * utime(2) names two of them, and the third -- the inode change time -- is not
     * the caller's to set, so it is left to the filesystem.
     */
    ext_lock();
    const int rc = ext4_atime_set(r.path, (uint32_t)times->actime);
    const int mrc = (rc == EOK)
                        ? ext4_mtime_set(r.path, (uint32_t)times->modtime)
                        : rc;
    ext_unlock();

    if (mrc != EOK) {
        errno = mrc;
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* The VFS ops                                                         */
/* ------------------------------------------------------------------ */

static const esp_vfs_dir_ops_t s_ext_dir_ops = {
    .stat_p      = ext_stat,
    /*
     * NULL, as in espix's own root ops: symlinks and the access check are
     * espix's, not a lower filesystem's. espix's VFS guards every op with
     * NO_LOWER, so these answer ENOSYS rather than being called.
     */
    .link_p      = NULL,
    .access_p    = NULL,
    .unlink_p    = ext_unlink,
    .rename_p    = ext_rename,
    .opendir_p   = ext_opendir,
    .readdir_p   = ext_readdir,
    .readdir_r_p = ext_readdir_r,
    .telldir_p   = ext_telldir,
    .seekdir_p   = ext_seekdir,
    .closedir_p  = ext_closedir,
    .mkdir_p     = ext_mkdir,
    .rmdir_p     = ext_rmdir,
    .truncate_p  = ext_truncate,
    .ftruncate_p = ext_ftruncate,
    .utime_p     = ext_utime,
};

/* fcntl and ioctl stay NULL: ENOSYS is the right answer for both on a plain
 * filesystem, and neither has a lower to forward to. */
static const esp_vfs_fs_ops_t s_ext_ops = {
    .open_p   = ext_open,
    .close_p  = ext_close,
    .read_p   = ext_read,
    .pread_p  = ext_pread,
    .write_p  = ext_write,
    .pwrite_p = ext_pwrite,
    .lseek_p  = ext_lseek,
    .fstat_p  = ext_fstat,
    .fsync_p  = ext_fsync,
    .fcntl_p  = NULL,
    .ioctl_p  = NULL,
#ifdef CONFIG_VFS_SUPPORT_DIR
    .dir      = &s_ext_dir_ops,
#endif
};

/* ------------------------------------------------------------------ */
/* Mounting                                                            */
/* ------------------------------------------------------------------ */

static uint16_t sb_u16(const uint8_t *sb, size_t off)
{
    return (uint16_t)(sb[off] | ((uint16_t)sb[off + 1] << 8));
}

static uint32_t sb_u32(const uint8_t *sb, size_t off)
{
    return (uint32_t)sb[off] | ((uint32_t)sb[off + 1] << 8) |
           ((uint32_t)sb[off + 2] << 16) | ((uint32_t)sb[off + 3] << 24);
}

/*
 * Why a mount failed, in the superblock's own terms.
 *
 * ext4_mount() answers ENOTSUP (134 under picolibc and newlib alike) for several
 * different things -- a superblock whose fields or checksum do not add up, an
 * incompatible feature this build does not implement -- and the code alone does
 * not say which. The superblock does, and this is the only place that can read
 * it: the block device handle is here, while /dev/sda3 cannot be opened from
 * userland at all because dev.c refuses a block node on purpose (see the note
 * there). That is why "which feature does it object to" was unanswerable until
 * this existed.
 *
 * Called only after a failure, so the working path pays nothing for it.
 *
 * offsetof() rather than numbers because the vendored struct is inside a
 * #pragma pack(push, 1) and mirrors the on-disk layout exactly -- and because
 * the checksum is computed over everything before its own field, which is what
 * the kernel's ext4_superblock_csum() does too. Reading through offsetof means a
 * field moving in a future vendored header moves here as well.
 */
static void log_why_not_mounted(esp_blockdev_handle_t dev, const char *path)
{
    uint8_t sb[EXT4_SUPERBLOCK_SIZE];

    if (dev->ops->read(dev, sb, sizeof(sb), EXT4_SUPERBLOCK_OFFSET,
                       sizeof(sb)) != ESP_OK) {
        espix_klog(ESPIX_KLOG_ERROR, TAG,
                   "%s: ext4_mount failed and the superblock cannot be read "
                   "either, so this is the device, not the filesystem", path);
        return;
    }

    const size_t csum_off = offsetof(struct ext4_sblock, checksum);

    const uint32_t compat   = sb_u32(sb, offsetof(struct ext4_sblock,
                                                 features_compatible));
    const uint32_t incompat = sb_u32(sb, offsetof(struct ext4_sblock,
                                                 features_incompatible));
    const uint32_t ro       = sb_u32(sb, offsetof(struct ext4_sblock,
                                                  features_read_only));
    /* What lwext4's own ext4_fs_check_features() masks off before deciding. */
    const uint32_t bad_in   = incompat & ~(uint32_t)CONFIG_SUPPORTED_FINCOM;
    const uint32_t bad_ro   = ro & ~(uint32_t)CONFIG_SUPPORTED_FRO_COM;

    const uint32_t stored   = sb_u32(sb, csum_off);
    const uint32_t computed = ext4_crc32c(EXT4_CRC32_INIT, sb,
                                         (uint32_t)csum_off);

    /*
     * One value per line, because a klog line is ESPIX_KLOG_LINE_MAX -- 120
     * characters including the tag -- and the first version of this silently
     * lost the end of every line, which is exactly the end that carries the
     * answer. Nine short lines beat three long ones that stop mid-number.
     */
    espix_klog(ESPIX_KLOG_ERROR, TAG,
               "%s: magic %04x (want %04x), block %u, inode %u, desc %u",
               path, (unsigned)sb_u16(sb, offsetof(struct ext4_sblock, magic)),
               (unsigned)EXT4_SUPERBLOCK_MAGIC,
               (unsigned)(1024u << sb_u32(sb, offsetof(struct ext4_sblock,
                                                       log_block_size))),
               (unsigned)sb_u16(sb, offsetof(struct ext4_sblock, inode_size)),
               (unsigned)sb_u16(sb, offsetof(struct ext4_sblock, desc_size)));

    espix_klog(ESPIX_KLOG_ERROR, TAG,
               "%s: inodes %lu, blocks %lu, %lu blocks/grp, %lu inodes/grp, "
               "first ino %lu",
               path,
               (unsigned long)sb_u32(sb, offsetof(struct ext4_sblock,
                                                  inodes_count)),
               (unsigned long)sb_u32(sb, offsetof(struct ext4_sblock,
                                                  blocks_count_lo)),
               (unsigned long)sb_u32(sb, offsetof(struct ext4_sblock,
                                                  blocks_per_group)),
               (unsigned long)sb_u32(sb, offsetof(struct ext4_sblock,
                                                  inodes_per_group)),
               (unsigned long)sb_u32(sb, offsetof(struct ext4_sblock,
                                                  first_inode)));

    espix_klog(ESPIX_KLOG_ERROR, TAG, "%s: compat   %08lx",
               path, (unsigned long)compat);
    espix_klog(ESPIX_KLOG_ERROR, TAG, "%s: incompat %08lx, unsupported %08lx",
               path, (unsigned long)incompat, (unsigned long)bad_in);
    espix_klog(ESPIX_KLOG_ERROR, TAG, "%s: ro_compat %08lx, unsupported %08lx",
               path, (unsigned long)ro, (unsigned long)bad_ro);

    if (ro & EXT4_FRO_COM_METADATA_CSUM) {
        /* Only meaningful when the filesystem carries checksums at all: without
         * that feature the stored value is zero by design. */
        espix_klog(ESPIX_KLOG_ERROR, TAG, "%s: csum stored %08lx computed %08lx",
                   path, (unsigned long)stored, (unsigned long)computed);
    }
}

#if CONFIG_ESPIX_FS_EXT4_FAIL_WRITE_AFTER > 0
/*
 * A block device that refuses the Nth write, for the one question nothing else
 * can answer: what lwext4 does when the medium says no.
 *
 * It lives here rather than in a test harness because espix owns this layer -- the
 * adapter is built around the esp_blockdev the caller passed in -- and because the
 * port's own caveats say that a passing e2fsck is not evidence that an injected
 * failure rolls back. The handle below is espix's own and its ctx is the device
 * underneath, so every operation forwards except write, which counts and then
 * refuses.
 *
 * Built only when CONFIG_ESPIX_FS_EXT4_FAIL_WRITE_AFTER is set; zero, the
 * default, compiles none of this.
 */
typedef struct {
    esp_blockdev_handle_t real;
    unsigned              writes;
} ext_fail_bd_t;

static ext_fail_bd_t          s_fail_ctx;
static struct esp_blockdev    s_fail_dev;

static esp_err_t fail_read(esp_blockdev_handle_t h, uint8_t *dst,
                           size_t dst_size, uint64_t src, size_t len)
{
    ext_fail_bd_t *f = (ext_fail_bd_t *)h->ctx;

    return f->real->ops->read(f->real, dst, dst_size, src, len);
}

static esp_err_t fail_write(esp_blockdev_handle_t h, const uint8_t *src,
                            uint64_t dst_addr, size_t len)
{
    ext_fail_bd_t *f = (ext_fail_bd_t *)h->ctx;

    f->writes++;

    if (f->writes >= (unsigned)CONFIG_ESPIX_FS_EXT4_FAIL_WRITE_AFTER) {
        espix_klog(ESPIX_KLOG_ERROR, TAG,
                   "fail-inject: refusing write %u (%u bytes at %llu)",
                   f->writes, (unsigned)len, (unsigned long long)dst_addr);
        return ESP_FAIL;
    }
    return f->real->ops->write(f->real, src, dst_addr, len);
}

static esp_err_t fail_erase(esp_blockdev_handle_t h, uint64_t start, size_t len)
{
    ext_fail_bd_t *f = (ext_fail_bd_t *)h->ctx;

    return f->real->ops->erase(f->real, start, len);
}

static esp_err_t fail_sync(esp_blockdev_handle_t h)
{
    ext_fail_bd_t *f = (ext_fail_bd_t *)h->ctx;

    return f->real->ops->sync(f->real);
}

static esp_err_t fail_ioctl(esp_blockdev_handle_t h, const uint8_t cmd, void *args)
{
    ext_fail_bd_t *f = (ext_fail_bd_t *)h->ctx;

    return f->real->ops->ioctl(f->real, cmd, args);
}

static esp_err_t fail_release(esp_blockdev_handle_t h)
{
    ext_fail_bd_t *f = (ext_fail_bd_t *)h->ctx;

    /* The real handle is the caller's to release, not this shim's -- espix owns
     * it either way, and releasing it twice is worse than not releasing it. */
    (void)f;
    return ESP_OK;
}

static const esp_blockdev_ops_t s_fail_ops = {
    .read    = fail_read,
    .write   = fail_write,
    .erase   = fail_erase,
    .sync    = fail_sync,
    .ioctl   = fail_ioctl,
    .release = fail_release,
};

/* The device the adapter is given: the real one, or the shim in front of it. */
static esp_blockdev_handle_t fail_bd(esp_blockdev_handle_t dev)
{
    s_fail_ctx.real   = dev;
    s_fail_ctx.writes = 0;

    s_fail_dev.ctx          = &s_fail_ctx;
    s_fail_dev.device_flags = dev->device_flags;
    s_fail_dev.geometry     = dev->geometry;
    s_fail_dev.ops          = &s_fail_ops;

    espix_klog(ESPIX_KLOG_WARN, TAG,
               "fail-inject: write %d will be refused (CONFIG_ESPIX_FS_EXT4_FAIL_WRITE_AFTER)",
               CONFIG_ESPIX_FS_EXT4_FAIL_WRITE_AFTER);

    return &s_fail_dev;
}
#endif /* CONFIG_ESPIX_FS_EXT4_FAIL_WRITE_AFTER > 0 */

/*
 * The mount itself, done here rather than by the vendored component: its
 * adapter registers a mount with IDF's VFS, which is what this file exists to
 * avoid, so the two calls it leaves open -- ext4_device_register() and
 * ext4_mount() -- are made below.
 *
 * Same ordering rule as fat.c: nothing is published to espix's namespace until
 * everything else has succeeded, so no path can reach a mount that is not ready.
 */
esp_err_t espix_fs_mount_ext(const char *path, esp_blockdev_handle_t dev,
                             bool read_only,
                             uint16_t owner_uid, uint16_t owner_gid)
{
    if (path == NULL || dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Declared before the first goto rather than beside its first use: the
     * failure paths return it, and one of them is reachable before the adapter
     * exists. */
    esp_err_t err = ESP_OK;

    /* Ahead of the driver: a device that reads as empty is a different complaint
     * from a volume that is not ext, and worth saying so before mounting. */
    if (dev->geometry.disk_size == 0) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "%s: the device reports no size", path);
        return ESP_ERR_INVALID_ARG;
    }

    ext_mount_t *m = mount_reserve(path);
    if (m == NULL) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "%s: no free mount slot", path);
        return ESP_ERR_NO_MEM;
    }

    /* Created before it is needed, so lwext4's hooks are usable the moment they
     * are installed. One for every mount; see the note at the top of the file. */
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateRecursiveMutex();
        if (s_lock == NULL) {
            mount_release(m);
            return ESP_ERR_NO_MEM;
        }
    }

    /* Named from the slot index, which is unique while the mount lasts, and well
     * inside lwext4's CONFIG_EXT4_MAX_BLOCKDEV_NAME and _MAX_MP_NAME (both 32).
     * The trailing slash is required, not cosmetic: ext4_mount() returns ENOTSUP
     * for a mount point without one (ext4.c:380). */
    const int idx = (int)(m - s_mounts);
    snprintf(m->dev, sizeof(m->dev), "ext%d", idx);
    snprintf(m->mp, sizeof(m->mp), "/ext%d/", idx);

    /*
     * lwext4 is a 512-byte-sector filesystem, so the adapter's physical block
     * size is the lower device's read size, at least 512 and rounded up to a
     * power of two. It also has to divide the device exactly, which any
     * partition table's view does and which is checked rather than assumed.
     */
    uint32_t bs = dev->geometry.read_size;
    if (bs < 512) {
        bs = 512;
    }
    for (uint32_t p = 512; p < bs; p <<= 1) {
        bs = p << 1;
    }

    if (bs == 0 || dev->geometry.disk_size % bs != 0) {
        espix_klog(ESPIX_KLOG_ERROR, TAG,
                   "%s: %llu bytes is not a whole number of %u-byte blocks",
                   path, (unsigned long long)dev->geometry.disk_size,
                   (unsigned)bs);
        goto fail_slot;
    }

    const lwext4_port_bdl_config_t cfg = {
        .physical_block_size = bs,
        /* DMA-capable internal memory: the lower device is USB MSC or SDMMC, both
         * of which DMA this buffer, and neither of which can DMA from PSRAM. */
        .buffer_caps      = MALLOC_CAP_DMA | MALLOC_CAP_8BIT,
        .buffer_alignment = bs,
        /* One block. Wider transfers are a throughput question and this is a
         * correctness-first first version; the adapter's own default is the
         * same, spelled out because zero and one are not obviously the same. */
        .transfer_buffer_blocks = 1,
        .read_only        = read_only,
        /* Conservative, and it costs: every write is handed to the device rather
         * than left in the adapter. The port says to disable it only when the lower
         * device already provides the ordering lwext4's journal needs, which USB
         * MSC does not. */
        .sync_after_write = !read_only,
        /* An attestation about the device, not about intent: everything espix
         * mounts is a real filesystem on real media rather than raw
         * erase-before-write flash. Not asked of a read-only adapter, but true,
         * so that enabling writes later is not also a change of claim. */
        .lower_device_supports_rewrite = true,
    };

#if CONFIG_ESPIX_FS_EXT4_FAIL_WRITE_AFTER > 0
    const esp_blockdev_handle_t lower = fail_bd(dev);
#else
    const esp_blockdev_handle_t lower = dev;
#endif

    err = lwext4_port_bdl_create(lower, &cfg, &m->adapter);
    if (err != ESP_OK) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "%s: cannot adapt the device: %s",
                   path, esp_err_to_name(err));
        goto fail_slot;
    }

    int rc = ext4_device_register(lwext4_port_bdl_get(m->adapter), m->dev);
    if (rc != EOK) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "%s: cannot register %s: %d",
                   path, m->dev, rc);
        err = ESP_FAIL;
        goto fail_adapter;
    }

    /* read_only as the caller asked for it, and the same flag the ops consult
     * before touching anything -- so the mount and the refusals agree by
     * construction rather than by discipline. A volume that is not ext answers
     * EINVAL or ENOENT here, and that is reported as "not found" rather than as a
     * failure, because "this is not an ext filesystem" is the useful thing to
     * say. */
    rc = ext4_mount(m->dev, m->mp, read_only);
    m->read_only = read_only;
    if (rc != EOK) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "%s: cannot mount %s: %d",
                   path, m->mp, rc);
        /* ENOTSUP is the one that does not explain itself; ask the superblock. */
        if (rc == ENOTSUP) {
            log_why_not_mounted(dev, path);
        }
        err = (rc == EINVAL || rc == ENOENT) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
        goto fail_dev;
    }

    if (ext4_mount_setup_locks(m->mp, &s_ext_locks) != EOK) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "%s: cannot install the locks", path);
        err = ESP_FAIL;
        goto fail_mount;
    }

    /*
     * A writable mount needs the journal running, and starting it is espix's job:
     * lwext4's ext4_mount() does not, and every mutation is answered ENOTSUP until
     * something does -- fail-closed, which is the right direction to find this out
     * in. It is also where journal recovery happens, so a volume pulled mid-write
     * is put right here rather than at the first write after it.
     *
     * A volume with no journal cannot be written through this port at all. That is
     * reported as "cannot write" rather than mounted read-only behind the caller's
     * back: they asked for writes, and the honest answer is that this volume cannot
     * take them.
     */
    if (!read_only) {
        const int jrc = ext4_journal_start(m->mp);

        if (jrc != EOK) {
            espix_klog(ESPIX_KLOG_ERROR, TAG,
                       "%s: no journal (%d), so it cannot be written -- mount it "
                       "read-only, or give the volume one",
                       path, jrc);
            err = ESP_ERR_NOT_SUPPORTED;
            goto fail_mount;
        }

        /*
         * Said out loud, once, because this is the one place espix's own code ends
         * up in the path of an ordinary write: an ext4 volume's extent tree is
         * handled by the port's experimental implementation, and its caveats are
         * real (components/esp_lwext4/doc/CAVEATS.md). Refusing to write at all is
         * the other half of that choice, and a mount that never asks for rw never
         * reaches any of it.
         */
        struct ext4_sblock *sb = NULL;

        if (ext4_get_sblock(m->mp, &sb) == EOK && sb != NULL &&
            (sb->features_incompatible & EXT4_FINCOM_EXTENTS) != 0) {
            espix_klog(ESPIX_KLOG_INFO, TAG,
                       "%s: writable, and its extent tree is handled by the "
                       "experimental port -- see esp_lwext4/doc/CAVEATS.md",
                       path);
        }
    }

    /*
     * ESPIX_FS_META_LOWER: an ext inode is the source of truth for a mode and an
     * owner, so espix adds nothing to them and the permission check reads what the
     * file says. That is what Unix does, and it is what makes `ls -l` and the
     * check agree by construction rather than by coincidence.
     *
     * chmod and chown still refuse, and for a different reason than FAT's: ext has
     * somewhere to put them, but writing an inode needs a writable mount. See
     * docs/ROADMAP.md for what that milestone takes.
     */
    err = espix_vfs_add_mount(path, &s_ext_ops, m, ESPIX_FS_META_LOWER,
                              owner_uid, owner_gid);
    if (err != ESP_OK) {
        if (!read_only) {
            (void)ext4_journal_stop(m->mp);
        }
        ext4_umount(m->mp);
        goto fail_dev;
    }

    espix_klog(ESPIX_KLOG_INFO, TAG, "mounted %s at %s (%u-byte blocks)",
               m->dev, path, (unsigned)bs);
    return ESP_OK;

fail_mount:
    ext4_umount(m->mp);
fail_dev:
    ext4_device_unregister(m->dev);
fail_adapter:
    lwext4_port_bdl_destroy(m->adapter);
    m->adapter = NULL;
fail_slot:
    mount_release(m);
    return err;
}

esp_err_t espix_fs_unmount_ext(const char *path)
{
    if (path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ext_mount_t *m = mount_find(path);
    if (m == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    /* Out of the namespace first, and refused while a file or directory is still
     * open on it -- the volume does not come out from under a reader. See
     * espix_vfs_del_mount(). */
    const esp_err_t err = espix_vfs_del_mount(path);
    if (err != ESP_OK) {
        return err;
    }

    if (espix_vfs_mount_dead(path)) {
        /*
         * Nothing below this point is reached, because everything below it
         * touches the lower block device: ext4_umount() writes the superblock
         * back and flushes lwext4's block cache, and the adapter's destroy syncs
         * the device. Its device was pulled out from under it, so either would be
         * the use-after-free this branch exists to avoid.
         *
         * So lwext4's mount point and the adapter are left behind, and only
         * espix's side is given back. That is the same shape as fat.c's
         * dead-mount branch, which leaves FatFs's volume slot standing; what it
         * costs here is one of lwext4's two mount points and a few kilobytes,
         * until the board next boots. See docs/KNOWN-ISSUES.md.
         */
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "%s: device gone -- leaving %s behind in lwext4", path, m->mp);
        m->adapter = NULL;
        mount_release(m);

        espix_klog(ESPIX_KLOG_INFO, TAG, "unmounted %s", path);
        return ESP_OK;
    }

    const int rc = ext4_umount(m->mp);
    if (rc != EOK) {
        /* Reported, not retried: the mount is gone from espix's table already,
         * and putting it back would leave a volume nothing routes to. */
        espix_klog(ESPIX_KLOG_WARN, TAG, "%s: unmounting %s: %d", path, m->mp, rc);
    }

    ext4_device_unregister(m->dev);

    const esp_err_t derr = lwext4_port_bdl_destroy(m->adapter);
    if (derr != ESP_OK) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "%s: releasing the adapter: %s",
                   path, esp_err_to_name(derr));
    }
    m->adapter = NULL;
    mount_release(m);

    espix_klog(ESPIX_KLOG_INFO, TAG, "unmounted %s", path);
    return ESP_OK;
}

/*
 * How much of a mounted ext volume is left, in bytes.
 *
 * lwext4 answers this itself -- ext4_mount_point_stats() -- so this is plumbing
 * rather than arithmetic, unlike the FAT side, which has to do esp_vfs_fat_info's
 * job by hand because that function looks its mount up by base path.
 *
 * A volume whose device has been pulled answers with an error rather than
 * numbers: its mount is marked dead (see vfs.c), and asking lwext4 to count free
 * blocks would read through a block device espix_usb has released.
 */
esp_err_t espix_fs_stat_ext(const char *path, uint64_t *total,
                            uint64_t *free_bytes)
{
    if (path == NULL || total == NULL || free_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ext_mount_t *m = mount_find(path);
    if (m == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (espix_vfs_mount_dead(path)) {
        return ESP_ERR_INVALID_STATE;
    }

    struct ext4_mount_stats st;

    ext_lock();
    const int rc = ext4_mount_point_stats(m->mp, &st);
    ext_unlock();

    if (rc != EOK) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "%s: free space: %d", path, rc);
        return ESP_FAIL;
    }

    /* Blocks, not bytes, on both sides, so the multiplication is done once and
     * in 64 bits: a 3 TB volume overflows a 32-bit product several times over. */
    const uint64_t bs = st.block_size;
    *total      = st.blocks_count * bs;
    *free_bytes = st.free_blocks_count * bs;

    return ESP_OK;
}
