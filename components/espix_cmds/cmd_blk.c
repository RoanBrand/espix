/*
 * Block devices: lsblk, blkid, mount and umount.
 *
 * The first two describe the storage the USB host found: which device, how big,
 * which filesystems are on it, and, for the ones espix has no driver for, saying
 * so rather than staying silent.
 *
 * The other two mount and unmount those filesystems. Mounting goes through
 * espix's own VFS -- the volume is *not* registered at a prefix, because that
 * would route IDF directly to it with the permission check skipped (the reason
 * is in components/espix_fs/dev.c) -- and the filesystem itself is FAT, driven
 * out of espix_fs/fat.c. What this file owns is the part that needs to know
 * about both: which device and partition a name refers to, the block device view
 * over it, and the record that lets `umount` give that view back.
 *
 * All four commands exist in a build without the USB host, and explain
 * themselves. A command that vanishes when an option is off leaves the user
 * comparing their device against documentation and guessing.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "espix_auth.h"
#include "espix_cmds_priv.h"
#include "espix_fs.h"
#include "espix_shell.h"
#include "espix_usb.h"

#define TAG "blk"

#define LSBLK_USAGE  "usage: lsblk [disk]\n"
#define BLKID_USAGE  "usage: blkid [device]...\n"
#define MOUNT_USAGE \
    "usage: mount [-o uid=<id>[,gid=<id>]] [device|/dev/device path]\n"
#define UMOUNT_USAGE "usage: umount path|device...\n"

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

        /*
         * A note rather than a column: it describes the whole table, not the row
         * above it, and the listing has no room for a flag that is false almost
         * always. The alternative -- printing fewer rows and saying nothing -- is
         * what this exists to stop.
         */
        if (devs[i].table_skipped) {
            espix_printf(s, "%s: entries not shown (an unnameable type, or an "
                            "entry outside the device)\n", devs[i].name);
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
/*
 * A device operand in the form the block layer names things: `sda1`, whether it
 * was written that way or as `/dev/sda1`.
 *
 * Both spellings are in use everywhere else, and the /dev names are what a user
 * sees in `ls /dev` -- which is where the name of a stick is looked up from in
 * the first place. Accepting only one of them would mean remembering which, and
 * the argument is already known to be a device rather than a path, so there is
 * nothing to confuse it with.
 */
static const char *dev_operand(const char *arg, char *buf, size_t len)
{
    if (strncmp(arg, "/dev/", 5) == 0) {
        strlcpy(buf, arg + 5, len);
        return buf;
    }
    return arg;
}

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

        char devbuf[ESPIX_USB_NAME_MAX];
        const char *want = dev_operand(argv[a], devbuf, sizeof(devbuf));

        for (size_t i = 0; i < n && !found; i++) {
            found = dev_named(&devs[i], want);
        }
        if (!found) {
            espix_eprintf(s, "blkid: %s: no such device\n", want);
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
            char devbuf[ESPIX_USB_NAME_MAX];
            const char *want = dev_operand(argv[a], devbuf, sizeof(devbuf));

            if (strcmp(want, devs[i].name) == 0) {
                disk_wanted = true;
            }
            for (size_t j = 0; j < devs[i].nparts; j++) {
                if (strcmp(want, devs[i].parts[j].name) == 0) {
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
            /* A flag rather than a value, so not through kv(): this says the
             * partition list beside it is not the whole table, which a script
             * reading only sda1..sda4 would otherwise never learn. */
            if (devs[i].table_skipped) {
                espix_printf(s, " SKIPPED=\"1\"");
            }
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
/* Mounting                                                            */
/* ------------------------------------------------------------------ */

/*
 * What is mounted where.
 *
 * espix_fs owns the mount table itself; this is the part only the command layer
 * can know -- which device a mount came from, and the block device *view* this
 * file created for a partition, which has to be given back when the mount goes.
 * A superfloppy has no view: it mounts the disk's own block device, which
 * espix_usb owns and this file must never release.
 *
 * The records are not locked. Mounting is rare and the mount table underneath is
 * properly serialised, so the worst two concurrent mounts can do is both pick the
 * same free slot and leave one record behind — which shows as `umount` saying
 * `not mounted` for a mount that exists.
 */
typedef struct {
    bool                  used;
    char                  dev[ESPIX_USB_NAME_MAX];
    char                  path[ESPIX_PATH_MAX];
    esp_blockdev_handle_t view;
    /*
     * The session that mounted it. A non-root mount is allowed where the mount
     * point is already the caller's (see cmd_mount), and the volume then belongs
     * to them -- which is also what makes them the one who may unmount it, the
     * way Linux's `user` option does.
     */
    uint16_t              uid;
} mount_rec_t;

static mount_rec_t s_mounts[ESPIX_FS_MAX_MOUNTS];

static mount_rec_t *mount_by_path(const char *path)
{
    for (size_t i = 0; i < ESPIX_FS_MAX_MOUNTS; i++) {
        if (s_mounts[i].used && strcmp(s_mounts[i].path, path) == 0) {
            return &s_mounts[i];
        }
    }
    return NULL;
}

static mount_rec_t *mount_by_dev(const char *dev)
{
    for (size_t i = 0; i < ESPIX_FS_MAX_MOUNTS; i++) {
        if (s_mounts[i].used && strcmp(s_mounts[i].dev, dev) == 0) {
            return &s_mounts[i];
        }
    }
    return NULL;
}

static mount_rec_t *mount_free_slot(void)
{
    for (size_t i = 0; i < ESPIX_FS_MAX_MOUNTS; i++) {
        if (!s_mounts[i].used) {
            return &s_mounts[i];
        }
    }
    return NULL;
}

/*
 * The disk a name refers to, and the partition within it when the name is a
 * partition's. `part` comes back NULL for a whole disk, which is the superfloppy
 * case: the filesystem is on the disk itself, not on a partition of it.
 */
static bool blk_lookup(const espix_usb_dev_t *devs, size_t n, const char *name,
                       const espix_usb_dev_t **disk,
                       const espix_usb_part_t **part)
{
    for (size_t i = 0; i < n; i++) {
        for (size_t j = 0; j < devs[i].nparts; j++) {
            if (strcmp(devs[i].parts[j].name, name) == 0) {
                *disk = &devs[i];
                *part = &devs[i].parts[j];
                return true;
            }
        }
        if (strcmp(devs[i].name, name) == 0) {
            *disk = &devs[i];
            *part = NULL;
            return true;
        }
    }
    return false;
}

/*
 * mount: put a FAT filesystem espix found on the port into the namespace.
 *
 * FAT and only FAT. The other names `lsblk` prints are there to say what a
 * volume *is* -- an attempt to mount one says "not supported" rather than
 * looking like a broken driver, which is the same reason the column exists.
 */
/*
 * `uid=<id>[,gid=<id>]`, the value `mount -o` takes.
 *
 * Numeric only. espix has no passwd lookup -- getpwnam is not implemented, and
 * the names a user would rather type live in /etc/passwd, which espix_auth reads
 * for itself -- so `id` prints the number to use. Names arrive with the fstab
 * parser, which has to resolve them anyway and can share cmd_fs.c's resolver.
 */
static bool parse_owner(espix_session_t *s, const char *arg,
                        uint16_t *uid, uint16_t *gid)
{
    char buf[64];

    if (strlcpy(buf, arg, sizeof(buf)) >= sizeof(buf)) {
        espix_eprintf(s, "mount: -o: too long\n");
        return false;
    }

    for (char *tok = strtok(buf, ","); tok != NULL; tok = strtok(NULL, ",")) {
        uint16_t   *out   = NULL;
        const char *value = NULL;

        if (strncmp(tok, "uid=", 4) == 0) {
            out   = uid;
            value = tok + 4;
        } else if (strncmp(tok, "gid=", 4) == 0) {
            out   = gid;
            value = tok + 4;
        } else {
            espix_eprintf(s, "mount: -o %s: only uid= and gid= are understood\n",
                          tok);
            return false;
        }

        char *end = NULL;
        const unsigned long n = strtoul(value, &end, 10);
        if (end == value || *end != '\0' || n > UINT16_MAX) {
            espix_eprintf(s, "mount: -o %s: not an id (a number, 0..65535)\n",
                          tok);
            return false;
        }
        *out = (uint16_t)n;
    }

    return true;
}

static int cmd_mount(espix_session_t *s, int argc, char **argv)
{
    if (!espix_usb_host_built()) {
        return no_host(s, "mount");
    }

    /* No operands: what is mounted. Readable by anyone, as /proc/mounts is. */
    if (argc == 1) {
        for (size_t i = 0; i < ESPIX_FS_MAX_MOUNTS; i++) {
            if (s_mounts[i].used) {
                espix_printf(s, "%s on %s type vfat\n", s_mounts[i].dev,
                             s_mounts[i].path);
            }
        }
        return 0;
    }
    /*
     * `-o uid=<id>[,gid=<id>]`, anywhere among the arguments rather than only
     * first: GNU mount accepts it after the operands because getopt permutes,
     * and `mount sda1 /mnt -o uid=esp` is what fingers type. The first cut here
     * took it in position one only, which made the feature unusable in the
     * spelling anyone would reach for -- found by running exactly that.
     *
     * Root only: a session mounting at a directory of its own gets itself as the
     * owner with no option at all, and an option that could name someone else
     * would let a session hand away a volume it cannot then use.
     */
    uint16_t owner_uid = (s != NULL) ? s->uid : 0;
    uint16_t owner_gid = (s != NULL) ? s->gid : 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") != 0) {
            continue;
        }
        if (i + 1 >= argc) {
            espix_eprintf(s, MOUNT_USAGE);
            return 1;
        }
        if (s == NULL || s->uid != 0) {
            espix_eprintf(s, "mount: -o is root's to use\n");
            return 1;
        }
        if (!parse_owner(s, argv[i + 1], &owner_uid, &owner_gid)) {
            return 1;
        }

        /* Drop the pair, keeping everything else in order. */
        for (int j = i; j + 2 <= argc; j++) {
            argv[j] = argv[j + 2];
        }
        argc -= 2;
        break;
    }

    if (argc != 3) {
        espix_eprintf(s, MOUNT_USAGE);
        return 1;
    }

    char devbuf[ESPIX_USB_NAME_MAX];
    const char *devname = dev_operand(argv[1], devbuf, sizeof(devbuf));

    /* Resolved against the session's cwd like every other path argument, then
     * required to exist -- see below. */
    char abs[ESPIX_PATH_MAX];
    if (!espix_cmd_path(s, argv[2], abs, sizeof(abs))) {
        return 1;
    }
    const char *path = abs;

    espix_usb_dev_t devs[ESPIX_USB_MAX_DEVS];
    const size_t n = attached(s, "mount", devs);

    const espix_usb_dev_t  *disk = NULL;
    const espix_usb_part_t *part = NULL;

    if (!blk_lookup(devs, n, devname, &disk, &part)) {
        espix_eprintf(s, "mount: %s: no such device\n", devname);
        return 1;
    }

    const char *fstype  = (part != NULL) ? part->fstype : disk->fstype;
    const bool  foreign = (part != NULL) ? part->foreign : disk->foreign;

    if (fstype[0] == '\0') {
        espix_eprintf(s, "mount: %s: no filesystem espix can read\n", devname);
        return 1;
    }
    if (foreign || strcmp(fstype, "vfat") != 0) {
        espix_eprintf(s, "mount: %s: %s is not supported\n", devname, fstype);
        return 1;
    }

    /*
     * The mount point has to be there already. Making it would be a directory
     * appearing out of nowhere, and hiding whatever stood there is the caller's
     * business rather than something to do silently.
     */
    struct stat st;
    if (stat(path, &st) != 0) {
        espix_eprintf(s, "mount: %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (!S_ISDIR(st.st_mode)) {
        espix_eprintf(s, "mount: %s: not a directory\n", path);
        return 1;
    }

    /*
     * Root mounts anywhere; anyone else only where they already own the
     * directory. That is Linux's `user` option in effect -- the fstab entry which
     * grants it is, here, the mount point being theirs -- and it is what keeps a
     * session from mounting over a path it has no business claiming.
     *
     * Ownership comes from espix_fs_owner(), not from st.st_uid: espix does not
     * fill that field at all (nothing in the tree assigns it), because a stat is
     * what the filesystem below says and ownership is espix's own rule. sftp,
     * exec and the access checker all ask the same way, so this does too.
     *
     * The volume then belongs to them without anything else being done about it:
     * espix_fs_mount_fat() is handed the mounting session's uid and gid, and the
     * mode rule answers from that. So a stick `esp` mounts reads and writes as
     * `esp`, and a stick root mounts stays root's, which is what `sudo mount`
     * gets.
     */
    uint16_t dir_uid = 0;
    espix_fs_owner(path, NULL, &dir_uid, NULL);

    if (s == NULL || (s->uid != 0 && dir_uid != s->uid)) {
        espix_eprintf(s, "mount: %s: only its owner or root can mount here "
                         "(directory uid %u, session uid %u)\n",
                      path, (unsigned)dir_uid, (unsigned)(s ? s->uid : 0));
        return 1;
    }

    if (mount_by_path(path) != NULL) {
        espix_eprintf(s, "mount: %s: already mounted\n", path);
        return 1;
    }
    const mount_rec_t *other = mount_by_dev(devname);
    if (other != NULL) {
        /* Two FAT contexts over one disk would each cache the same sectors and
         * both write them back, so this is refused rather than allowed to be
         * confusing later. */
        espix_eprintf(s, "mount: %s: already mounted on %s\n", devname,
                      other->path);
        return 1;
    }

    mount_rec_t *rec = mount_free_slot();
    if (rec == NULL) {
        espix_eprintf(s, "mount: no free mount slot\n");
        return 1;
    }

    /*
     * The device is named by its *disk*, and a partition becomes a view over
     * that device using the offsets the partition table gave -- the same numbers
     * `lsblk` prints. Nothing is copied, and nothing is read until the
     * filesystem asks.
     *
     * espix's own view rather than IDF's esp_blockdev_generic_partition_get():
     * that one takes its offsets as `size_t`, so a 30GB partition on a 32-bit
     * target mounts as the low 32 bits of itself and fails later rather than
     * here. part.c has the arithmetic.
     */
    esp_blockdev_handle_t disk_bdl = espix_usb_dev_blockdev(disk->name);
    if (disk_bdl == NULL) {
        espix_eprintf(s, "mount: %s: no block device (unplugged?)\n", disk->name);
        return 1;
    }

    esp_blockdev_handle_t view = NULL;
    esp_blockdev_handle_t dev  = disk_bdl;

    if (part != NULL) {
        const esp_err_t err = espix_fs_partition_view(
            disk_bdl, part->start, part->size, &view);
        if (err != ESP_OK) {
            espix_eprintf(s, "mount: %s: cannot read the partition: %s\n",
                          devname, esp_err_to_name(err));
            return 1;
        }
        dev = view;
    }

    const esp_err_t err = espix_fs_mount_fat(path, dev, owner_uid, owner_gid);
    if (err != ESP_OK) {
        if (view != NULL) {
            view->ops->release(view);
        }
        if (err == ESP_ERR_INVALID_STATE) {
            espix_eprintf(s, "mount: %s: already mounted\n", path);
        } else if (err == ESP_ERR_NOT_FOUND) {
            espix_eprintf(s, "mount: %s: not a FAT filesystem\n", devname);
        } else if (err == ESP_ERR_NO_MEM) {
            /* Two things answer with this one: FatFs has no free volume, or an
             * allocation failed. fat.c logs which, on this console, so this says
             * only what the command itself knows. */
            espix_eprintf(s, "mount: %s: cannot mount (no free volume, or out "
                             "of memory)\n", devname);
        } else {
            espix_eprintf(s, "mount: %s: %s\n", devname, esp_err_to_name(err));
        }
        return 1;
    }

    memset(rec, 0, sizeof(*rec));
    rec->used = true;
    rec->view = view;
    rec->uid  = (s != NULL) ? s->uid : 0;
    snprintf(rec->dev, sizeof(rec->dev), "%s", devname);
    snprintf(rec->path, sizeof(rec->path), "%s", path);

    /* Silent on success, like mount(8) without -v: a command that reports every
     * routine thing is one whose output nobody reads. */
    return 0;
}

/* One operand: a mount point, or a device name. Returns 0 when it went. */
static int umount_one(espix_session_t *s, const char *arg)
{
    /*
     * A path -- resolved like every other command's path argument -- or a device
     * name. umount(8) takes either and there is no reason to be stricter.
     */
    char abs[ESPIX_PATH_MAX];
    mount_rec_t *rec = NULL;

    if (espix_cmd_path(s, arg, abs, sizeof(abs))) {
        rec = mount_by_path(abs);
    }
    if (rec == NULL) {
        char devbuf[ESPIX_USB_NAME_MAX];
        rec = mount_by_dev(dev_operand(arg, devbuf, sizeof(devbuf)));
    }
    if (rec == NULL) {
        espix_eprintf(s, "umount: %s: not mounted\n", arg);
        return 1;
    }

    if (s == NULL || (s->uid != 0 && rec->uid != s->uid)) {
        espix_eprintf(s, "umount: %s: mounted by another user\n", rec->path);
        return 1;
    }

    const esp_err_t err = espix_fs_unmount_fat(rec->path);
    if (err == ESP_ERR_INVALID_STATE) {
        espix_eprintf(s, "umount: %s: busy -- a file is open on it\n", rec->path);
        return 1;
    }
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        espix_eprintf(s, "umount: %s: %s\n", rec->path, esp_err_to_name(err));
        return 1;
    }

    /*
     * The view was this file's to create and is this file's to release. The
     * disk's own block device is not: that one belongs to espix_usb, and
     * releasing it here would be a double free the moment the stick is pulled.
     */
    if (rec->view != NULL) {
        rec->view->ops->release(rec->view);
    }
    memset(rec, 0, sizeof(*rec));

    return 0;
}

/*
 * umount: take it back out, and give back what was made for it.
 *
 * Several operands, as umount(8) takes: `umount /mnt/a /mnt/b` is how a script
 * tidies up, and stopping at the first failure would leave the rest mounted with
 * nothing said about them. Every operand is attempted, and the exit status is 1
 * if any of them failed -- a single "not mounted" among three good ones is worth
 * knowing about rather than swallowing.
 *
 * Refused while anything is open on the mount, by the layer that can tell --
 * espix_fs answers ESP_ERR_INVALID_STATE rather than pulling a volume out from
 * under a reader who is halfway through a file.
 */
static int cmd_umount(espix_session_t *s, int argc, char **argv)
{
    if (!espix_usb_host_built()) {
        return no_host(s, "umount");
    }
    if (argc < 2) {
        espix_eprintf(s, UMOUNT_USAGE);
        return 1;
    }

    /* Root and the mounter; checked per operand, in umount_one(), because one
     * session can own one mount and not another. */
    int status = 0;
    for (int i = 1; i < argc; i++) {
        if (umount_one(s, argv[i]) != 0) {
            status = 1;
        }
    }
    return status;
}

/* ------------------------------------------------------------------ */

/*
 * The device a mount came from has gone.
 *
 * Called from espix_usb's detach hook, *before* the USB stack gives the block
 * device back -- that order is the whole point, and it is why this exists rather
 * than being left to an umount command: nothing else notices a stick leaving.
 *
 * Marked dead first, so anything still holding a file fails instead of reading
 * memory the USB stack is about to free, and then unmounted so the mount point is
 * free again. The unmount refuses while a file is open, and that refusal is fine:
 * the mount stays, marked, refusing everything until its reader is finished.
 */
void espix_blk_device_gone(const char *dev)
{
    if (dev == NULL) {
        return;
    }

    for (size_t i = 0; i < ESPIX_FS_MAX_MOUNTS; i++) {
        mount_rec_t *rec = &s_mounts[i];

        /*
         * Prefix, not equality: the hook names the *disk* -- "sda" -- while a
         * record holds whatever was mounted, which is usually a partition of it
         * ("sda1"). One letter plus digits means a disk name can never be a
         * prefix of another disk's, so this cannot match the wrong volume.
         */
        if (!rec->used || strncmp(rec->dev, dev, strlen(dev)) != 0) {
            continue;
        }

        char path[ESPIX_PATH_MAX];
        strlcpy(path, rec->path, sizeof(path));

        (void)espix_fs_mount_dead(path);

        /* Silent on purpose: no session is behind this, and the klog line the
         * mark writes is the record of it. */
        if (espix_fs_unmount_fat(path) == ESP_OK) {
            if (rec->view != NULL) {
                rec->view->ops->release(rec->view);
                rec->view = NULL;
            }
            memset(rec, 0, sizeof(*rec));
        }
    }
}

/* ------------------------------------------------------------------ */
/* /etc/fstab                                                          */
/* ------------------------------------------------------------------ */

#define FSTAB_PATH  "/etc/fstab"

/*
 * The file, when it has never been written.
 *
 * Created rather than shipped: the rootfs skeleton is built at boot, and the
 * comment *is* the documentation -- whoever opens this should not need a manual
 * to find out that the columns are not Linux's.
 *
 * Nothing is enabled in it. CONFIG_FATFS_VOLUME_COUNT is 2, and a wildcard on a
 * four-partition stick would ask for three volumes, so a default policy would be
 * a default failure. Uncommenting a line is the opt-in.
 */
static const char FSTAB_TEMPLATE[] =
    "# espix fstab -- not Linux's. Three fields, doing the jobs theirs do.\n"
    "#\n"
    "#   <device>       <mount point>   <owner>   [flags]\n"
    "#\n"
    "# device       what lsblk prints: sda1, or sd*1 for partition 1 of any disk\n"
    "# mount point  a template; %s becomes the device name\n"
    "# owner        an account name, a uid, or - for root\n"
    "# flags        noauto, to leave the device alone until somebody mounts it\n"
    "#\n"
    "# Mounting happens when a device is attached and unmounting when it is\n"
    "# removed. A volume belongs to the owner named here, so the example below\n"
    "# gives `esp` a stick it can read and write without sudo.\n"
    "#\n"
    "#   sd*1   /media/%s   esp\n"
    "#   sda4   /srv/backup -        noauto\n";

/* `*` matches any run of characters, and that is the whole pattern language:
 * enough for "partition N of any disk", and small enough to check by reading. */
static bool fstab_glob(const char *pat, const char *name)
{
    while (*pat != '\0') {
        if (*pat == '*') {
            pat++;
            if (*pat == '\0') {
                return true;
            }
            for (const char *p = name; *p != '\0'; p++) {
                if (fstab_glob(pat, p)) {
                    return true;
                }
            }
            return false;
        }
        if (*name == '\0' || *pat != *name) {
            return false;
        }
        pat++;
        name++;
    }
    return *name == '\0';
}

/* `uid`/`gid` from an owner field: an account name, a number, or `-` for root. */
static bool fstab_owner(const char *word, uint16_t *uid, uint16_t *gid)
{
    if (strcmp(word, "-") == 0) {
        *uid = 0;
        *gid = 0;
        return true;
    }

    char *end = NULL;
    const unsigned long n = strtoul(word, &end, 10);
    if (end != word && *end == '\0') {
        if (n > UINT16_MAX) {
            return false;
        }
        *uid = (uint16_t)n;
        *gid = (uint16_t)n;
        return true;
    }

    espix_user_t u;
    if (espix_auth_lookup(word, &u) != ESP_OK) {
        return false;
    }
    *uid = u.uid;
    *gid = u.gid;
    return true;
}

/* The mount point, with `%s` replaced by the device name when it has one. A
 * point without `%s` is used as written, which is how a policy names one
 * particular stick. Replaced by hand rather than through printf: a format string
 * read out of a file is a class of bug worth not having, root's file or not. */
static void fstab_path(char *out, size_t len, const char *tmpl, const char *name)
{
    const char *at = strstr(tmpl, "%s");

    if (at == NULL) {
        strlcpy(out, tmpl, len);
        return;
    }

    const size_t head = (size_t)(at - tmpl);
    const size_t mid  = strlen(name);
    const size_t tail = strlen(at + 2);

    if (head + mid + tail + 1 > len) {
        out[0] = '\0';
        return;
    }

    memcpy(out, tmpl, head);
    memcpy(out + head, name, mid);
    memcpy(out + head + mid, at + 2, tail + 1);
}

/*
 * Mount one volume where fstab says, as the owner it names.
 *
 * Quiet about failure: this runs with nobody watching, and a log line is the
 * only thing anybody can do about it at that moment.
 */
static void fstab_mount(const espix_usb_dev_t *disk,
                        const espix_usb_part_t *part,
                        const char *path, uint16_t uid, uint16_t gid)
{
    const char *fstype  = (part != NULL) ? part->fstype : disk->fstype;
    const bool  foreign = (part != NULL) ? part->foreign : disk->foreign;

    if (fstype[0] == '\0' || foreign || strcmp(fstype, "vfat") != 0) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "%s: %s is not one to mount", path,
                   fstype[0] != '\0' ? fstype : "unrecognised");
        return;
    }

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "%s: no such directory; not mounting there", path);
        return;
    }
    if (mount_by_path(path) != NULL) {
        return;                     /* already mounted; nothing to add */
    }

    esp_blockdev_handle_t disk_bdl = espix_usb_dev_blockdev(disk->name);
    if (disk_bdl == NULL) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "%s: no block device", disk->name);
        return;
    }

    esp_blockdev_handle_t view = NULL;
    esp_blockdev_handle_t dev  = disk_bdl;

    if (part != NULL &&
        espix_fs_partition_view(disk_bdl, part->start, part->size,
                                &view) != ESP_OK) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "%s: cannot read the partition",
                   path);
        return;
    }
    if (view != NULL) {
        dev = view;
    }

    const esp_err_t err = espix_fs_mount_fat(path, dev, uid, gid);
    if (err != ESP_OK) {
        if (view != NULL) {
            view->ops->release(view);
        }
        espix_klog(ESPIX_KLOG_WARN, TAG, "%s: cannot mount: %s", path,
                   esp_err_to_name(err));
        return;
    }

    mount_rec_t *rec = mount_free_slot();
    if (rec == NULL) {
        (void)espix_fs_unmount_fat(path);
        espix_klog(ESPIX_KLOG_WARN, TAG, "%s: no free mount slot", path);
        return;
    }

    memset(rec, 0, sizeof(*rec));
    rec->used = true;
    rec->view = view;
    snprintf(rec->dev, sizeof(rec->dev), "%s",
             (part != NULL) ? part->name : disk->name);
    snprintf(rec->path, sizeof(rec->path), "%s", path);
    /*
     * `uid` is the *owner*, not a mounter, because there is no session here --
     * and that is also who may unmount it, which is what an fstab `user` entry
     * means on Linux.
     */
    rec->uid = uid;

    espix_klog(ESPIX_KLOG_INFO, TAG, "%s: mounted %s at %s", FSTAB_PATH,
               rec->dev, path);
}

