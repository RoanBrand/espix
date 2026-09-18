#!/usr/bin/env python3
"""
Add READ CAPACITY(16), READ(16) and WRITE(16) to the downloaded
espressif/usb_host_msc, and use them.

Why this exists
---------------
The component speaks SCSI READ CAPACITY(10) (opcode 0x25) and READ10/WRITE10
(0x28/0x2A) only. Every one of those has a 32-bit LBA field, so a disk with
more than 2^32 sectors is unreadable past that point and READ CAPACITY(10)
itself saturates at 0xFFFFFFFF -- which is what espix measured on a real 3.6TB
Samsung T9: it reported exactly 2,199,023,255,040 bytes, 512 * 0xFFFFFFFF, the
field clamping rather than the disk's actual size. SBC-2 says what a SCSI
initiator is supposed to do when READ CAPACITY(10) returns that value: issue
READ CAPACITY(16) instead, which has a 64-bit field. FreeBSD's da(4) driver
follows exactly that rule (sys/cam/scsi/scsi_da.c).

This adds the three commands to msc_scsi_bot.c/.h, following the (10) versions
already there byte for byte -- same CBW_BASE_INIT, same bswap-into-CDB shape --
and switches msc_host_install_device() to try READ CAPACITY(16) first, falling
back to (10) for a device that does not support it (indicated by non-ESP_OK,
which covers both a STALL and a device that understood the opcode but rejected
it). msc_bdl.c's geometry.disk_size already carries a uint64_t and needed no
change; its read/write paths gain a 64-bit LBA check and call the (16) command
when the address needs more than 32 bits, since the great majority of reads on
any disk are within the first 2TiB and do not need the extra command overhead.

This is the same shape of problem as tools/patch-fatfs.py's FF_LBA64: a 32-bit
field somewhere in the read path silently truncates a large disk, and the fix
is additive.

Why here and not upstream first
--------------------------------
espressif/usb_host_msc is on the registry with no fork exposing (16), the
component is downloaded fresh by the component manager, and vendoring it would
mean owning several files this does not otherwise touch. Patching what the
manager downloaded keeps managed_components/ untracked and the manifest
honest, at the cost of mutating a tree the build system considers read-only --
the same trade tools/patch-littlefs.py makes.

No upstream patch file yet: unlike the littlefs and fatfs patches, this one
touches four files for one feature and is more naturally offered as a PR
against the component's own repository than as a patch dropped in tools/.

Failure policy
--------------
Loud, never silent. A version this was not written against, or an anchor that
moved, stops the build with a message saying so.
"""

import re
import sys
from pathlib import Path

EXPECTED_VERSION = "1.3.0"
COMPONENT = "espressif__usb_host_msc"

MARK_HEADER = "scsi_cmd_read_capacity_16"
MARK_SOURCE = "scsi_cmd_read_capacity_16("
# Two separate insertions land in msc_host_install_device(): the local
# variable and the capacity logic that uses it. One shared marker breaks the
# moment the first insertion succeeds, because its own text then contains the
# marker the second checks for -- exactly the bug patch-littlefs.py's own
# history warns about. Each gets a marker unique to itself.
MARK_INSTALL_LOCALS = "uint64_t block_count_16 = 0;"
MARK_INSTALL_CAPACITY = "block_count_16) == ESP_OK"
HEADER_ANCHOR = (
    "esp_err_t scsi_cmd_read_capacity(msc_host_device_handle_t device,\n"
    "                                 uint32_t *block_size,\n"
    "                                 uint32_t *block_count);"
)

