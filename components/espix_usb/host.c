/*
 * The host role: install the stack, watch for mass-storage devices, and keep a
 * table of what is attached.
 *
 * Stage 1 reads the partition table and stops there: sector 0 for an MBR, and
 * for a GPT disk the entry array at LBA 1. The partition table is what makes
 * `lsblk` worth having, and reading it is not a mount: no filesystem driver is
 * consulted, nothing is registered with the VFS, and a stick espix cannot drive
 * is described just as carefully as one it can. A mount is a different piece of
 * work and a more dangerous one -- see docs/USB-HOST.md.
 *
 * Everything USB happens on the two tasks the stack brings with it. The host
 * library needs one calling usb_host_lib_handle_events() forever, or it stops
 * enumerating; the MSC driver starts its own and calls back from it, so the
 * callback context is a task and not an interrupt, and the disk reads happen
 * there. That is what Espressif's own example does, and it is the only way to
 * know a device's geometry before publishing it.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "usb/msc_host.h"
#include "usb/usb_host.h"

#include "esp_mbr.h"
#include "esp_mbr_utils.h"

#include "espix_kernel.h"
#include "espix_usb.h"
#include "espix_usb_priv.h"

#define TAG "usb"

/*
 * The host library's task: it blocks in usb_host_lib_handle_events() and wakes
 * for port changes, which is the cheap half. The MSC driver's task is the one
 * that blocks on USB transfers and does the reading.
 */
#define HOST_TASK_STACK 4096
#define HOST_TASK_PRIO  4

/* Above the console session so a slow stick cannot stall a prompt, and below
 * lwip so bulk transfers cannot crowd out the network. */
#define MSC_TASK_STACK  4096
#define MSC_TASK_PRIO   5

/*
 * The largest sector this will read. Devices report 512 in practice; 4096 exists
 * (modern HDDs) and 2048 is optical. The buffer is sized by what the device
 * reports, not by this.
 */
#define SECTOR_MAX 4096

/* Offsets in a FAT boot sector, from the FAT specification. */
#define FAT_FSTYPE_FAT32  0x52      /* BS_FilSysType, "FAT32   " */
#define FAT_LABEL_FAT32   0x47      /* BS_VolLab, 11 space-padded bytes */
#define FAT_FSTYPE_FAT16  0x36      /* BS_FilSysType, "FAT12   "/"FAT16   " */
#define FAT_LABEL_FAT16   0x2B

#define MBR_SIGNATURE_OFFSET 510

/*
 * One MBR partition entry: 16 bytes, four of them starting at 446. Only the LBA
 * fields are read -- the CHS fields beside them carry the same information in a
 * form this cannot use, and they are the half that was already obsolete when the
 * format was written.
 */
#define MBR_ENTRIES        4
#define MBR_ENTRY_OFFSET   446
#define MBR_ENTRY_SIZE     16
#define MBR_ENTRY_TYPE     4        /* byte 4 of the entry: the type code */
#define MBR_ENTRY_LBA      8        /* four bytes, little-endian, from byte 8 */
#define MBR_ENTRY_SECTORS  12

/*
 * A GPT header, from the UEFI specification -- the same layout and the same
 * offsets FatFs parses in ff.c (its GPTH_* and GPTE_* constants, under
 * FF_LBA64). Only what the walk needs: where the entry array is, how many
 * entries it holds, and how big each one is.
 *
 * The signature is the test for which kind of table a disk has: eight bytes at
 * a fixed offset, read from LBA 1. Sector 0 cannot answer it, because a GPT
 * disk's protective MBR is a real MBR entry typed 0xEE that covers the whole
 * disk.
 */
#define GPT_HEADER_LBA      1
#define GPT_SIGNATURE       "EFI PART"
#define GPT_HDR_PT_LBA      72      /* QWORD: the entry array's first LBA */
#define GPT_HDR_PT_COUNT    80      /* DWORD: entries the array holds */
#define GPT_HDR_ENTRY_SIZE  84      /* DWORD: bytes per entry */
#define GPT_HDR_PT_BCC      88      /* DWORD: CRC32 of the whole entry array */

#define GPT_ENTRY_SIZE      128     /* the size every table this has met uses */
#define GPT_ENTRY_TYPE_GUID 0       /* 16 bytes: what the partition is for */
#define GPT_ENTRY_UUID      16      /* 16 bytes: this entry's own identity */
#define GPT_ENTRY_FIRST_LBA 32
#define GPT_ENTRY_LAST_LBA  40

/*
 * The ceiling FatFs enforces on its own GPT reader, and the size of the
 * reference layout in the specification: 128 entries of 128 bytes. A header
 * claiming more is not one to walk.
 */
#define GPT_ENTRIES_MAX     128

/* A GUID written the way Linux writes one: 36 characters and a terminator. */
#define GUID_STR_LEN        36

/*
 * The EFI System Partition. FAT, so espix can mount it, and absent from the
 * library's type table, so without a line of espix's own it is a type nothing
 * can name.
 */
#define MBR_TYPE_EFI_FAT   0xEF

/*
 * An ISO 9660 volume's primary descriptor, which is where that filesystem keeps
 * its name: logical sector 16 of the ISO's own 2048-byte sectors, so 32768 bytes
 * in, with the type code at byte 0, "CD001" at 1, and the 32-byte volume
 * identifier at 40. All of it inside the first 512 bytes of the block, so one
 * read answers both the question and the label.
 */
#define ISO_PVD_OFFSET     32768
#define ISO_MAGIC_OFFSET   1
#define ISO_LABEL_OFFSET   40
#define ISO_LABEL_LEN      32

/* BS_VolLab: 11 space-padded bytes, the same shape as an ISO's identifier. */
#define FAT_LABEL_LEN      11

typedef struct {
    bool                     in_use;
    uint8_t                  addr;      /* USB address, so a repeat can be recognised */
    espix_usb_dev_t          info;      /* what a session gets to see */
    msc_host_device_handle_t device;    /* the MSC handle, needed to uninstall */
    esp_blockdev_handle_t    bdl;       /* espix's own blockdev on that device */
} usb_dev_t;

static usb_dev_t s_devs[ESPIX_USB_MAX_DEVS];
static SemaphoreHandle_t s_lock;
static bool s_installed;

/* One attach at a time: the MSC callback and an explicit `usbscan` can otherwise
 * race for the same address, and the loser gets a claimed interface it does not
 * know about. */
static SemaphoreHandle_t s_attach_lock;

/*
 * The work queue, and why the device work does not happen where it is announced.
 *
 * A client's transfer completions are delivered only from inside
 * usb_host_client_handle_events() -- its endpoint list is serviced there, and
 * that is where a transfer's callback runs (usb_host.c:1136). The MSC driver
 * invokes our callback from exactly that function, so an install waiting for a
 * SCSI transfer to complete is waiting for a loop it is itself blocking: each
 * transfer burns its full 5s timeout (msc_host.c:703), the ready-state retry
 * does that up to fifty times, and a device takes minutes to fail -- with no log
 * line until it does.
 *
 * So the callback only notes the address and returns; `usb:work` does the
 * install, where the driver's event loop is free to deliver the completions.
 */
#define WORK_QUEUE_LEN  8
/*
 * 6KB, not the 4KB this started at: the attach hook now mounts /etc/fstab
 * volumes here, FatFs alone wants about 2KB, and the device array the applier
 * copies is another couple of KB. The old size overflowed -- a panic on every
 * attach, which with a stick left in means a boot loop.
 */
#define WORK_TASK_STACK 6144
#define WORK_TASK_PRIO  3           /* below usb:host(4) and USB MSC(5) */

/* How often the worker sweeps the pool when nothing has been queued. Devices are
 * claimed by event; this is what covers the case where the event never came. */
#define SCAN_PERIOD_TICKS pdMS_TO_TICKS(5000)

/*
 * A device that cannot be claimed is not retried on every sweep: the failure is
 * reported once and the address is then left alone for a minute. Without this a
 * device the driver refuses would fill the kernel log with the same line every
 * five seconds, which is how a real failure gets lost.
 */
#define ATTACH_FAIL_BACKOFF pdMS_TO_TICKS(60000)

typedef struct {
    uint8_t    addr;
    TickType_t until;
} attach_backoff_t;

static attach_backoff_t s_backoff[ESPIX_USB_LSUSB_MAX];

static QueueHandle_t s_work;

/*
 * Serialises every open/close pair on the monitor client.
 *
 * usb_host_device_open() refuses a second open of the same address *by the same
 * client* (usb_host.c:1362), and three of our own callers use that client: the
 * monitor's arrival callback, the interface check before an attach, and `lsusb`.
 * Without this, two of them racing for one device makes one report a device with
 * no interfaces at all -- which is exactly how the SSD was refused at boot.
 */
static SemaphoreHandle_t s_open_lock;

/*
 * A second client, whose only job is to say out loud what the storage path
 * cannot. The MSC callback is reached for mass-storage devices alone, so a hub
 * -- or a keyboard, or anything else no driver claims -- is enumerated,
 * addressed and then never mentioned again. This client sees every device and
 * logs it.
 *
 * It also owns the handles `lsusb` opens, because a device handle belongs to the
 * client that opened it.
 */
#define MONITOR_EVENTS 8            /* queued event messages */

/*
 * The last few devices by address, so a removal line can name what left. The
 * library does not reuse an address until the device is gone, so a plain
 * address-keyed table is enough.
 */
typedef struct {
    uint8_t  addr;
    uint16_t id_vendor;
    uint16_t id_product;
} monitor_name_t;

static usb_host_client_handle_t s_monitor;
static monitor_name_t s_seen[ESPIX_USB_LSUSB_MAX];

/* Copies into a fixed buffer, always terminating. Device strings are whatever
 * the device felt like sending, so a truncation here is not a failure. */
static void copy_str(char *dst, size_t dst_len, const char *src)
{
    if (dst == NULL || dst_len == 0) {
        return;
    }
    size_t i = 0;
    if (src != NULL) {
        for (; src[i] != '\0' && i + 1 < dst_len; i++) {
            dst[i] = src[i];
        }
    }
    dst[i] = '\0';
}

/*
 * Device strings arrive as the 16-bit code units of a USB string descriptor, and
 * wchar_t is two bytes on this target (__SIZEOF_WCHAR_T__ is 2), so the buffer
 * the MSC driver hands back is the same UTF-16 -- and a character outside the
 * BMP is a surrogate pair, one that either source can also cut in half. One
 * decoder for both, because the difference is only where the length comes from.
 *
 * Sessions get UTF-8, because that is what a terminal is.
 */
