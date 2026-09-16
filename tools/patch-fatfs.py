#!/usr/bin/env python3
"""
Expose an ESP-IDF FatFs entry point that mounts without registering a path.

Why this exists
---------------
espix owns the namespace: it registers its own VFS as the fallback, and every
mounted filesystem sits *underneath* it, reached by a direct call to the driver's
ops rather than by a second name in the namespace. That is where the permission
check lives, and it is the whole reason struct lower_t in vfs.c holds an
esp_vfs_fs_ops_t rather than a base path -- see components/espix_fs/vfs.c.

FatFs cannot be mounted that way as shipped, and the three reasons are separate:

  * every public entry point that mounts a block device -- esp_vfs_fat_bdl_mount()
    among them -- ends in esp_vfs_register_fs(), so the filesystem is reachable at
    that prefix with the checks skipped;
  * the ops tables (s_vfs_fat, s_vfs_fat_dir) are file-scope static, so there is
    no way to reach the driver directly either;
  * the context those ops need is built *inside* esp_vfs_fat_register(), whose
    only other job is the registration, so the two cannot be separated from
    outside the file.

What it adds
------------
1. fat_ctx_create(): the context construction lifted out of
   esp_vfs_fat_register(), which now calls it -- one implementation, not a copy
   that could drift from it.
2. esp_vfs_fat_ctx_create(): builds that context and registers nothing.
3. esp_vfs_fat_ctx_free(): the reverse, freeing what esp_vfs_fat_unregister_path()
   frees and nothing else.
4. esp_vfs_fat_get_ops(): hands out the ops tables (their dir ops hang off them).
The declarations go into vfs/vfs_fat_internal.h, which sits in fatfs's public
include dirs, so espix reaches them without fatfs having to depend on vfs
publicly -- which is what declaring them in esp_vfs_fat.h would require, since
that header would then name esp_vfs_fs_ops_t.

Paths passed to those ops must already be relative to the mount, exactly as
esp_vfs would have made them: espix strips its mount prefix before calling. See
components/espix_fs/fat.c, which is the only caller.

Where this writes, and why there
--------------------------------
Not managed_components: fatfs is a *built-in* IDF component, and the registry
publishes no espressif/fatfs that could override it (checked -- it 404s), so this
patches the IDF installation in place. Two consequences. The change is additive:
no existing behaviour changes, and the refactor keeps esp_vfs_fat_register()
doing exactly what it did. And the CMake hook re-applies it on every build, so a
reinstalled or upgraded IDF is patched again rather than silently reverting --
which is the failure this would otherwise produce, since the symptom of an
unpatched tree is an undefined reference to esp_vfs_fat_ctx_create() pointing at
espix rather than at the real cause.

tools/esp_vfs_fat-ctx.patch is the same change as a git patch, ready to send
upstream. When it lands, delete this script, that patch, and the
execute_process() hook in CMakeLists.txt.

Failure policy
--------------
Loud, never silent. An IDF version this was not written against, or an anchor
that moved, stops the build with a message saying which one.
"""

import os
import re
import sys
from pathlib import Path

EXPECTED_IDF = "6.1.0"

# Each insertion is guarded by a symbol unique to *it*: one shared "patched"
# marker would make a second insertion look like a repeat of the first, which is
# a silence this script exists to avoid.
MARK_H_CTX = "esp_vfs_fat_ctx_create"
MARK_C_OPS = "esp_vfs_fat_get_ops"
MARK_C_CTX = "esp_vfs_fat_ctx_create"
MARK_C_FREE = "esp_vfs_fat_ctx_free"
MARK_C_HELPER = "fat_ctx_create"


class AnchorMissing(Exception):
    pass


def idf_version(idf_path: Path) -> str:
    version_cmake = idf_path / "tools" / "cmake" / "version.cmake"
    try:
        text = version_cmake.read_text()
    except OSError as exc:
        raise AnchorMissing(f"cannot read {version_cmake}: {exc}") from None
    parts = {}
    for name in ("MAJOR", "MINOR", "PATCH"):
        match = re.search(rf"IDF_VERSION_{name} (\d+)", text)
        if match is None:
            raise AnchorMissing(f"no IDF_VERSION_{name} in {version_cmake}")
        parts[name] = match.group(1)
    return ".".join(parts[n] for n in ("MAJOR", "MINOR", "PATCH"))


