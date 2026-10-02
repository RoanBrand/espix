/*
 * The /dev tree: the device nodes espix answers for.
 *
 * The engine is synth.c; this file is the tree itself plus the block-device
 * pool behind it. It lives inside espix's own VFS rather than being registered
 * at /dev so that every call passes espix_fs_access_check() -- see synth.c.
 *
 * Application partitions are world-readable because they hold firmware code and
 * no secrets. A node exists only while its partition does: the 16MB A/B table
 * has no factory, so /dev/factory is absent there rather than present and
 * broken; both OTA slots are listed, the passive one included, so the previous
 * image can be read. Which slot is running is a question for upgrade --slots,
 * not for a directory listing -- a device node cannot carry that.
 *
 * The storage partition is deliberately absent. Its raw image contains
 * /etc/ssh/host_ecdsa_key and /etc/wifi.conf, protected by file permissions
 * today -- tests/suites/10-fs.sh asserts an ordinary account is refused both --
 * and a readable raw device would hand them over while bypassing the filesystem
 * entirely.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_partition.h"

#include "espix_kernel.h"

#include "espix_fs_priv.h"

static const char *TAG = "devfs";

static const synth_node_t s_dev_nodes[] = {
    { "/dev/null",    SYNTH_NULL, S_IFCHR | 0666, 0, 0, NULL },
    { "/dev/factory", SYNTH_APP,  S_IFREG | 0444, 0, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL },
    { "/dev/ota0",    SYNTH_APP,  S_IFREG | 0444, 0, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL },
    { "/dev/ota1",    SYNTH_APP,  S_IFREG | 0444, 0, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL },
};

#define DEV_NODE_COUNT ((int)(sizeof(s_dev_nodes) / sizeof(s_dev_nodes[0])))

/*
 * Block devices get nodes too, so /dev/sda and /dev/sda1 exist the way they do
 * everywhere else and mount can take the name someone would type.
 *
 * A fixed pool rather than malloc: a bounded array cannot fragment the heap and
 * cannot fail to allocate. Twenty is what the USB component can have attached
 * at once -- four disks, four partitions each -- so a sequence of attaches
 * cannot outgrow it. The node is embedded so the pointer lookup() hands out
 * lives as long as the reservation, and path points into the entry's buffer.
 */
#define ESPIX_DEV_BLOCK_MAX  20
#define ESPIX_DEV_BLOCK_PATH 16        /* "/dev/sda1" and its NUL */

typedef struct {
    synth_node_t node;
    char         own_path[ESPIX_DEV_BLOCK_PATH];
    bool         used;
} dev_blk_t;

static dev_blk_t         s_blocks[ESPIX_DEV_BLOCK_MAX];
static SemaphoreHandle_t s_block_lock;

static void block_lock_init(void)
{
    if (s_block_lock == NULL) {
        s_block_lock = xSemaphoreCreateMutex();
    }
}

/* The block node at a position in the listing, or NULL. Positional and in pool
 * order, so a listing is stable while nothing is plugged or unplugged. */
static const synth_node_t *block_at(int nth)
{
    synth_node_t *found = NULL;

    block_lock_init();
    xSemaphoreTake(s_block_lock, portMAX_DELAY);
    for (int i = 0; i < ESPIX_DEV_BLOCK_MAX; i++) {
        if (!s_blocks[i].used) {
            continue;
        }
        if (nth-- == 0) {
            found = &s_blocks[i].node;
            break;
        }
    }
    xSemaphoreGive(s_block_lock);
    return found;
}

static const synth_node_t *block_lookup(const char *abs_path)
{
    synth_node_t *found = NULL;

    block_lock_init();
    xSemaphoreTake(s_block_lock, portMAX_DELAY);
    for (int i = 0; i < ESPIX_DEV_BLOCK_MAX; i++) {
        if (s_blocks[i].used && strcmp(abs_path, s_blocks[i].own_path) == 0) {
            found = &s_blocks[i].node;
            break;
        }
    }
    xSemaphoreGive(s_block_lock);
    return found;
}

/*
 * Give a block device a name in /dev, or take it away. name is the device name
 * without the directory: "sda" for a disk, "sda1" for a partition.
 *
 * Called from the USB attach/detach hook, so this runs in the USB task, and the
 * pool is locked because a session can be listing or reading /dev meanwhile.
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

    block_lock_init();
    xSemaphoreTake(s_block_lock, portMAX_DELAY);

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
            xSemaphoreGive(s_block_lock);
            return ESP_OK;
        }
    }

    if (slot != NULL) {
        strlcpy(slot->own_path, path, sizeof(slot->own_path));
        slot->node.path = slot->own_path;
        slot->node.kind = SYNTH_BLOCK;
        slot->node.mode = S_IFBLK | 0660;
        slot->node.size = size;
        slot->used      = true;
    }
    xSemaphoreGive(s_block_lock);

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

    block_lock_init();
    xSemaphoreTake(s_block_lock, portMAX_DELAY);
    for (int i = 0; i < ESPIX_DEV_BLOCK_MAX; i++) {
        if (s_blocks[i].used && strcmp(s_blocks[i].own_path, path) == 0) {
            s_blocks[i].used = false;
            break;
        }
    }
    xSemaphoreGive(s_block_lock);
}

static const synth_tree_t s_dev_tree = {
    .prefix     = "/dev",
    .prefix_len = 4,
    .nodes      = s_dev_nodes,
    .count      = DEV_NODE_COUNT,
    .dyn_at     = block_at,
    .dyn_lookup = block_lookup,
};

const synth_tree_t *espix_fs_dev_tree(void)
{
    block_lock_init();
    return &s_dev_tree;
}
