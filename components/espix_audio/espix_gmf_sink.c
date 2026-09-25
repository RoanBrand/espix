#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_gmf_err.h"
#include "esp_gmf_io.h"
#include "esp_gmf_payload.h"
#include "esp_gmf_oal_mem.h"
#include "esp_gmf_obj.h"

#include "espix_kernel.h"
#include "espix_bt.h"
#include "espix_gmf_sink.h"

#define TAG "a2dp_sink"

typedef struct {
    esp_gmf_io_t base;
    bool         open;
} sink_t;

static esp_gmf_err_t sink_init_cb(void *cfg, esp_gmf_obj_handle_t *io);

static esp_gmf_err_t sink_open(esp_gmf_io_handle_t io)
{
    ((sink_t *)io)->open = true;
    return ESP_GMF_ERR_OK;
}

static esp_gmf_err_t sink_close(esp_gmf_io_handle_t io)
{
    ((sink_t *)io)->open = false;
    return ESP_GMF_ERR_OK;
}

static esp_gmf_err_t sink_seek(esp_gmf_io_handle_t io, uint64_t pos)
{
    (void)io;
    (void)pos;
    return ESP_GMF_ERR_OK;
}

static esp_gmf_err_io_t sink_acquire_write(esp_gmf_io_handle_t io, void *payload,
                                           uint32_t wanted_size, int block_ticks)
{
    (void)io;
    (void)payload;
    (void)wanted_size;
    (void)block_ticks;
    return ESP_GMF_IO_OK;
}

/*
 * The payload's PCM goes into the A2DP ring. A full ring is not an error: it is
 * the sink telling us it has enough for now, so the IO blocks here (yielding)
 * and the pipeline slows to the link's rate rather than dropping samples.
 */
static esp_gmf_err_io_t sink_release_write(esp_gmf_io_handle_t io, void *payload, int block_ticks)
{
    (void)io;
    (void)block_ticks;

    esp_gmf_payload_t *p = (esp_gmf_payload_t *)payload;
    if (p == NULL || p->valid_size == 0) {
        return ESP_GMF_IO_OK;
    }

    size_t off = 0;
    while (off < p->valid_size) {
        if (espix_bt_audio_write((const uint8_t *)p->buf + off, p->valid_size - off) == ESP_OK) {
            off = p->valid_size;
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    return ESP_GMF_IO_OK;
}

static esp_gmf_err_t sink_delete(esp_gmf_io_handle_t io)
{
    if (io != NULL) {
        sink_t *s = (sink_t *)io;
        esp_gmf_io_deinit(io);
        esp_gmf_oal_free(s);
    }
    return ESP_GMF_ERR_OK;
}

esp_gmf_err_t espix_gmf_sink_init(const char *tag, esp_gmf_io_handle_t *io)
{
    if (io == NULL) {
        return ESP_GMF_ERR_INVALID_ARG;
    }
    *io = NULL;

    sink_t *s = esp_gmf_oal_calloc(1, sizeof(sink_t));
    if (s == NULL) {
        return ESP_GMF_ERR_MEMORY_LACK;
    }

    esp_gmf_obj_t *obj = (esp_gmf_obj_t *)s;
    obj->new_obj = sink_init_cb;
    obj->del_obj = sink_delete;
    if (esp_gmf_obj_set_tag(obj, (tag != NULL) ? tag : "io_a2dp") != ESP_GMF_ERR_OK) {
        esp_gmf_oal_free(s);
        return ESP_GMF_ERR_FAIL;
    }

    s->base.dir            = ESP_GMF_IO_DIR_WRITER;
    s->base.type           = ESP_GMF_IO_TYPE_BYTE;
    s->base.open           = sink_open;
    s->base.close          = sink_close;
    s->base.seek           = sink_seek;
    s->base.acquire_write  = sink_acquire_write;
    s->base.release_write  = sink_release_write;

    const esp_gmf_io_cfg_t cfg = {
        .thread = {
            .stack        = 3 * 1024,
            .prio         = 5,
            .core         = 1,
            .stack_in_ext = true,
        },
        .buffer_cfg = {
            /* The IO open allocates a data bus when buffer_size is non-zero;
             * zero is what made it fail with "Failed to create data bus". */
            .io_size     = 4096,
            .buffer_size = 4096,
        },
        .enable_speed_monitor = false,
    };
    if (esp_gmf_io_init(obj, (esp_gmf_io_cfg_t *)&cfg) != ESP_GMF_ERR_OK) {
        esp_gmf_obj_delete(obj);
        return ESP_GMF_ERR_FAIL;
    }
    *io = obj;
    return ESP_GMF_ERR_OK;
}

static esp_gmf_err_t sink_init_cb(void *cfg, esp_gmf_obj_handle_t *io)
{
    (void)cfg;
    return espix_gmf_sink_init("io_a2dp", (esp_gmf_io_handle_t *)io);
}
