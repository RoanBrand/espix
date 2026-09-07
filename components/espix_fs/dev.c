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
 */

#include <errno.h>
#include <fcntl.h>
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
} dev_kind_t;

typedef struct {
    const char *path;
    dev_kind_t  kind;
    mode_t      mode;       /* type bits included */
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
    { "/dev/null",    DEV_NULL,    S_IFCHR | 0666 },
    { "/dev/factory", DEV_FACTORY, S_IFREG | 0444 },
};

#define DEV_COUNT ((int)(sizeof(s_nodes) / sizeof(s_nodes[0])))

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
    return NULL;
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
