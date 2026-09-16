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
 * you step around by running a program. This is exactly why NuttX enforces file
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
 * mount littlefs at "/.lfs", and rewrite "/etc/hostname" to
 * "/.lfs/etc/hostname". That works, and it publishes a second name for the
 * root: anything spelling "/.lfs/..." addresses the filesystem with these
 * checks skipped.
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
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"

#include "esp_littlefs.h"
#include "esp_vfs.h"
#include "esp_vfs_ops.h"

#include "espix_fs.h"
#include "espix_kernel.h"
#include "espix_proc.h"

#include "espix_fs_priv.h"

#define TAG "fs"

/*
 * The layers below, one entry per mount.
 *
 * A struct passed as the VFS context rather than file-scope statics, because
 * `mount` was on the roadmap and a second mount would otherwise mean unpicking
 * this. That second mount has arrived, so the struct grew a prefix: entry 0 is
 * the root (prefix ""), and a path goes to the longest prefix that claims it.
 *
 * The context handed to esp_vfs_register_fs() is entry 0 and says nothing about
 * which mount a call belongs to, so each op picks its mount by path, by fd or by
 * directory handle -- whichever the call carries.
 */
typedef struct {
    const esp_vfs_fs_ops_t  *ops;
    const esp_vfs_dir_ops_t *dir;
    void                    *ctx;
    char                     prefix[ESPIX_FS_PREFIX_MAX];
    size_t                   len;
    /*
     * False when the filesystem keeps no ownership or mode of its own (FAT).
     * espix's rule still answers for such a path -- there is nothing stored to
     * read -- but a *write* has nowhere to go, so chmod and chown refuse rather
     * than stamping an attribute into whichever filesystem can hold one.
     */
    bool                     stored_metadata;
    bool                     used;
    /*
     * Who owns what a metadata-less mount holds -- the uid and gid of whoever
     * mounted it, the shape Linux gives a removable volume with uid= and gid=.
     * ESPIX_FS_OWNER_RULE for the root and for anything that carries ownership of
     * its own: those keep answering from the rule, as they always have.
     */
    uint16_t                 owner_uid;
    uint16_t                 owner_gid;
} lower_t;

static lower_t s_mounts[ESPIX_FS_MAX_MOUNTS];

/*
 * Mounts are added and removed while other sessions are opening files, so the
 * table is read and written under a spinlock. Every critical section below is a
 * handful of instructions and none of them blocks.
 */
static portMUX_TYPE s_mount_lock = portMUX_INITIALIZER_UNLOCKED;

/*
 * The fds this VFS has handed out, and what each one belongs to.
 *
 * An fd carries no path, so which mount it belongs to has to be remembered --
 * but the *number* cannot be the lower filesystem's own. FatFs and the littlefs
 * port both count from zero, espix calls their ops directly rather than through
 * esp_vfs, and IDF's own table, the console and the sockets are using those
 * numbers too. Two files on two filesystems then share one entry: a rootfs close
 * clears it, and the next read on the stick is answered EBADF. Measured, and
 * written up in docs/KNOWN-ISSUES.md -- it cost the first two copies after every
 * boot.
 *
 * So the number is IDF's: esp_vfs_register_fd_with_local_fd() picks an unused
 * one, which is what keeps this table honest, keeps fds out of the range the
 * sockets use, and keeps them inside `fd_set` for select(). The layer below is
 * called with its own number, which is what the table is for. IDF frees the slot
 * itself when the fd is closed, so there is nothing to unregister here.
 *
 * Indexed by fd, sized to the range below the device fds, which are handed out
 * directly and never appear here.
 */
typedef struct {
    int     lower_fd;   /* the number the layer below knows it by, -1 = free */
    uint8_t mount;      /* index into s_mounts, +1 */
} fd_slot_t;

static fd_slot_t s_fds[ESPIX_DEV_FD_BASE];

/*
 * Every slot starts free, and the sentinel is -1 rather than 0: a zeroed table
 * would otherwise say every fd is live with mount index 0, which is one before
 * the root. Done here rather than with an initialiser because C has no way to
 * write a designated value for a whole array.
 */
static void fd_table_init(void)
{
    for (int i = 0; i < ESPIX_DEV_FD_BASE; i++) {
        s_fds[i].lower_fd = -1;
    }
}

/* The VFS id esp_vfs_register_fd() needs, from the registration at the bottom
 * of this file. */
static esp_vfs_id_t s_vfs_id = -1;

static bool fd_slot_get(int fd, fd_slot_t *out)
{
    if (fd < 0 || fd >= ESPIX_DEV_FD_BASE) {
        return false;
    }

    bool ours = false;
    portENTER_CRITICAL(&s_mount_lock);
    if (s_fds[fd].lower_fd >= 0) {
        *out = s_fds[fd];
        ours = true;
    }
    portEXIT_CRITICAL(&s_mount_lock);
    return ours;
}
#define ESPIX_FS_DIRS  8
static struct {
    DIR    *dirp;
    uint8_t mount;                 /* index + 1 */
} s_dir_map[ESPIX_FS_DIRS];

