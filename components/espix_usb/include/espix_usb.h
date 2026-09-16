/*
 * USB host: the OTG port in host mode, and the storage devices found on it.
 *
 * Stage 1 is enumeration and nothing else. A device is found, identified, and
 * its partition table read, and then espix stops -- nothing here is mounted and
 * nothing is registered with the VFS. That is not caution for its own sake: a
 * filesystem mounted at its own prefix is routed by IDF before espix's
 * permission check ever sees a path (components/espix_fs/dev.c says why), so
 * mounting means a mount table, and a mount table is not this.
 *
 * The port has one role at a time, chosen at build time
 * (CONFIG_ESPIX_USB_ROLE in espix_net/Kconfig), so in a device-role build there
 * is no host stack at all and every table below is empty.
 * espix_usb_host_built() distinguishes that from "nothing is plugged in", which
 * is what lets `lsblk` explain itself instead of looking like a broken driver.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Four slots, because a hub is the reason this exists and one socket is not
 * worth a table. Named the way Linux names them, sda..sdd, and handed out in
 * slot order so a device keeps its name for as long as it stays plugged in.
 */
#define ESPIX_USB_MAX_DEVS   4
#define ESPIX_USB_MAX_PARTS  4       /* what an MBR holds; no GPT yet */
#define ESPIX_USB_NAME_MAX   8       /* "sda1" */
#define ESPIX_USB_FSTYPE_MAX 12      /* "exfat/ntfs" */
#define ESPIX_USB_LABEL_MAX  24      /* Volume labels are 11 bytes on disk */
#define ESPIX_USB_STR_MAX    64      /* Device strings, converted to UTF-8 */

/*
 * One partition, or rather one entry of the partition table. MBR entries are
 * what the disk offers, not what espix can read: reporting "exfat/ntfs" for a
 * filesystem no driver here can open is the point of the command, and the
 * alternative -- saying nothing -- reads as an empty disk.
 */
typedef struct {
    char     name[ESPIX_USB_NAME_MAX];          /* "sda1" */
    char     fstype[ESPIX_USB_FSTYPE_MAX];      /* "" when nothing is recognised */
    char     label[ESPIX_USB_LABEL_MAX];        /* "" when the volume has none */
    uint64_t start;                             /* byte offset in the disk */
    uint64_t size;                              /* bytes */
    bool     foreign;   /* recognised, but no driver here will ever open it */
} espix_usb_part_t;

typedef struct {
    bool     in_use;
    char     name[ESPIX_USB_NAME_MAX];          /* "sda" */
    char     manufacturer[ESPIX_USB_STR_MAX];   /* the strings the device reports; */
    char     product[ESPIX_USB_STR_MAX];        /* any of them may be empty */
    char     serial[ESPIX_USB_STR_MAX];
    uint16_t id_vendor;
    uint16_t id_product;
    uint32_t sector_size;
    uint64_t size;                              /* bytes */
    bool     table_read;                        /* sector 0 was read and parsed */
    /*
     * The disk's own filesystem, when it has one: a device with no partition
     * table whose sector 0 is a filesystem's boot sector -- a "superfloppy". Then
     * the filesystem belongs to this row rather than to a partition, and
     * `lsblk`/`blkid` show it the way Linux's tools do.
     */
    char     fstype[ESPIX_USB_FSTYPE_MAX];      /* "" when the disk has a table */
    char     label[ESPIX_USB_LABEL_MAX];        /* its volume label, if any */
    bool     foreign;                           /* recognised, no driver for it */
    size_t   nparts;
    espix_usb_part_t parts[ESPIX_USB_MAX_PARTS];
} espix_usb_dev_t;

/* True in a host-role build. False says the role is device, not that it failed. */
bool espix_usb_host_built(void);

/*
 * Install the host stack and start looking for devices. Returns as soon as the
 * stack is up: an empty socket is the normal case, and enumeration happens on
 * the USB task afterwards.
 *
 * Does nothing and returns ESP_OK when the role is device, so the boot sequence
 * does not have to care which build it is.
 */
