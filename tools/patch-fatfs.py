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

exFAT, and the 64-bit LBAs it needs
-----------------------------------
The rest of what this patches is the exFAT feature, and every edit is gated on
CONFIG_ESPIX_FS_EXFAT so that with the option off each file is byte for byte what
shipped:

  * ffconf.h's FF_FS_EXFAT is hardcoded 0 with no Kconfig for it, so the line is
    patched to follow the espix symbol instead.
  * FF_LBA64 is hardcoded 0 beside it and is not optional: ff.c:3545 refuses any
    volume whose last LBA does not fit in 32 bits, which at a 512-byte sector is
    2TiB -- smaller than the partition exFAT is wanted for.
  * ffconf.h's bare FF_USE_LABEL reaches C rather than #if in a block that only
    exFAT compiles, so the symbol it names needs a fallback. See docs/UPSTREAM.md.
  * diskio_impl.h's read/write function pointers take uint32_t, and ff_disk_read()
    hands them an LBA_t. That truncation is implicit at the call site and silent
    -- sector 0x100000000 reads somewhere else on the medium -- so the struct's
    sector type is DISKIO_SECT (LBA_t under FF_LBA64, DWORD without it) and every
    backend that assigns into the struct is declared with the same, which turns
    the mismatch into a build error.
  * diskio_bdl.c and the three backends beside it widen with it. diskio_bdl.c is
    the one espix registers, so its sector parameters and GET_SECTOR_COUNT buffer
    go all the way to 64 bits; the others narrow at sdmmc_read_sectors(), wl_read()
    and esp_partition_read(), which take 32 bits themselves.

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


def insert_after(text: str, anchor: str, addition: str, marker: str,
                 label: str) -> str:
    """For an anchor whose line the inserted block itself contains.

    insert_before() writes `addition + anchor`, so if `addition` ends with the
    anchor's own text the two are indistinguishable on the next run -- the
    marker is in the text either way, so it reports "already patched" having
    already inserted the block twice. Writing after the anchor instead leaves
    the anchor unique.
    """
    if marker in text:
        return text
    if anchor not in text:
        raise AnchorMissing(f"{label}: anchor not found")
    return text.replace(anchor, anchor + addition, 1)


def sub_once(text: str, pattern: "re.Pattern", new: str, marker: str,
             label: str) -> str:
    """replace_once() for a line whose alignment is not known in advance.

    A single wrong tab count makes a literal match into a silent no-op, so for
    these the shape of the line is matched instead.
    """
    if marker in text:
        return text
    matches = pattern.findall(text)
    if len(matches) != 1:
        raise AnchorMissing(f"{label}: expected exactly one match, found "
                            f"{len(matches)}")
    return pattern.sub(lambda _: new, text, count=1)


def replace_once(text: str, old: str, new: str, marker: str,
                 label: str) -> str:
    if marker in text:
        return text
    if text.count(old) != 1:
        raise AnchorMissing(f"{label}: expected exactly one match, found "
                            f"{text.count(old)}")
    return text.replace(old, new, 1)


# exFAT is off in ESP-IDF and there is no Kconfig for it: ffconf.h hard-codes
# it to 0, unlike the options around it which read CONFIG_FATFS_*. So the line
# is patched to follow an espix symbol instead, which makes the feature a
# build choice. Another project building against this IDF sees no
# CONFIG_ESPIX_FS_EXFAT at all, and an unknown identifier in #if is 0 -- so it
# gets today's behaviour.
FFCONF_OLD = "#define FF_FS_EXFAT" + (chr(9) * 2) + "0"
FFCONF_NEW = chr(10).join([
    "/*",
    " * espix: an #ifdef, not a bare symbol: sdkconfig.h defines only what is",
    " * set to y, so CONFIG_ESPIX_FS_EXFAT is an undeclared identifier when the",
    " * option is off -- and ffconf.h values reach C code, not only #if.",
    " */",
    "#ifdef CONFIG_ESPIX_FS_EXFAT",
    "#define FF_FS_EXFAT" + (chr(9) * 2) + "1",
    "#else",
    "#define FF_FS_EXFAT" + (chr(9) * 2) + "0",
    "#endif",
])