HEADER_ADDITION = '''
/**
 * @brief SCSI READ CAPACITY(16) (opcode 0x9E, service action 0x10).
 *
 * Same purpose as scsi_cmd_read_capacity(), with a 64-bit block count: SBC-2
 * says a device whose capacity does not fit the (10) command's 32-bit field
 * returns 0xFFFFFFFF from it and expects this to be tried instead.
 *
 * @param[out] block_count The last valid LBA plus one -- what SBC-2 calls the
 *                          "returned logical block address" is the *last*
 *                          block, not the count, so the +1 is done here rather
 *                          than left for every caller to remember.
 */
esp_err_t scsi_cmd_read_capacity_16(msc_host_device_handle_t device,
                                    uint32_t *block_size,
                                    uint64_t *block_count);

/**
 * @brief SCSI READ(16) (opcode 0x88): like scsi_cmd_read10(), with a 64-bit
 *        sector address for a device READ CAPACITY(16) was needed for.
 */
esp_err_t scsi_cmd_read16(msc_host_device_handle_t device,
                          uint8_t *data,
                          uint64_t sector_address,
                          uint32_t num_sectors,
                          uint32_t sector_size);

/**
 * @brief SCSI WRITE(16) (opcode 0x8A): the write side of scsi_cmd_read16().
 */
esp_err_t scsi_cmd_write16(msc_host_device_handle_t device,
                           const uint8_t *data,
                           uint64_t sector_address,
                           uint32_t num_sectors,
                           uint32_t sector_size);

'''

SOURCE_ANCHOR = "esp_err_t scsi_cmd_unit_ready(msc_host_device_handle_t dev)"

# scsi_read16/write16/read_capacity_16 CDBs, laid out from FreeBSD's
# sys/cam/scsi/scsi_all.h (struct scsi_rw_16, struct scsi_read_capacity_16):
# opcode, a flags byte, an 8-byte big-endian LBA, then either a 4-byte
# transfer length (rw) or a 4-byte allocation length plus two reserved bytes
# (capacity). __builtin_bswap64 for the LBA is the same idiom the file already
# uses via __builtin_bswap32 -- there is no bswap64 anywhere in this file to
# match against, so this introduces the first one.
SOURCE_ADDITION = '''
typedef struct __attribute__((packed))
{
    msc_cbw_t base;
    uint8_t opcode;
    uint8_t flags;
    uint64_t address;
    uint32_t length;
    uint8_t reserved1;
    uint8_t control;
} cbw_read16_t;

typedef struct __attribute__((packed))
{
    msc_cbw_t base;
    uint8_t opcode;
    uint8_t flags;
    uint64_t address;
    uint32_t length;
    uint8_t reserved1;
    uint8_t control;
} cbw_write16_t;

#define SCSI_CMD_READ16  0x88
#define SCSI_CMD_WRITE16 0x8A
#define SCSI_CMD_SERVICE_ACTION_IN16    0x9E
#define SCSI_SAI_READ_CAPACITY_16       0x10

typedef struct __attribute__((packed))
{
    msc_cbw_t base;
    uint8_t opcode;
    uint8_t service_action;    // low 5 bits; upper bits reserved, left 0
    uint64_t address;          // unused for READ CAPACITY(16): always 0
    uint32_t alloc_length;
    uint8_t reserved1;
    uint8_t control;
} cbw_read_capacity16_t;

typedef struct __attribute__((packed))
{
    uint64_t address;   // last valid LBA, not a count -- see the +1 below
    uint32_t block_size;
    uint8_t rest[20];   // p_type/prot_en, lbppbe, lalba/lbpme/lbprz, reserved
} cbw_read_capacity16_response_t;

esp_err_t scsi_cmd_read16(msc_host_device_handle_t dev,
                          uint8_t *data,
                          uint64_t sector_address,
                          uint32_t num_sectors,
                          uint32_t sector_size)
{
    if (num_sectors != 0 && sector_size > UINT32_MAX / num_sectors) {
        return ESP_ERR_INVALID_SIZE;
    }

    msc_device_t *device = (msc_device_t *)dev;
    cbw_read16_t cbw = {
        CBW_BASE_INIT(IN_DIR, CBW_CMD_SIZE(cbw_read16_t), num_sectors * sector_size),
        .opcode = SCSI_CMD_READ16,
        .flags = 0, // lun
        .address = __builtin_bswap64(sector_address),
        .length = __builtin_bswap32(num_sectors),
    };

    esp_err_t ret = bot_execute_command(device, &cbw.base, data, num_sectors * sector_size);

    // In case of an error, get an error code
    if (unlikely(ret != ESP_OK)) {
        MSC_RETURN_ON_ERROR( scsi_cmd_sense(device, NULL));
    }
    return ret;
}

esp_err_t scsi_cmd_write16(msc_host_device_handle_t dev,
                           const uint8_t *data,
                           uint64_t sector_address,
                           uint32_t num_sectors,
                           uint32_t sector_size)
{
    if (num_sectors != 0 && sector_size > UINT32_MAX / num_sectors) {
        return ESP_ERR_INVALID_SIZE;
    }

    msc_device_t *device = (msc_device_t *)dev;
    cbw_write16_t cbw = {
        CBW_BASE_INIT(OUT_DIR, CBW_CMD_SIZE(cbw_write16_t), num_sectors * sector_size),
        .opcode = SCSI_CMD_WRITE16,
        .address = __builtin_bswap64(sector_address),
        .length = __builtin_bswap32(num_sectors),
    };

    esp_err_t ret = bot_execute_command(device, &cbw.base, (void *)data, num_sectors * sector_size);

    // In case of an error, get an error code
    if (unlikely(ret != ESP_OK)) {
        MSC_RETURN_ON_ERROR( scsi_cmd_sense(device, NULL));
    }
    return ret;
}

esp_err_t scsi_cmd_read_capacity_16(msc_host_device_handle_t dev, uint32_t *block_size, uint64_t *block_count)
{
    msc_device_t *device = (msc_device_t *)dev;
    cbw_read_capacity16_response_t response;

    cbw_read_capacity16_t cbw = {
        CBW_BASE_INIT(IN_DIR, CBW_CMD_SIZE(cbw_read_capacity16_t), sizeof(response)),
        .opcode = SCSI_CMD_SERVICE_ACTION_IN16,
        .service_action = SCSI_SAI_READ_CAPACITY_16,
        .alloc_length = __builtin_bswap32(sizeof(response)),
    };

    esp_err_t ret = bot_execute_command(device, &cbw.base, &response, sizeof(response));

    // In case of an error, get an error code
    if (unlikely(ret != ESP_OK)) {
        MSC_RETURN_ON_ERROR( scsi_cmd_sense(device, NULL));
        return ret;
    }

    // SBC-2: this field is the *last* LBA, not a count.
    *block_count = __builtin_bswap64(response.address) + 1;
    *block_size = __builtin_bswap32(response.block_size);

    return ret;
}

'''



