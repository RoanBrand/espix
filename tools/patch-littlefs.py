#!/usr/bin/env python3
"""
Add a custom-attribute API and a mount-only entry point to the downloaded
joltwallet/littlefs.

Why this exists
---------------
LittleFS carries POSIX-style metadata in user attributes -- `lfs_setattr`,
`lfs_getattr`, `lfs_removeattr` are public API and the ESP port already uses one
('t') to store mtime. But the port never exposes the `lfs_t *` those need:
`_efs[]` is file-scope static, every lookup is static, and `littlefs_api.h` sits
under PRIV_INCLUDE_DIRS. ESP-IDF's VFS cannot route around it either, having no
chmod or attribute concept in `esp_vfs_fs_ops_t`.

So espix would have no way to put a file's mode on the file. Three of the four
additions fix that, modelled directly on the port's own mtime helpers
(esp_littlefs_update_mtime_attr / esp_littlefs_get_mtime_attr), taking the same
lock and the same label lookup.

The fourth is esp_littlefs_mount(). esp_vfs_littlefs_register() both mounts the
filesystem and registers it at a base path, and espix needs the first without
the second: it registers its *own* VFS as the root -- which is where a
permission check belongs, the same place Linux and NuttX put theirs -- and calls
this driver through the returned ops. Were littlefs also published at a path,
that path would address the filesystem with espix's checks bypassed.

This is the least clean option available, and it is chosen deliberately over the
alternatives rather than for want of them: the component is on the registry and
has no fork exposing attributes, vendoring it would mean owning a 92KB file, and
a git-source dependency would mean maintaining a public fork. Patching what the
component manager downloaded keeps `managed_components/` untracked and the
manifest honest, at the cost of mutating a tree the build system considers
read-only.

The exit is `tools/esp_littlefs-attrs.patch`, the same change as a git patch
ready to send upstream. When it lands, delete this script, that patch, and the
execute_process() hook in CMakeLists.txt, and bump the version.

Failure policy
--------------
Loud, never silent. A version this was not written against, or an anchor that
moved, stops the build with a message saying so -- because skipping quietly
would surface as an undefined-reference error pointing at espix rather than at
the real cause.
"""

import re
import sys
from pathlib import Path

EXPECTED_VERSION = "1.22.3"
COMPONENT = "joltwallet__littlefs"
# Each insertion is guarded by a symbol unique to *it*, not one flag per file.
# A single shared marker breaks the moment one file takes two insertions: the
# second sees the first's symbol and skips, silently, which is exactly what it
# did on the first clean-fetch test of the mount addition.
MARK_INCLUDE = "esp_vfs_ops.h"
MARK_ATTRS   = "esp_littlefs_setattr"
# Not "esp_littlefs_mount": that is a substring of the port's pre-existing
# esp_littlefs_mounted(), so it matches before anything has been added and the
# insertion skips itself. The open paren is what distinguishes them.
MARK_MOUNT   = "esp_littlefs_mount("

HEADER_ANCHOR = '#ifdef __cplusplus\n} // extern "C"\n#endif'

# The mount-only entry point names esp_vfs_fs_ops_t, so the public header has to
# be able to see it. Added by its own anchor rather than inside the block below,
# because an #include nested in `extern "C" {` is the kind of thing that works
# until someone compiles the header from C++.
INCLUDE_ANCHOR = '#include "esp_partition.h"'

INCLUDE_ADDITION = '''#include "esp_partition.h"
#include "esp_vfs_ops.h"   /* for esp_littlefs_mount()'s esp_vfs_fs_ops_t */'''

SOURCE_ANCHOR = (
    "esp_err_t esp_littlefs_info(const char* partition_label, "
    "size_t *total_bytes, size_t *used_bytes){"
)

# Anchored on the definition (brace, not semicolon) so it does not match the
# header. Must come after `static esp_vfs_fs_ops_t s_vfs_littlefs` is defined,
# which this does; esp_littlefs_init() is forward-declared at the top.
MOUNT_ANCHOR = ("esp_err_t esp_vfs_littlefs_register"
                "(const esp_vfs_littlefs_conf_t * conf)\n{")

