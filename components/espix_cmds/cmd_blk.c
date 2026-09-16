/*
 * Block devices: lsblk and blkid.
 *
 * These describe the storage the USB host found. Nothing mounts it yet -- a
 * filesystem needs a mount table espix does not have, and without one an
 * IDF-routed prefix skips the permission check entirely (the reason is spelled
 * out in docs/USB-HOST.md) -- so what these commands are for is knowing what is
 * attached: which device, how big, which filesystems are on it, and, for the
 * ones espix has no driver for, saying so rather than staying silent.
 *
 * Both exist in a build without the USB host, and explain themselves. A command
 * that vanishes when an option is off leaves the user comparing their device
 * against documentation and guessing.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "espix_cmds_priv.h"
#include "espix_shell.h"
#include "espix_usb.h"

#define LSBLK_USAGE "usage: lsblk [disk]\n"
#define BLKID_USAGE "usage: blkid [device]...\n"

/*
 * Column widths. NAME and FSTYPE are measured per listing, the way `ls` measures
 * its columns: a stick with one FAT partition should not be laid out for a disk
 * full of Linux partitions, and a table whose widths change halfway down is
 * unreadable. SIZE and TYPE are fixed because the formatter's widest output
 * bounds them -- "20480G" is six characters and "part" is four.
 */
#define LSBLK_SIZE_W   7
#define LSBLK_TYPE_W   4
#define SIZE_CELL_MAX  16
#define FSTYPE_CELL_MAX (ESPIX_USB_FSTYPE_MAX + 16)

/*
 * The disabled-feature convention from espix_net's `usb`: the command stays and
 * answers the question. The role is a build-time choice, so this is not a
 * failure to report, it is which image this is.
 */
static int no_host(espix_session_t *s, const char *cmd)
{
    espix_eprintf(s, "%s: usb host was not built into this image "
                    "(CONFIG_ESPIX_USB_ROLE_HOST)\n", cmd);
    return 1;
}

/* The device table, or a report of why there is none. */
static size_t attached(espix_session_t *s, const char *cmd, espix_usb_dev_t *out)
{
    if (!espix_usb_present()) {
        espix_eprintf(s, "%s: usb host is not running; dmesg will say why\n", cmd);
        return 0;
    }
    return espix_usb_devlist(out, ESPIX_USB_MAX_DEVS);
}

/*
 * The FSTYPE cell of a disk row: the filesystem on the disk itself when it has
 * one (a superfloppy -- no partition table, sector 0 is the filesystem's boot
 * sector), otherwise empty, or the word for a sector 0 that could not be read.
 */
static void disk_fstype_cell(const espix_usb_dev_t *d, char *out, size_t out_len)
{
    if (d->fstype[0] != '\0') {
        snprintf(out, out_len, "%s%s", d->fstype,
                 d->foreign ? " (unsupported)" : "");
        return;
    }
    snprintf(out, out_len, "%s", d->table_read ? "" : "unreadable");
}

/*
 * The FSTYPE cell of a partition row: the name the partition table gives, plus
 * the marker that says espix has no driver for it. Naming it is most of the point
 * of this command existing before mounting does; leaving the column blank would
 * read as an empty partition.
 */
static void fstype_cell(const espix_usb_part_t *p, char *out, size_t out_len)
{
    if (p->foreign && p->fstype[0] != '\0') {
        snprintf(out, out_len, "%s (unsupported)", p->fstype);
    } else {
        snprintf(out, out_len, "%s", p->fstype);
    }
}

/*
 * One lsblk row. The name is padded by hand rather than with %-*s, because
 * printf counts bytes and the tree characters are three bytes for one column
 * each -- left to printf, every partition row would start two columns early.
 */
static void row(espix_session_t *s, int name_w, int fstype_w, const char *tree,
                const char *name, const char *size, const char *type,
                const char *fstype, const char *label)
{
    const int cols = (int)(strlen(tree) / 3 + strlen(name));

    espix_printf(s, "%s%s%*s %-*s %-*s %-*s %s\n",
                 tree, name, name_w - cols, "",
                 LSBLK_SIZE_W, size, LSBLK_TYPE_W, type, fstype_w, fstype,
                 label);
}

/* Widest of two, for the measuring pass. */
static int wider(int a, size_t b)
{
    return (b > (size_t)a) ? (int)b : a;
}