static size_t utf8_encode(uint32_t cp, char *dst)
{
    if (cp < 0x80) {
        dst[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        dst[0] = (char)(0xC0 | (cp >> 6));
        dst[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        dst[0] = (char)(0xE0 | (cp >> 12));
        dst[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    dst[0] = (char)(0xF0 | (cp >> 18));
    dst[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    dst[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    dst[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* `units` is the number of code units to read, or SIZE_MAX for NUL-terminated. */
static void utf16_to_utf8(const uint16_t *src, size_t units, char *dst,
                          size_t dst_len)
{
    size_t o = 0;

    if (dst_len == 0) {
        return;
    }

    for (size_t i = 0; i < units && src[i] != 0 && o + 4 < dst_len; i++) {
        uint32_t cp = (uint32_t)src[i];

        if (cp >= 0xD800 && cp <= 0xDBFF) {
            const uint32_t lo = (i + 1 < units) ? (uint32_t)src[i + 1] : 0;
            if (lo < 0xDC00 || lo > 0xDFFF) {
                continue;   /* half a pair with nothing to pair with */
            }
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            i++;
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            continue;
        }
        o += utf8_encode(cp, dst + o);
    }
    dst[o] = '\0';
}

static void wstr_to_utf8(const wchar_t *src, char *dst, size_t dst_len)
{
    if (dst_len == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    utf16_to_utf8((const uint16_t *)(const void *)src, SIZE_MAX, dst, dst_len);
}

/* Enough code units for a 96-byte string descriptor, which is longer than any
 * descriptor worth printing. */
#define STR_DESC_UNITS 48

/* A USB string descriptor as UTF-8, or empty when the device has none. */
static void str_desc_to_utf8(const usb_str_desc_t *desc, char *dst, size_t dst_len)
{
    if (dst_len == 0) {
        return;
    }
    dst[0] = '\0';

    if (desc == NULL || desc->bLength <= USB_STANDARD_DESC_SIZE) {
        return;
    }

    size_t units = (desc->bLength - USB_STANDARD_DESC_SIZE) / 2;
    if (units > STR_DESC_UNITS) {
        units = STR_DESC_UNITS;
    }

    /*
     * wData is a member of a packed struct, so its address cannot be taken --
     * -Werror=address-of-packed-member is right about that. Copy the code units
     * out first; there is no alignment to preserve in doing so.
     */
    uint16_t buf[STR_DESC_UNITS];
    memcpy(buf, (const void *)desc->wData, units * sizeof(uint16_t));

    utf16_to_utf8(buf, units, dst, dst_len);
}

/*
 * The MBR type code, named the way a mount would have to name it. One name for
 * the three FAT types, as Linux does: the driver is the same one.
 *
 * `foreign` marks the types this recognises but no driver here will ever open --
 * exFAT/NTFS, Linux, and the protective MBR of a GPT disk. Naming those is most
 * of the reason `lsblk` exists before mounting does; a column that stays blank
 * for them reads as an empty disk.
 */
static const char *fstype_name(uint8_t type, bool *foreign)
{
    switch (type) {
    case ESP_EXT_PART_TYPE_FAT12:
    case ESP_EXT_PART_TYPE_FAT16:
    case ESP_EXT_PART_TYPE_FAT32:
        return "vfat";
    case ESP_EXT_PART_TYPE_LITTLEFS:
        return "littlefs";
    case ESP_EXT_PART_TYPE_RAW_DATA:
        return "raw";
    case ESP_EXT_PART_TYPE_LINUX_ANY:
        *foreign = true;
        return "linux";
    case ESP_EXT_PART_TYPE_EXFAT_OR_NTFS:
        /* One MBR code covers both, so this is as specific as sector 0 gets. */
        *foreign = true;
        return "exfat/ntfs";
    case ESP_EXT_PART_TYPE_GPT_PROTECTIVE_MBR:
        *foreign = true;
        return "gpt";
    default:
        return "";
    }
}

/*
 * A four-byte little-endian field: an MBR entry's LBA or sector count, or a
 * field of a GPT header.
 */
static uint32_t entry_u32(const uint8_t *entry, size_t at)
{
    return (uint32_t)entry[at] | ((uint32_t)entry[at + 1] << 8) |
           ((uint32_t)entry[at + 2] << 16) | ((uint32_t)entry[at + 3] << 24);
}

/* An eight-byte one, which is how a GPT counts LBAs. */
static uint64_t entry_u64(const uint8_t *entry, size_t at)
{
    return (uint64_t)entry_u32(entry, at) |
           ((uint64_t)entry_u32(entry, at + 4) << 32);
}

/*
 * The name for a partition entry's *type byte*: the library's table first, then
 * espix's own additions. "" means nothing here can name it, which is not the same
 * as a type that means "no filesystem": the walk keeps the row either way and
 * counts it, so `lsblk` can say that the listing is not the whole table.
 */
static const char *partition_type_name(uint8_t raw, bool *foreign)
{
    uint8_t parsed = ESP_EXT_PART_TYPE_NONE;

    /*
     * The type table stays the library's rather than a copy here, so a code it
     * learns upstream arrives without an edit; the names espix prints are espix's
     * either way.
     */
    (void)esp_mbr_parse_default_supported_partition_types(raw, &parsed);
    if (parsed != ESP_EXT_PART_TYPE_NONE) {
        return fstype_name(parsed, foreign);
    }

    /*
     * 0xEF is the EFI System Partition -- "EFI (FAT-12/16/32)" to fdisk, and what
     * every Arch, CachyOS and Windows installer writes. It is FAT, so espix can
     * mount it, and the library has no code for it: without this line the only
     * mountable partition on an installer stick is the one espix cannot name.
     */
    if (raw == MBR_TYPE_EFI_FAT) {
        return "vfat";
    }
    return "";
}

/*
 * The GPT type GUIDs worth naming, in the byte order they are stored in on disk
 * -- the first three fields little-endian, so a GUID is not one memcmp away from
 * its spelling. Microsoft basic data is byte for byte FatFs's own GUID_MS_Basic
 * (ff.c), which makes it a second opinion rather than a transcription; the
 * others are the discoverable-partition GUIDs from the UEFI specification, and
 * what confirms them is a real GPT disk naming its rows.
 *
 * Deliberately short, and the division of labour is the point: a GUID says what
 * the partition is *for*, not what is on it. "Microsoft basic data" covers every
 * FAT, exFAT, NTFS and BitLocker volume Windows ever wrote, so it names the row
 * and the partition's own boot sector still has the last word -- the same split
 * the MBR walk makes between its type byte and its content probe.
 */
static const uint8_t GPT_GUID_ESP[16] =
    {0x28,0x73,0x2A,0xC1,0x1F,0xF8,0xD2,0x11,0xBA,0x4B,0x00,0xA0,0xC9,0x3E,0xC9,0x3B};
static const uint8_t GPT_GUID_MS_BASIC[16] =
    {0xA2,0xA0,0xD0,0xEB,0xE5,0xB9,0x33,0x44,0x87,0xC0,0x68,0xB6,0xB7,0x26,0x99,0xC7};
static const uint8_t GPT_GUID_MS_RESERVED[16] =
    {0x16,0xE3,0xC9,0xE3,0x5C,0x0B,0xB8,0x4D,0x81,0x7D,0xF9,0x2D,0xF0,0x02,0x15,0xAE};
static const uint8_t GPT_GUID_LINUX_FS[16] =
    {0xAF,0x3D,0xC6,0x0F,0x83,0x84,0x72,0x47,0x8E,0x79,0x3D,0x69,0xD8,0x47,0x7D,0xE4};

/*
 * The name for a GPT type GUID, the counterpart of partition_type_name(). ""
 * means nothing here knows it, which leaves the boot sector to answer alone --
 * and there is no "unknown GUID" string, because a GUID nothing recognises is
 * information about espix, not about the disk.
 */
static const char *gpt_type_name(const uint8_t *guid, bool *foreign)
{
    if (memcmp(guid, GPT_GUID_ESP, sizeof(GPT_GUID_ESP)) == 0) {
        return "vfat";      /* FAT, and the one GPT type espix can mount */
    }
    if (memcmp(guid, GPT_GUID_MS_BASIC, sizeof(GPT_GUID_MS_BASIC)) == 0) {
        return "msdata";
    }
    if (memcmp(guid, GPT_GUID_MS_RESERVED, sizeof(GPT_GUID_MS_RESERVED)) == 0) {
        return "msreserved";
    }
    if (memcmp(guid, GPT_GUID_LINUX_FS, sizeof(GPT_GUID_LINUX_FS)) == 0) {
        *foreign = true;
        return "linux";
    }
    return "";
}

/*
 * A GUID spelled the way Linux spells one, so that a PARTUUID read off espix can
 * be typed into /etc/fstab, and the value `blkid` on the other end of the cable
 * prints is the value espix prints. Linux's own spelling is the GUID's fields as
 * written -- first three little-endian -- so the bytes are reordered here rather
 * than rewritten.
 *
 * A GUID of all zeros is no identity at all: empty, because a row of zeros would
 * match every other row of zeros. The same rule the MBR side applies to a zero
 * disk signature, for the same reason.
 */
static void guid_format(char *dst, size_t dst_len, const uint8_t *raw)
{
    /* 36 characters and a terminator. Stated rather than derived, because the
     * guard is also what lets the compiler see the snprintf cannot truncate. */
    if (dst == NULL || dst_len < GUID_STR_LEN + 1) {
        if (dst != NULL && dst_len > 0) {
            dst[0] = '\0';
        }
        return;
    }

    uint8_t seen = 0;
    for (size_t i = 0; i < 16; i++) {
        seen |= raw[i];
    }
    if (seen == 0) {
        dst[0] = '\0';
        return;
    }

    snprintf(dst, dst_len,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             raw[3], raw[2], raw[1], raw[0], raw[5], raw[4], raw[7], raw[6],
             raw[8], raw[9], raw[10], raw[11], raw[12], raw[13], raw[14],
             raw[15]);
}

/* The FAT boot sector's volume serial, and the MBR's own identifier. */
#define FAT_SERIAL_OFFSET   0x43
#define MBR_DISK_ID_OFFSET  0x1B8

static bool has_mbr_signature(const uint8_t *sector)
{
    return sector[MBR_SIGNATURE_OFFSET] == 0x55 &&
           sector[MBR_SIGNATURE_OFFSET + 1] == 0xAA;
}

/*
 * The FAT boot sector's volume-label offset, or 0 when sector 0 is not a FAT boot
 * sector at all. Which of the two offsets applies is decided by the
 * filesystem-type string the formatter wrote -- informational per the
 * specification, and exactly the question being asked here.
 */
static size_t fat_label_offset(const uint8_t *sector)
{
    if (memcmp(sector + FAT_FSTYPE_FAT32, "FAT32", 5) == 0) {
        return FAT_LABEL_FAT32;
    }
    if (memcmp(sector + FAT_FSTYPE_FAT16, "FAT12", 5) == 0 ||
        memcmp(sector + FAT_FSTYPE_FAT16, "FAT16", 5) == 0) {
        return FAT_LABEL_FAT16;
    }
    return 0;       /* neither string is a FAT volume's, so this is not one */
}

/*
 * The MBR has nowhere to put a volume label, so it comes from the FAT boot
 * sector: eleven space-padded bytes, at 0x2B on FAT12/16 and 0x47 on FAT32.
 * Bytes are copied as they stand: a label written in a non-ASCII code page is not
 * transcoded, because nothing here knows which page that was.
 */
/*
 * A fixed-width, space-padded label, trimmed and NUL-terminated. Left empty when
 * it is all spaces or the formatter's own idea of unnamed -- FAT's "NO NAME",
 * which an ISO would be unlikely to use but would mean the same thing.
 */
static void label_copy(const char *src, size_t src_len, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }

    size_t n = src_len;
    while (n > 0 && src[n - 1] == ' ') {
        n--;
    }
    if (n == 0 || (n == 7 && memcmp(src, "NO NAME", 7) == 0)) {
        return;
    }

    const size_t len = (n < out_len - 1) ? n : out_len - 1;
    memcpy(out, src, len);
    out[len] = '\0';
}

static void fat_label(const uint8_t *sector, char *out, size_t out_len)
{
    const size_t off = fat_label_offset(sector);

    if (off == 0) {
        return;     /* not a FAT boot sector, so it has no FAT label */
    }
    label_copy((const char *)sector + off, FAT_LABEL_LEN, out, out_len);
}

/* The FAT boot sector's volume serial, at 0x43 in the block the label comes from. */
static void fat_serial(const uint8_t *sector, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0 || fat_label_offset(sector) == 0) {
        return;     /* not a FAT boot sector, so it has no serial */
    }

    const uint32_t serial = (uint32_t)sector[FAT_SERIAL_OFFSET] |
                            ((uint32_t)sector[FAT_SERIAL_OFFSET + 1] << 8) |
                            ((uint32_t)sector[FAT_SERIAL_OFFSET + 2] << 16) |
                            ((uint32_t)sector[FAT_SERIAL_OFFSET + 3] << 24);

    if (serial == 0) {
        return;     /* nothing to identify; a zero would match every other zero */
    }

    snprintf(out, out_len, "%04X-%04X", (unsigned)(serial >> 16),
             (unsigned)(serial & 0xFFFFu));
}

/* Linux's PARTUUID: the disk signature, a dash, and the entry number. */
static void partuuid_format(char *out, size_t out_len, uint32_t disk_id,
                            unsigned entry)
{
    if (out == NULL || out_len == 0 || disk_id == 0) {
        return;
    }

    snprintf(out, out_len, "%08x-%02u", (unsigned)disk_id, entry);
}

/*
 * An ISO 9660 primary volume descriptor, in the block given: byte 0 is the type
 * code (1 = primary), bytes 1-5 the magic "CD001", and the volume identifier at
 * 40 -- the volume's own name for itself, which is what makes an installer
 * recognisable ("COS_202604") rather than a 2.8G mystery.
 */
static bool iso_descriptor(const uint8_t *block, char *out_label, size_t out_len)
{
    if (block[0] != 1 || memcmp(block + ISO_MAGIC_OFFSET, "CD001", 5) != 0) {
        return false;
    }
    label_copy((const char *)block + ISO_LABEL_OFFSET, ISO_LABEL_LEN,
               out_label, out_len);
    return true;
}

/*
 * The same, from a block the caller has not read. 32768 is a multiple of every
 * sector size a block device here reports -- 512, 1024, 2048, 4096 -- so the
 * read is aligned wherever it is asked for, which is not true of the offset the
 * ext probe uses.
 */
static bool iso_descriptor_read(usb_dev_t *d, size_t unit, uint64_t offset,
                                uint8_t *buf, char *out_label, size_t out_len)
{
    if (d->bdl->ops->read(d->bdl, buf, unit, offset, unit) != ESP_OK) {
        return false;
    }
    return iso_descriptor(buf, out_label, out_len);
}

/*
 * Does the ext2/3/4 superblock sit in this region?
 *
 * The superblock begins 1024 bytes into the volume and its `s_magic` is 0xEF53 at
 * offset 56 of it -- bytes 1080 and 1081, little-endian 53 EF. Where those bytes
 * actually are depends on the sector size, and that is the whole subtlety:
 *
 *   - 512 or 1024 byte sectors: they are in the *second* block, so the caller's
 *     buffer has to be refilled from offset 1024. That address is a multiple of
 *     both, so the read is aligned, which is what a block device requires.
 *   - 2048 or 4096: they are already inside the block the caller read, and a read
 *     at 1024 would be *unaligned* -- the same trap that makes the library's own
 *     esp_ext_part_list_bdl_read() unusable on a 4K-sector disk.
 *
 * `buf` is clobbered in the first case, which is why block_fstype() checks every
 * signature that lives in the first 512 bytes -- FAT, exFAT, NTFS and the ISO
 * descriptor -- before calling this.
 */
static bool ext_region_is_ext(usb_dev_t *d, uint64_t base, size_t unit,
                              uint8_t *buf)
{
    /* Where an ext volume keeps its superblock, and the two fields of it that say
     * so: s_magic (0xEF53) and s_log_block_size, which is the block size as a
     * power of two and cannot exceed 6 -- 64 KiB -- on any ext ever built. */
    const size_t super     = 1024;
    const size_t magic     = 56;
    const size_t block_log = 24;

    const bool in_this_block = unit >= super + block_log + 4;

    if (!in_this_block &&
        d->bdl->ops->read(d->bdl, buf, unit, base + super, unit) != ESP_OK) {
        return false;
    }

    /* Either the superblock is in the block we were given, or the read above
     * replaced it with the block it starts in. */
    const uint8_t *sb = in_this_block ? buf + super : buf;

    if (sb[magic] != 0x53 || sb[magic + 1] != 0xEF) {
        return false;
    }

    /*
     * The magic alone is two bytes, and two bytes turn up in data that is not ext
     * once in 65536. The block size beside it has to be a power of two no larger
     * than 64 KiB, which is the check libblkid makes too; that is the difference
     * between naming somebody's volume and guessing at it.
     */
    const uint32_t log2_block = (uint32_t)sb[block_log] |
                                ((uint32_t)sb[block_log + 1] << 8) |
                                ((uint32_t)sb[block_log + 2] << 16) |
                                ((uint32_t)sb[block_log + 3] << 24);
    return log2_block <= 6;
}

/*
 * What filesystem the block given belongs to, or "" for one nothing here can
 * name. Called for a whole device whose sector 0 is a filesystem's own boot
 * sector rather than an MBR -- a "superfloppy", which is how appliance-formatted
 * sticks arrive -- and for each partition of a device that has a table, because a
 * type byte is a hint and this is the evidence.
 *
 * FAT is read, label and all; exFAT, NTFS, ext and ISO 9660 are named, because
 * naming what cannot be read is the point of the whole report.
 *
 * `block` holds the region's first block and may be refilled: the ext probe reads
 * a second block on sector sizes that cannot hold its superblock, so every check
 * that needs only the first bytes -- including the ISO's -- runs before it.
 */
/*
 * BitLocker's own identifier, a fixed 16-byte GUID libbde's bde_volume.h names
 * bde_identifier -- used by the "Windows 7" (fixed-disk) and "To Go"
 * (removable-media) volume layouts alike, at a different fixed offset in
 * each. bde_identifier_used_disk_space_only, a Windows 8+ mode that only
 * encrypts the space actually in use, is a second, equally fixed GUID at the
 * same two offsets and is checked for too, since there is no way to tell in
 * advance which mode formatted a given drive.
 *
 * The offsets are the sum of every field ahead of `identifier` in each struct
 * (boot_entry_point through bootcode): 160 for windows_7, 424 for to_go. Both
 * fit inside the 512-byte sector already in hand, so no extra read is needed.
 */
#define BITLOCKER_ID_OFFSET_FIXED     160
#define BITLOCKER_ID_OFFSET_REMOVABLE 424
#define BITLOCKER_ID_LEN              16

static const uint8_t BITLOCKER_GUID[BITLOCKER_ID_LEN] = {
    0x3b, 0xd6, 0x67, 0x49, 0x29, 0x2e, 0xd8, 0x4a,
    0x83, 0x99, 0xf6, 0xa3, 0x39, 0xe3, 0xd0, 0x01,
};
static const uint8_t BITLOCKER_GUID_USED_SPACE_ONLY[BITLOCKER_ID_LEN] = {
    0x3b, 0x4d, 0xa8, 0x92, 0x80, 0xdd, 0x0e, 0x4d,
    0x9e, 0x4e, 0xb1, 0xe3, 0x28, 0x4e, 0xae, 0xd8,
};

static bool bitlocker_identifier(const uint8_t *block)
{
    return memcmp(block + BITLOCKER_ID_OFFSET_FIXED, BITLOCKER_GUID,
                  BITLOCKER_ID_LEN) == 0 ||
           memcmp(block + BITLOCKER_ID_OFFSET_FIXED, BITLOCKER_GUID_USED_SPACE_ONLY,
                  BITLOCKER_ID_LEN) == 0 ||
           memcmp(block + BITLOCKER_ID_OFFSET_REMOVABLE, BITLOCKER_GUID,
                  BITLOCKER_ID_LEN) == 0 ||
           memcmp(block + BITLOCKER_ID_OFFSET_REMOVABLE, BITLOCKER_GUID_USED_SPACE_ONLY,
                  BITLOCKER_ID_LEN) == 0;
}

static const char *block_fstype(usb_dev_t *d, uint64_t base, uint8_t *block,
                                size_t unit, bool *foreign)
{
    /*
     * Checked before the FAT probe below, deliberately: "BitLocker To Go" --
     * what `manage-bde` writes on a password-protected removable drive --
     * is a lookalike FAT32 boot sector on purpose. Microsoft's own format
     * (libbde's bde_volume.h, which this was checked against) keeps the real
     * FAT32 layout intact -- volume label at 0x2B/0x47, "FAT32   " at 0x52,
     * both exactly where fat_label_offset() looks -- so that a BitLocker-
     * unaware OS reads it as a formatted FAT32 volume rather than refusing
     * it outright. Only past the boot code, at a fixed offset that differs
     * between the fixed-disk and removable-media layouts, does a 16-byte GUID
     * replace what would otherwise be more boot code. That GUID is the only
     * honest way to tell the two apart, and this is the case that found it: a
     * BitLocker-encrypted 512G partition on a real disk read as `vfat`.
     */
    if (bitlocker_identifier(block)) {
        *foreign = true;
        return "bitlocker";
    }
    if (fat_label_offset(block) != 0) {
        return "vfat";
    }
    /* The OEM-name field doubles as the signature for both of these. */
    if (memcmp(block + 3, "EXFAT   ", 8) == 0) {
        *foreign = true;
        return "exfat";
    }
    if (memcmp(block + 3, "NTFS    ", 8) == 0) {
        *foreign = true;
        return "ntfs";
    }
    /*
     * An ISO's descriptor is at 32768, not here -- except in the case that found
     * this: a hybrid image's partition begins exactly at its own descriptor, which
     * is the 2.8G entry of an Arch or CachyOS installer whose type byte is 0x00
     * and whose content is the only thing that names it.
     */
    if (iso_descriptor(block, NULL, 0)) {
        *foreign = true;
        return "iso9660";
    }
    /*
     * ext2/3/4, and the reason a Linux-prepared stick used to read as a bare
     * disk: ext keeps its superblock at 1024, so a whole-device ext volume has
     * nothing at all in sector 0 for any of the checks above to find. Named
     * without claiming a version, because s_magic does not distinguish 2, 3 and 4
     * -- the feature flags do -- and "ext4" would be a guess. Last, because this
     * is the check that may refill the buffer.
     */
    if (ext_region_is_ext(d, base, unit, block)) {
        *foreign = true;
        return "ext2/3/4";
    }
    return "";
}

/*
 * A partition's name: the disk's, then the entry's number. Numbered by the
 * *entry* rather than by the row, so a table with a gap keeps its numbering and
 * espix agrees with `lsblk`, `fdisk` and `blkid` on the other side of the cable
 * instead of quietly renumbering itself.
 *
 * Written out rather than formatted: snprintf("%s%u") into an eight-byte buffer
 * is what -Wformat-truncation exists to flag, and it is right to. Three digits
 * is all an index needs -- an MBR holds four entries and a GPT array 128.
 */
static void part_name(char *dst, size_t dst_len, const char *disk, uint32_t entry)
{
    if (dst == NULL || dst_len == 0) {
        return;
    }

    size_t n = 0;
    while (n < 3 && n + 1 < dst_len && disk[n] != '\0') {
        dst[n] = disk[n];
        n++;
    }

    char digits[3];
    size_t nd = 0;
    while (entry > 0 && nd < sizeof(digits)) {
        digits[nd++] = (char)('0' + (int)(entry % 10));
        entry /= 10;
    }
    while (nd > 0 && n + 1 < dst_len) {
        dst[n++] = digits[--nd];
    }
    dst[n] = '\0';
}

/*
 * Validates the entry array against the header's own checksum (GPTH_PtBcc),
 * a whole-array CRC32 the UEFI specification defines for exactly this
 * purpose. FatFs checks the *header's* own checksum before trusting it
 * (test_gpt_header, ff.c) but never reads this one back, even though its own
 * formatter writes it -- so this is stricter than FatFs, not merely matching
 * it.
 *
 * Written because of a real 3.6TB Samsung T9: the walk below produced four
 * entries that failed its per-entry sanity checks, on a disk whose same four
 * LBAs read back as nothing but zeros under a direct, independent read from
 * a different host. What produced those four entries in espix's own read was
 * not pinned down -- the BOT status wrapper's dataResidue field already
 * catches a genuine short SCSI transfer before it would ever reach here, and
 * every offset this walk computed checked out exactly against the real
 * header's own fields once decoded by hand. Whichever it was, a checksum
 * that does not match the one the table carries for itself is reason enough
 * not to build a single row from it -- which also closes a case the walk's
 * own per-entry checks cannot: a padding-region entry that happens to pass
 * every one of them by coincidence would otherwise become a phantom
 * partition with no warning at all.
 *
 * A read failure here is reported by the caller exactly as a read failure
 * during the walk is; this only decides whether the walk should trust what
 * it is about to read, not whether the read itself succeeded.
 */
static bool gpt_entries_crc_ok(usb_dev_t *d, uint8_t *buf, size_t unit,
                               uint64_t table_lba, uint32_t entries,
                               uint32_t entry_size, uint32_t stored_crc)
{
    /*
     * esp_rom_crc.h documents its own chaining recipe: pre-invert the seed
     * once, thread the running value between calls with no per-chunk
     * inversion, invert once at the end, then XOR by the named algorithm's
     * xorout. GPT's checksum is the ordinary CRC-32 (init=0xFFFFFFFF,
     * xorout=0xFFFFFFFF -- the same one FatFs's own byte-at-a-time crc32()
     * implements in ff.c, poly 0xEDB88320 reflected), and for that specific
     * pair the pre-invert of the seed and the final invert-then-xorout
     * cancel exactly: ~(0xFFFFFFFF) is 0, and (~crc) ^ 0xFFFFFFFF is crc
     * unchanged. So the seed starts at 0 and the raw running value is
     * compared directly -- no inversion anywhere in this function.
     *
     * Got this wrong the first time by starting at 0xFFFFFFFF and inverting
     * at the end, which is the recipe for a *different* pair of
     * (init, xorout) values, not this one -- and it rejected a real GPT
     * table's genuinely valid entry array on the first attempt.
     */
    const uint64_t total_bytes = (uint64_t)entries * entry_size;
    const uint64_t sectors = (total_bytes + unit - 1) / unit;
    uint32_t crc = 0;
    uint64_t done = 0;

    for (uint64_t s = 0; s < sectors; s++) {
        if (d->bdl->ops->read(d->bdl, buf, unit, (table_lba + s) * unit,
                              unit) != ESP_OK) {
            return false;
        }
        const uint64_t remaining = total_bytes - done;
        const size_t   take      = remaining < unit ? (size_t)remaining : unit;
        crc = esp_rom_crc32_le(crc, buf, take);
        done += take;
    }

    return crc == stored_crc;
}

/*
 * A GPT disk, whose partition table is at LBA 1 rather than in sector 0.
 *
 * This is the case that turned up as a 3.6TB Samsung T9: three partitions and a
 * 3.1TB exFAT volume among them, arriving behind a *protective* MBR, so the walk
 * below found one entry typed 0xEE covering the whole disk and described a 4TB
 * drive as a single unreadable partition. The protective entry is honest about
 * sector 0 and useless as a description of the disk.
 *
 * The test is the GPT header's own signature, not the 0xEE entry: an installer
 * image that carries both tables is a GPT disk with a hybrid MBR, and following
 * its protective entry would be just as wrong there. By the time this is called,
 * sector 0 has an MBR signature, so the disk has one table or the other.
 *
 * The entry array is up to 16K and is walked a sector at a time in the caller's
 * buffer. An array holds 128 entries; a disk with two or three partitions that
 * matter is the ordinary case, and allocating the whole array to read them would
 * be 16K of heap for nothing. That is also why an entry's GUIDs are read out
 * before the partition probe refills the buffer they came in.
 *
 * The array is checked against the header's own checksum before any of that --
 * see gpt_entries_crc_ok() -- so a table that fails it is never walked at all,
 * rather than producing rows this cannot otherwise tell apart from real ones.
 *
 * Returns true when the disk is a GPT disk and has been handled here -- including
 * when its header, or its entry array's checksum, was refused, because the MBR
 * walk has nothing better to say about a disk whose real table is the one this
 * just rejected.
 */
static bool gpt_read(usb_dev_t *d, uint8_t *buf, size_t unit)
{
    if (unit < GPT_ENTRY_SIZE) {
        return false;       /* an entry would straddle sectors */
    }

    if (d->bdl->ops->read(d->bdl, buf, unit, (uint64_t)GPT_HEADER_LBA * unit,
                          unit) != ESP_OK) {
        return false;
    }
    if (memcmp(buf, GPT_SIGNATURE, sizeof(GPT_SIGNATURE) - 1) != 0) {
        return false;
    }

    const uint64_t table_lba  = entry_u64(buf, GPT_HDR_PT_LBA);
    const uint32_t entries    = entry_u32(buf, GPT_HDR_PT_COUNT);
    const uint32_t entry_size = entry_u32(buf, GPT_HDR_ENTRY_SIZE);
    const uint32_t entries_crc = entry_u32(buf, GPT_HDR_PT_BCC);

    /*
     * The two rules FatFs applies to a GPT header before reading its table, and
     * for the same reasons: an entry size other than 128 bytes would run past the
     * entry it describes, and an array longer than the specification's 128 is a
     * header to refuse rather than to walk off the end of.
     */
    if (entry_size != GPT_ENTRY_SIZE || entries == 0 || entries > GPT_ENTRIES_MAX) {
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "%s: a GPT table this cannot walk (%u entries of %u bytes)",
                   d->info.name, (unsigned)entries, (unsigned)entry_size);
        d->info.table_skipped = true;
        return true;
    }

    /*
     * A whole-array read, purely to add its bytes into a running CRC32 --
     * `buf` is not kept past this, and the walk below reads every sector of
     * the array again as it goes. Reading it twice costs nothing worth
     * avoiding (16K at most, and only once per attach) next to what a wrong
     * table would cost: a phantom partition, or a real one silently missing.
     * See gpt_entries_crc_ok().
     */
    if (!gpt_entries_crc_ok(d, buf, unit, table_lba, entries, entry_size,
                           entries_crc)) {
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "%s: GPT entry array checksum does not match the header; "
                   "not walked", d->info.name);
        d->info.table_skipped = true;
        return true;
    }

    uint64_t loaded = UINT64_MAX;       /* which entry-array sector is in `buf` */

    for (uint32_t i = 0; i < entries; i++) {
        const uint64_t offset = (uint64_t)i * entry_size;
        const uint64_t lba    = table_lba + offset / unit;
        const size_t   at     = (size_t)(offset % unit);

        if (lba != loaded) {
            if (d->bdl->ops->read(d->bdl, buf, unit, lba * unit, unit) != ESP_OK) {
                espix_klog(ESPIX_KLOG_WARN, TAG,
                           "%s: cannot read the GPT entry array", d->info.name);
                d->info.table_skipped = true;
                break;
            }
            loaded = lba;
        }

        const uint8_t *entry = buf + at;

        /*
         * A slot with no type GUID is one the table never used, which is most of
         * a 128-entry array. The table ends when the entries do, not when a zero
         * appears -- the same distinction the MBR walk makes between an empty
         * entry and a 0x00 type byte, and for the same reason: a table can carry
         * a zeroed field inside an entry that is in use.
         */
        bool used = false;
        for (size_t b = 0; b < 16 && !used; b++) {
            used = entry[GPT_ENTRY_TYPE_GUID + b] != 0;
        }
        if (!used) {
            continue;
        }

        const uint64_t first = entry_u64(entry, GPT_ENTRY_FIRST_LBA);
        const uint64_t last  = entry_u64(entry, GPT_ENTRY_LAST_LBA);

        /* LBAs read off somebody's disk, checked before they are multiplied, so
         * a nonsense pair cannot wrap into a plausible-looking row. */
        if (last < first) {
            espix_klog(ESPIX_KLOG_WARN, TAG,
                       "%s: GPT entry %u ends before it starts",
                       d->info.name, (unsigned)(i + 1));
            d->info.table_skipped = true;
            continue;
        }

        const uint64_t start = first * unit;
        const uint64_t size  = (last - first + 1) * unit;

        if (d->info.size > 0 &&
            (start >= d->info.size || size > d->info.size - start)) {
            espix_klog(ESPIX_KLOG_WARN, TAG,
                       "%s: GPT entry %u is not inside the device",
                       d->info.name, (unsigned)(i + 1));
            d->info.table_skipped = true;
            continue;
        }

        /*
         * Four slots is espix's ceiling, not the table's. A disk with more
         * partitions than that lists the first four and is counted as incomplete,
         * because a script reading sda1..sda4 would otherwise never learn that
         * there was an sda5.
         */
        if (d->info.nparts >= ESPIX_USB_MAX_PARTS) {
            d->info.table_skipped = true;
            continue;
        }

        espix_usb_part_t *p = &d->info.parts[d->info.nparts];

        part_name(p->name, sizeof(p->name), d->info.name, i + 1);
        p->start   = start;
        p->size    = size;
        p->foreign = false;
        guid_format(p->partuuid, sizeof(p->partuuid), entry + GPT_ENTRY_UUID);
        copy_str(p->fstype, sizeof(p->fstype), gpt_type_name(entry, &p->foreign));

        /*
         * Then the partition's own first block, exactly as the MBR walk reads it
         * and for the same reason: the type GUID says what the partition is meant
         * for and the boot sector says what is on it, and where the two disagree
         * the content is what to believe. Both GUIDs are already out of the entry
         * by here, because this read refills the buffer they came in.
         */
        if (d->bdl->ops->read(d->bdl, buf, unit, start, unit) == ESP_OK) {
            bool foreign = false;
            const char *content = block_fstype(d, start, buf, unit, &foreign);

            if (content[0] != '\0') {
                copy_str(p->fstype, sizeof(p->fstype), content);
                p->foreign = foreign;

                if (strcmp(content, "vfat") == 0) {
                    fat_label(buf, p->label, sizeof(p->label));
                    fat_serial(buf, p->uuid, sizeof(p->uuid));
                } else if (strcmp(content, "iso9660") == 0) {
                    (void)iso_descriptor(buf, p->label, sizeof(p->label));
                }
            }
        }

        /*
         * The probe above refilled `buf`, so the entry-array sector that was in
         * it is gone: forget which one it was, or the next entry of a
         * four-entries-per-sector array gets read out of a partition's boot
         * sector. The same trap the MBR walk copies its table out of the way to
         * avoid, met from the other side — and it hid two of the three
         * partitions on the first real GPT disk this ran against, silently,
         * because a boot sector's zeroes look exactly like unused entries.
         */
        loaded = UINT64_MAX;

        /*
         * A partition holding no filesystem is not one espix failed to name: a
         * Microsoft reserved entry holds none by definition, and an empty EFI
         * partition is an ordinary thing to find on a disk prepared by Windows.
         * It takes a type GUID nothing knows *and* content nothing recognises to
         * leave the column empty, and that is a fact about the disk worth
         * admitting to.
         */
        if (p->fstype[0] == '\0') {
            d->info.table_skipped = true;
        }
        d->info.nparts++;
    }

    return true;
}

/*
 * Sector 0 -- or, on a GPT disk, LBA 1 and the entry array behind it -- then one
 * more read per partition for its own first block and its label.
 *
 * The read goes through the block device, whose geometry asks for whole sectors
 * at aligned addresses, and the buffer ends up on the USB wire unchanged -- so it
 * is internal, DMA-capable memory. External RAM cannot be reached by the S3's USB
 * DMA engine; the P4's can, which is what
 * CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM enables, and that option exists
 * only for the P4 for exactly this reason.
 *
 * A disk with no MBR signature is not an error and gets no log line: a stick
 * formatted without a partition table -- a "superfloppy" -- is an ordinary thing
 * to find in a drawer, and it still appears in `lsblk` as a bare disk.
 */
static void read_partition_table(usb_dev_t *d)
{
    if (d->bdl == NULL || d->bdl->ops == NULL || d->bdl->ops->read == NULL) {
        return;
    }

    const size_t unit = d->bdl->geometry.read_size;
    if (unit == 0 || unit > SECTOR_MAX) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "%s: refusing a %u byte sector size",
                   d->info.name, (unsigned)unit);
        return;
    }

    uint8_t *sector = heap_caps_malloc(unit, MALLOC_CAP_DMA);
    if (sector == NULL) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "%s: no memory for sector 0",
                   d->info.name);
        return;
    }

    const esp_err_t err = d->bdl->ops->read(d->bdl, sector, unit, 0, unit);
    if (err != ESP_OK) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "%s: cannot read sector 0: %s",
                   d->info.name, esp_err_to_name(err));
        free(sector);
        return;
    }

    d->info.table_read = true;

    if (!has_mbr_signature(sector)) {
        /*
         * No partition table -- but sector 0 may still be a filesystem. That is a
         * "superfloppy": one volume covering the whole device, which is how
         * appliance-formatted sticks arrive and how most of them used to ship.
         * The filesystem belongs to the disk row rather than to a partition,
         * which is where `lsblk` puts it too. FAT is readable here (label and
         * all); exFAT, NTFS, ext and ISO 9660 are only named, because naming what
         * cannot be read is the point of this report.
         */
        bool foreign = false;
        const char *type = block_fstype(d, 0, sector, unit, &foreign);

        /*
         * A pure ISO -- written to a stick with no partition table at all -- keeps
         * nothing in sector 0, because its descriptor is 32768 bytes in, where the
         * ISO's logical sector 16 lives. One read, and only when nothing above
         * recognised the sector.
         */
        if (type[0] == '\0' &&
            iso_descriptor_read(d, unit, ISO_PVD_OFFSET, sector,
                                d->info.label, sizeof(d->info.label))) {
            type = "iso9660";
            foreign = true;
        }

        if (type[0] != '\0') {
            copy_str(d->info.fstype, sizeof(d->info.fstype), type);
            d->info.foreign = foreign;
            if (strcmp(type, "vfat") == 0) {
                fat_label(sector, d->info.label, sizeof(d->info.label));
                fat_serial(sector, d->info.uuid, sizeof(d->info.uuid));
            }
        }
        free(sector);
        return;
    }

    /*
     * Walked here rather than through esp_mbr_parse(), because of one rule that
     * matters on real media: the library ends the table at a 0x00 *type byte*. A
     * hybrid ISO image -- an Arch or CachyOS installer, whose first entry is typed
     * 0x00 with a real start and size -- then loses every entry after it,
     * including the FAT EFI partition that is the only thing on such a stick espix
     * can mount. Measured on a CachyOS 202604 installer: 2.8G of ISO 9660 typed
     * 0x00, then 23M of EFI FAT typed 0xEF, and espix showed neither. A table ends
     * when an entry is *empty* -- no start and no size -- which is a different
     * thing entirely.
     *
     * The type table is still the library's: see partition_type_name().
     */
    /*
     * Copied out of the MBR before the walk, because the per-partition probes
     * below read into `sector`: the table has to outlive the walk's own use of
     * that buffer, and reading an entry out of a block that has since been
     * refilled is how a partition goes missing.
     */
    uint8_t table[MBR_ENTRIES * MBR_ENTRY_SIZE];
    memcpy(table, sector + MBR_ENTRY_OFFSET, sizeof(table));

    /* PARTUUID's other half: the table's identity at 0x1B8, left zero when it is zero. */
    d->info.disk_id = (uint32_t)sector[MBR_DISK_ID_OFFSET] |
                      ((uint32_t)sector[MBR_DISK_ID_OFFSET + 1] << 8) |
                      ((uint32_t)sector[MBR_DISK_ID_OFFSET + 2] << 16) |
                      ((uint32_t)sector[MBR_DISK_ID_OFFSET + 3] << 24);

    /*
     * A GPT disk, and *after* the two reads above rather than before them: a GPT
     * disk arrives with an MBR signature, because its protective MBR is a real
     * entry typed 0xEE covering the whole disk, so the walk below would report a
     * single unreadable partition and stop. But gpt_read() reads LBA 1 into
     * `sector`, so sector 0 has to be taken out of that buffer first -- and the
     * order is the whole reason this sits down here rather than beside the
     * signature check.
     *
     * The test is the GPT header rather than the 0xEE entry, because a hybrid
     * installer image carries both tables and the GPT one is the real one.
     */
    if (gpt_read(d, sector, unit)) {
        free(sector);
        return;
    }

    for (size_t i = 0; i < MBR_ENTRIES && d->info.nparts < ESPIX_USB_MAX_PARTS; i++) {
        const uint8_t *entry = table + i * MBR_ENTRY_SIZE;
        const uint8_t  raw = entry[MBR_ENTRY_TYPE];
        const uint32_t lba_start = entry_u32(entry, MBR_ENTRY_LBA);
        const uint32_t lba_sectors = entry_u32(entry, MBR_ENTRY_SECTORS);

        if (lba_start == 0 && lba_sectors == 0) {
            break;              /* empty: the table ends here */
        }

        /*
         * Scaled by the medium's own sector size, the convention the library used
         * and the one espix documents. The MBR specification says these counts are
         * in 512-byte units; on every device this has seen the two agree, and
         * assuming 512 on a 4K-sector device would be wrong in a way nothing here
         * could observe.
         */
        const uint64_t start = (uint64_t)lba_start * unit;
        const uint64_t size  = (uint64_t)lba_sectors * unit;

        /*
         * Every number here is data off somebody's stick, so it is checked before
         * it is used: an entry that falls outside the device is not shown, and is
         * counted so the listing can say so.
         */
        if (size == 0 ||
            (d->info.size > 0 &&
             (start >= d->info.size || size > d->info.size - start))) {
            espix_klog(ESPIX_KLOG_WARN, TAG,
                       "%s: entry %u is not inside the device", d->info.name,
                       (unsigned)(i + 1));
            d->info.table_skipped = true;
            continue;
        }

        espix_usb_part_t *p = &d->info.parts[d->info.nparts];

        /* "sda1", numbered by the entry rather than by the row: see part_name(). */
        part_name(p->name, sizeof(p->name), d->info.name, (uint32_t)(i + 1));
        p->start   = start;
        p->size    = size;
        partuuid_format(p->partuuid, sizeof(p->partuuid), d->info.disk_id,
                        (unsigned)(i + 1));
        p->foreign = false;
        copy_str(p->fstype, sizeof(p->fstype),
                 partition_type_name(raw, &p->foreign));

        /*
         * Then the partition's own first block, once, which is the evidence the
         * type byte can only hint at: a FAT boot sector (also how a 0x00-typed
         * entry gets a name at all), an ISO's volume descriptor, or ext's
         * superblock. Where the content is recognised it wins over the byte, since
         * a partition table being slightly wrong is not something to believe.
         */
        if (d->bdl->ops->read(d->bdl, sector, unit, start, unit) == ESP_OK) {
            bool foreign = false;
            const char *content = block_fstype(d, start, sector, unit, &foreign);

            if (content[0] != '\0') {
                copy_str(p->fstype, sizeof(p->fstype), content);
                p->foreign = foreign;

                if (strcmp(content, "vfat") == 0) {
                    fat_label(sector, p->label, sizeof(p->label));
                    fat_serial(sector, p->uuid, sizeof(p->uuid));
                } else if (strcmp(content, "iso9660") == 0) {
                    /* Recognised by block_fstype() a moment ago; this fills the
                     * label out of the same block. */
                    (void)iso_descriptor(sector, p->label, sizeof(p->label));
                }
            }
        }

        /*
         * A row is a row even when nothing can name it: its size is real and worth
         * seeing. It is counted rather than hidden, so `lsblk` can admit that the
         * listing is not the whole table.
         */
        if (p->fstype[0] == '\0') {
            d->info.table_skipped = true;
        }
        d->info.nparts++;
    }

    free(sector);
}