FFCONF_LABEL_BLOCK = chr(10).join([
    "/*",
    " * espix: sdkconfig.h defines only what is set to y, so a bare",
    " * CONFIG_FATFS_USE_LABEL is an undeclared identifier when the option is n.",
    " * That is normally harmless because ff.c only uses FF_USE_LABEL inside",
    " * #if -- but the block at ff.c:2357 tests it in C, and enabling exFAT is",
    " * what compiles that block. Reported upstream; see docs/UPSTREAM.md.",
    " */",
    "#ifndef CONFIG_FATFS_USE_LABEL",
    "#define CONFIG_FATFS_USE_LABEL" + (chr(9) * 1) + "0",
    "#endif",
]) + chr(10)
MARK_FFCONF_LABEL = " * what compiles that block."

# The readdir failure, at ERROR level.
#
# IDF logs this FRESULT at DEBUG (vfs_fat.c:1155), and nothing turns DEBUG on
# for that tag in an espix build -- so the one number that separates the two
# possible causes never reaches the ring. It matters because IDF maps *both* to
# EIO (vfs_fat.c:420-421):
#
#   FR_DISK_ERR  a read or write failed -- diskio_bdl.c would have logged it too
#   FR_INT_ERR   a read succeeded and returned data FatFs could not parse, which
#                on exFAT is usually the entry-set checksum (ff.c:2190)
#
# The offset is added for the same reason: it says how far into the directory the
# walk had got, which is where a bad sector would sit.
VFS_READDIR_OLD = "\n".join([
    "    FRESULT res = f_readdir(&fat_dir->ffdir, &fat_dir->filinfo);",
    "    if (res != FR_OK) {",
    "        *out_dirent = NULL;",
    '        ESP_LOGD(TAG, "%s: fresult=%d", __func__, res);',
    "        return fresult_to_errno(res);",
    "    }",
])
VFS_READDIR_NEW = "\n".join([
    "    FRESULT res = f_readdir(&fat_dir->ffdir, &fat_dir->filinfo);",
    "    if (res != FR_OK) {",
    "        *out_dirent = NULL;",
    "        /* espix: error level, and the offset -- see tools/patch-fatfs.py.",
    "         * FR_DISK_ERR and FR_INT_ERR both reach errno as EIO, so the number",
    "         * is the only thing that says whether a read failed or one returned",
    "         * data FatFs could not parse. */",
    '        ESP_LOGE(TAG, "%s: fresult=%d at entry offset %lu", __func__, res,',
    "                 (unsigned long)fat_dir->offset);",
    "        return fresult_to_errno(res);",
    "    }",
])
MARK_VFS_READDIR = "espix: error level, and the offset"

# FR_INT_ERR and FR_DISK_ERR share an errno, and they do not mean the same thing.
#
#   FR_DISK_ERR  a read or write failed
#   FR_INT_ERR   a read succeeded and returned data FatFs could not parse -- on
#                exFAT, usually the entry-set checksum at ff.c:2190
#
# IDF maps both to EIO (vfs_fat.c:420-421), so nothing above it can tell "a read
# failed" from "a read lied", and a truncated directory looks exactly like a
# small one. EBADMSG is the errno for "the data is malformed": an app, or a
# person reading `ls`, learns that the medium answered and the answer did not
# make sense, which is a different problem with a different next step.
FRESULT_ERRNO_OLD = "        case FR_INT_ERR:        return EIO;"
FRESULT_ERRNO_NEW = chr(10).join([
    "        /* espix: EBADMSG, not EIO -- see the note in tools/patch-fatfs.py.",
    "         * FR_DISK_ERR is the failed read; this is one that succeeded and",
    "         * returned data FatFs could not parse. */",
    "        case FR_INT_ERR:        return EBADMSG;",
])
MARK_FRESULT_ERRNO = "case FR_INT_ERR:        return EBADMSG;"

