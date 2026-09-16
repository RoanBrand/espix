/*
 * espix root filesystem.
 *
 * espix registers its *own* VFS as the fallback (empty base path), which makes
 * it the real root: paths are /bin/hello, /etc/hostname, /home/... rather than
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

#include "esp_blockdev.h"
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
 * Mount table shape: the root plus three, from components/espix_fs/vfs.c. A
 * caller keeping its own per-mount records sizes them with this, so the two
 * cannot drift apart.
 */
#define ESPIX_FS_MAX_MOUNTS 4

/*
 * Mount the FAT filesystem on `dev` at `path` -- the second filesystem in the
 * namespace, and the first one that is not littlefs.
 *
 * Reached through espix's own VFS like the root, so a permission check applies
 * there too; see components/espix_fs/fat.c and tools/patch-fatfs.py for what
 * that costs. `path` is the *mount point*: the caller is expected to have made
 * it, and its contents are hidden for as long as the mount lasts, exactly as on
 * any Unix.
 *
 * The block device stays the caller's -- this never releases the handle, because
 * a whole disk and a partition view over one are both mountable and only the
 * caller knows which it made. Release it after espix_fs_unmount_fat().
 *
 * `owner_uid` and `owner_gid` own everything the volume holds, because FAT keeps
 * no ownership of its own: the uid= and gid= Linux gives a removable volume, so
 * the person who plugged the stick in can write to it. They are the mounting
 * session's ids, told to this rather than looked up.
 *
 * Never formats: a volume that does not mount is reported, not overwritten.
 */
esp_err_t espix_fs_mount_fat(const char *path, esp_blockdev_handle_t dev,
                             uint16_t owner_uid, uint16_t owner_gid);

/*
 * Unmount it. ESP_ERR_INVALID_STATE while a file or directory is still open on
 * the mount -- a lower filesystem's fd cannot be revoked, so the caller has to
 * close up first.
 */
esp_err_t espix_fs_unmount_fat(const char *path);

/*
 * The device a mount came from has been unplugged.
 *
 * The mount is *marked*, not removed: it keeps claiming its paths so they do not
 * quietly start resolving inside the rootfs, and every operation on it fails
 * instead of reaching a filesystem whose block device espix_usb has already
 * released. Unmount it afterwards to give the rest back -- espix_fs_unmount_fat()
 * knows not to sync a volume whose device is gone.
 */
esp_err_t espix_fs_mount_dead(const char *path);

/*
 * A block device view over a partition of `parent`, for mounting `sda1` rather
 * than `sda`. Released with its own ops->release, which frees the view and
 * leaves the parent alone -- the parent belongs to whoever made it.
 *
 * IDF's esp_blockdev_generic_partition_get() does the same thing with 32-bit
 * offsets, which cannot express a partition larger than 4GB on an ESP32 target.
 * Since that is one FAT32 volume on an ordinary stick, this is 64-bit; see
 * part.c.
 */
esp_err_t espix_fs_partition_view(esp_blockdev_handle_t parent, uint64_t start,
                                  uint64_t size, esp_blockdev_handle_t *out);

/*
 * Give a block device a name in /dev, or take it away: "sda" for a disk, "sda1"
 * for a partition of one.
 *
 * These exist so a device has the name every other system gives it -- `ls /dev`
 * lists them, `stat` reports the medium's size, `mount /dev/sda1` works -- and
 * so a mount point can be found without knowing anything about the driver
 * underneath. Opening one is refused: raw sector access would have to know which
 * device it holds and whether it is mounted, so it is a feature of its own
 * rather than something to fake here.
 *
 * Registered by whoever knows about both halves, since nothing in espix_fs knows
 * what USB is and espix_usb knows nothing about the VFS. A name that already
 * exists is updated rather than duplicated, and both calls are safe from any
 * task: the pool is locked because a session can be listing /dev meanwhile.
 */
esp_err_t espix_dev_register_block(const char *name, uint64_t size);
void espix_dev_unregister_block(const char *name);

/*
 * Resolve `path` against `cwd` into `out` (absolute, no "." or ".." segments,
 * no trailing slash except for "/" itself). Used by every shell command that
 * takes a path, so relative paths behave the same everywhere.
 */
esp_err_t espix_fs_resolve(const char *cwd, const char *path,
                           char *out, size_t out_len);

/*
 * True if `abs_path` is `dir` or something underneath it. Both must already be
 * absolute and normalised -- espix_fs_resolve() output, in other words.
 *
 * The subtlety worth having in one place is the boundary: a plain prefix match
 * lets /home/esp claim /home/espix, which would hand one account's files to
 * another whose name it happens to begin with. "/" contains everything.
 *
 * Two callers with the same requirement: the owner rule, matching a path
 * against each account's home, and a process's root.
 */
bool espix_fs_within(const char *abs_path, const char *dir);

/* Recursive delete, used by `rm -r`. */
esp_err_t espix_fs_rm_rf(const char *abs_path);

/* ------------------------------------------------------------------ */
/* File modes                                                          */
/* ------------------------------------------------------------------ */

/*
 * All twelve bits, and every one of them is consulted somewhere.
 *
 * espix stored nine for a long time, on the rule that it will not keep a bit it
 * does not act on -- a mode someone can set and read back but which changes
 * nothing is worse than an error. The other three now have somewhere to be
 * read:
 *
 *   - setuid on a regular file: espix_proc gives the process the file's uid
 *     instead of the launching session's.
 *   - setgid on a regular file: the same for gid.
 *   - sticky on a directory: espix_fs_access_check() lets you remove only what
 *     you own, which is what makes a 1777 /tmp safe to share.
 *
 * The combinations that still mean nothing are refused by name rather than
 * stored and ignored -- setuid or setgid on a *directory*, which Linux uses for
 * group inheritance espix does not implement, and sticky on a *file*, which
 * Linux ignores. See chmod_parse().
 *
 * Worth being honest about what setuid buys here: the S3 has no MMU, so an app
 * already shares the address space with the kernel and setuid is a guardrail
 * against mistakes rather than a boundary against hostile code. It is
 * implemented now because the hardware that makes it a real boundary -- the
 * S31, which has an MMU -- exists.
 */
#define ESPIX_MODE_BITS 07777

/* The nine permission bits alone, where the owner/group/other triads are meant
 * rather than the whole stored mode. */
#define ESPIX_PERM_BITS 0777

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

/*
 * Set the mode only if it is not already what it should be.
 *
 * For a component that owns a file and wants it locked down every boot,
 * whether or not this device has ever had it locked down before. The guard is
 * the point: espix_fs_chmod() writes the attribute whenever it differs from
 * the rule, so calling it unconditionally on every boot costs a LittleFS
 * metadata write on every boot -- a block erase, forever, to arrive at the
 * mode the file already had.
 *
 * ESP_OK when the mode already matched and nothing was written.
 */
esp_err_t espix_fs_ensure_mode(const char *abs_path, mode_t mode);

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
