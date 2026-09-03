#!/usr/bin/env python3
"""
Add a public custom-attribute API to the downloaded joltwallet/littlefs.

Why this exists
---------------
LittleFS carries POSIX-style metadata in user attributes -- `lfs_setattr`,
`lfs_getattr`, `lfs_removeattr` are public API and the ESP port already uses one
('t') to store mtime. But the port never exposes the `lfs_t *` those need:
`_efs[]` is file-scope static, every lookup is static, and `littlefs_api.h` sits
under PRIV_INCLUDE_DIRS. ESP-IDF's VFS cannot route around it either, having no
chmod or attribute concept in `esp_vfs_fs_ops_t`.

So espix would have no way to put a file's mode on the file. This adds three
functions, modelled directly on the port's own mtime helpers
(esp_littlefs_update_mtime_attr / esp_littlefs_get_mtime_attr), taking the same
lock and the same label lookup.

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
MARKER = "esp_littlefs_setattr"

HEADER_ANCHOR = '#ifdef __cplusplus\n} // extern "C"\n#endif'

SOURCE_ANCHOR = (
    "esp_err_t esp_littlefs_info(const char* partition_label, "
    "size_t *total_bytes, size_t *used_bytes){"
)

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


def insert_after(path, anchor, addition, what):
    text = path.read_text(encoding="utf-8")

    if MARKER in text:
        return False  # already patched; configure runs on every build

    n = text.count(anchor)
    if n != 1:
        die(
            f"{what}: expected exactly one anchor, found {n}.\n"
            f"  file:   {path}\n"
            f"  anchor: {anchor.splitlines()[0]!r}\n"
            f"  The upstream source moved. Re-derive the anchor rather than "
            f"loosening this check."
        )

    path.write_text(text.replace(anchor, addition + anchor, 1), encoding="utf-8")
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

    a = insert_after(header, HEADER_ANCHOR, HEADER_ADDITION, "header")
    b = insert_after(source, SOURCE_ANCHOR, SOURCE_ADDITION, "source")

    if a or b:
        print("patch-littlefs: added the custom-attribute API to "
              f"{COMPONENT} {EXPECTED_VERSION}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
