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
    FILE *f = fopen(abs_path, "rb");
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
        return 0755;
    }
    return looks_executable(abs_path) ? 0755 : 0644;
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
    if (attr_read(abs_path, &attr) == ESP_OK) {
        return (mode_t)(attr.mode & ESPIX_MODE_BITS);
    }

    return mode_from_rule(abs_path, st);
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
    if (stat(abs_path, &st) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    /*
     * A mode the rule already produces is not worth storing. Removing the
     * attribute instead is what keeps a device that nobody has chmod'd free of
     * mode data entirely -- `chmod +x` then `chmod -x` on a plain file leaves
     * no trace, and costs one metadata write rather than two.
     */
    if (mode == mode_from_rule(abs_path, &st)) {
        return esp_littlefs_removeattr(ESPIX_FS_ROOT_PARTITION, abs_path,
                                       ESPIX_FS_ATTR_POSIX);
    }

    /*
     * Read-modify-write rather than a blind write, so that uid and gid survive
     * a chmod once something sets them. Nothing does today; the fields are on
     * disk from the start because widening the record later would mean
     * rewriting every file that had one.
     */
    espix_fs_posix_attr_t attr = { 0 };
    (void)attr_read(abs_path, &attr);

    attr.mode = (uint16_t)mode;

    return esp_littlefs_setattr(ESPIX_FS_ROOT_PARTITION, abs_path,
                                ESPIX_FS_ATTR_POSIX, &attr, sizeof(attr));
}

/*
 * "-rwxr-xr-x", the way ls writes it.
 *
 * Never emits s, S, t or T. espix stores nine bits, not twelve: setuid, setgid
 * and sticky are each defined in terms of the owner of the file they are set
 * on, and nothing sets an owner yet. Printing those letters would report a
 * protection that nothing implements, to someone reading `ls -l` who would
 * reasonably believe it.
 */
void espix_fs_mode_str(mode_t mode, bool is_dir, char *out, size_t len)
{
    char s[11];

    s[0] = is_dir ? 'd' : '-';
    for (int i = 0; i < 9; i++) {
        /* Bit 8 is owner-read, bit 0 is other-execute. */
        s[1 + i] = (mode & (mode_t)(1u << (8 - i))) ? "rwx"[i % 3] : '-';
    }
    s[10] = '\0';

    strlcpy(out, s, len);
}
