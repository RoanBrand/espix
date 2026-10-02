/*
 * Synthetic filesystems: the trees espix answers for without a lower
 * filesystem -- /dev and /proc.
 *
 * Both live inside espix's own VFS rather than being registered as separate
 * filesystems at /dev and /proc. That is deliberate: espix owns the "" prefix
 * so that every file call passes through espix_fs_access_check(), and
 * registering a second VFS at a path would give that path a route with the
 * checks skipped. So the trees are consulted from vfs_open() and friends,
 * after the check -- see vfs.c.
 *
 * This file is the engine. dev.c defines the /dev tree and its block-device
 * pool; proc.c defines /proc, whose files are generated on every read. A node
 * is one of four kinds and the engine reads them all.
 *
 * The fds are the interesting constraint. espix's VFS hands esp_vfs the lower
 * filesystem's fd directly, and esp_vfs stores it in a local_fd_t, which on
 * every target except Linux is a uint8_t, so a synthetic fd has to fit in
 * 0..255. The top of the range is reserved for them (ESPIX_SYNTH_FD_BASE) and
 * vfs_open() refuses a lower-filesystem fd that reaches into it rather than
 * letting it alias -- an enforced reservation instead of a hoped-for one.
 *
 * A slot rather than a bare node index because two readers of one file need
 * two file positions, and a generated file has no position of its own at all.
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

static const char *TAG = "synth";

/* How long a generated file may be. Every /proc file espix publishes is a few
 * hundred bytes; a longer one is truncated rather than allocated, because the
 * point of /proc here is to cost nothing at rest. */
#define SYNTH_TEXT_MAX 512

static const synth_tree_t *s_trees[ESPIX_SYNTH_TREES_MAX];
static int                 s_tree_count;

typedef struct {
    const synth_node_t *node;   /* NULL when the slot is free */
    off_t               pos;
} synth_slot_t;

typedef struct {
    DIR                 dir;    /* first: esp_vfs stamps dd_vfs_idx here */
    struct dirent       ent;
    const synth_tree_t *tree;
    int                 cursor;
    bool                in_use;
} synth_dir_t;

static synth_slot_t      s_slots[ESPIX_SYNTH_FD_COUNT];
static synth_dir_t       s_dirs[ESPIX_SYNTH_DIR_MAX];
static SemaphoreHandle_t s_lock;

void espix_synth_register_tree(const synth_tree_t *tree)
{
    if (tree == NULL || s_tree_count >= ESPIX_SYNTH_TREES_MAX) {
        return;
    }
    s_trees[s_tree_count++] = tree;
}

void espix_synth_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (s_tree_count == 0) {
        espix_synth_register_tree(espix_fs_dev_tree());
        espix_synth_register_tree(espix_fs_proc_tree());
    }
}

/* The partition behind an app node, or NULL when this image has no such slot.
 * Not cached: the answer cannot change, but the walk is over a handful of
 * entries and a cache would need a lock on every read. */
static const esp_partition_t *app_of(const synth_node_t *n)
{
    if (n->kind != SYNTH_APP) {
        return NULL;
    }
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                    (esp_partition_subtype_t)n->subtype, NULL);
}

/* A path that names a tree itself ("/dev", "/proc"). */
static const synth_tree_t *dir_tree(const char *abs_path)
{
    for (int t = 0; t < s_tree_count; t++) {
        if (strcmp(abs_path, s_trees[t]->prefix) == 0) {
            return s_trees[t];
        }
    }
    return NULL;
}

/* A path inside a tree ("/dev/null", "/proc/meminfo"). */
static const synth_tree_t *under_tree(const char *abs_path)
{
    for (int t = 0; t < s_tree_count; t++) {
        const synth_tree_t *tr = s_trees[t];
        if (strncmp(abs_path, tr->prefix, tr->prefix_len) == 0 &&
            abs_path[tr->prefix_len] == '/') {
            return tr;
        }
    }
    return NULL;
}

const void *espix_synth_lookup(const char *abs_path)
{
    const synth_tree_t *tr = under_tree(abs_path);
    if (tr == NULL) {
        return NULL;
    }

    for (int i = 0; i < tr->count; i++) {
        const synth_node_t *n = &tr->nodes[i];
        if (strcmp(abs_path, n->path) != 0) {
            continue;
        }
        /* A node for a partition this image does not have does not exist. */
        if (n->kind == SYNTH_APP && app_of(n) == NULL) {
            return NULL;
        }
        return n;
    }

    return (tr->dyn_lookup != NULL) ? tr->dyn_lookup(abs_path) : NULL;
}

bool espix_synth_isdir(const char *abs_path)
{
    return dir_tree(abs_path) != NULL;
}