INSTALL_ANCHOR = (
    "    uint32_t block_size, block_count;\n"
    "    const usb_config_desc_t *config_desc;\n"
    "    msc_device_t *msc_device;"
)

INSTALL_ADDITION = (
    "    uint32_t block_size, block_count;\n"
    "    uint64_t block_count_16 = 0;\n"
    "    const usb_config_desc_t *config_desc;\n"
    "    msc_device_t *msc_device;"
)

# scsi_cmd_read_capacity() stays first: every device answers it, and it is one
# command instead of two on the overwhelming majority of drives that are under
# 2TiB. (16) is tried only when (10) reports the 0xFFFFFFFF SBC-2 uses to mean
# "ask again with the bigger command" -- exactly the sequence FreeBSD's da(4)
# follows in scsi_da.c. A device that STALLs or errors on (16) is not fatal:
# the (10) result stands, so the drive is usable up to 2TiB rather than not at
# all, which is the honest degrade for old media that predates the command.
CAPACITY_ANCHOR = (
    "    MSC_GOTO_ON_ERROR( scsi_cmd_read_capacity(msc_device, &block_size, &block_count) );\n"
    "\n"
    "    // Validate peer-controlled block size before FatFS sizes fs->win from a\n"
    "    // truncated WORD while reads use the full uint32_t length (BBP 574).\n"
    "    MSC_GOTO_ON_FALSE(block_size >= 512 &&\n"
    "                      block_size <= 4096 &&\n"
    "                      (block_size & (block_size - 1)) == 0,\n"
    "                      ESP_ERR_INVALID_SIZE);\n"
    "\n"
    "    msc_device->disk.block_size = block_size;\n"
    "    msc_device->disk.block_count = block_count;"
)