/* Finds a free slot and names it, lowest first. The caller holds the lock.
 *
 * The slot is marked taken here rather than at publish: two callers must not be
 * able to claim the same one, and slot_by_addr() finds an attach already in
 * progress by this flag. `info.in_use` is the separate, later flag that makes the
 * entry visible to sessions -- taking a slot and publishing it are not the same
 * event, and conflating them was a bug: only the public flag was ever set, so
 * every attach landed in slot 0 and a second device would have replaced the
 * first. */
static usb_dev_t *slot_claim(uint8_t addr)
{
    for (size_t i = 0; i < ESPIX_USB_MAX_DEVS; i++) {
        if (!s_devs[i].in_use) {
            s_devs[i].in_use = true;
            s_devs[i].addr = addr;
            snprintf(s_devs[i].info.name, sizeof(s_devs[i].info.name),
                     "sd%c", (char)('a' + i));
            return &s_devs[i];
        }
    }
    return NULL;
}

/* The slot holding this USB address, or NULL. The caller holds the lock. */
static usb_dev_t *slot_by_addr(uint8_t addr)
{
    for (size_t i = 0; i < ESPIX_USB_MAX_DEVS; i++) {
        if (s_devs[i].in_use && s_devs[i].addr == addr) {
            return &s_devs[i];
        }
    }
    return NULL;
}