bool espix_synth_under(const char *abs_path)
{
    return under_tree(abs_path) != NULL;
}

static off_t node_size(const synth_node_t *n)
{
    switch (n->kind) {
    case SYNTH_BLOCK:
        return (off_t)n->size;
    case SYNTH_APP: {
        const esp_partition_t *p = app_of(n);
        return (p != NULL) ? (off_t)p->size : 0;
    }
    default:
        /* A generated file has no size to report; Linux reports 0 too, and the
         * reader reads to EOF rather than to a length. */
        return 0;
    }
}

void espix_synth_stat(const void *handle, struct stat *st)
{
    const synth_node_t *n = handle;

    memset(st, 0, sizeof(*st));
    st->st_mode    = n->mode;
    st->st_nlink   = 1;
    st->st_size    = node_size(n);
    st->st_blksize = 4096;
}

void espix_synth_dir_stat(struct stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_mode    = S_IFDIR | 0755;
    st->st_nlink   = 2;
    st->st_size    = 0;
    st->st_blksize = 4096;
}

bool espix_synth_mode(const char *abs_path, mode_t *out)
{
    const synth_node_t *n = espix_synth_lookup(abs_path);
    if (n == NULL) {
        return false;
    }
    *out = n->mode & ESPIX_MODE_BITS;
    return true;
}

int espix_synth_open(const void *handle, int flags)
{
    const synth_node_t *n = handle;

    /*
     * A block node is a name, not a stream: it exists so a device has the name
     * every other system gives it and so mount can take it. Raw sector access
     * would have to know which device it holds and refuse a mounted one, so
     * opening one says so instead of pretending. Linux differs; see the POSIX
     * surface table in docs/ROADMAP.md.
     */
    if (n->kind == SYNTH_BLOCK) {
        errno = EOPNOTSUPP;
        return -1;
    }

    /* Read-only nodes refuse a writable open outright, the way a read-only
     * filesystem does, rather than accepting it and failing every write. */
    if (n->kind == SYNTH_APP || n->kind == SYNTH_TEXT) {
        const int acc = flags & O_ACCMODE;
        if (acc == O_WRONLY || acc == O_RDWR) {
            errno = EROFS;
            return -1;
        }
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < ESPIX_SYNTH_FD_COUNT; i++) {
        if (s_slots[i].node == NULL) {
            s_slots[i].node = n;
            s_slots[i].pos  = 0;
            xSemaphoreGive(s_lock);
            return ESPIX_SYNTH_FD_BASE + i;
        }
    }
    xSemaphoreGive(s_lock);

    errno = EMFILE;
    return -1;
}

static synth_slot_t *slot_of(int fd)
{
    const int i = fd - ESPIX_SYNTH_FD_BASE;

    if (i < 0 || i >= ESPIX_SYNTH_FD_COUNT || s_slots[i].node == NULL) {
        return NULL;
    }
    return &s_slots[i];
}