def insert_before(text: str, anchor: str, addition: str, marker: str,
                  label: str) -> str:
    if marker in text:
        return text
    if anchor not in text:
        raise AnchorMissing(f"{label}: anchor not found")
    return text.replace(anchor, addition + anchor, 1)


def replace_once(text: str, old: str, new: str, marker: str,
                 label: str) -> str:
    if marker in text:
        return text
    if text.count(old) != 1:
        raise AnchorMissing(f"{label}: expected exactly one match, found "
                            f"{text.count(old)}")
    return text.replace(old, new, 1)


HEADER_INCLUDE_ANCHOR = '#include <stddef.h>\n'
HEADER_INCLUDE_BLOCK = '''#include "esp_vfs_ops.h"   // espix: esp_vfs_fs_ops_t, for esp_vfs_fat_get_ops()

/*
 * espix additions (see tools/patch-fatfs.py): mount a FAT filesystem without
 * registering it at a base path, for a VFS that owns the namespace itself and
 * reaches this driver through the ops below.
 *
 * Declared here rather than in esp_vfs_fat.h because that header would then name
 * esp_vfs_fs_ops_t and have to depend on vfs publicly -- a bigger change than
 * this one, and one upstream can make when it takes these.
 *
 * Paths handed to those ops must be relative to the mount point already, the way
 * esp_vfs would have made them.
 */
esp_err_t esp_vfs_fat_ctx_create(const esp_vfs_fat_conf_t* conf, FATFS** out_fs,
                                 void** out_ctx);
esp_err_t esp_vfs_fat_ctx_free(void* ctx);
const esp_vfs_fs_ops_t* esp_vfs_fat_get_ops(void);

'''

HEADER_DECLS_ANCHOR = None
HEADER_DECLS_BLOCK = ''

C_OPS_ANCHOR = "esp_err_t esp_vfs_fat_register(const esp_vfs_fat_conf_t* conf, FATFS** out_fs)\n"
C_OPS_BLOCK = '''/*
 * espix: the ops tables themselves, so a caller can drive this driver without
 * going through esp_vfs. The dir ops hang off s_vfs_fat.
 */
const esp_vfs_fs_ops_t* esp_vfs_fat_get_ops(void)
{
    return &s_vfs_fat;
}

'''

C_REGISTER_OLD = '''esp_err_t esp_vfs_fat_register(const esp_vfs_fat_conf_t* conf, FATFS** out_fs)
{
    size_t ctx = find_context_index_by_path(conf->base_path);
    if (ctx < FF_VOLUMES) {
        if (out_fs) {
            *out_fs = &s_fat_ctxs[ctx]->fs;
        }
        return ESP_ERR_INVALID_STATE;
    }

    ctx = find_unused_context_index();
    if (ctx == FF_VOLUMES) {
        return ESP_ERR_NO_MEM;
    }

    size_t max_files = conf->max_files;
    if (max_files < 1) {
        max_files = 1;  // ff_memalloc(max_files * sizeof(bool)) below will fail if max_files == 0
    }

    size_t ctx_size = sizeof(vfs_fat_ctx_t) + max_files * sizeof(FIL);
    vfs_fat_ctx_t* fat_ctx = (vfs_fat_ctx_t*) ff_memalloc(ctx_size);
    if (fat_ctx == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(fat_ctx, 0, ctx_size);
#ifdef CONFIG_VFS_SUPPORT_DIR
    SLIST_INIT(&fat_ctx->open_dirs);
#endif
    fat_ctx->flags = ff_memalloc(max_files * sizeof(*fat_ctx->flags));
    if (fat_ctx->flags == NULL) {
        free(fat_ctx);
        return ESP_ERR_NO_MEM;
    }
    memset(fat_ctx->flags, 0, max_files * sizeof(*fat_ctx->flags));
    fat_ctx->max_files = max_files;
    strlcpy(fat_ctx->fat_drive, conf->fat_drive, sizeof(fat_ctx->fat_drive) - 1);
    strlcpy(fat_ctx->base_path, conf->base_path, sizeof(fat_ctx->base_path) - 1);

    esp_err_t err = esp_vfs_register_fs(conf->base_path, &s_vfs_fat, ESP_VFS_FLAG_CONTEXT_PTR | ESP_VFS_FLAG_STATIC, fat_ctx);
    if (err != ESP_OK) {
        free(fat_ctx->flags);
        free(fat_ctx);
        return err;
    }

    _lock_init(&fat_ctx->lock);
    s_fat_ctxs[ctx] = fat_ctx;

    //compatibility
    s_fat_ctx = fat_ctx;

    if (out_fs) {
        *out_fs = &fat_ctx->fs;
    }

    return ESP_OK;
}
'''

