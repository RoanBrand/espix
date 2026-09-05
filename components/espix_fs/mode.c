/*
 * File modes: a rule for the common case, an inode attribute for the rest.
 *
 * LittleFS stores no permission bits of its own, but it does carry user
 * attributes -- small blobs in an entry's metadata, which SPEC.md describes as
 * meant for exactly this sort of thing and which the ESP port already uses to
 * hold mtime. That is where a mode belongs, and it is what the README always
 * said this would use.
 *
 * Getting at them needs `lfs_setattr`, which needs the `lfs_t *` the port keeps
 * private; docs/UPSTREAM.md has the finding and tools/patch-littlefs.py carries
 * the three-function patch that fixes it.
 *
 * Two halves, and the split is what keeps this cheap:
 *
 *   - A *rule* supplies the default. Directories are 0755, files that start
 *     with the ELF magic are 0755, everything else is 0644. Nothing is stored,
 *     so the flashed rootfs image needs no mode data -- which is not a nicety:
 *     the image builder writes no attributes at all, so without the rule
 *     nothing in /bin would be executable after a storage-flash.
 *
 *   - The attribute records only what someone changed with chmod. A file
 *     nobody has chmod'd carries none, LFS_ERR_NOATTR comes back, and the rule
 *     answers. A mode that matches the rule again *removes* the attribute
 *     rather than storing it.
 *
 * The consequence worth knowing: a flash write happens when you run chmod and
 * at no other time, which matters on a filesystem that pays a block erase per
 * write.
 *
 * What this no longer needs is bookkeeping. An earlier version kept the
 * deviations in /etc/modes keyed by path, which meant every rename and every
 * delete had to be followed by hand in `mv`, `rm`, `rm -r` and the SFTP server,
 * and an app renaming a file itself would have lost the mode. LittleFS moves an
 * attribute with the entry and drops it with the file, so all of that is gone.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_littlefs.h"

#include "espix_fs.h"
#include "espix_kernel.h"

#include "espix_fs_priv.h"

#define TAG "fs"

/* ------------------------------------------------------------------ */
/* The rule                                                            */
/* ------------------------------------------------------------------ */

/*
 * True if `abs_path` starts with the ELF magic.
 *
 * This is the same test the shell used to gate execution on before there were
 * mode bits, moved here so "is it a program" is asked in one place. It costs an
 * open and a four-byte read per rule-derived file, which `ls -l` pays per entry
 * -- acceptable because LittleFS caches the block, and because a stored mode
 * short-circuits it entirely.
 */
static bool looks_executable(const char *abs_path)
{
    /*
     * Privileged, and it has to be.
     *
     * This open is espix asking itself what kind of file this is, on the way to
     * deciding the very mode the permission check is about to consult. Left
     * unprivileged it recurses without bound: the check needs the mode, the
     * rule computes the mode by opening the file, that open runs the check, and
     * the stack is gone in milliseconds -- which showed up as heap corruption
     * with an innocent task holding the pieces, never as a stack trace through
     * here.
     *
     * The four bytes read never reach the caller; only the answer does.
     */
    espix_fs_priv_begin();
    FILE *f = fopen(abs_path, "rb");
    espix_fs_priv_end();

    if (f == NULL) {
        return false;
    }

    char         magic[4] = { 0 };
    const size_t got      = fread(magic, 1, sizeof(magic), f);
    fclose(f);

    return got == sizeof(magic) && memcmp(magic, "\177ELF", sizeof(magic)) == 0;
}

static mode_t mode_from_rule(const char *abs_path, const struct stat *st)
{
    if (S_ISDIR(st->st_mode)) {
        /*
         * /tmp is the one directory everybody is expected to be able to write,
         * and 1777 is what every Unix gives it: writable by all, but the sticky
         * bit means you may remove only what you own.
         *
         * In the rule rather than stamped on at boot, for the same reasons the
         * rest of the rule exists. It costs no flash write, it survives a
         * storage-flash -- the image carries no attributes at all -- and a
         * deliberate chmod still wins, because a stored attribute always beats
         * the rule.
         *
         * Without this, ownership made /tmp root's and 0755, and no ordinary
         * user could write there at all. That was a regression the moment
         * permissions started being enforced.
         */
        if (strcmp(abs_path, "/tmp") == 0) {
            return 01777;
        }
        return 0755;
    }
    return looks_executable(abs_path) ? 0755 : 0644;
}

static espix_fs_owner_rule_t s_owner_rule;

void espix_fs_set_owner_rule(espix_fs_owner_rule_t rule)
{
    s_owner_rule = rule;
}