/* Empties a slot and hands back what it held. The caller holds the lock. */
static void slot_release(usb_dev_t *d, msc_host_device_handle_t *device,
                         esp_blockdev_handle_t *bdl, char *name, size_t name_len)
{
    *device = d->device;
    *bdl = d->bdl;
    copy_str(name, name_len, d->info.name);
    memset(d, 0, sizeof(*d));
}

/*
 * Does this device have an interface the MSC driver would claim?
 *
 * ESP_OK when it does, ESP_ERR_NOT_FOUND when it does not, and anything else
 * when the descriptors could not be read at all -- which is worth telling apart,
 * because "no storage here" and "could not look" mean different things to the
 * person reading dmesg.
 *
 * Every caller checks before handing a device to the driver, and that is not
 * politeness: extract_config_from_descriptor() asserts on this lookup
 * (msc_host.c:228), so a device with no bulk-only interface aborts the board.
 * `usbprobe 1`, aimed at the hub, would do exactly that.
 *
 * The whole active configuration is walked, not the first few interfaces: a
 * device with four video interfaces and storage on the fifth is still storage.
 */
static esp_err_t addr_msc_interface(uint8_t addr)
{
    if (s_monitor == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_open_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    usb_device_handle_t dev = NULL;
    esp_err_t err = usb_host_device_open(s_monitor, addr, &dev);
    if (err != ESP_OK) {
        xSemaphoreGive(s_open_lock);
        return err;
    }

    const usb_config_desc_t *cd = NULL;
    bool yes = false;

    if (usb_host_get_active_config_descriptor(dev, &cd) == ESP_OK && cd != NULL) {
        int offset = 0;
        const usb_standard_desc_t *next = (const usb_standard_desc_t *)cd;

        while ((next = usb_parse_next_descriptor_of_type(
                    next, cd->wTotalLength, USB_B_DESCRIPTOR_TYPE_INTERFACE,
                    &offset)) != NULL) {
            const usb_intf_desc_t *ifc = (const usb_intf_desc_t *)next;

            if (espix_usb_iface_is_msc_bot(ifc->bInterfaceClass,
                                           ifc->bInterfaceSubClass,
                                           ifc->bInterfaceProtocol)) {
                yes = true;
                break;
            }
        }
        err = yes ? ESP_OK : ESP_ERR_NOT_FOUND;
    } else {
        err = ESP_ERR_INVALID_RESPONSE;
    }

    if (usb_host_device_close(s_monitor, dev) != ESP_OK) {
        /* An unreleased open reference is how a device object ends up never
         * being freed, and the address it held never being reusable. */
        espix_klog(ESPIX_KLOG_WARN, TAG, "addr %u: close failed", addr);
    }
    xSemaphoreGive(s_open_lock);

    return err;
}

/*
 * A device arrived: open it, find out what it is, read its table, publish it.
 *
 * Three callers: the MSC driver's callback (the normal path), `usbprobe`, and
 * `usbscan`. The last two exist because the first is not trusted to be reached at
 * all -- if the driver's event delivery is broken, storage still has to work, and
 * the device pool can be polled instead.
 *
 * The slot is filled before it is marked in use, so a session running `lsblk`
 * during the few hundred milliseconds this takes sees either nothing or the whole
 * disk -- never half of one.
 */
static esp_err_t attach_device(uint8_t address)
{
    /*
     * One at a time. Installing a device claims its interface, and two callers
     * racing for the same address would leave the loser holding a claim it does
     * not know about -- which in turn stops the device ever being freed.
     */
    xSemaphoreTake(s_attach_lock, portMAX_DELAY);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool known = (slot_by_addr(address) != NULL);
    xSemaphoreGive(s_lock);
    if (known) {
        xSemaphoreGive(s_attach_lock);
        return ESP_OK;
    }

    /*
     * Refuse anything the class driver cannot claim. This is the difference
     * between `usbprobe 1` reporting "not storage" and the board aborting on the
     * driver's assert.
     */
    const esp_err_t msc = addr_msc_interface(address);
    if (msc != ESP_OK) {
        espix_klog(msc == ESP_ERR_NOT_FOUND ? ESPIX_KLOG_INFO : ESPIX_KLOG_WARN, TAG,
                   msc == ESP_ERR_NOT_FOUND
                       ? "addr %u: no mass-storage interface"
                       : "addr %u: cannot read its interfaces: %s",
                   address, esp_err_to_name(msc));
        xSemaphoreGive(s_attach_lock);
        return (msc == ESP_ERR_NOT_FOUND) ? ESP_ERR_NOT_SUPPORTED : msc;
    }

    msc_host_device_handle_t device = NULL;
    esp_err_t err = msc_host_install_device(address, &device);
    if (err != ESP_OK) {
        /*
         * ESP_ERR_NOT_SUPPORTED from here is worth naming, because it is not a
         * fault: hcd_pipe_alloc() returns it for one reason only, "no more free
         * channels" (hcd_dwc.c:2119). The S3's host core has a fixed number of
         * them, every open device costs a control pipe, a hub costs one more for
         * its interrupt endpoint, and a bulk-only storage device costs two more
         * -- so the second drive behind a hub is refused while the first works,
         * which reads like a broken device until it is spelled out.
         */
        if (err == ESP_ERR_NOT_SUPPORTED) {
            espix_klog(ESPIX_KLOG_WARN, TAG,
                       "device at address %u: no free host channels "
                       "(hcd_pipe_alloc); a second storage device does not fit",
                       address);
        } else {
            espix_klog(ESPIX_KLOG_WARN, TAG,
                       "device at address %u: msc_host_install_device: %s",
                       address, esp_err_to_name(err));
        }
        xSemaphoreGive(s_attach_lock);
        return err;
    }

    msc_host_device_info_t info;
    err = msc_host_get_device_info(device, &info);
    if (err != ESP_OK) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "no device info: %s", esp_err_to_name(err));
        msc_host_uninstall_device(device);
        xSemaphoreGive(s_attach_lock);
        return err;
    }

    /* espix's own handle rather than the one the driver keeps: this is the one a
     * mount would read through, and holding it does not tie up the driver's. */
    esp_blockdev_handle_t bdl = NULL;
    err = msc_host_get_blockdev(device, &bdl);
    if (err != ESP_OK) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "no block device: %s", esp_err_to_name(err));
        msc_host_uninstall_device(device);
        xSemaphoreGive(s_attach_lock);
        return err;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    usb_dev_t *d = slot_claim(address);
    if (d != NULL) {
        d->device = device;
        d->bdl = bdl;
        d->info.sector_size = info.sector_size;
        d->info.size = (uint64_t)info.sector_count * info.sector_size;
        d->info.id_vendor = info.idVendor;
        d->info.id_product = info.idProduct;
        wstr_to_utf8(info.iManufacturer, d->info.manufacturer,
                     sizeof(d->info.manufacturer));
        wstr_to_utf8(info.iProduct, d->info.product, sizeof(d->info.product));
        wstr_to_utf8(info.iSerialNumber, d->info.serial, sizeof(d->info.serial));
    }
    xSemaphoreGive(s_lock);

    if (d == NULL) {
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "more than %d devices attached; ignoring this one",
                   ESPIX_USB_MAX_DEVS);
        msc_host_release_blockdev(bdl);
        msc_host_uninstall_device(device);
        xSemaphoreGive(s_attach_lock);
        return ESP_ERR_NO_MEM;
    }

    read_partition_table(d);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    d->info.in_use = true;
    xSemaphoreGive(s_lock);

    espix_klog(ESPIX_KLOG_INFO, TAG,
               "%s: addr %u %s (%04x:%04x) %" PRIu64 " bytes, %u partition%s",
               d->info.name, address,
               d->info.product[0] ? d->info.product : "unnamed",
               info.idVendor, info.idProduct, d->info.size,
               (unsigned)d->info.nparts, d->info.nparts == 1 ? "" : "s");

    /* After publishing, so whatever the hook registers is usable the moment the
     * attach is over -- and before the lock goes, so the row cannot change. */
    espix_usb_dev_hook_fire(&d->info, true);

    xSemaphoreGive(s_attach_lock);
    return ESP_OK;
}

