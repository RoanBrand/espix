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
#include "espix_gmf_sink.h"

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
 * One pool for the process: the loader registers the IO readers, the decoder
 * and the effect elements (including the hardware ASRC, when
 * CONFIG_GMF_AUDIO_EFFECT_INIT_ASRC is on), and espix adds its A2DP writer.
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

    esp_gmf_io_handle_t sink = NULL;
    if (espix_gmf_sink_init("io_a2dp", &sink) != ESP_GMF_ERR_OK ||
        esp_gmf_pool_register_io(s_pool, sink, "io_a2dp") != ESP_GMF_ERR_OK) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "cannot register the A2DP sink");
        return ESP_FAIL;
    }
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
                                  els, 2, "io_a2dp", &s_pipe) != ESP_GMF_ERR_OK) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "cannot build the pipeline");
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
