#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_gmf_err.h"
#include "esp_gmf_element.h"
#include "esp_gmf_pool.h"
#include "esp_gmf_pipeline.h"
#include "esp_gmf_task.h"
#include "esp_gmf_audio_dec.h"
#include "esp_gmf_audio_helper.h"
#include "gmf_loader_setup_defaults.h"

#include "espix_kernel.h"
#include "espix_bt.h"
#include "espix_audio.h"
#include "esp_gmf_obj.h"
#include "esp_gmf_port.h"
#include "esp_gmf_data_bus.h"

#define TAG "audio"

static esp_gmf_pool_handle_t     s_pool;
static esp_gmf_pipeline_handle_t s_pipe;
static esp_gmf_task_handle_t     s_task;
static volatile bool             s_running;
static char                      s_uri[200];

/* ------------------------------------------------------------------ */
/* The audio stack's allocator, moved to PSRAM.                        */
/*                                                                     */
/* esp_audio_codec (prebuilt) and the effects code reach memory        */
/* through media_lib_malloc/calloc/realloc/free, declared *weak* in    */
/* the components that provide them and defaulting to plain malloc --  */
/* internal RAM. Strong definitions win, but they have to live in an   */
/* object the linker already pulls, so they live here.                 */
/*                                                                     */
/* __attribute__((used)) because --gc-sections would otherwise drop    */
/* the ones nothing references by name, leaving only free strong.      */
/* ------------------------------------------------------------------ */
#define MEDIA_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

__attribute__((used)) void *media_lib_malloc(size_t size)
{
    return heap_caps_malloc(size, MEDIA_CAPS);
}

__attribute__((used)) void *media_lib_calloc(size_t num, size_t size)
{
    return heap_caps_calloc(num, size, MEDIA_CAPS);
}

__attribute__((used)) void *media_lib_realloc(void *buf, size_t size)
{
    return heap_caps_realloc(buf, size, MEDIA_CAPS);
}

__attribute__((used)) void media_lib_free(void *buf)
{
    heap_caps_free(buf);
}

/* ------------------------------------------------------------------ */
/* The pipeline                                                        */
/* ------------------------------------------------------------------ */

/*
 * The pipeline's output is a *port*, not an IO -- the same shape the simple
 * player uses: the tail element is handed a writer port whose release callback
 * is where the PCM leaves the framework. (An IO registered in the pool does not
 * give the element an out port to acquire from; that was "Failed to acquire
 * out".)
 *
 * A full A2DP ring is not an error: it is the sink saying it has enough, so the
 * callback blocks (yielding) and the pipeline slows to the link's rate rather
 * than dropping samples.
 */
static int a2dp_acquire_write(void *handle, esp_gmf_data_bus_block_t *blk,
                              uint32_t wanted_size, int block_ticks)
{
    (void)handle;
    (void)blk;
    (void)block_ticks;
    return (int)wanted_size;
}