static void on_connected(uint8_t address)
{
    /*
     * Hand the address on and return immediately. See the work-queue comment
     * above: an install started from here waits on completions that only this
     * call stack could deliver, and stalls for minutes per device.
     */
    if (s_work == NULL || xQueueSend(s_work, &address, 0) != pdTRUE) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "addr %u: no room to queue it", address);
    }
}

static void work_task(void *arg)
{
    (void)arg;

    for (;;) {
        uint8_t address = 0;

        if (xQueueReceive(s_work, &address, SCAN_PERIOD_TICKS) == pdTRUE) {
            /* The driver's event loop is free here, so its transfers complete
             * and the install runs at device speed rather than at its timeouts. */
            (void)attach_device(address);
            continue;
        }

        /*
         * Nothing was queued, so sweep the pool: insurance against an event that
         * never arrived, and the reason a device is claimed whether or not anyone
         * announced it. The sweep is cheap -- the address list is a pool scan,
         * and only a device with no slot yet is opened to look at its
         * interfaces.
         *
         * Here rather than in the host task, and that matters: attach blocks on
         * the class driver's transfers, which are serviced by the library the
         * host task drives. Doing it there would deadlock the port instead.
         */
        (void)espix_usb_host_scan_pool();
    }
}

/*
 * A device left, or was pulled without warning -- the driver reports both the
 * same way. Our block device handle is released first: it does not own the
 * device, and once the device is uninstalled there is nothing left to read.
 */