/*
 * Longest prefix wins, and a prefix only claims a path it matches whole -- so
 * "/mnt" does not take "/mntx". The root is the fallback rather than a candidate:
 * it is what every path reaches when nothing longer matches.
 */
static const lower_t *mount_by_path(const char *abs_path)
{
    const lower_t *best = &s_mounts[0];
    size_t         best_len = 0;
    const size_t   path_len = strlen(abs_path);

    portENTER_CRITICAL(&s_mount_lock);
    for (size_t i = 1; i < ESPIX_FS_MAX_MOUNTS; i++) {
        const lower_t *m = &s_mounts[i];
        if (!m->used || m->len <= best_len || m->len > path_len) {
            continue;
        }
        if (strncmp(abs_path, m->prefix, m->len) == 0 &&
            (abs_path[m->len] == '\0' || abs_path[m->len] == '/')) {
            best = m;
            best_len = m->len;
        }
    }
    portEXIT_CRITICAL(&s_mount_lock);
    return best;
}

/* The layer below a fd espix handed out, or NULL when it is not one of ours --
 * a device fd, a socket, or something already closed. */
static const lower_t *mount_of_slot(const fd_slot_t *s)
{
    return &s_mounts[s->mount - 1];
}

/*
 * IDF's fd for a file just opened below, or -1.
 *
 * The number comes from esp_vfs rather than from the filesystem, which is the
 * whole point: it is unique against the console, the sockets and every other
 * mount, so the entry it gets here cannot be someone else's. `local_fd` is left
 * as IDF set it -- equal to the fd -- so this VFS's ops are called with the
 * number it handed out, and the lower filesystem's own number travels in the
 * table instead.
 */
static int fd_slot_alloc(int lower_fd, const lower_t *mount)
{
    int fd = -1;

    if (esp_vfs_register_fd_with_local_fd(s_vfs_id, -1, false, &fd) != ESP_OK) {
        return -1;
    }

    /* Above the device fds there is no entry to write, and esp_vfs would have to
     * be handing out a number in a range the devices reserved. Refuse rather than
     * index past the table; sockets and the console cannot reach this far in
     * practice, and "cannot in practice" is not a reason to write out of bounds. */
    if (fd < 0 || fd >= ESPIX_DEV_FD_BASE) {
        if (fd >= 0) {
            (void)esp_vfs_unregister_fd(s_vfs_id, fd);
        }
        return -1;
    }

    portENTER_CRITICAL(&s_mount_lock);
    s_fds[fd].lower_fd = lower_fd;
    s_fds[fd].mount    = (uint8_t)((mount - s_mounts) + 1);
    portEXIT_CRITICAL(&s_mount_lock);

    return fd;
}

/* IDF releases its own entry when the fd is closed, so this only forgets which
 * layer the number belonged to. */
static void fd_slot_free(int fd)
{
    if (fd < 0 || fd >= ESPIX_DEV_FD_BASE) {
        return;
    }

    portENTER_CRITICAL(&s_mount_lock);
    s_fds[fd].lower_fd = -1;
    s_fds[fd].mount    = 0;
    portEXIT_CRITICAL(&s_mount_lock);
}

static const lower_t *mount_by_dir(DIR *pdir)
{
    const lower_t *l = NULL;

    portENTER_CRITICAL(&s_mount_lock);
    for (size_t i = 0; i < ESPIX_FS_DIRS; i++) {
        if (s_dir_map[i].dirp == pdir && s_dir_map[i].mount != 0) {
            l = &s_mounts[s_dir_map[i].mount - 1];
            break;
        }
    }
    portEXIT_CRITICAL(&s_mount_lock);
    return l;
}

static void dir_map_set(DIR *pdir, const lower_t *mount)
{
    portENTER_CRITICAL(&s_mount_lock);
    for (size_t i = 0; i < ESPIX_FS_DIRS; i++) {
        if (s_dir_map[i].dirp == pdir || s_dir_map[i].mount == 0) {
            s_dir_map[i].dirp  = pdir;
            s_dir_map[i].mount = (uint8_t)((mount - s_mounts) + 1);
            break;
        }
    }
    portEXIT_CRITICAL(&s_mount_lock);
}

static void dir_map_clear(DIR *pdir)
{
    portENTER_CRITICAL(&s_mount_lock);
    for (size_t i = 0; i < ESPIX_FS_DIRS; i++) {
        if (s_dir_map[i].dirp == pdir) {
            s_dir_map[i].dirp  = NULL;
            s_dir_map[i].mount = 0;
            break;
        }
    }
    portEXIT_CRITICAL(&s_mount_lock);
}

/* Refuse rather than crash when the layer below does not implement something. */
#define NO_LOWER(expr) ((expr) == NULL)

static int enosys(void)
{
    errno = ENOSYS;
    return -1;
}