static int cmd_lsblk(espix_session_t *s, int argc, char **argv)
{
    const char *want = NULL;

    if (!espix_usb_host_built()) {
        return no_host(s, "lsblk");
    }

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            espix_eprintf(s, "lsblk: unknown option '%s'\n" LSBLK_USAGE, argv[i]);
            return 1;
        }
        if (want != NULL) {
            espix_eprintf(s, "lsblk: %s: one device at a time\n" LSBLK_USAGE,
                          argv[i]);
            return 1;
        }
        want = argv[i];
    }

    espix_usb_dev_t devs[ESPIX_USB_MAX_DEVS];
    const size_t n = attached(s, "lsblk", devs);

    /*
     * Measure before printing. The name column has to hold the tree as well as
     * the name -- two display columns, six bytes -- and the filesystem column has
     * to hold the longest cell in this listing, marker included.
     */
    int name_w = (int)strlen("NAME");
    int fstype_w = (int)strlen("FSTYPE");
    size_t matched = 0;
    char cell[FSTYPE_CELL_MAX];

    for (size_t i = 0; i < n; i++) {
        if (want != NULL && strcmp(want, devs[i].name) != 0) {
            continue;
        }
        matched++;

        name_w = wider(name_w, strlen(devs[i].name));
        disk_fstype_cell(&devs[i], cell, sizeof(cell));
        fstype_w = wider(fstype_w, strlen(cell));

        for (size_t j = 0; j < devs[i].nparts; j++) {
            name_w = wider(name_w, 2 + strlen(devs[i].parts[j].name));
            fstype_cell(&devs[i].parts[j], cell, sizeof(cell));
            fstype_w = wider(fstype_w, strlen(cell));
        }
    }

    if (matched == 0) {
        if (want != NULL) {
            espix_eprintf(s, "lsblk: %s: no such device\n", want);
            return 1;
        }
        /* Nothing attached is an answer, not a failure: lsblk prints nothing for
         * it, and so does this. */
        return 0;
    }

    row(s, name_w, fstype_w, "", "NAME", "SIZE", "TYPE", "FSTYPE", "LABEL");

    for (size_t i = 0; i < n; i++) {
        if (want != NULL && strcmp(want, devs[i].name) != 0) {
            continue;
        }

        char size[SIZE_CELL_MAX];
        char disk_fstype[FSTYPE_CELL_MAX];
        espix_cmd_size(size, sizeof(size), devs[i].size, true);
        disk_fstype_cell(&devs[i], disk_fstype, sizeof(disk_fstype));

        /*
         * The disk's label is the product string it calls itself by -- unless the
         * disk *is* a filesystem, in which case its volume label is the useful
         * name and the product is already in `blkid` and dmesg.
         */
        row(s, name_w, fstype_w, "", devs[i].name, size, "disk", disk_fstype,
            devs[i].label[0] != '\0' ? devs[i].label : devs[i].product);

        for (size_t j = 0; j < devs[i].nparts; j++) {
            const espix_usb_part_t *p = &devs[i].parts[j];
            char part_size[SIZE_CELL_MAX];
            char fstype[FSTYPE_CELL_MAX];

            espix_cmd_size(part_size, sizeof(part_size), p->size, true);
            fstype_cell(p, fstype, sizeof(fstype));

            /* The tree, the way lsblk draws it: every partition but the last
             * gets a tee, the last a corner. */
            row(s, name_w, fstype_w,
                (j + 1 == devs[i].nparts) ? "\u2514\u2500" : "\u251c\u2500",
                p->name, part_size, "part", fstype, p->label);
        }
    }

    return 0;
}

/* One key="value" pair, or nothing when the value is unknown: blkid omits what
 * it cannot determine, and an empty string is not a determination. */
static void kv(espix_session_t *s, const char *key, const char *value)
{
    if (value != NULL && value[0] != '\0') {
        espix_printf(s, " %s=\"%s\"", key, value);
    }
}

static void kv_num(espix_session_t *s, const char *key, uint64_t value)
{
    espix_printf(s, " %s=\"%llu\"", key, (unsigned long long)value);
}

/* VID and PID are hex everywhere else that prints them, so they are hex here. */
static void kv_hex(espix_session_t *s, const char *key, uint16_t value)
{
    espix_printf(s, " %s=\"%04x\"", key, value);
}