# ff_memalloc(), which every FatFs allocation comes from: the filesystem object,
# the LFN buffer, a file's own buffer, and -- the one that matters -- fs->win, the
# sector window every read lands in (ff.c:3517).
#
# A read DMAs into fs->win, and the USB host invalidates the cache over the
# transfer buffer when the transfer completes. esp_cache_msync() may only be given
# a region whose address *and* size both meet the cache alignment -- "cache memory
# synchronization to an unaligned address region may silently corrupt the memory"
# -- and malloc() gives 8-byte alignment. So the first and last cache lines of
# every sector read were corrupted, silently, with the transfer reporting success.
# That is what truncated a 452-entry directory at 240 entries, and what put
# nonsense in the last four GPT entries of the same drive: the corruption is
# always at the end of a read, and only while the cache still holds other bytes
# for those lines, which is the first accesses after a mount.
#
# 64 rather than the 32 this build's cache line is: the line size is a build
# option (GOTCHAS.md) and this is a compile-time constant, and over-aligning is
# free. MALLOC_CAP_DMA as well as MALLOC_CAP_8BIT because these buffers are DMA
# targets, and saying so is cheaper than rediscovering it.
FFSYSTEM_INCLUDE_OLD = "#include <stdlib.h>\t\t/* with POSIX API */"
FFSYSTEM_INCLUDE_NEW = chr(10).join([
    "#include <stdlib.h>\t\t/* with POSIX API */",
    '#include "esp_heap_caps.h"',
])
FFSYSTEM_ALLOC_OLD = "\treturn malloc((size_t)msize);\t/* Allocate a new memory block */"
FFSYSTEM_ALLOC_NEW = chr(10).join([
    "\t/* espix: cache-aligned -- this is what fs->win comes from, and a read",
    "\t * DMAs into it. See the note in tools/patch-fatfs.py. */",
    "\treturn heap_caps_aligned_alloc(64, (size_t)msize,",
    "\t                               MALLOC_CAP_8BIT | MALLOC_CAP_DMA);",
])
MARK_FFSYSTEM = "espix: cache-aligned -- this is what fs->win"
MARK_FFSYSTEM_INCLUDE = '#include "esp_heap_caps.h"'

MARK_FFCONF = "CONFIG_ESPIX_FS_EXFAT"

# FF_LBA64 is a second hardcoded 0 in the same file, and exFAT does not work
# without it above 2TiB: ff.c refuses a volume whose last LBA does not fit in 32
# bits (ff.c:3545, FR_NO_FILESYSTEM), which is every exFAT volume large enough to
# want exFAT. It follows the same symbol as FF_FS_EXFAT -- ffconf.h itself
# documents the dependency ("To enable 64-bit LBA, also exFAT needs to be
# enabled") -- so one option turns both on.
#
# One line, matched on its shape rather than its alignment: the value column of
# this file is tabs whose count differs per option (FF_FS_EXFAT uses two, some
# use one), and hardcoding the wrong count is a silent no-op. The pattern is
# anchored to end-of-line so it cannot catch FF_LBA64's neighbours.
FFCONF_LBA64_RE = re.compile(r"^#define FF_LBA64[ \t]+0[ \t]*$", re.MULTILINE)
FFCONF_LBA64_NEW = chr(10).join([
    "/*",
    " * espix: the same #ifdef shape as FF_FS_EXFAT below, and for the same",
    " * reason. An exFAT volume past 2TiB cannot be addressed with 32-bit LBAs",
    " * at all -- ff.c:3545 rejects it outright.",
    " */",
    "#ifdef CONFIG_ESPIX_FS_EXFAT",
    "#define FF_LBA64" + (chr(9) * 2) + "1",
    "#else",
    "#define FF_LBA64" + (chr(9) * 2) + "0",
    "#endif",
])
MARK_FFCONF_LBA64 = "CONFIG_ESPIX_FS_EXFAT" + chr(10) + "#define FF_LBA64"

