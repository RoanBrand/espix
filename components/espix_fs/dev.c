/*
 * Device nodes: /dev/null and /dev/factory.
 *
 * These live inside espix's own VFS rather than being registered as a separate
 * filesystem at "/dev". That is deliberate and it is the whole reason this file
 * looks the way it does: espix owns the "" prefix so that *every* file call in
 * the system passes through espix_fs_access_check() (see vfs.c and fs.c).
 * Registering a second VFS at a path would give that path a route to a
 * filesystem with the checks skipped -- exactly what fs.c warns about for
 * littlefs. So the device table is consulted from vfs_open(), after the check.
 *
 * The fds are the interesting constraint. espix's VFS hands esp_vfs the lower
 * filesystem's fd directly, and esp_vfs stores it in a `local_fd_t`, which on
 * every target except Linux is a **uint8_t**:
 *
 *     esp-idf/components/vfs/private_include/esp_vfs_private.h
 *     typedef uint8_t local_fd_t;
 *
 * so a device fd has to fit in 0..255. The first design reserved something like
 * 0x1000 to be obviously distinct; that would have been silently truncated to 0
 * and aliased littlefs's first open file. Instead the devices take the top of
 * the byte, and vfs_open() refuses a lower-filesystem fd that reaches into the
 * range rather than letting it alias -- an enforced reservation instead of a
 * hoped-for one. littlefs's fds are small indices into a cache that starts at a
 * handful of entries, and FD_SETSIZE here is MEMP_NUM_NETCONN, so the range is
 * never approached in practice; the check is there so "never in practice" does
 * not have to be trusted.
 *
 * A slot rather than a bare device index because two readers of /dev/factory
 * need two file positions.
 *
 * /dev is also a *directory* here, not a littlefs one: vfs_opendir() hands back
 * a synthetic DIR built on the table below, and vfs.c refuses every other
 * operation under /dev. So the directory lists the nodes, and a file an older
 * image happened to leave inside it is neither shown nor reachable.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_partition.h"

#include "espix_fs.h"
#include "espix_kernel.h"

#include "espix_fs_priv.h"

static const char *TAG = "devfs";

typedef enum {
    DEV_NULL,
    DEV_FACTORY,
    DEV_BLOCK,
} dev_kind_t;

typedef struct {
    const char *path;
    dev_kind_t  kind;
    mode_t      mode;       /* type bits included */
    uint64_t    size;       /* block nodes only; 0 elsewhere */
} dev_node_t;

/*
 * /dev/factory is world-readable because it holds firmware code and no secrets.
 *
 * The `storage` partition is deliberately absent. Its raw image contains
 * /etc/ssh/host_ecdsa_key and /etc/wifi.conf, which are protected by file
 * permissions today -- tests/suites/10-fs.sh asserts an ordinary account is
 * refused both -- and a readable raw device would hand them over while
 * bypassing the filesystem entirely. Exposing it would have to be root-only,
 * and that is a decision to take on its own rather than as a side effect of
 * wanting somewhere to read from.
 */
static const dev_node_t s_nodes[] = {
    { "/dev/null",    DEV_NULL,    S_IFCHR | 0666, 0 },
    { "/dev/factory", DEV_FACTORY, S_IFREG | 0444, 0 },
};

#define DEV_COUNT ((int)(sizeof(s_nodes) / sizeof(s_nodes[0])))

/*
 * Block devices get nodes too, so that /dev/sda and /dev/sda1 exist the way they
 * do everywhere else, and `mount` can take the name someone would type.
 *
 * Registered from outside -- espix_dev_register_block() -- because nothing in
 * espix_fs knows what a USB device is, and this is the seam that keeps it that
 * way: the component that knows both wires the two together.
 *
 * A fixed pool rather than malloc, for the reason the directory pool below is:
 * a bounded array cannot fragment the heap and cannot fail to allocate. Twenty
 * is what the USB component can have attached at once -- four disks, and four
 * partitions each -- so the pool cannot be outgrown by a sequence of attaches.
 *
 * The node is embedded so the pointer lookup() hands out lives as long as the
 * reservation, and `path` points into the entry's own buffer.
 */