/* Does this device answer to this name -- its own, or one of its partitions? */
static bool dev_named(const espix_usb_dev_t *d, const char *name)
{
    if (strcmp(d->name, name) == 0) {
        return true;
    }
    for (size_t j = 0; j < d->nparts; j++) {
        if (strcmp(d->parts[j].name, name) == 0) {
            return true;
        }
    }
    return false;
}

/*
 * blkid: the machine-readable view, one line per disk and per partition.
 *
 * Unlike Linux's blkid, TYPE on a whole-disk line reads "disk": there is no
 * filesystem to name there, and inventing one for a partition table would be
 * worse than saying what it is. Everything else follows blkid's KEY="value"
 * shape, and a key with no known value is left out rather than printed empty.
 */
static int cmd_blkid(espix_session_t *s, int argc, char **argv)
{
    if (!espix_usb_host_built()) {
        return no_host(s, "blkid");
    }

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            espix_eprintf(s, "blkid: unknown option '%s'\n" BLKID_USAGE, argv[i]);
            return 1;
        }
    }

    espix_usb_dev_t devs[ESPIX_USB_MAX_DEVS];
    const size_t n = attached(s, "blkid", devs);
    int status = 0;

    /* An operand that names nothing is a mistake to report: printing fewer lines
     * than asked and exiting 0 is the kind of answer a script believes. */
    for (int a = 1; a < argc; a++) {
        bool found = false;

        for (size_t i = 0; i < n && !found; i++) {
            found = dev_named(&devs[i], argv[a]);
        }
        if (!found) {
            espix_eprintf(s, "blkid: %s: no such device\n", argv[a]);
            status = 1;
        }
    }

    for (size_t i = 0; i < n; i++) {
        /* With no operand, every row. With operands, the rows they name:
         * `blkid sda1` prints that partition alone, because the value is what was
         * asked for rather than the tree. */
        bool disk_wanted = (argc == 1);
        bool part_wanted = (argc == 1);

        for (int a = 1; a < argc; a++) {
            if (strcmp(argv[a], devs[i].name) == 0) {
                disk_wanted = true;
            }
            for (size_t j = 0; j < devs[i].nparts; j++) {
                if (strcmp(argv[a], devs[i].parts[j].name) == 0) {
                    part_wanted = true;
                }
            }
        }
        if (!disk_wanted && !part_wanted) {
            continue;
        }

        if (disk_wanted) {
            /* TYPE is the filesystem when the disk itself is one (a superfloppy
             * with no partition table); otherwise there is no filesystem to name
             * and "disk" is the honest value. */
            espix_printf(s, "%s: TYPE=\"%s\"", devs[i].name,
                         devs[i].fstype[0] != '\0' ? devs[i].fstype : "disk");
            kv(s, "PRODUCT", devs[i].product);
            kv(s, "MANUFACTURER", devs[i].manufacturer);
            kv(s, "SERIAL", devs[i].serial);
            kv(s, "LABEL", devs[i].label);
            kv_hex(s, "VID", devs[i].id_vendor);
            kv_hex(s, "PID", devs[i].id_product);
            kv_num(s, "SIZE", devs[i].size);
            espix_printf(s, "\n");
        }

        for (size_t j = 0; j < devs[i].nparts; j++) {
            const espix_usb_part_t *p = &devs[i].parts[j];
            bool named = false;

            for (int a = 1; a < argc; a++) {
                named = named || strcmp(argv[a], p->name) == 0;
            }
            if (argc > 1 && !named) {
                continue;
            }

            espix_printf(s, "%s: TYPE=\"%s\"", p->name,
                         p->fstype[0] != '\0' ? p->fstype : "unknown");
            kv(s, "LABEL", p->label);
            kv_num(s, "START", p->start);
            kv_num(s, "SIZE", p->size);
            espix_printf(s, "\n");
        }
    }

    return status;
}

/* ------------------------------------------------------------------ */

static espix_cmd_t s_blk_cmds[] = {
    { .name = "lsblk", .fn = cmd_lsblk,
      /* The MBR limitation belongs where someone will read it, which is the
       * command's own help rather than a document they have not opened. */
      .help = "list block devices and their filesystems (MBR only, no GPT)",
      .usage = "lsblk [disk]" },
    { .name = "blkid", .fn = cmd_blkid,
      .help = "print a device's identity, filesystem and label",
      .usage = "blkid [device]..." },
};

void espix_cmds_register_blk(void)
{
    espix_cmds_register_table(s_blk_cmds,
                              sizeof(s_blk_cmds) / sizeof(s_blk_cmds[0]));
}