int espix_synth_close(int fd)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    synth_slot_t *s = slot_of(fd);
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
static ssize_t read_at(const synth_node_t *n, void *dst, size_t size, off_t off)
{
    if (n->kind == SYNTH_NULL) {
        return 0;                       /* always end of input */
    }

    if (n->kind == SYNTH_TEXT) {
        char         buf[SYNTH_TEXT_MAX];
        const size_t len = (n->gen != NULL) ? n->gen(buf, sizeof(buf)) : 0;

        if (off < 0 || off >= (off_t)len) {
            return 0;
        }
        size_t want = size;
        if (off + (off_t)want > (off_t)len) {
            want = (size_t)((off_t)len - off);
        }
        memcpy(dst, buf + off, want);
        return (ssize_t)want;
    }

    const esp_partition_t *p = app_of(n);
    if (p == NULL) {
        errno = EIO;
        return -1;
    }
    if (off < 0 || off >= (off_t)p->size) {
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

ssize_t espix_synth_read(int fd, void *dst, size_t size)
{
    synth_slot_t *s = slot_of(fd);
    if (s == NULL) {
        errno = EBADF;
        return -1;
    }

    const ssize_t got = read_at(s->node, dst, size, s->pos);
    if (got > 0) {
        s->pos += got;
    }
    return got;
}

ssize_t espix_synth_pread(int fd, void *dst, size_t size, off_t off)
{
    synth_slot_t *s = slot_of(fd);
    if (s == NULL) {
        errno = EBADF;
        return -1;
    }
    return read_at(s->node, dst, size, off);
}

ssize_t espix_synth_write(int fd, const void *data, size_t size)
{
    (void)data;

    synth_slot_t *s = slot_of(fd);
    if (s == NULL) {
        errno = EBADF;
        return -1;
    }
    if (s->node->kind != SYNTH_NULL) {
        errno = EROFS;
        return -1;
    }
    /* Swallowed, and reported as written: that is what makes it a sink. */
    return (ssize_t)size;
}

off_t espix_synth_lseek(int fd, off_t off, int whence)
{
    synth_slot_t *s = slot_of(fd);
    if (s == NULL) {
        errno = EBADF;
        return -1;
    }

    const off_t end = node_size(s->node);
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

int espix_synth_fstat(int fd, struct stat *st)
{
    synth_slot_t *s = slot_of(fd);
    if (s == NULL) {
        errno = EBADF;
        return -1;
    }
    espix_synth_stat(s->node, st);
    return 0;
}

int espix_synth_fsync(int fd)
{
    if (slot_of(fd) == NULL) {
        errno = EBADF;
        return -1;
    }
    return 0;                           /* nothing is ever pending */
}

/* ------------------------------------------------------------------ */
/* Directory handles                                                   */
/* ------------------------------------------------------------------ */

static synth_dir_t *dir_of(DIR *pdir)
{
    for (int i = 0; i < ESPIX_SYNTH_DIR_MAX; i++) {
        if (s_dirs[i].in_use && (DIR *)&s_dirs[i] == pdir) {
            return &s_dirs[i];
        }
    }
    return NULL;
}

bool espix_synth_dirp(DIR *pdir)
{
    return pdir != NULL && dir_of(pdir) != NULL;
}

DIR *espix_synth_opendir(const char *abs_path)
{
    const synth_tree_t *tr = (abs_path != NULL) ? dir_tree(abs_path) : NULL;
    if (tr == NULL) {
        errno = ENOTDIR;
        return NULL;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < ESPIX_SYNTH_DIR_MAX; i++) {
        if (!s_dirs[i].in_use) {
            s_dirs[i].in_use = true;
            s_dirs[i].cursor = 0;
            s_dirs[i].tree   = tr;
            xSemaphoreGive(s_lock);
            return (DIR *)&s_dirs[i];
        }
    }
    xSemaphoreGive(s_lock);

    errno = ENFILE;
    return NULL;
}

int espix_synth_readdir_r(DIR *pdir, struct dirent *entry, struct dirent **out)
{
    synth_dir_t *d = dir_of(pdir);
    if (d == NULL) {
        errno = EBADF;
        return -1;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    /*
     * The static table first, skipping a node whose partition this image does
     * not have; then whatever the tree offers dynamically (the live block
     * pool), which is why the cursor is only ever advanced here.
     */
    const synth_node_t *n = NULL;
    while (d->cursor < d->tree->count) {
        const synth_node_t *cand = &d->tree->nodes[d->cursor++];
        if (cand->kind == SYNTH_APP && app_of(cand) == NULL) {
            continue;
        }
        n = cand;
        break;
    }
    if (n == NULL && d->tree->dyn_at != NULL) {
        n = d->tree->dyn_at(d->cursor - d->tree->count);
        if (n != NULL) {
            d->cursor++;
        }
    }

    if (n == NULL) {
        xSemaphoreGive(s_lock);
        *out = NULL;
        return 0;
    }

    const char *base = strrchr(n->path, '/') + 1;

    memset(entry, 0, sizeof(*entry));
    switch (n->kind) {
    case SYNTH_NULL:  entry->d_type = DT_CHR; break;
    case SYNTH_BLOCK: entry->d_type = DT_BLK; break;
    default:          entry->d_type = DT_REG; break;
    }
    strlcpy(entry->d_name, base, sizeof(entry->d_name));

    xSemaphoreGive(s_lock);

    *out = entry;
    return 0;
}

struct dirent *espix_synth_readdir(DIR *pdir)
{
    synth_dir_t *d = dir_of(pdir);
    if (d == NULL) {
        errno = EBADF;
        return NULL;
    }

    struct dirent *out = NULL;
    if (espix_synth_readdir_r(pdir, &d->ent, &out) != 0) {
        return NULL;
    }
    return out;
}

long espix_synth_telldir(DIR *pdir)
{
    synth_dir_t *d = dir_of(pdir);
    return (d != NULL) ? d->cursor : -1;
}

void espix_synth_seekdir(DIR *pdir, long offset)
{
    synth_dir_t *d = dir_of(pdir);
    if (d != NULL && offset >= 0) {
        d->cursor = (int)offset;
    }
}

int espix_synth_closedir(DIR *pdir)
{
    synth_dir_t *d = dir_of(pdir);
    if (d == NULL) {
        errno = EBADF;
        return -1;
    }

    d->in_use = false;
    d->cursor = 0;
    d->tree   = NULL;
    return 0;
}