/* Every line that matches this device, or the template if the file is absent. */
static void fstab_apply(const espix_usb_dev_t *devs, size_t n, const char *dev)
{
    FILE *f = fopen(FSTAB_PATH, "r");
    if (f == NULL) {
        FILE *t = fopen(FSTAB_PATH, "w");
        if (t != NULL) {
            fputs(FSTAB_TEMPLATE, t);
            fclose(t);
            espix_klog(ESPIX_KLOG_INFO, TAG,
                       "%s: written, with the examples commented out",
                       FSTAB_PATH);
        }
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), f) != NULL) {
        char *hash = strchr(line, '#');
        if (hash != NULL) {
            *hash = '\0';
        }

        char name[ESPIX_USB_NAME_MAX];
        char point[ESPIX_PATH_MAX];
        char owner[ESPIX_USER_MAX];
        char flags[32] = "";

        if (sscanf(line, "%7s %127s %31s %31s", name, point, owner, flags) < 3) {
            continue;
        }
        if (strstr(flags, "noauto") != NULL) {
            continue;
        }

        uint16_t uid = 0;
        uint16_t gid = 0;
        if (!fstab_owner(owner, &uid, &gid)) {
            espix_klog(ESPIX_KLOG_WARN, TAG, "%s: no such account: %s",
                       FSTAB_PATH, owner);
            continue;
        }

        for (size_t i = 0; i < n; i++) {
            if (strcmp(devs[i].name, dev) != 0) {
                continue;           /* a different disk */
            }

            /* The disk itself first -- a superfloppy has no partitions -- and
             * then each partition. */
            if (fstab_glob(name, devs[i].name)) {
                char path[ESPIX_PATH_MAX];
                fstab_path(path, sizeof(path), point, devs[i].name);
                if (path[0] != '\0') {
                    fstab_mount(&devs[i], NULL, path, uid, gid);
                }
            }

            for (size_t j = 0; j < devs[i].nparts; j++) {
                if (!fstab_glob(name, devs[i].parts[j].name)) {
                    continue;
                }
                char path[ESPIX_PATH_MAX];
                fstab_path(path, sizeof(path), point, devs[i].parts[j].name);
                if (path[0] != '\0') {
                    fstab_mount(&devs[i], &devs[i].parts[j], path, uid, gid);
                }
            }
        }
    }

    fclose(f);
}