MOUNT_ADDITION = '''esp_err_t esp_littlefs_mount(const esp_vfs_littlefs_conf_t *conf,
                             const esp_vfs_fs_ops_t **out_ops, void **out_ctx)
{
    int index;

    if (conf == NULL || out_ops == NULL || out_ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = esp_littlefs_init(conf, &index);
    if (err != ESP_OK) {
        ESP_LOGE(ESP_LITTLEFS_TAG, "Failed to initialize LittleFS");
        return err;
    }

    /* Recorded for F_GETPATH, not registered. Unlike
     * esp_vfs_littlefs_register() this tolerates a NULL base path, there being
     * no base path to register. */
    if (conf->base_path != NULL) {
        strlcat(_efs[index]->base_path, conf->base_path, ESP_VFS_PATH_MAX + 1);
    }

    *out_ops = &s_vfs_littlefs;
    *out_ctx = _efs[index];
    return ESP_OK;
}

'''

HEADER_ADDITION = '''
/**
 * @brief Set a LittleFS custom (user) attribute on a file or directory.
 *
 * Attributes are identified by an 8-bit type and are stored in the entry's
 * metadata, which means LittleFS moves them with the file on rename and drops
 * them with it on delete. Type 't' is used internally for mtime.
 *
 * @param partition_label  Label of the mounted partition, or NULL for the first
 *                         partition with subtype "littlefs".
 * @param path             Path relative to the mount point.
 * @param type             Attribute type, 0x00-0xff.
 * @param buf              Attribute value.
 * @param size             Bytes of `buf`, at most LFS_ATTR_MAX.
 *
 * @return ESP_OK, ESP_ERR_NOT_FOUND if the partition is not mounted, or
 *         ESP_FAIL if LittleFS rejected the write.
 */
esp_err_t esp_littlefs_setattr(const char *partition_label, const char *path,
                               uint8_t type, const void *buf, size_t size);

/**
 * @brief Read a LittleFS custom (user) attribute.
 *
 * A stored attribute shorter than `size` is zero-padded; one longer is
 * truncated. `out_size`, when non-NULL, receives the size actually stored,
 * which may exceed `size`.
 *
 * @return ESP_OK, ESP_ERR_NOT_FOUND if the partition is not mounted or the
 *         attribute does not exist, or ESP_FAIL on any other error.
 */
esp_err_t esp_littlefs_getattr(const char *partition_label, const char *path,
                               uint8_t type, void *buf, size_t size,
                               size_t *out_size);

/**
 * @brief Remove a LittleFS custom (user) attribute. Absent is not an error.
 */
esp_err_t esp_littlefs_removeattr(const char *partition_label, const char *path,
                                  uint8_t type);

/**
 * @brief Mount a LittleFS partition without publishing it in the VFS.
 *
 * esp_vfs_littlefs_register() does two separable things: it mounts the
 * filesystem, and it registers the driver at a base path so that paths route to
 * it. This does only the first, and hands back the driver so a caller can
 * invoke it directly.
 *
 * That is what a stackable filesystem needs. A VFS layered on top of this one
 * -- adding permission checks, a union mount, an overlay -- has to be the only
 * name in the namespace, or the layer beneath it stays addressable and the
 * layering can be bypassed by spelling the lower path. Holding the ops and
 * context instead of routing through a second base path avoids that entirely.
 *
 * `conf->base_path` may be NULL or "". It is stored for the port's own
 * F_GETPATH handling and is not registered anywhere.
 *
 * @param conf      Mount configuration, as for esp_vfs_littlefs_register().
 * @param out_ops   Receives the driver's operations table.
 * @param out_ctx   Receives the context to pass as those operations' first
 *                  argument.
 *
 * @return ESP_OK, or the error esp_vfs_littlefs_register() would have given.
 */
esp_err_t esp_littlefs_mount(const esp_vfs_littlefs_conf_t *conf,
                             const esp_vfs_fs_ops_t **out_ops, void **out_ctx);

'''