#define ESPIX_DEV_BLOCK_MAX  20
#define ESPIX_DEV_BLOCK_PATH 16        /* "/dev/sda1" and its NUL */

typedef struct {
    dev_node_t node;
    char       own_path[ESPIX_DEV_BLOCK_PATH];
    bool       used;
} dev_blk_t;

static dev_blk_t s_blocks[ESPIX_DEV_BLOCK_MAX];

/* The block node at a position in the directory listing, or NULL. Positional and
 * in pool order, so a listing is stable while nothing is plugged or unplugged. */
static const dev_node_t *block_at(int nth)
{
    for (int i = 0; i < ESPIX_DEV_BLOCK_MAX; i++) {
        if (!s_blocks[i].used) {
            continue;
        }
        if (nth-- == 0) {
            return &s_blocks[i].node;
        }
    }
    return NULL;
}

typedef struct {
    const dev_node_t *node;     /* NULL when the slot is free */
    off_t             pos;
} dev_slot_t;

static dev_slot_t        s_slots[ESPIX_DEV_FD_COUNT];
static SemaphoreHandle_t s_lock;

/* The app partition, found once. NULL until the first open of /dev/factory,
 * and NULL for ever on a build without one. */
static const esp_partition_t *s_factory;

void espix_dev_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
}

/*
 * Give a block device a name in /dev, or take it away. `name` is the device name
 * without the directory: "sda" for a disk, "sda1" for a partition.
 *
 * Called from the USB attach/detach hook, so this runs in the USB task, and the
 * pool is locked because a session can be reading /dev meanwhile.
 *
 * Registering a name that already exists updates it rather than adding a second
 * node: a re-attach of the same slot arrives with the same name, and the pool
 * must not fill with duplicates.
 */
esp_err_t espix_dev_register_block(const char *name, uint64_t size)
{
    char path[ESPIX_DEV_BLOCK_PATH];

    if (name == NULL || name[0] == '\0' ||
        snprintf(path, sizeof(path), "/dev/%.*s", ESPIX_DEV_BLOCK_PATH - 7,
                 name) >= (int)sizeof(path)) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    dev_blk_t *slot = NULL;
    for (int i = 0; i < ESPIX_DEV_BLOCK_MAX; i++) {
        if (!s_blocks[i].used) {
            if (slot == NULL) {
                slot = &s_blocks[i];
            }
            continue;
        }
        if (strcmp(s_blocks[i].own_path, path) == 0) {
            s_blocks[i].node.size = size;
            xSemaphoreGive(s_lock);
            return ESP_OK;
        }
    }

    if (slot != NULL) {
        strlcpy(slot->own_path, path, sizeof(slot->own_path));
        slot->node.path = slot->own_path;
        slot->node.kind = DEV_BLOCK;
        slot->node.mode = S_IFBLK | 0660;
        slot->node.size = size;
        slot->used      = true;
    }
    xSemaphoreGive(s_lock);

    if (slot == NULL) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "no room for %s in /dev", path);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void espix_dev_unregister_block(const char *name)
{
    char path[ESPIX_DEV_BLOCK_PATH];

    if (name == NULL || name[0] == '\0' ||
        snprintf(path, sizeof(path), "/dev/%.*s", ESPIX_DEV_BLOCK_PATH - 7,
                 name) >= (int)sizeof(path)) {
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < ESPIX_DEV_BLOCK_MAX; i++) {
        if (s_blocks[i].used && strcmp(s_blocks[i].own_path, path) == 0) {
            s_blocks[i].used = false;
            break;
        }
    }
    xSemaphoreGive(s_lock);
}

static const esp_partition_t *factory(void)
{
    if (s_factory == NULL) {
        s_factory = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                             ESP_PARTITION_SUBTYPE_APP_FACTORY,
                                             NULL);
        if (s_factory == NULL) {
            espix_klog(ESPIX_KLOG_WARN, TAG, "no factory partition to expose");
        }
    }
    return s_factory;
}