/* An fd this VFS never handed out: not a device, and not on any mount. */
static int ebadf(void)
{
    errno = EBADF;
    return -1;
}

/*
 * Resolve a path the caller gave us against the caller's working directory,
 * and refuse it if the caller's process may not name it.
 *
 * This is what gives a loaded app a working directory, and it only works
 * because espix owns this VFS. ESP-IDF has none to offer -- its chdir() is a
 * hardcoded ENOSYS stub and its getcwd() always answers "/" -- but a VFS
 * registered at "" matches *any* path in get_vfs_for_path(), relative ones
 * included, and translate_path() hands it over verbatim. So "data.txt" arrives
 * here and espix resolves it itself; IDF's stubs never come into it.
 *
 * espix_fs_resolve() is the same normalisation every shell command goes
 * through, so `cd ..` from the shell and chdir("..") from an app agree about
 * what they mean.
 *
 * An absolute path still needs normalising, not just passing along: an app is
 * free to open "/etc/../etc/hostname", and the mode attribute is keyed by
 * path, so two spellings of one file must not become two entries.
 *
 * Which is also what makes the root test below sound. `..` is resolved by
 * popping segments *before* anything compares prefixes, so "/srv/www/../../etc"
 * is already "/etc" when it is asked about -- a confined process cannot climb
 * out by spelling its way there, and there is no second syntax to get wrong.
 *
 * Sets errno itself, because it now has two reasons to fail.
 */
static const char *resolve(const char *path, char *buf, size_t len)
{
    if (path == NULL) {
        errno = ENAMETOOLONG;
        return NULL;
    }

    /*
     * One lookup for both, because this runs on every path operation in the
     * system and espix_proc_cwd() and espix_proc_root() would each walk the
     * process table to answer half of it. For the commonest callers -- an SSH
     * session task, SFTP, the console, anything of espix's own -- that walk
     * finds nothing and returns the default, so paying for it twice is pure
     * waste on the hottest path there is.
     */
    const char *cwd  = "/";
    const char *root = "";
    espix_proc_paths(&cwd, &root);

    if (espix_fs_resolve(cwd, path, buf, len) != ESP_OK) {
        errno = ENAMETOOLONG;
        return NULL;
    }

    /*
     * ENOENT rather than EACCES on purpose. "Permission denied" confirms the
     * path exists, and a confined process probing one path at a time would map
     * the filesystem it is supposed to be unable to see.
     *
     * espix_fs_root_permits() is asked rather than `root` compared here, so
     * that this and espix_fs_admin_check() cannot drift apart about what a root
     * means -- including the exemption for a raised task.
     */
    if (root[0] != '\0' && !espix_fs_root_permits(buf)) {
        errno = ENOENT;
        return NULL;
    }
    return buf;
}

/* The path ops all start the same way; this keeps that from being eleven
 * copies of five lines. resolve() has set errno. */
#define RESOLVE_OR_FAIL(path, fail)                                           \
    char        resolved__[ESPIX_PATH_MAX];                                   \
    const char *p = resolve((path), resolved__, sizeof(resolved__));          \
    if (p == NULL) {                                                          \
        return (fail);                                                        \
    }

/* ------------------------------------------------------------------ */
/* File operations                                                     */
/* ------------------------------------------------------------------ */

static int vfs_open(void *ctx, const char *path, int flags, int mode)
{
    RESOLVE_OR_FAIL(path, -1);

    const int err = espix_fs_access_check(p, ESPIX_FS_ACCESS_OPEN, flags);
    if (err != 0) {
        errno = err;
        return -1;
    }
    /*
     * Devices are answered here -- after the access check above, never before.
     * That ordering is the reason the table lives inside this VFS at all: a
     * filesystem registered at "/dev" would be reached without the check, which
     * is precisely what fs.c warns about.
     */
    const void *dev = espix_dev_lookup(p);
    if (dev != NULL) {
        return espix_dev_open(dev, flags);
    }
    if (espix_dev_isdir(p)) {
        errno = EISDIR;
        return -1;
    }
    if (espix_dev_underdev(p)) {
        /*
         * The subtree is espix's and answers to the table alone, so a name
         * that is not a node does not exist -- and no create can land on a
         * synthetic directory. Both branches keep /dev out of littlefs
         * entirely, which is what hides whatever an older image left there.
         */
        errno = (flags & O_CREAT) ? EROFS : ENOENT;
        return -1;
    }

    /*
     * Chosen after the two /dev branches above rather than before: /dev is
     * answered without reaching a filesystem at all, so no mount is needed
     * until this point.
     */
    const lower_t *l = mount_by_path(p);

    if (NO_LOWER(l->ops->open_p)) {
        return enosys();
    }

    /*
     * Whether this call is the one that brings the file into existence has to
     * be decided before the open, not after: O_CREAT on a file that is already
     * there must not re-stamp an owner.
     */
    struct stat before;
    const bool  creating = (flags & O_CREAT) && stat(p, &before) != 0;

    const int fd = l->ops->open_p(l->ctx, p, flags, mode);

    /*
     * The device fd reservation, enforced rather than assumed. esp_vfs stores a
     * local fd in a uint8_t, so devices take the top of that range; if the
     * filesystem ever hands out an fd that far up, the two would alias and a
     * read of somebody's file would come back as firmware. Refuse instead.
     * littlefs indexes a cache that starts at a few entries and FD_SETSIZE here
     * is MEMP_NUM_NETCONN, so this is unreachable in practice -- which is why
     * it is worth a branch rather than a comment promising it cannot happen.
     */
    if (fd >= ESPIX_DEV_FD_BASE) {
        if (!NO_LOWER(l->ops->close_p)) {
            l->ops->close_p(l->ctx, fd);
        }
        errno = EMFILE;
        return -1;
    }

    if (fd < 0) {
        return fd;                      /* the layer below set errno */
    }

    /*
     * And the caller never sees that number: it gets a fd from esp_vfs, which is
     * unique against the console, the sockets and every other mount, and the
     * filesystem's own number is remembered for the calls that follow. See the
     * comment on s_fds for what happens without this.
     */
    const int packed = fd_slot_alloc(fd, l);
    if (packed < 0) {
        /* Out of fds rather than out of anything the caller can free: hand the
         * lower one back instead of leaking it. */
        if (!NO_LOWER(l->ops->close_p)) {
            l->ops->close_p(l->ctx, fd);
        }
        errno = EMFILE;
        return -1;
    }

    if (creating) {
        espix_fs_claim(p);
    }
    return packed;
}