static void on_disconnected(msc_host_device_handle_t device)
{
    msc_host_device_handle_t gone = NULL;
    esp_blockdev_handle_t bdl = NULL;
    char name[ESPIX_USB_NAME_MAX] = "";

    xSemaphoreTake(s_attach_lock, portMAX_DELAY);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < ESPIX_USB_MAX_DEVS; i++) {
        if (s_devs[i].device == device) {
            /* While the row is still whole: slot_release() clears it, and the
             * hook is what takes the names back out of /dev. */
            espix_usb_dev_hook_fire(&s_devs[i].info, false);
            slot_release(&s_devs[i], &gone, &bdl, name, sizeof(name));
            break;
        }
    }
    xSemaphoreGive(s_lock);

    if (bdl != NULL) {
        msc_host_release_blockdev(bdl);
    }

    /*
     * Uninstalled whether or not this table knew about it. A device the driver
     * installed and we did not -- an attach that failed after the install, say --
     * still holds a claimed interface, and a claimed interface means
     * usb_host_device_close() refuses and the device is never freed. One leaked
     * reference poisons that address for the rest of the boot, so the one case
     * that must not happen is leaving it installed out of tidiness.
     */
    const esp_err_t err = msc_host_uninstall_device(device);
    if (gone == NULL) {
        espix_klog(err == ESP_OK ? ESPIX_KLOG_WARN : ESPIX_KLOG_DEBUG, TAG,
                   err == ESP_OK
                       ? "device %p was installed without a slot; released"
                       : "device %p was not ours to uninstall",
                   (void *)device);
    } else {
        espix_klog(ESPIX_KLOG_INFO, TAG, "%s: removed", name);
    }

    xSemaphoreGive(s_attach_lock);
}