static int a2dp_release_write(void *handle, esp_gmf_data_bus_block_t *blk, int block_ticks)
{
    (void)handle;
    (void)block_ticks;
    if (blk == NULL || blk->valid_size == 0) {
        return 0;
    }
    size_t off = 0;
    while (off < blk->valid_size) {
        if (espix_bt_audio_write((const uint8_t *)blk->buf + off, blk->valid_size - off) == ESP_OK) {
            off = blk->valid_size;
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    return 0;
}

/*
 * One pool for the process: the loader registers the IO readers, the decoder
 * and the effect elements (including the hardware ASRC, when
 * CONFIG_GMF_AUDIO_EFFECT_INIT_ASRC is on).
 */
static esp_err_t ensure_pool(void)
{
    if (s_pool != NULL) {
        return ESP_OK;
    }
    if (esp_gmf_pool_init(&s_pool) != ESP_GMF_ERR_OK) {
        return ESP_FAIL;
    }
    gmf_loader_setup_io_default(s_pool);
    gmf_loader_setup_audio_codec_default(s_pool);
    gmf_loader_setup_audio_effects_default(s_pool);

    return ESP_OK;
}

static void teardown(void)
{
    if (s_pipe != NULL) {
        esp_gmf_pipeline_stop(s_pipe);
        esp_gmf_pipeline_destroy(s_pipe);
        s_pipe = NULL;
    }
    if (s_task != NULL) {
        esp_gmf_task_deinit(s_task);
        s_task = NULL;
    }
    s_running = false;
}

esp_err_t espix_audio_play(const char *uri)
{
    if (uri == NULL || uri[0] == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!espix_bt_ready()) {
        /* play may be the first command; bring the controller up here. The
         * sink is not required yet -- the ring's backpressure holds playback
         * until A2DP connects. */
        const esp_err_t e = espix_bt_init();
        if (e != ESP_OK) {
            return e;
        }
    }
    if (!espix_bt_a2d_connected()) {
        espix_klog(ESPIX_KLOG_INFO, TAG, "no sink yet; playback waits for A2DP");
    }

    if (ensure_pool() != ESP_OK) {
        return ESP_FAIL;
    }
    teardown();

    const bool net = (strncmp(uri, "http://", 7) == 0 ||
                      strncmp(uri, "https://", 8) == 0);
    const char *els[] = { "aud_dec", "aud_asrc" };

    if (esp_gmf_pool_new_pipeline(s_pool, net ? "io_http" : "io_file",
                                  els, 2, NULL, &s_pipe) != ESP_GMF_ERR_OK) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "cannot build the pipeline");
        teardown();
        return ESP_FAIL;
    }

    esp_gmf_port_handle_t out_port = NEW_ESP_GMF_PORT_OUT_BYTE(
        a2dp_acquire_write, a2dp_release_write, NULL, NULL, 8192, ESP_GMF_MAX_DELAY);
    if (out_port == NULL ||
        esp_gmf_pipeline_reg_el_port(s_pipe, OBJ_GET_TAG(s_pipe->last_el),
                                     ESP_GMF_IO_DIR_WRITER, out_port) != ESP_GMF_ERR_OK) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "cannot attach the A2DP out port");
        teardown();
        return ESP_FAIL;
    }

    /* Tell the decoder what the stream is, from the URI's extension. */
    esp_gmf_element_handle_t dec = NULL;
    if (esp_gmf_pipeline_get_el_by_name(s_pipe, "aud_dec", &dec) == ESP_GMF_ERR_OK && dec != NULL) {
        esp_gmf_info_sound_t info = { 0 };
        if (esp_gmf_audio_helper_get_audio_type_by_uri(uri, &info.format_id) == ESP_GMF_ERR_OK) {
            esp_gmf_audio_dec_reconfig_by_sound_info(dec, &info);
        }
    }

    if (esp_gmf_pipeline_set_in_uri(s_pipe, uri) != ESP_GMF_ERR_OK) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "%s: cannot set uri", uri);
        teardown();
        return ESP_FAIL;
    }

    esp_gmf_task_cfg_t cfg = DEFAULT_ESP_GMF_TASK_CONFIG();
    cfg.thread.stack        = 6 * 1024;
    cfg.thread.prio         = 20;   /* above the Wi-Fi/lwIP tasks on core 1 */
    cfg.thread.core         = 1;
    cfg.thread.stack_in_ext = true; /* the stack is in PSRAM */

    if (esp_gmf_task_init(&cfg, &s_task) != ESP_GMF_ERR_OK ||
        esp_gmf_pipeline_bind_task(s_pipe, s_task) != ESP_GMF_ERR_OK ||
        esp_gmf_pipeline_loading_jobs(s_pipe) != ESP_GMF_ERR_OK ||
        esp_gmf_pipeline_run(s_pipe) != ESP_GMF_ERR_OK) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "cannot start %s", uri);
        teardown();
        return ESP_FAIL;
    }

    strlcpy(s_uri, uri, sizeof(s_uri));
    s_running = true;
    return ESP_OK;
}

esp_err_t espix_audio_stop(void)
{
    if (s_pipe == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    teardown();
    return ESP_OK;
}

const char *espix_audio_state(void)
{
    return s_running ? "playing" : "idle";
}