static int vfs_close(void *ctx, int fd)
{
    if (espix_dev_fd(fd)) {
        return espix_dev_close(fd);
    }
    fd_slot_t slot;
    if (!fd_slot_get(fd, &slot)) {
        return ebadf();
    }
    const lower_t *l = mount_of_slot(&slot);

    const int rc = NO_LOWER(l->ops->close_p)
                       ? enosys() : l->ops->close_p(l->ctx, slot.lower_fd);
    /* Forgotten either way: a failed close still leaves the fd not ours. esp_vfs
     * releases its own entry around this call, so nothing is unregistered. */
    fd_slot_free(fd);
    return rc;
}

static ssize_t vfs_read(void *ctx, int fd, void *dst, size_t size)
{
    if (espix_dev_fd(fd)) {
        return espix_dev_read(fd, dst, size);
    }
    fd_slot_t slot;
    if (!fd_slot_get(fd, &slot)) {
        return ebadf();
    }
    const lower_t *l = mount_of_slot(&slot);
    return NO_LOWER(l->ops->read_p)
               ? enosys() : l->ops->read_p(l->ctx, slot.lower_fd, dst, size);
}

static ssize_t vfs_write(void *ctx, int fd, const void *data, size_t size)
{
    if (espix_dev_fd(fd)) {
        return espix_dev_write(fd, data, size);
    }
    fd_slot_t slot;
    if (!fd_slot_get(fd, &slot)) {
        return ebadf();
    }
    const lower_t *l = mount_of_slot(&slot);
    return NO_LOWER(l->ops->write_p)
               ? enosys() : l->ops->write_p(l->ctx, slot.lower_fd, data, size);
}

static ssize_t vfs_pread(void *ctx, int fd, void *dst, size_t size, off_t off)
{
    if (espix_dev_fd(fd)) {
        return espix_dev_pread(fd, dst, size, off);
    }
    fd_slot_t slot;
    if (!fd_slot_get(fd, &slot)) {
        return ebadf();
    }
    const lower_t *l = mount_of_slot(&slot);
    return NO_LOWER(l->ops->pread_p)
               ? enosys() : l->ops->pread_p(l->ctx, slot.lower_fd, dst, size, off);
}

static ssize_t vfs_pwrite(void *ctx, int fd, const void *src, size_t size,
                          off_t off)
{
    if (espix_dev_fd(fd)) {
        /* Only /dev/null accepts writes, and it discards them, so the offset
         * changes nothing. */
        (void)off;
        return espix_dev_write(fd, src, size);
    }
    fd_slot_t slot;
    if (!fd_slot_get(fd, &slot)) {
        return ebadf();
    }
    const lower_t *l = mount_of_slot(&slot);
    return NO_LOWER(l->ops->pwrite_p)
               ? enosys()
               : l->ops->pwrite_p(l->ctx, slot.lower_fd, src, size, off);
}

static off_t vfs_lseek(void *ctx, int fd, off_t size, int mode)
{
    if (espix_dev_fd(fd)) {
        return espix_dev_lseek(fd, size, mode);
    }
    fd_slot_t slot;
    if (!fd_slot_get(fd, &slot)) {
        return ebadf();
    }
    const lower_t *l = mount_of_slot(&slot);
    return NO_LOWER(l->ops->lseek_p)
               ? enosys() : l->ops->lseek_p(l->ctx, slot.lower_fd, size, mode);
}