C_REGISTER_NEW = '''/*
 * espix: everything esp_vfs_fat_register() does that is not registration.
 *
 * Lifted out of it rather than copied, so a context built for a caller that
 * registers nothing is the same context: if this file's idea of one changes,
 * both paths change together. The lock is left to the callers, because a
 * registration that fails frees the context and must not close a lock it never
 * initialised.
 */
static esp_err_t fat_ctx_create(const esp_vfs_fat_conf_t* conf, vfs_fat_ctx_t** out_ctx)
{
    size_t max_files = conf->max_files;
    if (max_files < 1) {
        max_files = 1;  // ff_memalloc(max_files * sizeof(bool)) below will fail if max_files == 0
    }

    size_t ctx_size = sizeof(vfs_fat_ctx_t) + max_files * sizeof(FIL);
    vfs_fat_ctx_t* fat_ctx = (vfs_fat_ctx_t*) ff_memalloc(ctx_size);
    if (fat_ctx == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(fat_ctx, 0, ctx_size);
#ifdef CONFIG_VFS_SUPPORT_DIR
    SLIST_INIT(&fat_ctx->open_dirs);
#endif
    fat_ctx->flags = ff_memalloc(max_files * sizeof(*fat_ctx->flags));
    if (fat_ctx->flags == NULL) {
        free(fat_ctx);
        return ESP_ERR_NO_MEM;
    }
    memset(fat_ctx->flags, 0, max_files * sizeof(*fat_ctx->flags));
    fat_ctx->max_files = max_files;
    strlcpy(fat_ctx->fat_drive, conf->fat_drive, sizeof(fat_ctx->fat_drive) - 1);
    strlcpy(fat_ctx->base_path, conf->base_path, sizeof(fat_ctx->base_path) - 1);

    *out_ctx = fat_ctx;
    return ESP_OK;
}

/*
 * espix: a context with no VFS entry. The caller drives esp_vfs_fat_get_ops()
 * itself and frees this with esp_vfs_fat_ctx_free() when the volume is
 * unmounted. base_path is bookkeeping only: nothing here strips it.
 */
esp_err_t esp_vfs_fat_ctx_create(const esp_vfs_fat_conf_t* conf, FATFS** out_fs, void** out_ctx)
{
    if (conf == NULL || out_fs == NULL || out_ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    vfs_fat_ctx_t* fat_ctx = NULL;
    esp_err_t err = fat_ctx_create(conf, &fat_ctx);
    if (err != ESP_OK) {
        return err;
    }

    _lock_init(&fat_ctx->lock);
    *out_fs = &fat_ctx->fs;
    *out_ctx = fat_ctx;
    return ESP_OK;
}

/*
 * espix: the reverse of esp_vfs_fat_ctx_create(), freeing what
 * esp_vfs_fat_unregister_path() frees and nothing else. The volume is the
 * caller's to unmount first -- f_mount(NULL, drv, 0) -- because only it knows
 * the drive number.
 */
esp_err_t esp_vfs_fat_ctx_free(void* ctx)
{
    if (ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    vfs_fat_ctx_t* fat_ctx = (vfs_fat_ctx_t*) ctx;
    _lock_close(&fat_ctx->lock);
#if !FF_FS_TINY && FF_USE_DYN_BUFFER
    for (size_t i = 0; i < fat_ctx->max_files; ++i) {
        if (fat_ctx->files[i].buf != NULL) {
            ff_memfree(fat_ctx->files[i].buf);
        }
    }
#endif
    free(fat_ctx->flags);
    free(fat_ctx);
    return ESP_OK;
}

esp_err_t esp_vfs_fat_register(const esp_vfs_fat_conf_t* conf, FATFS** out_fs)
{
    size_t ctx = find_context_index_by_path(conf->base_path);
    if (ctx < FF_VOLUMES) {
        if (out_fs) {
            *out_fs = &s_fat_ctxs[ctx]->fs;
        }
        return ESP_ERR_INVALID_STATE;
    }

    ctx = find_unused_context_index();
    if (ctx == FF_VOLUMES) {
        return ESP_ERR_NO_MEM;
    }

    vfs_fat_ctx_t* fat_ctx = NULL;
    esp_err_t err = fat_ctx_create(conf, &fat_ctx);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_vfs_register_fs(conf->base_path, &s_vfs_fat, ESP_VFS_FLAG_CONTEXT_PTR | ESP_VFS_FLAG_STATIC, fat_ctx);
    if (err != ESP_OK) {
        free(fat_ctx->flags);
        free(fat_ctx);
        return err;
    }

    _lock_init(&fat_ctx->lock);
    s_fat_ctxs[ctx] = fat_ctx;

    //compatibility
    s_fat_ctx = fat_ctx;

    if (out_fs) {
        *out_fs = &fat_ctx->fs;
    }

    return ESP_OK;
}
'''