CAPACITY_ADDITION = (
    "    MSC_GOTO_ON_ERROR( scsi_cmd_read_capacity(msc_device, &block_size, &block_count) );\n"
    "\n"
    "    // Validate peer-controlled block size before FatFS sizes fs->win from a\n"
    "    // truncated WORD while reads use the full uint32_t length (BBP 574).\n"
    "    MSC_GOTO_ON_FALSE(block_size >= 512 &&\n"
    "                      block_size <= 4096 &&\n"
    "                      (block_size & (block_size - 1)) == 0,\n"
    "                      ESP_ERR_INVALID_SIZE);\n"
    "\n"
    "    // SBC-2: 0xFFFFFFFF from the (10) command means \"ask again with (16)\".\n"
    "    // A device that does not support (16) keeps the (10) answer -- usable up\n"
    "    // to 2TiB, which is what it was before this patch existed.\n"
    "    if (block_count == 0xFFFFFFFF &&\n"
    "        scsi_cmd_read_capacity_16(msc_device, &block_size, &block_count_16) == ESP_OK &&\n"
    "        block_size >= 512 && block_size <= 4096 &&\n"
    "        (block_size & (block_size - 1)) == 0) {\n"
    "        msc_device->disk.block_size = block_size;\n"
    "        msc_device->disk.block_count_64 = block_count_16;\n"
    "    } else {\n"
    "        msc_device->disk.block_size = block_size;\n"
    "        msc_device->disk.block_count_64 = block_count;\n"
    "    }\n"
    "    msc_device->disk.block_count = block_count;"
)


COMMON_ANCHOR = (
    "typedef struct {\n"
    "    uint32_t block_size;    /**< Block size */\n"
    "    uint32_t block_count;   /**< Block count */\n"
    "} usb_disk_t;"
)

COMMON_ADDITION = (
    "typedef struct {\n"
    "    uint32_t block_size;    /**< Block size */\n"
    "    uint32_t block_count;   /**< Block count, from READ CAPACITY(10); \n"
    "                                 saturates at 0xFFFFFFFF for a disk\n"
    "                                 READ CAPACITY(16) was needed for -- see\n"
    "                                 block_count_64. */\n"
    "    uint64_t block_count_64; /**< The real count. Equal to block_count\n"
    "                                  except on a disk that needed (16). */\n"
    "} usb_disk_t;"
)

MARK_COMMON = "block_count_64;"

BDL_ANCHOR = (
    "        .disk_size = (uint64_t)dev->disk.block_count * dev->disk.block_size,"
)

BDL_ADDITION = (
    "        .disk_size = (uint64_t)dev->disk.block_count_64 * dev->disk.block_size,"
)

MARK_BDL_SIZE = "block_count_64 * dev->disk.block_size"

# msc_bdl_read/write take a 32-bit LBA today because scsi_cmd_read10/write10 do.
# A device whose real capacity needed READ CAPACITY(16) can still have most of
# its addressable range above 2^32 sectors -- true for the 3.6TB T9 this was
# written against, whose 3.1TB exFAT partition starts past the 2TiB mark -- so
# reads there have to go out (16) rather than fail the `lba > UINT32_MAX` guard
# that was the only option before this existed.
BDL_READ_ANCHOR = (
    "    const uint64_t lba = src_addr / block_size;\n"
    "    const uint64_t num_blocks = len / block_size;\n"
    "    if (lba > UINT32_MAX || num_blocks > UINT32_MAX) {\n"
    "        return ESP_ERR_INVALID_SIZE;\n"
    "    }\n"
    "\n"
    "    msc_device_t *dev = (msc_device_t *)h->ctx;\n"
    "    return scsi_cmd_read10(dev, dst, (uint32_t)lba, (uint32_t)num_blocks, block_size);"
)

