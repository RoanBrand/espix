/*
 * The host role: install the stack, watch for mass-storage devices, and keep a
 * table of what is attached.
 *
 * Stage 1 reads sector 0 and stops there. The partition table is what makes
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
#include "usb/msc_host.h"
#include "usb/usb_host.h"

#include "esp_mbr.h"

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
#define WORK_TASK_STACK 4096
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

static bool is_fat_type(uint8_t type)
{
    return type == ESP_EXT_PART_TYPE_FAT12 ||
           type == ESP_EXT_PART_TYPE_FAT16 ||
           type == ESP_EXT_PART_TYPE_FAT32;
}

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
static void fat_label(const uint8_t *sector, char *out, size_t out_len)
{
    const size_t off = fat_label_offset(sector);

    if (out_len == 0 || off == 0) {
        return;
    }

    const uint8_t *label = sector + off;
    size_t n = 11;
    while (n > 0 && label[n - 1] == ' ') {
        n--;
    }
    if (n == 0 || (n == 7 && memcmp(label, "NO NAME", 7) == 0)) {
        return;     /* the formatter's own idea of an unnamed volume */
    }

    const size_t len = (n < out_len - 1) ? n : out_len - 1;
    memcpy(out, label, len);
    out[len] = '\0';
}

/*
 * What a filesystem covering a whole device calls itself, or "". The case with
 * no partition table at all, where sector 0 is the filesystem's own boot sector
 * rather than an MBR -- a "superfloppy", which is how appliance-formatted sticks
 * arrive. FAT is read here; exFAT and NTFS are named because naming what cannot
 * be read is the point of the whole report.
 */
static const char *whole_device_fstype(const uint8_t *sector, bool *foreign)
{
    if (fat_label_offset(sector) != 0) {
        return "vfat";
    }
    /* The OEM-name field doubles as the signature for both of these. */
    if (memcmp(sector + 3, "EXFAT   ", 8) == 0) {
        *foreign = true;
        return "exfat";
    }
    if (memcmp(sector + 3, "NTFS    ", 8) == 0) {
        *foreign = true;
        return "ntfs";
    }
    return "";
}

/*
 * Sector 0, then one more read per FAT partition for its label.
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
         * all); exFAT and NTFS are only named, because naming what cannot be read
         * is the point of this report.
         */
        bool foreign = false;
        const char *type = whole_device_fstype(sector, &foreign);

        if (type[0] != '\0') {
            copy_str(d->info.fstype, sizeof(d->info.fstype), type);
            d->info.foreign = foreign;
            fat_label(sector, d->info.label, sizeof(d->info.label));
        }
        free(sector);
        return;
    }

    /*
     * Parsed from the buffer rather than through esp_ext_part_list_bdl_read(),
     * which reads a fixed 512 bytes and so cannot work on a 4K-sector disk.
     */
    {
        /* The medium's own sector size, so an LBA in the table scales to the byte
         * address the block device expects. */
        esp_mbr_parse_extra_args_t args = {
            .sector_size = (esp_ext_part_sector_size_t)unit,
        };
        esp_ext_part_list_t list = { 0 };

        if (esp_mbr_parse(sector, &list, &args) == ESP_OK) {
            for (esp_ext_part_list_item_t *it = esp_ext_part_list_item_head(&list);
                 it != NULL && d->info.nparts < ESPIX_USB_MAX_PARTS;
                 it = esp_ext_part_list_item_next(it)) {
                espix_usb_part_t *p = &d->info.parts[d->info.nparts];
                const uint8_t type = it->info.type;
                const bool fat = is_fat_type(type);

                /*
                 * "sda1", built by hand. The disk name is three characters
                 * because slot_claim() made it, and an MBR holds four entries, so
                 * the whole name is five bytes. Written out rather than
                 * formatted: snprintf("%s%u") into an 8-byte buffer is exactly
                 * what -Wformat-truncation exists to flag, and it is right to.
                 */
                if (d->info.nparts < 9) {
                    p->name[0] = d->info.name[0];
                    p->name[1] = d->info.name[1];
                    p->name[2] = d->info.name[2];
                    p->name[3] = (char)('1' + (int)d->info.nparts);
                    p->name[4] = '\0';
                }
                p->start = it->info.address;
                p->size = it->info.size;
                copy_str(p->fstype, sizeof(p->fstype),
                         fstype_name(type, &p->foreign));

                if (fat && d->bdl->ops->read(d->bdl, sector, unit, p->start,
                                             unit) == ESP_OK) {
                    fat_label(sector, p->label, sizeof(p->label));
                }
                d->info.nparts++;
            }
            esp_ext_part_list_deinit(&list);
        }
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