def main() -> int:
    args = sys.argv[1:]
    if args:
        if len(args) != 2 or args[0] != "--idf-path":
            print("usage: patch-fatfs.py [--idf-path PATH]", file=sys.stderr)
            return 2
        idf_path = Path(args[1])
    else:
        env = os.environ.get("IDF_PATH")
        if not env:
            print("patch-fatfs.py: no --idf-path and no IDF_PATH in the "
                  "environment", file=sys.stderr)
            return 2
        idf_path = Path(env)

    header = idf_path / "components" / "fatfs" / "vfs" / "vfs_fat_internal.h"
    source = idf_path / "components" / "fatfs" / "vfs" / "vfs_fat.c"

    try:
        version = idf_version(idf_path)
        if version != EXPECTED_IDF:
            raise AnchorMissing(
                f"written against ESP-IDF {EXPECTED_IDF}, this tree is {version}")

        header_text = header.read_text()
        source_text = source.read_text()

        header_new = insert_before(header_text, HEADER_INCLUDE_ANCHOR,
                                   HEADER_INCLUDE_BLOCK, MARK_H_CTX,
                                   str(header.name) + " declarations")

        source_new = insert_before(source_text, C_OPS_ANCHOR, C_OPS_BLOCK,
                                   MARK_C_OPS, str(source.name) + " get_ops")
        source_new = replace_once(source_new, C_REGISTER_OLD, C_REGISTER_NEW,
                                  MARK_C_HELPER, str(source.name) + " register")

    except AnchorMissing as exc:
        print(f"patch-fatfs.py: {exc}", file=sys.stderr)
        print("patch-fatfs.py: the anchors moved or the IDF version changed. "
              "Update this script (and tools/esp_vfs_fat-ctx.patch) against the "
              "new source, or check docs/UPSTREAM.md in case the change landed "
              "upstream and all of this can go.", file=sys.stderr)
        return 1

    changed = []
    if header_new != header_text:
        header.write_text(header_new)
        changed.append(header)
    if source_new != source_text:
        source.write_text(source_new)
        changed.append(source)

    if changed:
        print("patch-fatfs.py: patched " + ", ".join(str(p) for p in changed))
    else:
        print("patch-fatfs.py: already patched")
    return 0


if __name__ == "__main__":
    sys.exit(main())