/*
 * A storage device has appeared. Runs on the USB work task -- the context the
 * attach hook fires in, chosen so that transfers complete while a device is
 * installed -- so filesystem calls are safe here. That task's stack is the
 * budget to watch, not a guarantee: it is 3.5KB, and mounting is the deepest
 * thing ever asked to run on it.
 */
void espix_blk_device_added(const char *dev)
{
    if (dev == NULL) {
        return;
    }

    espix_usb_dev_t devs[ESPIX_USB_MAX_DEVS];
    const size_t n = espix_usb_devlist(devs, ESPIX_USB_MAX_DEVS);

    fstab_apply(devs, n, dev);
}

static espix_cmd_t s_blk_cmds[] = {
    { .name = "lsblk", .fn = cmd_lsblk,
      /* The MBR limitation belongs where someone will read it, which is the
       * command's own help rather than a document they have not opened. */
      .help = "list block devices and their filesystems (MBR only, no GPT)",
      .usage = "lsblk [disk]" },
    { .name = "blkid", .fn = cmd_blkid,
      .help = "print a device's identity, filesystem and label",
      .usage = "blkid [device]..." },
    { .name = "mount", .fn = cmd_mount,
      /* The root-only rule belongs in the help: a user who is told why is not
       * left thinking the command is broken. */
      .help = "mount a FAT filesystem (root, or the mount point's owner)",
      .usage = "mount [-o uid=<id>[,gid=<id>]] [device|/dev/device path]" },
    { .name = "umount", .fn = cmd_umount,
      .help = "unmount a mounted filesystem (its mounter, or root)",
      .usage = "umount path|device..." },
};

void espix_cmds_register_blk(void)
{
    espix_cmds_register_table(s_blk_cmds,
                              sizeof(s_blk_cmds) / sizeof(s_blk_cmds[0]));
}