static off_t dev_size(const dev_node_t *n)
{
    if (n->kind == DEV_BLOCK) {
        return (off_t)n->size;
    }
    if (n->kind == DEV_FACTORY) {
        const esp_partition_t *p = factory();
        return (p != NULL) ? (off_t)p->size : 0;
    }
    return 0;
}

const void *espix_dev_lookup(const char *abs_path)
{
    for (int i = 0; i < DEV_COUNT; i++) {
        if (strcmp(abs_path, s_nodes[i].path) == 0) {
            return &s_nodes[i];
        }
    }

    /* Locked, because a block node can be registered or taken away by the USB
     * task while this is walking the pool. */
    const void *found = NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < ESPIX_DEV_BLOCK_MAX; i++) {
        if (s_blocks[i].used && strcmp(abs_path, s_blocks[i].own_path) == 0) {
            found = &s_blocks[i].node;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return found;
}

void espix_dev_stat(const void *handle, struct stat *st)
{
    const dev_node_t *n = handle;

    memset(st, 0, sizeof(*st));
    st->st_mode    = n->mode;
    st->st_nlink   = 1;
    st->st_size    = dev_size(n);
    st->st_blksize = 4096;
}

int espix_dev_open(const void *handle, int flags)
{
    const dev_node_t *n = handle;

    /*
     * A block node is a name, not a stream. It exists so the device has the name
     * every other system gives it, and so `mount` can take it; raw sector access
     * is a feature of its own -- it would have to know which device it holds and
     * refuse to hand out one that is mounted -- so opening one says so instead of
     * pretending.
     *
     * ESPIX_NOT_POSIX: on Linux opening a block device gives a readable stream.
     * Here it is EOPNOTSUPP. See the POSIX surface table in docs/ROADMAP.md.
     */
    if (n->kind == DEV_BLOCK) {
        errno = EOPNOTSUPP;
        return -1;
    }

    /* Read-only devices refuse a writable open outright, the way a read-only
     * filesystem does, rather than accepting it and failing every write. */
    if (n->kind == DEV_FACTORY) {
        const int acc = flags & O_ACCMODE;
        if (acc == O_WRONLY || acc == O_RDWR) {
            errno = EROFS;
            return -1;
        }
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < ESPIX_DEV_FD_COUNT; i++) {
        if (s_slots[i].node == NULL) {
            s_slots[i].node = n;
            s_slots[i].pos  = 0;
            xSemaphoreGive(s_lock);
            return ESPIX_DEV_FD_BASE + i;
        }
    }
    xSemaphoreGive(s_lock);

    errno = EMFILE;
    return -1;
}

/* ------------------------------------------------------------------ */
/* The /dev directory                                                  */
/* ------------------------------------------------------------------ */

/*
 * /dev is a directory espix answers for without it existing on littlefs.
 * Paths inside it reach the device table and nothing else, so a file someone
 * left in an older image's /dev is neither listed nor reachable -- which is
 * also why no migration is needed to clean one up.
 *
 * The handle is what makes this fit ESP-IDF: esp_vfs_opendir() writes
 * dd_vfs_idx into the DIR it is handed back, and every later call routes on
 * that field, so the DIR must be the first member of whatever opendir()
 * returns. A real static pool, rather than malloc, because /dev holds two
 * fixed entries and a bounded pool cannot fragment the heap.
 *
 * Sized to CONFIG_ESPIX_SSH_MAX_SESSIONS rather than to a guess at how many
 * people list /dev at once. A handle lives only for the length of one `ls`, so
 * four would almost always do -- but "almost always" here fails as ENFILE from
 * opendir(), which reads as the device being out of something rather than as a
 * pool being small, and the parallel test runner alone holds four sessions
 * before anyone types anything. Eight slots is 8 * sizeof(dev_dir_t) of .bss to
 * make the failure unreachable.
 */
#define ESPIX_DEV_DIR_MAX 8

typedef struct {
    DIR            dir;     /* first: esp_vfs_opendir() stamps dd_vfs_idx here */
    struct dirent  ent;     /* readdir()'s answer, stable until the next call */
    int            cursor;  /* next node to hand out, 0..DEV_COUNT */
    bool           in_use;
} dev_dir_t;

static dev_dir_t s_dirs[ESPIX_DEV_DIR_MAX];

bool espix_dev_isdir(const char *abs_path)
{
    return strcmp(abs_path, "/dev") == 0;
}

bool espix_dev_underdev(const char *abs_path)
{
    return strncmp(abs_path, "/dev/", 5) == 0;
}

/* Only the owner triad is meaningful here; the type char and perms come from
 * the table, and the size is what ls shows (0 for a directory). */
void espix_dev_dir_stat(struct stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_mode    = S_IFDIR | 0755;
    st->st_nlink   = 2;
    st->st_size    = 0;
    st->st_blksize = 4096;
}

/*
 * A device path's permission bits, for the mode rule. The table is the one
 * place a device's mode is written down, so the permission check has to ask
 * it -- otherwise mode_from_rule() would answer 0644 for /dev/null and a
 * non-root shell could not redirect to it.
 */
bool espix_dev_mode(const char *abs_path, mode_t *out)
{
    const dev_node_t *n = espix_dev_lookup(abs_path);
    if (n == NULL) {
        return false;
    }
    *out = n->mode & ESPIX_MODE_BITS;
    return true;
}

static dev_dir_t *dir_of(DIR *pdir)
{
    for (int i = 0; i < ESPIX_DEV_DIR_MAX; i++) {
        if (s_dirs[i].in_use && (DIR *)&s_dirs[i] == pdir) {
            return &s_dirs[i];
        }
    }
    return NULL;
}

bool espix_dev_dirp(DIR *pdir)
{
    return pdir != NULL && dir_of(pdir) != NULL;
}

DIR *espix_dev_opendir(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < ESPIX_DEV_DIR_MAX; i++) {
        if (!s_dirs[i].in_use) {
            s_dirs[i].in_use = true;
            s_dirs[i].cursor  = 0;
            xSemaphoreGive(s_lock);
            return (DIR *)&s_dirs[i];
        }
    }
    xSemaphoreGive(s_lock);

    errno = ENFILE;
    return NULL;
}

