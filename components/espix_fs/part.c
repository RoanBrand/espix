/*
 * A block device view over a partition.
 *
 * Why not esp_blockdev_generic_partition_get()?
 * --------------------------------------------
 * IDF's does the same job with one difference that decides it: its start offset
 * and size are `size_t`, which is 32 bits on every ESP32 target. A 30GB
 * partition -- one FAT32 volume on an ordinary stick, which is exactly the case
 * this exists for -- cannot be expressed in it. The size truncates to its low 32
 * bits, the volume then mounts as a fraction of itself, and the failure arrives
 * later as reads past the truncation rather than at mount time, which is the
 * worst way for it to arrive.
 *
 * So this is IDF's implementation with 64-bit offsets and the same arithmetic,
 * minus the asserts: an over-long read on a user's stick is worth an error code,
 * not a panic.
 *
 * The view is released with its own ops->release, exactly as IDF's is. The parent
 * belongs to the caller and releasing a view never touches it.
 */

#include <stdlib.h>
#include <string.h>

#include "esp_blockdev.h"
#include "esp_err.h"

#include "espix_fs.h"

typedef struct {
    esp_blockdev_t        dev;
    esp_blockdev_handle_t parent;
    uint64_t              start;
} part_view_t;

/* The view's own bounds, checked before anything is translated. */
static bool within(const part_view_t *v, uint64_t addr, size_t len)
{
    return addr <= v->dev.geometry.disk_size &&
           len <= v->dev.geometry.disk_size - addr;
}

static esp_err_t part_read(esp_blockdev_handle_t dev, uint8_t *dst,
                           size_t dst_size, uint64_t src, size_t len)
{
    part_view_t *v = (part_view_t *)dev;

    if (v == NULL || dst == NULL || len > dst_size || !within(v, src, len)) {
        return ESP_ERR_INVALID_ARG;
    }
    return v->parent->ops->read(v->parent, dst, dst_size, v->start + src, len);
}

static esp_err_t part_write(esp_blockdev_handle_t dev, const uint8_t *src,
                            uint64_t dst, size_t len)
{
    part_view_t *v = (part_view_t *)dev;

    if (v == NULL || src == NULL || !within(v, dst, len)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (v->dev.device_flags.read_only) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return v->parent->ops->write(v->parent, src, v->start + dst, len);
}

static esp_err_t part_erase(esp_blockdev_handle_t dev, uint64_t start,
                            size_t len)
{
    part_view_t *v = (part_view_t *)dev;

    if (v == NULL || !within(v, start, len)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (v->dev.device_flags.read_only || v->parent->ops->erase == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return v->parent->ops->erase(v->parent, v->start + start, len);
}

static esp_err_t part_sync(esp_blockdev_handle_t dev)
{
    part_view_t *v = (part_view_t *)dev;

    if (v->parent->ops->sync == NULL) {
        return ESP_OK;
    }
    return v->parent->ops->sync(v->parent);
}

/*
 * Only the two commands that carry an address need translating. Everything else
 * -- sector size, and whatever a later version adds -- means the same thing to a
 * partition as to the disk it sits on.
 */
static esp_err_t part_ioctl(esp_blockdev_handle_t dev, const uint8_t cmd,
                            void *args)
{
    part_view_t *v = (part_view_t *)dev;

    if (v->parent->ops->ioctl == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (cmd != ESP_BLOCKDEV_CMD_MARK_DELETED &&
        cmd != ESP_BLOCKDEV_CMD_ERASE_CONTENTS) {
        return v->parent->ops->ioctl(v->parent, cmd, args);
    }
    if (args == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_blockdev_cmd_arg_erase_t *erase = (esp_blockdev_cmd_arg_erase_t *)args;
    if (!within(v, erase->start_addr, erase->erase_len)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_blockdev_cmd_arg_erase_t translated = *erase;
    translated.start_addr = v->start + erase->start_addr;

    return v->parent->ops->ioctl(v->parent, cmd, &translated);
}

static esp_err_t part_release(esp_blockdev_handle_t dev)
{
    free(dev);
    return ESP_OK;
}

static const esp_blockdev_ops_t s_part_ops = {
    .read    = part_read,
    .write   = part_write,
    .erase   = part_erase,
    .sync    = part_sync,
    .ioctl   = part_ioctl,
    .release = part_release,
};

esp_err_t espix_fs_partition_view(esp_blockdev_handle_t parent, uint64_t start,
                                  uint64_t size, bool readonly,
                                  esp_blockdev_handle_t *out)
{
    if (parent == NULL || parent->ops == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;

    /*
     * Against the parent's size, and in a form that cannot wrap: a partition
     * table is data off a user's stick, and `start + size` overflowing is exactly
     * the arithmetic a wrong one would break.
     */
    if (size == 0 || start >= parent->geometry.disk_size ||
        size > parent->geometry.disk_size - start) {
        return ESP_ERR_INVALID_ARG;
    }
    if (parent->ops->read == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    part_view_t *v = calloc(1, sizeof(*v));
    if (v == NULL) {
        return ESP_ERR_NO_MEM;
    }

    v->parent          = parent;
    v->start           = start;
    v->dev.ctx         = v;
    v->dev.ops         = &s_part_ops;
    v->dev.geometry    = parent->geometry;
    v->dev.device_flags = parent->device_flags;
    v->dev.geometry.disk_size = size;
    /*
     * The caller's answer, never the parent's: a view is the only place espix
     * can carry "this mount is read-only" without touching a handle it does not
     * own. The USB disk handle is cached and shared, so setting the flag there
     * would make every later mount of the same disk read-only too.
     *
     * FatFs reads it through ff_diskio_register_bdl(), whose status callback
     * answers STA_PROTECT for a read-only device; f_open then refuses a write
     * with FR_WRITE_PROTECTED (ff.c:3508) instead of attempting one and failing
     * at the disk. part_write() refuses as well, so the guarantee holds even for
     * a caller that reaches the block device directly.
     */
    if (readonly) {
        v->dev.device_flags.read_only = true;
    }

    *out = &v->dev;
    return ESP_OK;
}