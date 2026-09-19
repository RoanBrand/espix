/*
 * lsusb: what the USB-OTG port can see.
 *
 * This is the command for the question `lsblk` cannot answer. A hub, a keyboard
 * or anything else with no driver is enumerated and addressed like any device,
 * and then never mentioned again -- not by `lsblk`, not by `blkid`, and not in
 * `dmesg` unless ESPIX_USB_VERBOSE is on. Without this, "the hub is dead" and
 * "the hub works and nothing is plugged into it" look identical, and so do
 * "nothing is attached" and "the SoC never saw the device at all".
 *
 * Nothing here is mounted or mountable: it describes the port.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "espix_cmds_priv.h"
#include "espix_shell.h"
#include "espix_usb.h"

#define LSUSB_USAGE "usage: lsusb [-v]\n"

/* usb_speed_t, spelled out. The enum lives in the USB headers, which this
 * component's public header deliberately does not include. */
static const char *speed_name(uint8_t speed)
{
    switch (speed) {
    case 0:  return "low";
    case 1:  return "full";
    case 2:  return "high";
    default: return "?";
    }
}

/* The interface line, and for storage the one thing worth knowing about it. */
static void print_iface(espix_session_t *s, const espix_usb_iface_t *ifc)
{
    const char *name = espix_usb_class_name(ifc->class);

    espix_printf(s, "    interface: class %02x%s%s%s subclass %02x protocol %02x",
                 ifc->class,
                 name[0] ? " (" : "", name, name[0] ? ")" : "",
                 ifc->subclass, ifc->protocol);

    if (ifc->class != 0x08) {
        espix_printf(s, "\n");
        return;
    }
    if (espix_usb_iface_is_msc_bot(ifc->class, ifc->subclass, ifc->protocol)) {
        espix_printf(s, "  this is what the storage driver claims (BOT)\n");
    } else if (ifc->subclass == 0x06) {
        /* 0x50 is bulk-only transport, 0x62 is UAS, and msc_host_msc wants the
         * former only -- so this device enumerates and is then ignored. */
        espix_printf(s, "  not claimed: the storage driver takes BOT (50) only\n");
    } else {
        espix_printf(s, "\n");
    }
}

static int cmd_lsusb(espix_session_t *s, int argc, char **argv)
{
    bool verbose = false;

    if (!espix_usb_host_built()) {
        espix_eprintf(s, "lsusb: usb host was not built into this image "
                        "(CONFIG_ESPIX_USB_ROLE_HOST)\n");
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            verbose = true;
            continue;
        }
        espix_eprintf(s, "lsusb: unknown option '%s'\n" LSUSB_USAGE, argv[i]);
        return 1;
    }

    espix_usb_host_status_t st;
    espix_usb_host_status(&st);

    if (!st.running) {
        espix_eprintf(s, "lsusb: usb host is not running; dmesg will say why\n");
        return 1;
    }

    /*
     * The header is the part that answers the question when there are no rows.
     * Three numbers matter and they are not the same number: the pool is every
     * device object the library allocated, the list is the ones it will let you
     * open, and the client count says whether anything is listening. A pool
     * larger than the list is a device object that was never freed, and it is
     * what left an address permanently unopenable the first time this happened
     * (docs/USB-HOST.md).
     */
    espix_printf(s, "host: running, %d client%s, %d device object%s in the pool, "
                    "%d enumerated, root port %s, hub support %s\n",
                 st.clients, st.clients == 1 ? "" : "s",
                 st.devices, st.devices == 1 ? "" : "s",
                 st.enumerated,
                 st.root_port_suspended ? "suspended" : "powered",
                 st.hubs_built ? "built in" : "NOT built in");

    if (st.devices != st.enumerated) {
        espix_printf(s, "note: %d object%s in the pool that cannot be opened -- "
                        "a device that was removed and never freed\n",
                     st.devices - st.enumerated,
                     (st.devices - st.enumerated) == 1 ? "" : "s");
    }

    /*
     * On the heap, not the stack. espix_usb_desc_t is about 280 bytes and there
     * are eight of them, so the array alone is ~2.2 KB -- more than the session
     * task has to spare, and it is charged whether or not a single device is
     * attached. That is how `lsusb` on an empty port tripped the canary. Big
     * buffers belong on the heap, the same rule cmd_fs.c follows.
     */
    espix_usb_desc_t *devs = calloc(ESPIX_USB_LSUSB_MAX, sizeof(*devs));
    if (devs == NULL) {
        espix_eprintf(s, "lsusb: out of memory\n");
        return 1;
    }

    const size_t n = espix_usb_host_devices(devs, ESPIX_USB_LSUSB_MAX);

    for (size_t i = 0; i < n; i++) {
        const espix_usb_desc_t *d = &devs[i];
        const char *name = espix_usb_class_name(d->class);

        espix_printf(s, "addr %u: %04x:%04x class %02x%s%s%s %s\n",
                     d->addr, d->id_vendor, d->id_product, d->class,
                     name[0] ? " (" : "", name, name[0] ? ")" : "",
                     d->product[0] ? d->product : "");

        if (!verbose) {
            continue;
        }
        if (d->manufacturer[0] != '\0') {
            espix_printf(s, "    manufacturer: %s\n", d->manufacturer);
        }
        if (d->serial[0] != '\0') {
            espix_printf(s, "    serial:       %s\n", d->serial);
        }
        espix_printf(s, "    speed:        %s\n", speed_name(d->speed));

        for (size_t j = 0; j < d->n_ifaces; j++) {
            print_iface(s, &d->ifaces[j]);
        }
    }

    if (n == 0 && st.devices == 0) {
        /* Not an error, and not silent either: this is the line that separates
         * "nothing is plugged in" from "nothing was detected". */
        espix_printf(s, "(nothing on the port)\n");
    }

    free(devs);
    return 0;
}