/*
 * Who owns a file nobody has chowned.
 *
 * Delegated, because answering it means reading /etc/passwd and the dependency
 * only runs the other way; see the note on espix_fs_set_owner_rule(). What the
 * installed rule does is match the path against each account's home and take
 * the longest, which makes root -- whose home is "/" -- the owner of everything
 * outside anybody's home.
 *
 * Falling back to root rather than to the caller matters: this runs before
 * espix_auth_init() during boot, and a file that answered "owned by whoever is
 * asking" would be a hole rather than a default.
 */
static void owner_from_rule(const char *abs_path, uint16_t *uid, uint16_t *gid)
{
    *uid = 0;
    *gid = 0;

    if (s_owner_rule != NULL) {
        (void)s_owner_rule(abs_path, uid, gid);
    }
}

/* ------------------------------------------------------------------ */
/* The attribute                                                       */
/* ------------------------------------------------------------------ */

/*
 * espix mounts LittleFS as the fallback VFS with an empty base path, so an
 * absolute espix path is already the mount-relative path the port wants. That
 * is a property of how espix_fs_mount_root() registers it, not a general truth.
 */
static esp_err_t attr_read(const char *abs_path, espix_fs_posix_attr_t *out)
{
    return esp_littlefs_getattr(ESPIX_FS_ROOT_PARTITION, abs_path,
                                ESPIX_FS_ATTR_POSIX, out, sizeof(*out), NULL);
}

/* Everything the rules say about a path, with nothing stored. */
static void attr_from_rule(const char *abs_path, const struct stat *st,
                           espix_fs_posix_attr_t *out)
{
    /* Locals, then assign: the attribute is packed, so its members have no
     * address that can safely be taken. */
    uint16_t uid = 0;
    uint16_t gid = 0;
    owner_from_rule(abs_path, &uid, &gid);

    out->mode = (uint16_t)mode_from_rule(abs_path, st);
    out->uid  = uid;
    out->gid  = gid;
}

/*
 * What a path's mode and owner actually are: the stored attribute if there is
 * one, the rules otherwise.
 *
 * Going through here rather than reading the attribute directly is what keeps
 * chmod and chown from destroying each other. The attribute is one record of
 * three fields, so writing it to change a mode also writes a uid -- and a
 * blank one would silently hand a file in /home/esp to root. Seeding from the
 * rules first means the fields nobody is changing keep the values they had.
 */
static bool attr_effective(const char *abs_path, const struct stat *st,
                           espix_fs_posix_attr_t *out)
{
    if (attr_read(abs_path, out) == ESP_OK) {
        return true;
    }
    attr_from_rule(abs_path, st, out);
    return false;
}

/*
 * Persist an attribute, or drop it if the rules already say the same thing.
 *
 * Removing rather than storing is what keeps a device nobody has chmod'd or
 * chowned free of attribute data entirely -- `chmod +x` then `chmod -x` leaves
 * no trace, and a fresh rootfs image, which carries no attributes at all, stays
 * that way. It costs one metadata write rather than two.
 *
 * The comparison has to be against every field, not just the one that changed:
 * a file whose mode is back to the rule's answer but whose owner is not still
 * needs its record.
 */
static esp_err_t attr_store(const char *abs_path, const struct stat *st,
                            const espix_fs_posix_attr_t *attr)
{
    espix_fs_posix_attr_t rule;
    attr_from_rule(abs_path, st, &rule);

    if (attr->mode == rule.mode && attr->uid == rule.uid &&
        attr->gid == rule.gid) {
        const esp_err_t err = esp_littlefs_removeattr(ESPIX_FS_ROOT_PARTITION,
                                                      abs_path,
                                                      ESPIX_FS_ATTR_POSIX);
        /* Nothing stored is the state we wanted; not an error. */
        return (err == ESP_ERR_NOT_FOUND) ? ESP_OK : err;
    }

    return esp_littlefs_setattr(ESPIX_FS_ROOT_PARTITION, abs_path,
                                ESPIX_FS_ATTR_POSIX, attr, sizeof(*attr));
}

/* ------------------------------------------------------------------ */
/* Public                                                              */
/* ------------------------------------------------------------------ */

mode_t espix_fs_mode(const char *abs_path, const struct stat *st)
{
    struct stat own;

    if (abs_path == NULL) {
        return 0;
    }
    if (st == NULL) {
        if (stat(abs_path, &own) != 0) {
            return 0;
        }
        st = &own;
    }

    espix_fs_posix_attr_t attr;
    (void)attr_effective(abs_path, st, &attr);
    return (mode_t)(attr.mode & ESPIX_MODE_BITS);
}