static int vfs_fstat(void *ctx, int fd, struct stat *st)
{
    if (espix_dev_fd(fd)) {
        return espix_dev_fstat(fd, st);
    }
    fd_slot_t slot;
    if (!fd_slot_get(fd, &slot)) {
        return ebadf();
    }
    const lower_t *l = mount_of_slot(&slot);
    return NO_LOWER(l->ops->fstat_p)
               ? enosys() : l->ops->fstat_p(l->ctx, slot.lower_fd, st);
}

static int vfs_fsync(void *ctx, int fd)
{
    if (espix_dev_fd(fd)) {
        return espix_dev_fsync(fd);
    }
    fd_slot_t slot;
    if (!fd_slot_get(fd, &slot)) {
        return ebadf();
    }
    const lower_t *l = mount_of_slot(&slot);
    return NO_LOWER(l->ops->fsync_p)
               ? enosys() : l->ops->fsync_p(l->ctx, slot.lower_fd);
}

static int vfs_fcntl(void *ctx, int fd, int cmd, int arg)
{
    if (espix_dev_fd(fd)) {
        /* Nothing here has flags worth reporting, and F_GETFL returning 0 is
         * more useful to a caller than ENOSYS. */
        (void)cmd; (void)arg;
        return 0;
    }
    fd_slot_t slot;
    if (!fd_slot_get(fd, &slot)) {
        return ebadf();
    }
    const lower_t *l = mount_of_slot(&slot);
    return NO_LOWER(l->ops->fcntl_p)
               ? enosys() : l->ops->fcntl_p(l->ctx, slot.lower_fd, cmd, arg);
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
/*
 * ...and this is where stat() starts telling the truth about a mode.
 *
 * LittleFS stores no permission bits, so the port fills in S_IFREG or S_IFDIR
 * and stops. espix knows better -- espix_fs_mode() answers from the file's
 * attribute or from the rule -- and this is the one place every caller passes
 * through, so it belongs here rather than in each of them.
 *
 * It replaces three separate lookups. `ls -l`, the SFTP server and the exec
 * path each
 * called espix_fs_mode() alongside stat() because stat() could not be trusted;
 * now they read st_mode like any other program would, and an app that calls
 * stat() sees exactly what `ls -l` shows without espix wrapping anything.
 *
 * espix_fs_mode() may open the file to sniff the ELF magic, which re-enters
 * this VFS through open(). Safe, and not by luck: esp_vfs_open() holds no lock
 * while calling a filesystem op, and nothing here is holding one either. It
 * cannot recurse back into stat().
 */
static int vfs_stat(void *ctx, const char *path, struct stat *st)
{
    RESOLVE_OR_FAIL(path, -1);

    const void *dev = espix_dev_lookup(p);
    if (dev != NULL) {
        espix_dev_stat(dev, st);
        return 0;
    }
    if (espix_dev_isdir(p)) {
        espix_dev_dir_stat(st);
        return 0;
    }
    if (espix_dev_underdev(p)) {
        /* A name in /dev that is not a node is not there -- never littlefs. */
        errno = ENOENT;
        return -1;
    }

    const lower_t *l = mount_by_path(p);

    if (NO_LOWER(l->dir->stat_p)) {
        return enosys();
    }

    const int rc = l->dir->stat_p(l->ctx, p, st);
    if (rc == 0) {
        /*
         * Replaced, not or-ed. The filesystem below reports the type -- S_IFDIR,
         * S_IFREG -- and espix decides the permissions, which is the split Linux
         * draws between its VFS and the filesystems under it. FatFs fills in 0777
         * of its own, so or-ing espix's bits into them left every file on a
         * mounted stick world-writable and executable, while the access check
         * computed 0644 from the very same rule: what stat() reported and what
         * espix enforced disagreed.
         */
        st->st_mode = (st->st_mode & ~(mode_t)ESPIX_MODE_BITS) |
                      (espix_fs_mode(p, st) & ESPIX_MODE_BITS);
    }
    return rc;
}

static int vfs_unlink(void *ctx, const char *path)
{
    RESOLVE_OR_FAIL(path, -1);

    if (espix_dev_isdir(p) || espix_dev_underdev(p)) {
        /* Nothing in /dev is a name espix will edit. */
        errno = EROFS;
        return -1;
    }

    const int err = espix_fs_access_check(p, ESPIX_FS_ACCESS_UNLINK, 0);
    if (err != 0) {
        errno = err;
        return -1;
    }
    const lower_t *l = mount_by_path(p);
    return NO_LOWER(l->dir->unlink_p) ? enosys()
                                      : l->dir->unlink_p(l->ctx, p);
}

static int vfs_rename(void *ctx, const char *src, const char *dst)
{
    char abs_src[ESPIX_PATH_MAX];
    char abs_dst[ESPIX_PATH_MAX];

    /* No errno of its own: resolve() has set the right one, and overwriting it
     * here would report a path outside the root as merely too long. */
    if (resolve(src, abs_src, sizeof(abs_src)) == NULL ||
        resolve(dst, abs_dst, sizeof(abs_dst)) == NULL) {
        return -1;
    }

    /* A rename edits two names; either one landing in /dev is refused, in
     * both directions, because nothing there is a littlefs name. */
    if (espix_dev_isdir(abs_src) || espix_dev_underdev(abs_src) ||
        espix_dev_isdir(abs_dst) || espix_dev_underdev(abs_dst)) {
        errno = EROFS;
        return -1;
    }

    /* Both ends: a rename removes a name here and creates one there. */
    int err = espix_fs_access_check(abs_src, ESPIX_FS_ACCESS_RENAME, 0);
    if (err == 0) {
        err = espix_fs_access_check(abs_dst, ESPIX_FS_ACCESS_RENAME, 0);
    }
    if (err != 0) {
        errno = err;
        return -1;
    }
    /*
     * One filesystem for both names: moving data between two mounts is not a
     * rename, and EXDEV is what every Unix says about it rather than
     * half-doing it.
     */
    const lower_t *l = mount_by_path(abs_src);
    if (mount_by_path(abs_dst) != l) {
        errno = EXDEV;
        return -1;
    }

    return NO_LOWER(l->dir->rename_p)
               ? enosys() : l->dir->rename_p(l->ctx, abs_src, abs_dst);
}

static DIR *vfs_opendir(void *ctx, const char *name)
{
    RESOLVE_OR_FAIL(name, NULL);

    const int err = espix_fs_access_check(p, ESPIX_FS_ACCESS_OPENDIR, 0);
    if (err != 0) {
        errno = err;
        return NULL;
    }
    if (espix_dev_isdir(p)) {
        return espix_dev_opendir();
    }
    if (espix_dev_underdev(p)) {
        /* Only /dev itself is a directory here. */
        errno = ENOTDIR;
        return NULL;
    }
    const lower_t *l = mount_by_path(p);

    if (NO_LOWER(l->dir->opendir_p)) {
        errno = ENOSYS;
        return NULL;
    }

    /*
     * readdir and closedir get this handle and no path, so the mount it came
     * from is remembered here and looked up again on those calls.
     */
    DIR *pdir = l->dir->opendir_p(l->ctx, p);
    if (pdir != NULL) {
        dir_map_set(pdir, l);
    }
    return pdir;
}

static struct dirent *vfs_readdir(void *ctx, DIR *pdir)
{
    if (espix_dev_dirp(pdir)) {
        return espix_dev_readdir(pdir);
    }
    const lower_t *l = mount_by_dir(pdir);
    if (l == NULL) {
        errno = EBADF;
        return NULL;
    }
    if (NO_LOWER(l->dir->readdir_p)) {
        errno = ENOSYS;
        return NULL;
    }
    return l->dir->readdir_p(l->ctx, pdir);
}

static int vfs_readdir_r(void *ctx, DIR *pdir, struct dirent *entry,
                         struct dirent **out)
{
    if (espix_dev_dirp(pdir)) {
        return espix_dev_readdir_r(pdir, entry, out);
    }
    const lower_t *l = mount_by_dir(pdir);
    if (l == NULL) {
        return ebadf();
    }
    return NO_LOWER(l->dir->readdir_r_p)
               ? enosys() : l->dir->readdir_r_p(l->ctx, pdir, entry, out);
}

static long vfs_telldir(void *ctx, DIR *pdir)
{
    if (espix_dev_dirp(pdir)) {
        return espix_dev_telldir(pdir);
    }
    const lower_t *l = mount_by_dir(pdir);
    if (l == NULL) {
        return -1;
    }
    return NO_LOWER(l->dir->telldir_p) ? enosys()
                                       : l->dir->telldir_p(l->ctx, pdir);
}

static void vfs_seekdir(void *ctx, DIR *pdir, long offset)
{
    if (espix_dev_dirp(pdir)) {
        espix_dev_seekdir(pdir, offset);
        return;
    }
    const lower_t *l = mount_by_dir(pdir);
    if (l != NULL && !NO_LOWER(l->dir->seekdir_p)) {
        l->dir->seekdir_p(l->ctx, pdir, offset);
    }
}

static int vfs_closedir(void *ctx, DIR *pdir)
{
    if (espix_dev_dirp(pdir)) {
        return espix_dev_closedir(pdir);
    }
    const lower_t *l = mount_by_dir(pdir);
    if (l == NULL) {
        return ebadf();
    }
    const int rc = NO_LOWER(l->dir->closedir_p) ? enosys()
                                                : l->dir->closedir_p(l->ctx, pdir);
    dir_map_clear(pdir);
    return rc;
}

static int vfs_mkdir(void *ctx, const char *name, mode_t mode)
{
    RESOLVE_OR_FAIL(name, -1);

    /*
     * /dev itself is a real littlefs directory -- the mount point that keeps
     * `ls /` listing it -- so mkdir("/dev") is let through. A name *inside* it
     * is not: the device table owns that space and it holds no creatable
     * entries.
     */
    if (espix_dev_underdev(p)) {
        errno = EROFS;
        return -1;
    }

    const int err = espix_fs_access_check(p, ESPIX_FS_ACCESS_MKDIR, 0);
    if (err != 0) {
        errno = err;
        return -1;
    }
    const lower_t *l = mount_by_path(p);
    if (NO_LOWER(l->dir->mkdir_p)) {
        return enosys();
    }

    const int rc = l->dir->mkdir_p(l->ctx, p, mode);
    if (rc == 0) {
        espix_fs_claim(p);
    }
    return rc;
}

static int vfs_rmdir(void *ctx, const char *name)
{
    RESOLVE_OR_FAIL(name, -1);

    if (espix_dev_isdir(p) || espix_dev_underdev(p)) {
        /* The mount point stays, and nothing inside it is a real directory. */
        errno = EROFS;
        return -1;
    }

    const int err = espix_fs_access_check(p, ESPIX_FS_ACCESS_RMDIR, 0);
    if (err != 0) {
        errno = err;
        return -1;
    }
    const lower_t *l = mount_by_path(p);
    return NO_LOWER(l->dir->rmdir_p) ? enosys() : l->dir->rmdir_p(l->ctx, p);
}

static int vfs_truncate(void *ctx, const char *path, off_t length)
{
    RESOLVE_OR_FAIL(path, -1);

    if (espix_dev_isdir(p) || espix_dev_underdev(p)) {
        errno = EROFS;
        return -1;
    }

    const int err = espix_fs_access_check(p, ESPIX_FS_ACCESS_TRUNCATE, 0);
    if (err != 0) {
        errno = err;
        return -1;
    }
    const lower_t *l = mount_by_path(p);
    return NO_LOWER(l->dir->truncate_p)
               ? enosys() : l->dir->truncate_p(l->ctx, p, length);
}

static int vfs_ftruncate(void *ctx, int fd, off_t length)
{
    if (espix_dev_fd(fd)) {
        /* /dev/null is already empty and /dev/factory is read-only. */
        (void)length;
        errno = EINVAL;
        return -1;
    }
    fd_slot_t slot;
    if (!fd_slot_get(fd, &slot)) {
        return ebadf();
    }
    const lower_t *l = mount_of_slot(&slot);
    return NO_LOWER(l->dir->ftruncate_p)
               ? enosys() : l->dir->ftruncate_p(l->ctx, slot.lower_fd, length);
}

static int vfs_utime(void *ctx, const char *path, const struct utimbuf *times)
{
    RESOLVE_OR_FAIL(path, -1);

    if (espix_dev_isdir(p) || espix_dev_underdev(p)) {
        errno = EROFS;
        return -1;
    }
    const lower_t *l = mount_by_path(p);
    return NO_LOWER(l->dir->utime_p) ? enosys()
                                     : l->dir->utime_p(l->ctx, p, times);
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

/*
 * Fill in slot 0. Kept separate from espix_vfs_add_mount() above it because the
 * root is not a mount like the others: it has no prefix, it is the fallback
 * every path reaches, and it is registered with esp_vfs rather than only with
 * the table below.
 */
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

    portENTER_CRITICAL(&s_mount_lock);
    lower_t *root = &s_mounts[0];
    root->ops      = lower_ops;
    root->dir      = lower_ops->dir;
    root->ctx      = lower_ctx;
    root->prefix[0] = '\0';
    root->len      = 0;
    /* littlefs carries espix's own mode and owner attributes. */
    root->stored_metadata = true;
    root->owner_uid = ESPIX_FS_OWNER_RULE;
    root->owner_gid = ESPIX_FS_OWNER_RULE;
    root->used     = true;
    portEXIT_CRITICAL(&s_mount_lock);

    /*
     * "" is the fallback: any path no longer prefix claims. That is what makes
     * this the root, and only one VFS can hold it -- there is no "/" entry to
     * share, since esp_vfs_register_fs() requires a prefix of two characters or
     * more. Device VFSes such as /dev/uart keep working by being longer.
     *
     * STATIC because the tables above are `const` and outlive any call;
     * CONTEXT_PTR because the ops take the lower layer as their first argument.
     * The context is slot 0 and is used for nothing but identity: ops pick their
     * mount by path, fd or directory handle, this VFS serving every mount.
     *
     * *With_id* rather than the plain call, because the id it hands back is what
     * esp_vfs_register_fd() needs when a file is opened: the fd a caller gets is
     * allocated there, and that allocation is what keeps it from colliding with
     * the console, the sockets and the other filesystems' own numbering.
     */
    /* Before the VFS exists, so no fd can arrive before the table can answer. */
    fd_table_init();

    const esp_err_t err = esp_vfs_register_fs_with_id(
        &s_espix_vfs, ESP_VFS_FLAG_CONTEXT_PTR | ESP_VFS_FLAG_STATIC,
        &s_mounts[0], &s_vfs_id);

    if (err != ESP_OK) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "cannot register the root VFS: %s",
                   esp_err_to_name(err));
    }
    return err;
}