static void on_msc_event(const msc_host_event_t *event, void *arg)
{
    (void)arg;

    switch (event->event) {
    case MSC_DEVICE_CONNECTED:
        on_connected(event->device.address);
        break;
    case MSC_DEVICE_DISCONNECTED:
        on_disconnected(event->device.handle);
        break;
    default:
        /* Suspend and resume arrive with the host library's auto-suspend timer,
         * which espix never arms. Ignoring them by name beats a warning that
         * could only ever mean "correctly unused". */
        espix_klog(ESPIX_KLOG_DEBUG, TAG, "device event %d ignored",
                   (int)event->event);
        break;
    }
}

/*
 * The device monitor. Every device the library enumerated is announced here, not
 * just the ones a driver claims -- which is the whole point: without this line a
 * hub, a keyboard or anything else unrecognised is indistinguishable from an
 * empty socket, because nothing else in espix ever hears about it.
 */
static void monitor_remember(uint8_t addr, uint16_t vid, uint16_t pid)
{
    for (size_t i = 0; i < ESPIX_USB_LSUSB_MAX; i++) {
        if (s_seen[i].addr == addr || s_seen[i].addr == 0) {
            s_seen[i].addr = addr;
            s_seen[i].id_vendor = vid;
            s_seen[i].id_product = pid;
            return;
        }
    }
}

static bool monitor_forget(uint8_t addr, uint16_t *vid, uint16_t *pid)
{
    for (size_t i = 0; i < ESPIX_USB_LSUSB_MAX; i++) {
        if (addr != 0 && s_seen[i].addr == addr) {
            *vid = s_seen[i].id_vendor;
            *pid = s_seen[i].id_product;
            s_seen[i].addr = 0;
            return true;
        }
    }
    return false;
}

static void monitor_arrival(uint8_t addr)
{
    if (s_monitor == NULL) {
        return;
    }
    if (xSemaphoreTake(s_open_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }

    usb_device_handle_t dev = NULL;
    const esp_err_t err = usb_host_device_open(s_monitor, addr, &dev);
    if (err != ESP_OK) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "addr %u: cannot open: %s", addr,
                   esp_err_to_name(err));
        xSemaphoreGive(s_open_lock);
        return;
    }

    const usb_device_desc_t *dd = NULL;
    usb_device_info_t info;
    char product[ESPIX_USB_STR_MAX] = "";

    if (usb_host_get_device_descriptor(dev, &dd) != ESP_OK) {
        dd = NULL;
    }
    if (usb_host_device_info(dev, &info) == ESP_OK) {
        str_desc_to_utf8(info.str_desc_product, product, sizeof(product));
    }

    const uint16_t vid = dd ? dd->idVendor : 0;
    const uint16_t pid = dd ? dd->idProduct : 0;
    const uint8_t  cls = dd ? dd->bDeviceClass : 0;
    const char    *name = espix_usb_class_name(cls);

    monitor_remember(addr, vid, pid);

    char text[ESPIX_KLOG_LINE_MAX];
    if (name[0] != '\0' && product[0] != '\0') {
        snprintf(text, sizeof(text), "class %02x (%s) \"%s\"", cls, name, product);
    } else if (name[0] != '\0') {
        snprintf(text, sizeof(text), "class %02x (%s)", cls, name);
    } else if (product[0] != '\0') {
        snprintf(text, sizeof(text), "class %02x \"%s\"", cls, product);
    } else {
        snprintf(text, sizeof(text), "class %02x", cls);
    }

    espix_klog(ESPIX_KLOG_INFO, TAG, "addr %u: %04x:%04x %s", addr, vid, pid, text);

    if (usb_host_device_close(s_monitor, dev) != ESP_OK) {
        /* An open reference left behind means the device object is never freed
         * and the address it held is never reusable. */
        espix_klog(ESPIX_KLOG_WARN, TAG, "addr %u: close failed", addr);
    }
    xSemaphoreGive(s_open_lock);
}

static void monitor_removal(uint8_t addr)
{
    uint16_t vid = 0;
    uint16_t pid = 0;

    if (monitor_forget(addr, &vid, &pid) && vid != 0) {
        espix_klog(ESPIX_KLOG_INFO, TAG, "addr %u removed (%04x:%04x)",
                   addr, vid, pid);
        return;
    }
    espix_klog(ESPIX_KLOG_INFO, TAG, "addr %u removed", addr);
}

static void on_monitor_event(const usb_host_client_event_msg_t *event, void *arg)
{
    (void)arg;

    switch (event->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        monitor_arrival(event->new_dev.address);
        break;
    case USB_HOST_CLIENT_EVENT_DEV_REMOVED:
        /* Reported because the client asked for it. The alternative,
         * DEV_GONE, is only delivered for a device the client holds open, and
         * this client never keeps one. */
        monitor_removal(event->dev_removed.address);
        break;
    default:
        /* Suspend and resume need the host library's auto-suspend timer, which
         * espix does not arm. Ignoring them by name beats a warning that could
         * only ever mean "correctly unused". */
        espix_klog(ESPIX_KLOG_DEBUG, TAG, "monitor event %d ignored",
                   (int)event->event);
        break;
    }
}
/*
 * The host library's event loop, and the only reason this task exists: the
 * library requires one caller of usb_host_lib_handle_events() and does not run
 * one itself. There is no terminal error to handle, because nothing in espix
 * uninstalls the stack.
 */
static void host_task(void *arg)
{
    (void)arg;

    for (;;) {
        uint32_t flags = 0;

        /*
         * A short block rather than portMAX_DELAY, and the monitor drained
         * unconditionally: its events are posted after the library's own, and a
         * client drained only when the library happens to wake would leave a
         * hotplug line waiting for the next port change. Twenty idle wakeups a
         * second buy a log that arrives when the device does.
         */
        const esp_err_t err = usb_host_lib_handle_events(pdMS_TO_TICKS(50), &flags);
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            /* A timeout is the idle case. Anything else means the library is not
             * in a state to be serviced, and spinning on it at priority 4 would
             * starve the shell while it stayed broken. */
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        if (s_monitor != NULL) {
            (void)usb_host_client_handle_events(s_monitor, 0);
        }
        if (flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            espix_klog(ESPIX_KLOG_DEBUG, TAG, "every device released");
        }
    }
}

esp_err_t espix_usb_host_init(void)
{
    if (s_installed) {
        return ESP_OK;
    }

    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_attach_lock == NULL) {
        s_attach_lock = xSemaphoreCreateMutex();
        if (s_attach_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_open_lock == NULL) {
        s_open_lock = xSemaphoreCreateMutex();
        if (s_open_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

#if CONFIG_ESPIX_USB_VERBOSE
    /*
     * The stack's own view, which is where the answer usually is -- and which is
     * invisible without this, because almost everything it says is a DEBUG line.
     * Every tag the host library and the class drivers log under; nothing else
     * in the system is raised, so WiFi and lwip keep their own levels.
     */
    static const char *const tags[] = {
        "USBH", "ENUM", "EXT_HUB", "EXT_PORT", "HUB", "HCD DWC", "USB HOST",
        "USB_MSC",
    };
    for (size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); i++) {
        esp_log_level_set(tags[i], ESP_LOG_DEBUG);
    }
    espix_klog(ESPIX_KLOG_INFO, TAG, "verbose logging on (ESPIX_USB_VERBOSE)");
#endif

    /* intr_flags is the level IDF's own examples ask for, and the PHY is left to
     * the library: no espix target has an external one. */
    const usb_host_config_t host_config = {
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "usb_host_install: %s",
                   esp_err_to_name(err));
        return err;
    }

    if (xTaskCreate(host_task, "usb:host", HOST_TASK_STACK, NULL, HOST_TASK_PRIO,
                    NULL) != pdPASS) {
        usb_host_uninstall();
        return ESP_ERR_NO_MEM;
    }

    /*
     * The worker before the class driver, so a device that arrives during boot
     * is queued rather than lost.
     */
    s_work = xQueueCreate(WORK_QUEUE_LEN, sizeof(uint8_t));
    if (s_work == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(work_task, "usb:work", WORK_TASK_STACK, NULL, WORK_TASK_PRIO,
                    NULL) != pdPASS) {
        vQueueDelete(s_work);
        s_work = NULL;
        return ESP_ERR_NO_MEM;
    }

    /*
     * The monitor client, before the class driver so that nothing is missed. Its
     * failure is not fatal: `lsusb` loses the ability to name devices, because a
     * device handle belongs to the client that opened it, but the port count and
     * every storage function still work.
     */
    const usb_host_client_config_t monitor_config = {
        .is_synchronous = false,
        .max_num_event_msg = MONITOR_EVENTS,
        .flags.notify_dev_removed = 1,
        .async.client_event_callback = on_monitor_event,
        .async.callback_arg = NULL,
    };
    err = usb_host_client_register(&monitor_config, &s_monitor);
    if (err != ESP_OK) {
        s_monitor = NULL;
        espix_klog(ESPIX_KLOG_WARN, TAG, "no device monitor: %s",
                   esp_err_to_name(err));
    }

    const msc_host_driver_config_t msc_config = {
        .create_backround_task = true,
        .task_priority = MSC_TASK_PRIO,
        .stack_size = MSC_TASK_STACK,
        .core_id = tskNO_AFFINITY,
        .callback = on_msc_event,
        .callback_arg = NULL,
    };
    err = msc_host_install(&msc_config);
    if (err != ESP_OK) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "msc_host_install: %s",
                   esp_err_to_name(err));
        return err;
    }

    s_installed = true;
    espix_klog(ESPIX_KLOG_INFO, TAG, "host mode, %d device slot%s",
               ESPIX_USB_MAX_DEVS, ESPIX_USB_MAX_DEVS == 1 ? "" : "s");
    return ESP_OK;
}