# The last 32-bit field left in FatFs's read path, and the one FF_LBA64 alone
# does not reach. ff.c computes 64-bit LBAs and hands them to ff_disk_read(),
# which dispatches through ff_diskio_impl_t -- whose function pointers take
# uint32_t. The truncation is implicit, at the call site, and silent: sector
# 0x100000000 becomes 0 and reads somewhere else on the medium entirely.
#
# Guarded on FF_LBA64 so that with the option off this is a no-op, byte for
# byte: LBA_t is a DWORD then, and every backend's DWORD signature still
# matches. With it on, only diskio_bdl.c has to widen too -- it is the one
# backend espix registers, and the esp_blockdev ops under it already take
# uint64_t byte addresses.
#
# diskio_sdmmc.c, diskio_wl.c and diskio_rawflash.c keep their 32-bit
# signatures: it is only diskio_bdl.c that espix ever registers, and widening
# them would mean chasing 32-bit APIs (wl_read, sdmmc_read_sectors,
# esp_partition_read) that cannot take a 64-bit LBA anyway.
DISKIO_IMPL_OLD = "\n".join([
    "    DRESULT (*read) (unsigned char pdrv, unsigned char* buff, uint32_t sector, UINT count);  /*!< sector read function */",
    "    DRESULT (*write) (unsigned char pdrv, const unsigned char* buff, uint32_t sector, UINT count);   /*!< sector write function */",
])
DISKIO_IMPL_NEW = "\n".join([
    "    DRESULT (*read) (unsigned char pdrv, unsigned char* buff, DISKIO_SECT sector, UINT count);  /*!< sector read function */",
    "    DRESULT (*write) (unsigned char pdrv, const unsigned char* buff, DISKIO_SECT sector, UINT count);   /*!< sector write function */",
])
# Its own marker, distinct from the macro block's -- one shared marker would make
# the second edit look like a repeat of the first, which is the silence the
# markers exist to prevent.
MARK_DISKIO_IMPL = "buff, DISKIO_SECT sector, UINT count"
MARK_DISKIO_SECT = "espix: the sector type the interface is written with"

# The line the DISKIO_SECT block is inserted after, in diskio_impl.h. Chosen
# because the block's own text does not contain it, so the marker can be unique
# to the block.
DISKIO_SECT_ANCHOR = "#define FF_DRV_NOT_USED 0xFF\n"

# One definition of that type for every backend, because all five assign into the
# struct above and -Wincompatible-pointer-types refuses a mismatch. It is only
# diskio_bdl.c that espix registers, but the other four are compiled into the
# same component and have to agree with the struct all the same.
#
# A backend whose underlying API takes 32 bits -- sdmmc_read_sectors(),
# wl_read(), esp_partition_read() -- still hands DISKIO_SECT to it and narrows
# there. That is that API's limit and is unchanged from before; what this fixes
# is the interface FatFs itself calls through.
DISKIO_SECT_BLOCK = "\n".join([
    "/*",
    " * espix: the sector type the interface is written with. FF_LBA64 widens",
    " * LBA_t to a QWORD while these signatures were written for a DWORD, and a",
    " * driver declared with the narrower one does not fail to build by itself --",
    " * the mismatch only shows in the struct above, and only then if the compiler",
    " * is asked to look. FatFs passes a 64-bit LBA through that field, so the",
    " * narrow type truncates it silently: sector 0x100000000 reads somewhere else",
    " * on the medium entirely.",
    " */",
    "#if FF_LBA64",
    "#define DISKIO_SECT LBA_t",
    "#else",
    "#define DISKIO_SECT DWORD",
    "#endif",
    "",
])

DISKIO_BDL_HEADER_OLD = '#include "esp_compiler.h"\n'


# insert_before() would put this block ahead of the anchor, and the anchor line
# is repeated inside it -- so the next run would find the anchor's own copy
# first, mistake the insertion for the anchor, and insert a second block. The
# marker test cannot help: the same line is inside the block either way. So this
# one inserts *after* the anchor, where the following line is `static const char
# *TAG` and nothing repeats.
DISKIO_BDL_HEADER_NEW = '''
/*
 * espix 64-bit sector patch. The sector type is DISKIO_SECT, from
 * diskio_impl.h: FF_LBA64 widens LBA_t to a QWORD and the three functions below
 * pass it straight through to the esp_blockdev ops, which already take uint64_t
 * byte addresses as far down as msc_bdl_read()'s READ(16). Left at DWORD these
 * wrap around and read or write the wrong sector, with no error anywhere.
 */
'''
MARK_DISKIO_BDL_HEADER = "espix 64-bit sector patch"