esp_err_t espix_usb_init(void);

/* True once the host stack is installed; false in a device-role build. */
bool espix_usb_present(void);

/*
 * The attached devices, in slot order. Returns how many entries were written
 * and never more than n, so a caller with a stack array is safe.
 */
size_t espix_usb_devlist(espix_usb_dev_t *out, size_t n);

/*
 * Everything the host library can see on the port, which is not the same set as
 * the storage table above: a hub, a keyboard or anything else with no driver is
 * enumerated, addressed and described here, and nowhere else. `lsusb` prints it,
 * and it is the only way to tell "nothing was ever plugged in" from "something
 * was plugged in that espix has no driver for".
 */
#define ESPIX_USB_LSUSB_MAX  8      /* hubs and devices, seen at once */
#define ESPIX_USB_IFACES_MAX 4      /* interfaces worth printing per device */

typedef struct {
    uint8_t class;      /* bInterfaceClass */
    uint8_t subclass;
    uint8_t protocol;
} espix_usb_iface_t;

typedef struct {
    uint8_t  addr;                          /* USB address, 1..127 */
    uint8_t  class;                         /* bDeviceClass, 0 for per-interface */
    uint8_t  subclass;
    uint8_t  protocol;
    uint8_t  speed;                         /* usb_speed_t, as a number */
    uint16_t id_vendor;
    uint16_t id_product;
    char     manufacturer[ESPIX_USB_STR_MAX];   /* UTF-8; any of these may be */
    char     product[ESPIX_USB_STR_MAX];        /* empty, and most hubs have */
    char     serial[ESPIX_USB_STR_MAX];         /* no serial number at all */
    size_t   n_ifaces;
    espix_usb_iface_t ifaces[ESPIX_USB_IFACES_MAX];
} espix_usb_desc_t;

typedef struct {
    bool running;               /* the host stack is installed */
    int  devices;               /* device objects in the library's pool, hubs included */
    int  enumerated;            /* ... of those, fully enumerated and openable */
    int  clients;               /* registered clients: ours, and the class drivers' */
    bool root_port_suspended;
    bool hubs_built;            /* CONFIG_USB_HOST_HUBS_SUPPORTED */
} espix_usb_host_status_t;

/* Zeroed and false in a device-role build. */
void espix_usb_host_status(espix_usb_host_status_t *out);

/*
 * Fills up to n described devices, returning how many were written. Opening a
 * device to read its descriptors fails for one that was unplugged in between,
 * which is skipped rather than reported.
 */
size_t espix_usb_host_devices(espix_usb_desc_t *out, size_t n);

/* "hub", "mass storage", "human interface" ... or "" when unrecognised. */
const char *espix_usb_class_name(uint8_t class);

/*
 * Whether the mass-storage class driver would claim this interface. It wants
 * class 8, subclass 6 and protocol 0x50 -- bulk-only transport -- and nothing
 * else, so a UAS interface (protocol 0x62) is described by `lsusb` and then
 * silently ignored by the driver. That asymmetry is worth a name.
 */
bool espix_usb_iface_is_msc_bot(uint8_t class, uint8_t subclass, uint8_t protocol);

/*
 * Claim a device for storage without waiting to be told about it.
 *
 * The normal path is the MSC driver's callback, and these two exist because that
 * path cannot be relied on: it has been observed to deliver nothing at all while
 * the device pool was perfectly readable. Both do what the callback would have
 * done -- install the device with the MSC driver, describe it, read its
 * partition table, publish it -- so `lsblk` works either way.
 *
 * `espix_usb_host_probe()` takes an address from `lsusb`; `..._scan()` takes
 * every unclaimed storage device the library is offering. ESP_ERR_NOT_FOUND
 * means the address is not in the library's list -- absent, or present and
 * unopenable, which `lsusb`'s pool count distinguishes.
 */
esp_err_t espix_usb_host_probe(uint8_t addr);
size_t    espix_usb_host_scan(void);

#ifdef __cplusplus
}
#endif