int espix_dev_readdir_r(DIR *pdir, struct dirent *entry, struct dirent **out)
{
    dev_dir_t *d = dir_of(pdir);
    if (d == NULL) {
        errno = EBADF;
        return -1;
    }
    if (d->cursor >= DEV_COUNT + ESPIX_DEV_BLOCK_MAX) {
        *out = NULL;
        return 0;
    }

    /* Locked: the block half of the listing is the live pool, and the USB task
     * changes it. The static half does not care. */
    xSemaphoreTake(s_lock, portMAX_DELAY);

    const dev_node_t *n = (d->cursor < DEV_COUNT)
                        ? &s_nodes[d->cursor]
                        : block_at(d->cursor - DEV_COUNT);
    if (n == NULL) {
        xSemaphoreGive(s_lock);
        *out = NULL;
        return 0;
    }
    d->cursor++;

    const char *base = strrchr(n->path, '/') + 1;

    memset(entry, 0, sizeof(*entry));
    switch (n->kind) {
    case DEV_NULL:  entry->d_type = DT_CHR; break;
    case DEV_BLOCK: entry->d_type = DT_BLK; break;
    default:        entry->d_type = DT_REG; break;
    }
    strlcpy(entry->d_name, base, sizeof(entry->d_name));

    xSemaphoreGive(s_lock);

    *out = entry;
    return 0;
}

struct dirent *espix_dev_readdir(DIR *pdir)
{
    dev_dir_t *d = dir_of(pdir);
    if (d == NULL) {
        errno = EBADF;
        return NULL;
    }

    struct dirent *out = NULL;
    if (espix_dev_readdir_r(pdir, &d->ent, &out) != 0) {
        return NULL;
    }
    return out;
}

long espix_dev_telldir(DIR *pdir)
{
    dev_dir_t *d = dir_of(pdir);
    return (d != NULL) ? d->cursor : -1;
}