BDL_READ_ADDITION = (
    "    const uint64_t lba = src_addr / block_size;\n"
    "    const uint64_t num_blocks = len / block_size;\n"
    "    if (num_blocks > UINT32_MAX) {\n"
    "        return ESP_ERR_INVALID_SIZE;\n"
    "    }\n"
    "\n"
    "    /*\n"
    "     * The transfer DMAs into dst, and the USB host invalidates the cache\n"
    "     * over it when the transfer completes -- hcd_dwc.c's\n"
    "     * cache_sync_data_buffer(), which hands the caller's own buffer to\n"
    "     * esp_cache_msync(). That call may only be given a region whose address\n"
    "     * *and* size both meet the cache alignment: \"cache memory\n"
    "     * synchronization to an unaligned address region may silently corrupt\n"
    "     * the memory\" (GOTCHAS.md quotes it in full). IDF's own comment there\n"
    "     * says it accepts UNALIGNED data regardless, \"for cases where the class\n"
    "     * drivers force overwrite the allocated data buffers\".\n"
    "     *\n"
    "     * The callers here include FatFs's sector window and a user's own\n"
    "     * read() buffer, so this cannot be pushed onto them. An unaligned\n"
    "     * destination therefore gets a bounce buffer. 64 rather than the 32 this\n"
    "     * build's cache line is: the line size is a build option and this is a\n"
    "     * compile-time constant, and over-aligning is free.\n"
    "     *\n"
    "     * The write path does not need this: an OUT transfer syncs once, before\n"
    "     * the transfer, and passes ESP_CACHE_MSYNC_FLAG_UNALIGNED.\n"
    "     */\n"
    "    uint8_t *target = dst;\n"
    "    if (((uintptr_t)dst % 64) != 0) {\n"
    "        target = heap_caps_malloc(len, MALLOC_CAP_DMA | MALLOC_CAP_CACHE_ALIGNED);\n"
    "        if (target == NULL) {\n"
    "            return ESP_ERR_NO_MEM;\n"
    "        }\n"
    "    }\n"
    "\n"
    "    msc_device_t *dev = (msc_device_t *)h->ctx;\n"
    "    esp_err_t err;\n"
    "    if (lba > UINT32_MAX) {\n"
    "        err = scsi_cmd_read16(dev, target, lba, (uint32_t)num_blocks, block_size);\n"
    "    } else {\n"
    "        err = scsi_cmd_read10(dev, target, (uint32_t)lba, (uint32_t)num_blocks, block_size);\n"
    "    }\n"
    "\n"
    "    if (target != dst) {\n"
    "        if (err == ESP_OK) {\n"
    "            memcpy(dst, target, len);\n"
    "        }\n"
    "        heap_caps_free(target);\n"
    "    }\n"
    "    return err;"
)

MARK_BDL_READ = "scsi_cmd_read16(dev, target"

# The bounce needs two headers the component does not already pull in: the cache
# caps for the allocation, and memcpy. string.h first, matching the file's own
# ordering of system before project headers.
MSC_INCLUDES_ANCHOR = "#include \"msc_scsi_bot.h\"\n"
MSC_INCLUDES_ADDITION = (
    "#include <string.h>\n"
    "\n"
    "#include \"esp_heap_caps.h\"\n"
)
MARK_MSC_INCLUDES = "#include \"esp_heap_caps.h\""

BDL_WRITE_ANCHOR = (
    "    const uint64_t lba = dst_addr / block_size;\n"
    "    const uint64_t num_blocks = len / block_size;\n"
    "    if (lba > UINT32_MAX || num_blocks > UINT32_MAX) {\n"
    "        return ESP_ERR_INVALID_SIZE;\n"
    "    }\n"
    "\n"
    "    msc_device_t *dev = (msc_device_t *)h->ctx;\n"
    "    return scsi_cmd_write10(dev, src, (uint32_t)lba, (uint32_t)num_blocks, block_size);"
)