SOURCE_ADDITION = '''
esp_err_t esp_littlefs_setattr(const char *partition_label, const char *path,
                               uint8_t type, const void *buf, size_t size)
{
    int index;
    esp_err_t err = esp_littlefs_by_label(partition_label, &index);
    if (err != ESP_OK) return err;

    esp_littlefs_t *efs = _efs[index];
    sem_take(efs);
    int res = lfs_setattr(efs->fs, path, type, buf, size);
    sem_give(efs);

    if (res < 0) {
        errno = lfs_errno_remap(res);
        ESP_LOGV(ESP_LITTLEFS_TAG, "Failed to set attr %u on \\"%s\\" (%d)",
                 (unsigned)type, path, res);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t esp_littlefs_getattr(const char *partition_label, const char *path,
                               uint8_t type, void *buf, size_t size,
                               size_t *out_size)
{
    int index;
    esp_err_t err = esp_littlefs_by_label(partition_label, &index);
    if (err != ESP_OK) return err;

    esp_littlefs_t *efs = _efs[index];
    sem_take(efs);
    lfs_ssize_t res = lfs_getattr(efs->fs, path, type, buf, size);
    sem_give(efs);

    if (res < 0) {
        if (res != LFS_ERR_NOATTR) {
            errno = lfs_errno_remap(res);
        }
        return (res == LFS_ERR_NOATTR) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    if (out_size) *out_size = (size_t)res;
    return ESP_OK;
}

esp_err_t esp_littlefs_removeattr(const char *partition_label, const char *path,
                                  uint8_t type)
{
    int index;
    esp_err_t err = esp_littlefs_by_label(partition_label, &index);
    if (err != ESP_OK) return err;

    esp_littlefs_t *efs = _efs[index];
    sem_take(efs);
    int res = lfs_removeattr(efs->fs, path, type);
    sem_give(efs);

    if (res < 0 && res != LFS_ERR_NOATTR) {
        errno = lfs_errno_remap(res);
        return ESP_FAIL;
    }
    return ESP_OK;
}

'''


def die(msg):
    print(f"patch-littlefs: {msg}", file=sys.stderr)
    sys.exit(1)


def check_version(root):
    """Refuse to patch a version this was not written against.

    A dependency bump should be a deliberate act: the anchors below are the
    port's own source, and there is no promise they survive a release.
    """
    lock = root / "dependencies.lock"
    if not lock.exists():
        return  # first configure, before the manager has written one
    text = lock.read_text(encoding="utf-8")

    # Anchored to the component's own indentation. A looser `\s+version:` finds
    # the nested `- name: idf / version: '>=5.0'` requirement first and reports
    # the wrong version -- which it did, on the first run of this script.
    m = re.search(r"^  joltwallet/littlefs:\n(?:.*?\n)*?^    version: (\S+)$",
                  text, re.M)
    if m and m.group(1).strip("'\"") != EXPECTED_VERSION:
        die(
            f"joltwallet/littlefs is {m.group(1)}, but this patch was written "
            f"for {EXPECTED_VERSION}.\n"
            f"  Re-check the anchors in tools/patch-littlefs.py against the new "
            f"source, then update EXPECTED_VERSION.\n"
            f"  If the version now provides esp_littlefs_setattr itself, delete "
            f"this script and the hook in CMakeLists.txt."
        )


def insert_after(path, anchor, addition, what, marker, replacement=None):
    """Put `addition` in front of `anchor`, or swap the anchor for `replacement`.

    Named for what it does in the common case. The replacement form exists for
    the one addition that has to go *at* a line rather than before a block --
    adding an #include next to an existing one.
    """
    text = path.read_text(encoding="utf-8")

    if marker in text:
        return False  # this insertion is already in; configure runs every build

    n = text.count(anchor)
    if n != 1:
        die(
            f"{what}: expected exactly one anchor, found {n}.\n"
            f"  file:   {path}\n"
            f"  anchor: {anchor.splitlines()[0]!r}\n"
            f"  The upstream source moved. Re-derive the anchor rather than "
            f"loosening this check."
        )

    new = replacement if replacement is not None else addition + anchor
    path.write_text(text.replace(anchor, new, 1), encoding="utf-8")
    return True


def main():
    root = Path(__file__).resolve().parent.parent
    comp = root / "managed_components" / COMPONENT

    if not comp.is_dir():
        # Nothing downloaded yet. Not an error: CMake reconfigures after the
        # component manager runs, and this will be called again.
        return 0

    check_version(root)

    header = comp / "include" / "esp_littlefs.h"
    source = comp / "src" / "esp_littlefs.c"
    for p in (header, source):
        if not p.is_file():
            die(f"expected {p} to exist")

    done = [
        insert_after(header, INCLUDE_ANCHOR, "", "header include",
                     MARK_INCLUDE, replacement=INCLUDE_ADDITION),
        insert_after(header, HEADER_ANCHOR, HEADER_ADDITION, "header",
                     MARK_ATTRS),
        insert_after(source, SOURCE_ANCHOR, SOURCE_ADDITION, "source attrs",
                     MARK_ATTRS),
        insert_after(source, MOUNT_ANCHOR, MOUNT_ADDITION, "source mount",
                     MARK_MOUNT),
    ]

    if any(done):
        print("patch-littlefs: added the custom-attribute API to "
              f"{COMPONENT} {EXPECTED_VERSION}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