esp_err_t espix_vfs_add_mount(const char *prefix,
                              const esp_vfs_fs_ops_t *ops, void *ctx,
                              bool stored_metadata,
                              uint16_t owner_uid, uint16_t owner_gid)
{
    if (ops == NULL || ctx == NULL || prefix == NULL || ops->dir == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Absolute, at least one character, no trailing slash: a prefix is matched
     * whole, so "/mnt/" would never match "/mnt" and would quietly mount
     * nothing.
     */
    const size_t len = strlen(prefix);
    if (len < 1 || len >= ESPIX_FS_PREFIX_MAX || prefix[0] != '/' ||
        prefix[len - 1] == '/') {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_mount_lock);

    for (size_t i = 1; i < ESPIX_FS_MAX_MOUNTS; i++) {
        if (s_mounts[i].used && strcmp(s_mounts[i].prefix, prefix) == 0) {
            portEXIT_CRITICAL(&s_mount_lock);
            return ESP_ERR_INVALID_STATE;
        }
    }

    lower_t *slot = NULL;
    for (size_t i = 1; i < ESPIX_FS_MAX_MOUNTS; i++) {
        if (!s_mounts[i].used) {
            slot = &s_mounts[i];
            break;
        }
    }
    if (slot == NULL) {
        portEXIT_CRITICAL(&s_mount_lock);
        espix_klog(ESPIX_KLOG_WARN, TAG, "no free mount: %s", prefix);
        return ESP_ERR_NO_MEM;
    }

    slot->ops      = ops;
    slot->dir      = ops->dir;
    slot->ctx      = ctx;
    strlcpy(slot->prefix, prefix, sizeof(slot->prefix));
    slot->len      = len;
    slot->stored_metadata = stored_metadata;
    slot->owner_uid = owner_uid;
    slot->owner_gid = owner_gid;
    slot->used     = true;

    portEXIT_CRITICAL(&s_mount_lock);

    espix_klog(ESPIX_KLOG_INFO, TAG, "mounted %s", prefix);
    return ESP_OK;
}

esp_err_t espix_vfs_del_mount(const char *prefix)
{
    if (prefix == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_mount_lock);

    lower_t *slot = NULL;
    for (size_t i = 1; i < ESPIX_FS_MAX_MOUNTS; i++) {
        if (s_mounts[i].used && strcmp(s_mounts[i].prefix, prefix) == 0) {
            slot = &s_mounts[i];
            break;
        }
    }
    if (slot == NULL) {
        portEXIT_CRITICAL(&s_mount_lock);
        return ESP_ERR_NOT_FOUND;
    }

    /*
     * Refused while a file is still open on it. There is no way to revoke a
     * lower filesystem's fd -- it belongs to a task somewhere -- so the mount
     * stays until the caller closes up; pulling the volume out from under a
     * reader is the one outcome worse than a failed umount.
     */
    for (int fd = 0; fd < ESPIX_DEV_FD_BASE; fd++) {
        if (s_fds[fd].lower_fd >= 0 &&
            s_fds[fd].mount == (uint8_t)((slot - s_mounts) + 1)) {
            portEXIT_CRITICAL(&s_mount_lock);
            return ESP_ERR_INVALID_STATE;
        }
    }
    for (size_t i = 0; i < ESPIX_FS_DIRS; i++) {
        if (s_dir_map[i].mount == (uint8_t)((slot - s_mounts) + 1)) {
            portEXIT_CRITICAL(&s_mount_lock);
            return ESP_ERR_INVALID_STATE;
        }
    }

    slot->used = false;
    slot->ops  = NULL;
    slot->dir  = NULL;
    slot->ctx  = NULL;
    slot->len  = 0;
    slot->prefix[0] = '\0';

    portEXIT_CRITICAL(&s_mount_lock);

    espix_klog(ESPIX_KLOG_INFO, TAG, "unmounted %s", prefix);
    return ESP_OK;
}

bool espix_vfs_stores_metadata(const char *abs_path)
{
    return mount_by_path(abs_path)->stored_metadata;
}

bool espix_vfs_mount_owner(const char *abs_path, uint16_t *uid, uint16_t *gid)
{
    const lower_t *l = mount_by_path(abs_path);

    /* The sentinel means the rule answers -- the rootfs, and anything that keeps
     * ownership in the filesystem itself. */
    if (l == NULL || l->owner_uid == ESPIX_FS_OWNER_RULE) {
        return false;
    }
    if (uid != NULL) {
        *uid = l->owner_uid;
    }
    if (gid != NULL) {
        *gid = l->owner_gid;
    }
    return true;
}