DISKIO_BDL_EDITS = [
    # (old, new, marker, label)
    (
        "static DRESULT ff_bdl_read(BYTE pdrv, BYTE *buff, DWORD sector, UINT count)\n",
        "static DRESULT ff_bdl_read(BYTE pdrv, BYTE *buff, DISKIO_SECT sector, UINT count)\n",
        "ff_bdl_read(BYTE pdrv, BYTE *buff, DISKIO_SECT",
        "diskio_bdl.c read",
    ),
    (
        "static DRESULT ff_bdl_write(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count)\n",
        "static DRESULT ff_bdl_write(BYTE pdrv, const BYTE *buff, DISKIO_SECT sector, UINT count)\n",
        "ff_bdl_write(BYTE pdrv, const BYTE *buff, DISKIO_SECT",
        "diskio_bdl.c write",
    ),
    # The ioctl buffer is `LBA_t sz_drv` at ff.c:5954, so a 32-bit write here
    # leaves half of it uninitialised.
    (
        "        *((DWORD *)buff) = (DWORD)(drv->handle->geometry.disk_size / drv->fs_sector_size);\n",
        "        *((DISKIO_SECT *)buff) = (DISKIO_SECT)(drv->handle->geometry.disk_size / drv->fs_sector_size);\n",
        "*((DISKIO_SECT *)buff) = (DISKIO_SECT)(drv->handle->geometry.disk_size",
        "diskio_bdl.c sector count",
    ),
    # The CTRL_TRIM reads, named in full so the marker cannot match anything but
    # these two lines (a bare "DWORD start_sector" would also hit
    # diskio_sdmmc.c's, which is deliberately left alone).
    (
        "        DWORD start_sector = *((DWORD *)buff);\n"
        "        DWORD end_sector = *((DWORD *)buff + 1);\n",
        "        DISKIO_SECT start_sector = *((DISKIO_SECT *)buff);\n"
        "        DISKIO_SECT end_sector = *((DISKIO_SECT *)buff + 1);\n",
        "DISKIO_SECT start_sector = *((DISKIO_SECT *)buff)",
        "diskio_bdl.c trim",
    ),
    # A failed read is named with the byte address it was for. This is a
    # diagnostic, and it earns its place: a sector read that fails on the 3.1TB
    # T9 about one run in ten appears at the filesystem as an indistinguishable
    # `errno`, and where that address falls -- above the 2TiB mark, so the
    # widened 64-bit path, or below it, alongside the unexplained GPT-array
    # quirk the same drive already showed -- is the measurement that says which
    # layer to look at. Only a *failure* logs, so a working read costs one
    # compare.
    (
        "    esp_err_t err = drv->handle->ops->read(drv->handle, buff, count * sec_size,\n"
        "                                           (uint64_t)sector * sec_size, count * sec_size);\n"
        "    if (unlikely(err != ESP_OK)) {\n"
        "        ESP_LOGE(TAG, \"BDL read failed (0x%x)\", err);\n"
        "        return RES_ERROR;\n"
        "    }\n",
        "    esp_err_t err = drv->handle->ops->read(drv->handle, buff, count * sec_size,\n"
        "                                           (uint64_t)sector * sec_size, count * sec_size);\n"
        "    if (unlikely(err != ESP_OK)) {\n"
        "        /* espix: the address, not just the failure -- see the note in\n"
        "         * tools/patch-fatfs.py. Addr is absolute on the device this view\n"
        "         * was made from, so it compares directly with the disk's size. */\n"
        "        ESP_LOGE(TAG, \"BDL read failed (0x%x) at addr %llu, %u sector(s) of %u\",\n"
        "                 err, (unsigned long long)((uint64_t)sector * sec_size),\n"
        "                 (unsigned)count, (unsigned)sec_size);\n"
        "        return RES_ERROR;\n"
        "    }\n",
        "espix: the address, not just the failure",
        "diskio_bdl.c read diagnostic",
    ),
]