BDL_WRITE_ADDITION = (
    "    const uint64_t lba = dst_addr / block_size;\n"
    "    const uint64_t num_blocks = len / block_size;\n"
    "    if (num_blocks > UINT32_MAX) {\n"
    "        return ESP_ERR_INVALID_SIZE;\n"
    "    }\n"
    "\n"
    "    msc_device_t *dev = (msc_device_t *)h->ctx;\n"
    "    if (lba > UINT32_MAX) {\n"
    "        return scsi_cmd_write16(dev, src, lba, (uint32_t)num_blocks, block_size);\n"
    "    }\n"
    "    return scsi_cmd_write10(dev, src, (uint32_t)lba, (uint32_t)num_blocks, block_size);"
)

MARK_BDL_WRITE = "scsi_cmd_write16(dev, src"


def die(msg):
    print(f"patch-msc: {msg}", file=sys.stderr)
    sys.exit(1)


def check_version(root):
    """Refuse to patch a version this was not written against."""
    lock = root / "dependencies.lock"
    if not lock.exists():
        return  # first configure, before the manager has written one
    text = lock.read_text(encoding="utf-8")

    m = re.search(r"^  espressif/usb_host_msc:\n(?:.*?\n)*?^    version: (\S+)$",
                  text, re.M)
    if m and m.group(1).strip("'\"") != EXPECTED_VERSION:
        die(
            f"espressif/usb_host_msc is {m.group(1)}, but this patch was "
            f"written for {EXPECTED_VERSION}.\n"
            f"  Re-check the anchors in tools/patch-msc.py against the new "
            f"source, then update EXPECTED_VERSION.\n"
            f"  If the version now provides READ CAPACITY(16)/READ(16)/"
            f"WRITE(16) itself, delete this script and the hook in "
            f"CMakeLists.txt."
        )


def insert_after(path, anchor, addition, what, marker, replacement=None):
    """Put `addition` in front of `anchor`, or swap the anchor for `replacement`."""
    text = path.read_text(encoding="utf-8")

    if marker in text:
        return False  # already patched; configure runs every build

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

    header = comp / "include" / "esp_private" / "msc_scsi_bot.h"
    source = comp / "src" / "msc_scsi_bot.c"
    common = comp / "private_include" / "msc_common.h"
    host = comp / "src" / "msc_host.c"
    bdl = comp / "src" / "msc_bdl.c"
    for p in (header, source, common, host, bdl):
        if not p.is_file():
            die(f"expected {p} to exist")

    done = [
        insert_after(header, HEADER_ANCHOR, HEADER_ADDITION, "header",
                     MARK_HEADER),
        insert_after(source, SOURCE_ANCHOR, SOURCE_ADDITION, "source",
                     MARK_SOURCE),
        insert_after(common, COMMON_ANCHOR, "", "common struct",
                     MARK_COMMON, replacement=COMMON_ADDITION),
        insert_after(host, INSTALL_ANCHOR, "", "install locals",
                     MARK_INSTALL_LOCALS, replacement=INSTALL_ADDITION),
        insert_after(host, CAPACITY_ANCHOR, "", "install capacity",
                     MARK_INSTALL_CAPACITY, replacement=CAPACITY_ADDITION),
        insert_after(bdl, BDL_ANCHOR, "", "bdl geometry",
                     MARK_BDL_SIZE, replacement=BDL_ADDITION),
        insert_after(bdl, MSC_INCLUDES_ANCHOR, MSC_INCLUDES_ADDITION,
                     "bdl includes", MARK_MSC_INCLUDES),
        insert_after(bdl, BDL_READ_ANCHOR, "", "bdl read",
                     MARK_BDL_READ, replacement=BDL_READ_ADDITION),
        insert_after(bdl, BDL_WRITE_ANCHOR, "", "bdl write",
                     MARK_BDL_WRITE, replacement=BDL_WRITE_ADDITION),
    ]

    if any(done):
        print("patch-msc: added READ CAPACITY(16)/READ(16)/WRITE(16) to "
              f"{COMPONENT} {EXPECTED_VERSION}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