void espix_fs_owner(const char *abs_path, const struct stat *st,
                    uint16_t *uid, uint16_t *gid)
{
    struct stat own;

    if (uid != NULL) {
        *uid = 0;
    }
    if (gid != NULL) {
        *gid = 0;
    }
    if (abs_path == NULL) {
        return;
    }
    if (st == NULL) {
        if (stat(abs_path, &own) != 0) {
            return;
        }
        st = &own;
    }

    espix_fs_posix_attr_t attr;
    (void)attr_effective(abs_path, st, &attr);

    if (uid != NULL) {
        *uid = attr.uid;
    }
    if (gid != NULL) {
        *gid = attr.gid;
    }
}

bool espix_fs_is_executable(const char *abs_path)
{
    struct stat st;

    if (abs_path == NULL || stat(abs_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return false;
    }
    return (espix_fs_mode(abs_path, &st) & S_IXUSR) != 0;
}

esp_err_t espix_fs_chmod(const char *abs_path, mode_t mode)
{
    struct stat st;

    if (abs_path == NULL || (mode & ~(mode_t)ESPIX_MODE_BITS) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    /* ENOENT is the root refusing, and has to stay distinguishable from the
     * ownership refusal: callers map it to errno, and a path outside the root
     * must read as absent rather than as an I/O failure. */
    const int admin = espix_fs_admin_check(abs_path, false);
    if (admin == ENOENT) {
        return ESP_ERR_NOT_FOUND;
    }
    if (admin != 0) {
        return ESP_ERR_NOT_ALLOWED;
    }
    if (stat(abs_path, &st) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    espix_fs_posix_attr_t attr;
    (void)attr_effective(abs_path, &st, &attr);
    attr.mode = (uint16_t)mode;

    return attr_store(abs_path, &st, &attr);
}

esp_err_t espix_fs_ensure_mode(const char *abs_path, mode_t mode)
{
    struct stat st;

    if (abs_path == NULL || (mode & ~(mode_t)ESPIX_MODE_BITS) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (stat(abs_path, &st) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (espix_fs_mode(abs_path, &st) == mode) {
        return ESP_OK;          /* already right; nothing written */
    }
    return espix_fs_chmod(abs_path, mode);
}

esp_err_t espix_fs_chown(const char *abs_path, uint16_t uid, uint16_t gid)
{
    struct stat st;

    if (abs_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const int admin = espix_fs_admin_check(abs_path, uid != ESPIX_FS_KEEP_ID);
    if (admin == ENOENT) {
        return ESP_ERR_NOT_FOUND;      /* the root; see espix_fs_chmod() */
    }
    if (admin != 0) {
        return ESP_ERR_NOT_ALLOWED;
    }
    if (stat(abs_path, &st) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    espix_fs_posix_attr_t attr;
    (void)attr_effective(abs_path, &st, &attr);

    if (uid != ESPIX_FS_KEEP_ID) {
        attr.uid = uid;
    }
    if (gid != ESPIX_FS_KEEP_ID) {
        attr.gid = gid;
    }

    /*
     * Handing a file to somebody drops setuid and setgid, as chown(2) does.
     *
     * Not the main containment -- only root may chown at all, and only root can
     * therefore produce a root-owned setuid binary -- but it closes the shape of
     * the mistake where an administrator chowns a user's setuid program to root
     * meaning to tidy up, and grants it the superuser instead.
     */
    attr.mode &= (uint16_t)~(S_ISUID | S_ISGID);

    return attr_store(abs_path, &st, &attr);
}

/*
 * "-rwxr-xr-x", or "drwxrwxrwt", the way ls writes it.
 *
 * The three high bits fold into the execute column of the triad they belong to,
 * as ls(1) does it: lowercase when that execute bit is also set, uppercase when
 * it is not -- so `s` means setuid-and-executable and `S` means setuid on
 * something nobody can execute, which is nearly always a mistake worth seeing.
 *
 * espix emits these now because it acts on all three; while it did not, printing
 * them would have reported a protection nothing implemented to someone reading
 * `ls -l` who would reasonably have believed it.
 */
void espix_fs_mode_str(mode_t mode, bool is_dir, char *out, size_t len)
{
    char s[11];

    s[0] = is_dir ? 'd' : '-';
    for (int i = 0; i < 9; i++) {
        /* Bit 8 is owner-read, bit 0 is other-execute. */
        s[1 + i] = (mode & (mode_t)(1u << (8 - i))) ? "rwx"[i % 3] : '-';
    }

    /* Each replaces the execute character of its own triad. */
    if (mode & S_ISUID) {
        s[3] = (mode & S_IXUSR) ? 's' : 'S';
    }
    if (mode & S_ISGID) {
        s[6] = (mode & S_IXGRP) ? 's' : 'S';
    }
    if (mode & S_ISVTX) {
        s[9] = (mode & S_IXOTH) ? 't' : 'T';
    }
    s[10] = '\0';

    strlcpy(out, s, len);
}