/* ------------------------------------------------------------------ */
/* usbprobe and usbscan                                                */
/* ------------------------------------------------------------------ */

/*
 * These two exist because the normal path -- the class driver telling us a
 * device arrived -- has never once been seen to fire on this hardware, while the
 * device pool has been perfectly readable the whole time. They do the same work
 * the callback would have done, on demand:
 *
 *   usbprobe <addr>   claim one device, from `lsusb`
 *   usbscan           claim every storage device that has no driver yet
 *
 * `usbscan` is the one that makes `lsblk` work when the event path does not, and
 * it is also the check that separates "the driver cannot see the device" from
 * "the device cannot be claimed at all".
 */
static int cmd_usbprobe(espix_session_t *s, int argc, char **argv)
{
    if (!espix_usb_host_built()) {
        espix_eprintf(s, "usbprobe: usb host was not built into this image "
                         "(CONFIG_ESPIX_USB_ROLE_HOST)\n");
        return 1;
    }
    if (argc != 2 || argv[1][0] == '-' || argv[1][0] == '\0') {
        espix_eprintf(s, "usbprobe: one address, as `lsusb` prints it\n"
                         "usage: usbprobe <addr>\n");
        return 1;
    }

    char *end = NULL;
    const long addr = strtol(argv[1], &end, 10);
    if (end == argv[1] || *end != '\0' || addr < 1 || addr > 127) {
        espix_eprintf(s, "usbprobe: %s: not a usb address\n", argv[1]);
        return 1;
    }

    const esp_err_t err = espix_usb_host_probe((uint8_t)addr);
    if (err == ESP_ERR_NOT_FOUND) {
        espix_eprintf(s, "usbprobe: addr %ld is not in the library's list; "
                         "`lsusb` says whether it is in the pool\n", addr);
        return 1;
    }
    if (err == ESP_ERR_NOT_SUPPORTED) {
        /* The one case that must not reach the driver: it asserts on a device
         * with no bulk-only interface, which aborts the board. */
        espix_eprintf(s, "usbprobe: addr %ld has no mass-storage interface; "
                         "nothing for the storage driver to claim\n", addr);
        return 1;
    }
    if (err != ESP_OK) {
        espix_eprintf(s, "usbprobe: addr %ld: %s\n", addr, esp_err_to_name(err));
        return 1;
    }

    espix_printf(s, "addr %ld claimed; `lsblk` now lists it, and dmesg says "
                    "what it is\n", addr);
    return 0;
}

static int cmd_usbscan(espix_session_t *s, int argc, char **argv)
{
    if (!espix_usb_host_built()) {
        espix_eprintf(s, "usbscan: usb host was not built into this image "
                         "(CONFIG_ESPIX_USB_ROLE_HOST)\n");
        return 1;
    }
    if (argc != 1) {
        espix_eprintf(s, "usbscan: takes no arguments\nusage: usbscan\n");
        return 1;
    }

    const size_t n = espix_usb_host_scan();
    if (n == 0) {
        espix_printf(s, "nothing new to claim\n");
        return 0;
    }
    espix_printf(s, "claimed %u device%s; `lsblk` lists them now\n",
                 (unsigned)n, n == 1 ? "" : "s");
    return 0;
}

/* ------------------------------------------------------------------ */

static espix_cmd_t s_usbhost_cmds[] = {
    { .name = "lsusb", .fn = cmd_lsusb,
      .help = "list the devices the USB port has enumerated (hubs included)",
      .usage = "lsusb [-v]" },
    { .name = "usbprobe", .fn = cmd_usbprobe,
      .help = "claim one enumerated device for storage, by address",
      .usage = "usbprobe <addr>" },
    { .name = "usbscan", .fn = cmd_usbscan,
      .help = "claim every storage device the port is offering",
      .usage = "usbscan" },
};

void espix_cmds_register_usbhost(void)
{
    espix_cmds_register_table(s_usbhost_cmds,
                              sizeof(s_usbhost_cmds) / sizeof(s_usbhost_cmds[0]));
}