bool espix_usb_host_present(void)
{
    return s_installed;
}

size_t espix_usb_host_devlist(espix_usb_dev_t *out, size_t n)
{
    if (out == NULL || n == 0 || s_lock == NULL) {
        return 0;
    }

    size_t count = 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < ESPIX_USB_MAX_DEVS && count < n; i++) {
        if (s_devs[i].info.in_use) {
            out[count++] = s_devs[i].info;
        }
    }
    xSemaphoreGive(s_lock);

    return count;
}

/*
 * The block device under a named storage device, for the one caller that mounts
 * it. Borrowed and not owned: it belongs to the slot, and the slot gives it back
 * when the device goes -- which is what docs/KNOWN-ISSUES.md has an entry about,
 * because a mount that outlives the device is the case this does not solve.
 */
esp_blockdev_handle_t espix_usb_host_dev_blockdev(const char *name)
{
    esp_blockdev_handle_t bdl = NULL;

    if (name == NULL || s_lock == NULL) {
        return NULL;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < ESPIX_USB_MAX_DEVS; i++) {
        if (s_devs[i].info.in_use && strcmp(s_devs[i].info.name, name) == 0) {
            bdl = s_devs[i].bdl;
            break;
        }
    }
    xSemaphoreGive(s_lock);

    return bdl;
}

void espix_usb_host_status_query(espix_usb_host_status_t *out)
{
    out->running = s_installed;
#if CONFIG_USB_HOST_HUBS_SUPPORTED
    out->hubs_built = true;
#endif
    if (!s_installed) {
        return;
    }

    usb_host_lib_info_t info;
    if (usb_host_lib_info(&info) == ESP_OK) {
        out->devices = info.num_devices;
        out->clients = info.num_clients;
        out->root_port_suspended = info.root_port_suspended;
    }

    /*
     * The pool is not the list: `num_devices` counts every device object the
     * library has allocated, while the address list hands back only the ones that
     * are fully enumerated. A difference between the two is a device object that
     * was never freed, and it is the single most useful number here -- it is what
     * "3 in the pool, 2 listed" meant the first time this happened.
     */
    uint8_t addrs[ESPIX_USB_LSUSB_MAX];
    int found = 0;
    if (usb_host_device_addr_list_fill(sizeof(addrs), addrs, &found) == ESP_OK) {
        out->enumerated = found;
    }
}

/* Reads everything worth printing off an open device handle. */
static void describe_device(espix_usb_desc_t *out, usb_device_handle_t dev,
                            uint8_t addr)
{
    const usb_device_desc_t *dd = NULL;
    const usb_config_desc_t *cd = NULL;
    usb_device_info_t info;

    out->addr = addr;

    if (usb_host_get_device_descriptor(dev, &dd) == ESP_OK && dd != NULL) {
        out->class = dd->bDeviceClass;
        out->subclass = dd->bDeviceSubClass;
        out->protocol = dd->bDeviceProtocol;
        out->id_vendor = dd->idVendor;
        out->id_product = dd->idProduct;
    }

    if (usb_host_device_info(dev, &info) == ESP_OK) {
        out->speed = (uint8_t)info.speed;
        str_desc_to_utf8(info.str_desc_manufacturer, out->manufacturer,
                         sizeof(out->manufacturer));
        str_desc_to_utf8(info.str_desc_product, out->product,
                         sizeof(out->product));
        str_desc_to_utf8(info.str_desc_serial_num, out->serial,
                         sizeof(out->serial));
    }

    if (usb_host_get_active_config_descriptor(dev, &cd) != ESP_OK || cd == NULL) {
        return;
    }

    /*
     * The interfaces, which is where the driver decision is actually made: a
     * device's own class is 0 for anything that puts the class on its
     * interfaces, which is most storage devices. Only alternate setting 0 is
     * listed -- an alternate is the same interface at another bandwidth, not
     * another driver.
     */
    int offset = 0;
    const usb_standard_desc_t *next = (const usb_standard_desc_t *)cd;

    while (out->n_ifaces < ESPIX_USB_IFACES_MAX &&
           (next = usb_parse_next_descriptor_of_type(
                next, cd->wTotalLength, USB_B_DESCRIPTOR_TYPE_INTERFACE,
                &offset)) != NULL) {
        const usb_intf_desc_t *ifc = (const usb_intf_desc_t *)next;

        if (ifc->bAlternateSetting != 0) {
            continue;
        }
        espix_usb_iface_t *slot = &out->ifaces[out->n_ifaces++];
        slot->class = ifc->bInterfaceClass;
        slot->subclass = ifc->bInterfaceSubClass;
        slot->protocol = ifc->bInterfaceProtocol;
    }
}

size_t espix_usb_host_device_list(espix_usb_desc_t *out, size_t n)
{
    if (!s_installed || out == NULL || n == 0) {
        return 0;
    }
    if (s_monitor == NULL) {
        /* Without a client there is no way to open a device, so there are no
         * names to show -- only the count, which the status line carries. */
        return 0;
    }

    uint8_t addrs[ESPIX_USB_LSUSB_MAX];
    int found = 0;

    if (usb_host_device_addr_list_fill(sizeof(addrs), addrs, &found) != ESP_OK) {
        return 0;
    }
    if (found > (int)sizeof(addrs)) {
        found = (int)sizeof(addrs);
    }

    size_t filled = 0;

    for (int i = 0; i < found && filled < n; i++) {
        usb_device_handle_t dev = NULL;

        /* One open at a time, and the same client the monitor uses: a device
         * unplugged between the listing and here is skipped rather than
         * reported, and a re-open by the same client is refused by the library
         * rather than reported as a missing device. */
        if (xSemaphoreTake(s_open_lock, portMAX_DELAY) != pdTRUE) {
            break;
        }
        if (usb_host_device_open(s_monitor, addrs[i], &dev) != ESP_OK) {
            xSemaphoreGive(s_open_lock);
            continue;
        }
        memset(&out[filled], 0, sizeof(out[filled]));
        describe_device(&out[filled], dev, addrs[i]);
        if (usb_host_device_close(s_monitor, dev) != ESP_OK) {
            espix_klog(ESPIX_KLOG_WARN, TAG, "addr %u: close failed", addrs[i]);
        }
        xSemaphoreGive(s_open_lock);
        filled++;
    }

    /* Sorted by address, because the library hands them back in whatever order
     * its device pool happens to be in. */
    for (size_t i = 1; i < filled; i++) {
        espix_usb_desc_t key = out[i];
        size_t j = i;

        while (j > 0 && out[j - 1].addr > key.addr) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = key;
    }

    return filled;
}

/* Every USB address the library is currently offering, sorted. */
static int pool_addresses(uint8_t *out, size_t n)
{
    int found = 0;

    if (!s_installed || usb_host_device_addr_list_fill((int)n, out, &found) != ESP_OK) {
        return 0;
    }
    if (found > (int)n) {
        found = (int)n;
    }
    for (int i = 1; i < found; i++) {
        const uint8_t key = out[i];
        int j = i;
        while (j > 0 && out[j - 1] > key) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = key;
    }
    return found;
}


/*
 * Attach one device by address, whether or not anyone told us about it.
 *
 * `usbprobe <addr>` is this for a device the user names; `usbscan` is this for
 * every unclaimed storage device in the pool. Both bypass the MSC driver's event
 * callback, which is the half of the path that has never once been observed to
 * run.
 */
esp_err_t espix_usb_host_probe_addr(uint8_t addr)
{
    if (!s_installed) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t addrs[ESPIX_USB_LSUSB_MAX];
    const int found = pool_addresses(addrs, sizeof(addrs));
    bool present = false;

    for (int i = 0; i < found; i++) {
        if (addrs[i] == addr) {
            present = true;
            break;
        }
    }
    if (!present) {
        /* Either nothing is there, or it is there and the library cannot open it
         * -- the two are indistinguishable from the address list alone, which is
         * why `lsusb` prints the pool count next to the list. */
        return ESP_ERR_NOT_FOUND;
    }
    return attach_device(addr);
}

/* Records that this address failed to attach, so the sweep leaves it alone. */
static void backoff_failed(uint8_t addr)
{
    const TickType_t now = xTaskGetTickCount();

    for (size_t i = 0; i < ESPIX_USB_LSUSB_MAX; i++) {
        if (s_backoff[i].addr == addr || s_backoff[i].addr == 0) {
            s_backoff[i].addr = addr;
            s_backoff[i].until = now + ATTACH_FAIL_BACKOFF;
            return;
        }
    }
}

static bool backoff_active(uint8_t addr)
{
    const TickType_t now = xTaskGetTickCount();

    for (size_t i = 0; i < ESPIX_USB_LSUSB_MAX; i++) {
        /* Signed difference, so this still reads correctly across the tick
         * counter wrapping -- which it does in about 497 days. */
        if (s_backoff[i].addr == addr && s_backoff[i].until != 0 &&
            (int32_t)(now - s_backoff[i].until) < 0) {
            return true;
        }
    }
    return false;
}

size_t espix_usb_host_scan_pool(void)
{
    if (!s_installed) {
        return 0;
    }

    uint8_t addrs[ESPIX_USB_LSUSB_MAX];
    const int found = pool_addresses(addrs, sizeof(addrs));
    size_t attached = 0;

    for (int i = 0; i < found; i++) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        const bool known = (slot_by_addr(addrs[i]) != NULL);
        xSemaphoreGive(s_lock);

        if (known || backoff_active(addrs[i])) {
            continue;
        }
        const esp_err_t msc = addr_msc_interface(addrs[i]);
        if (msc == ESP_ERR_NOT_FOUND) {
            continue;                      /* opened, and nothing to claim */
        }
        if (msc != ESP_OK) {
            espix_klog(ESPIX_KLOG_DEBUG, TAG, "addr %u: cannot read interfaces: %s",
                       addrs[i], esp_err_to_name(msc));
            continue;
        }
        espix_klog(ESPIX_KLOG_INFO, TAG, "addr %u looks like storage; claiming it",
                   addrs[i]);
        if (attach_device(addrs[i]) == ESP_OK) {
            attached++;
        } else {
            backoff_failed(addrs[i]);
        }
    }
    return attached;
}