# The other three backends compiled into this component. All of them assign into
# ff_diskio_impl_t, so all of them have to be declared with DISKIO_SECT for it to
# compile -- but only their read/write signatures and the GET_SECTOR_COUNT buffer
# are touched: the sector argument still narrows at sdmmc_read_sectors(),
# wl_read() and esp_partition_read(), which is those APIs' own limit.
#
# Diskio sdmmc's CTRL_TRIM deliberately keeps its DWORD reads: they feed
# ff_sdmmc_trim(), which calls sdmmc_erase_sectors(card, size_t, size_t, ...),
# so widening them would only turn a truncation into a narrowing that
# -Wconversion would like a word about.
DISKIO_OTHERS = [
    (
        "diskio_sdmmc.c",
        [
            ("static DRESULT ff_sdmmc_read (BYTE pdrv, BYTE* buff, DWORD sector, UINT count)",
             "static DRESULT ff_sdmmc_read (BYTE pdrv, BYTE* buff, DISKIO_SECT sector, UINT count)",
             "ff_sdmmc_read (BYTE pdrv, BYTE* buff, DISKIO_SECT", "read"),
            ("static DRESULT ff_sdmmc_write (BYTE pdrv, const BYTE* buff, DWORD sector, UINT count)",
             "static DRESULT ff_sdmmc_write (BYTE pdrv, const BYTE* buff, DISKIO_SECT sector, UINT count)",
             "ff_sdmmc_write (BYTE pdrv, const BYTE* buff, DISKIO_SECT", "write"),
            ("            *((DWORD*) buff) = card->csd.capacity;",
             "            *((DISKIO_SECT*) buff) = card->csd.capacity;",
             "*((DISKIO_SECT*) buff) = card->csd.capacity", "sector count"),
        ],
    ),
    (
        "diskio_wl.c",
        [
            ("static DRESULT ff_wl_read (BYTE pdrv, BYTE *buff, DWORD sector, UINT count)",
             "static DRESULT ff_wl_read (BYTE pdrv, BYTE *buff, DISKIO_SECT sector, UINT count)",
             "ff_wl_read (BYTE pdrv, BYTE *buff, DISKIO_SECT", "read"),
            ("static DRESULT ff_wl_write (BYTE pdrv, const BYTE *buff, DWORD sector, UINT count)",
             "static DRESULT ff_wl_write (BYTE pdrv, const BYTE *buff, DISKIO_SECT sector, UINT count)",
             "ff_wl_write (BYTE pdrv, const BYTE *buff, DISKIO_SECT", "write"),
            ("        *((DWORD *) buff) = wl_size(wl_handle) / wl_sector_size(wl_handle);",
             "        *((DISKIO_SECT *) buff) = wl_size(wl_handle) / wl_sector_size(wl_handle);",
             "*((DISKIO_SECT *) buff) = wl_size(wl_handle)", "sector count"),
        ],
    ),
    (
        "diskio_rawflash.c",
        [
            ("static DRESULT ff_raw_read (BYTE pdrv, BYTE *buff, DWORD sector, UINT count)",
             "static DRESULT ff_raw_read (BYTE pdrv, BYTE *buff, DISKIO_SECT sector, UINT count)",
             "ff_raw_read (BYTE pdrv, BYTE *buff, DISKIO_SECT", "read"),
            ("static DRESULT ff_raw_write (BYTE pdrv, const BYTE *buff, DWORD sector, UINT count)",
             "static DRESULT ff_raw_write (BYTE pdrv, const BYTE *buff, DISKIO_SECT sector, UINT count)",
             "ff_raw_write (BYTE pdrv, const BYTE *buff, DISKIO_SECT", "write"),
            ("            *((DWORD *) buff) = s_sectors_count[pdrv];",
             "            *((DISKIO_SECT *) buff) = s_sectors_count[pdrv];",
             "*((DISKIO_SECT *) buff) = s_sectors_count[pdrv]", "sector count"),
        ],
    ),
]

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
    ffconf = idf_path / "components" / "fatfs" / "src" / "ffconf.h"
    ffsystem = idf_path / "components" / "fatfs" / "src" / "ffsystem.c"
    diskio_i = idf_path / "components" / "fatfs" / "diskio" / "diskio_impl.h"
    diskio_bdl = idf_path / "components" / "fatfs" / "diskio" / "diskio_bdl.c"
    diskio_others = {}
    diskio_others_new = {}
    diskio_others_old = {}
    for fname, edits in DISKIO_OTHERS:
        path = idf_path / "components" / "fatfs" / "diskio" / fname
        diskio_others[path] = edits

    try:
        version = idf_version(idf_path)
        if version != EXPECTED_IDF:
            raise AnchorMissing(
                f"written against ESP-IDF {EXPECTED_IDF}, this tree is {version}")

        header_text = header.read_text()
        source_text = source.read_text()
        ffconf_text = ffconf.read_text()
        ffsystem_text = ffsystem.read_text()
        diskio_i_text = diskio_i.read_text()
        diskio_bdl_text = diskio_bdl.read_text()

        header_new = insert_before(header_text, HEADER_INCLUDE_ANCHOR,
                                   HEADER_INCLUDE_BLOCK, MARK_H_CTX,
                                   str(header.name) + " declarations")

        source_new = insert_before(source_text, C_OPS_ANCHOR, C_OPS_BLOCK,
                                   MARK_C_OPS, str(source.name) + " get_ops")
        source_new = replace_once(source_new, C_REGISTER_OLD, C_REGISTER_NEW,
                                  MARK_C_HELPER, str(source.name) + " register")
        source_new = replace_once(source_new, VFS_READDIR_OLD, VFS_READDIR_NEW,
                                  MARK_VFS_READDIR,
                                  str(source.name) + " readdir diagnostic")
        source_new = replace_once(source_new, FRESULT_ERRNO_OLD,
                                  FRESULT_ERRNO_NEW, MARK_FRESULT_ERRNO,
                                  str(source.name) + " FR_INT_ERR errno")

        ffconf_new = replace_once(ffconf_text, FFCONF_OLD, FFCONF_NEW,
                                  MARK_FFCONF, str(ffconf.name) + " exFAT")
        ffconf_new = sub_once(ffconf_new, FFCONF_LBA64_RE, FFCONF_LBA64_NEW,
                              MARK_FFCONF_LBA64, str(ffconf.name) + " LBA64")
        ffconf_new = insert_before(ffconf_new, "#define FF_USE_LABEL",
                                   FFCONF_LABEL_BLOCK, MARK_FFCONF_LABEL,
                                   str(ffconf.name) + " label symbol")

        ffsystem_new = replace_once(ffsystem_text, FFSYSTEM_INCLUDE_OLD,
                                    FFSYSTEM_INCLUDE_NEW,
                                    MARK_FFSYSTEM_INCLUDE,
                                    str(ffsystem.name) + " heap caps")
        ffsystem_new = replace_once(ffsystem_new, FFSYSTEM_ALLOC_OLD,
                                    FFSYSTEM_ALLOC_NEW, MARK_FFSYSTEM,
                                    str(ffsystem.name) + " cache-aligned")

        diskio_i_new = insert_after(diskio_i_text, DISKIO_SECT_ANCHOR,
                                    DISKIO_SECT_BLOCK, MARK_DISKIO_SECT,
                                    str(diskio_i.name) + " DISKIO_SECT")
        diskio_i_new = replace_once(diskio_i_new, DISKIO_IMPL_OLD,
                                    DISKIO_IMPL_NEW, MARK_DISKIO_IMPL,
                                    str(diskio_i.name) + " 64-bit sector")

        diskio_bdl_new = insert_after(diskio_bdl_text, DISKIO_BDL_HEADER_OLD,
                                      DISKIO_BDL_HEADER_NEW,
                                      MARK_DISKIO_BDL_HEADER,
                                      str(diskio_bdl.name) + " 64-bit sector")
        for old, new, marker, label in DISKIO_BDL_EDITS:
            diskio_bdl_new = replace_once(diskio_bdl_new, old, new, marker,
                                          str(diskio_bdl.name) + " " + label)

        for file, edits in diskio_others.items():
            old_text = file.read_text()
            text = old_text
            for old, new, marker, label in edits:
                text = replace_once(text, old, new, marker,
                                    str(file.name) + " " + label)
            diskio_others_new[file] = text
            diskio_others_old[file] = old_text

    except AnchorMissing as exc:
        print(f"patch-fatfs.py: {exc}", file=sys.stderr)
        print("patch-fatfs.py: the anchors moved or the IDF version changed. "
              "Update this script (and tools/esp_vfs_fat-ctx.patch) against the "
              "new source, or check docs/UPSTREAM.md in case the change landed "
              "upstream and all of this can go.", file=sys.stderr)
        return 1

    changed = []
    to_write = [(header, header_text, header_new),
                (source, source_text, source_new),
                (ffconf, ffconf_text, ffconf_new),
                (ffsystem, ffsystem_text, ffsystem_new),
                (diskio_i, diskio_i_text, diskio_i_new),
                (diskio_bdl, diskio_bdl_text, diskio_bdl_new)]
    to_write += [(path, diskio_others_old[path], diskio_others_new[path])
                 for path in diskio_others_new]
    for path, old, new in to_write:
        if new != old:
            path.write_text(new)
            changed.append(path)

    if changed:
        print("patch-fatfs.py: patched " + ", ".join(str(p) for p in changed))
    else:
        print("patch-fatfs.py: already patched")
    return 0


if __name__ == "__main__":
    sys.exit(main())