void espix_dev_seekdir(DIR *pdir, long offset)
{
    dev_dir_t *d = dir_of(pdir);
    if (d != NULL && offset >= 0 && offset <= DEV_COUNT + ESPIX_DEV_BLOCK_MAX) {
        d->cursor = (int)offset;
    }
}

int espix_dev_closedir(DIR *pdir)
{
    dev_dir_t *d = dir_of(pdir);
    if (d == NULL) {
        errno = EBADF;
        return -1;
    }

    d->in_use = false;
    d->cursor = 0;
    return 0;
}

static dev_slot_t *slot_of(int fd)
{
    const int i = fd - ESPIX_DEV_FD_BASE;

    if (i < 0 || i >= ESPIX_DEV_FD_COUNT || s_slots[i].node == NULL) {
        return NULL;
    }
    return &s_slots[i];
}

int espix_dev_close(int fd)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    dev_slot_t *s = slot_of(fd);
    if (s != NULL) {
        s->node = NULL;
        s->pos  = 0;
    }
    xSemaphoreGive(s_lock);

    if (s == NULL) {
        errno = EBADF;
        return -1;
    }
    return 0;
}

/* Read at an explicit offset; the shared half of read() and pread(). */
static ssize_t dev_read_at(const dev_node_t *n, void *dst, size_t size,
                           off_t off)
{
    if (n->kind == DEV_NULL) {
        return 0;                       /* always end of input */
    }

    const esp_partition_t *p = factory();
    if (p == NULL) {
        errno = EIO;
        return -1;
    }
    if (off >= (off_t)p->size) {
        return 0;
    }

    size_t want = size;
    if (off + (off_t)want > (off_t)p->size) {
        want = (size_t)((off_t)p->size - off);
    }
    if (esp_partition_read(p, (size_t)off, dst, want) != ESP_OK) {
        errno = EIO;
        return -1;
    }
    return (ssize_t)want;
}

ssize_t espix_dev_read(int fd, void *dst, size_t size)
{
    dev_slot_t *s = slot_of(fd);
    if (s == NULL) {
        errno = EBADF;
        return -1;
    }

    const ssize_t got = dev_read_at(s->node, dst, size, s->pos);
    if (got > 0) {
        s->pos += got;
    }
    return got;
}

ssize_t espix_dev_pread(int fd, void *dst, size_t size, off_t off)
{
    dev_slot_t *s = slot_of(fd);
    if (s == NULL) {
        errno = EBADF;
        return -1;
    }
    return dev_read_at(s->node, dst, size, off);
}

ssize_t espix_dev_write(int fd, const void *data, size_t size)
{
    (void)data;

    dev_slot_t *s = slot_of(fd);
    if (s == NULL) {
        errno = EBADF;
        return -1;
    }
    if (s->node->kind != DEV_NULL) {
        errno = EROFS;
        return -1;
    }
    /* Swallowed, and reported as written: that is what makes it a sink. */
    return (ssize_t)size;
}

off_t espix_dev_lseek(int fd, off_t off, int whence)
{
    dev_slot_t *s = slot_of(fd);
    if (s == NULL) {
        errno = EBADF;
        return -1;
    }

    const off_t end = dev_size(s->node);
    off_t       to;

    switch (whence) {
    case SEEK_SET: to = off;          break;
    case SEEK_CUR: to = s->pos + off; break;
    case SEEK_END: to = end + off;    break;
    default:       errno = EINVAL;    return -1;
    }

    if (to < 0) {
        errno = EINVAL;
        return -1;
    }
    s->pos = to;
    return to;
}

int espix_dev_fstat(int fd, struct stat *st)
{
    dev_slot_t *s = slot_of(fd);
    if (s == NULL) {
        errno = EBADF;
        return -1;
    }
    espix_dev_stat(s->node, st);
    return 0;
}

int espix_dev_fsync(int fd)
{
    if (slot_of(fd) == NULL) {
        errno = EBADF;
        return -1;
    }
    return 0;                           /* nothing is ever pending */
}
