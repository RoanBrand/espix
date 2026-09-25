#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"

#include "esp_gmf_err.h"
#include "esp_audio_simple_player.h"

#include "espix_kernel.h"
#include "espix_bt.h"
#include "espix_audio.h"

#define TAG "audio"

static esp_asp_handle_t  s_player;
static volatile int      s_state = ESP_ASP_STATE_NONE;
static char              s_uri[160];

/*
 * The player's output: PCM, which goes to the A2DP source's ring buffer. The
 * blocker is the pacing -- the stack drains at 44.1 kHz stereo and the ring is
 * finite, so a full buffer means "not yet", and the player's task waits rather
 * than dropping audio.
 */
static int audio_out(uint8_t *data, int size, void *ctx)
{
    (void)ctx;
    if (data == NULL || size <= 0) {
        return 0;
    }
    size_t off = 0;
    while (off < (size_t)size) {
        if (espix_bt_audio_write(data + off, (size_t)size - off) == ESP_OK) {
            off = size;
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    return size;
}

static int audio_event(esp_asp_event_pkt_t *pkt, void *ctx)
{
    (void)ctx;
    if (pkt != NULL && pkt->type == ESP_ASP_EVENT_TYPE_STATE && pkt->payload != NULL) {
        s_state = *(esp_asp_state_t *)pkt->payload;
        espix_klog(ESPIX_KLOG_INFO, TAG, "player state %d", s_state);
    }
    return 0;
}

esp_err_t espix_audio_play(const char *uri)
{
    if (uri == NULL || uri[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!espix_bt_ready()) {
        /* `play` may be the first command; bring the controller up here. The
         * sink is not required yet (see the connect check below). */
        const esp_err_t e = espix_bt_init();
        if (e != ESP_OK) {
            return e;
        }
    }
    if (!espix_bt_a2d_connected()) {
        /*
         * No sink yet: the player fills the ring and blocks in its output
         * callback until one connects. That is deliberate -- `play` can be
         * started before the link, which matters because the link is what
         * costs the shell its memory, so the command that starts playback can
         * be issued while SSH still works.
         */
        espix_klog(ESPIX_KLOG_INFO, TAG, "no sink yet; playback waits for A2DP");
    }

    if (s_player == NULL) {
        esp_asp_cfg_t cfg = {
            .out              = { .cb = audio_out, .user_ctx = NULL },
            .task_prio        = 20,
            .task_stack       = 6 * 1024,
            .task_core        = 1,          /* core 0 belongs to BT and WiFi */
            .task_stack_in_ext = true,      /* the stack is in PSRAM */
        };
        const esp_gmf_err_t e = esp_audio_simple_player_new(&cfg, &s_player);
        if (e != ESP_GMF_ERR_OK) {
            espix_klog(ESPIX_KLOG_ERROR, TAG, "player create: %d", (int)e);
            return ESP_FAIL;
        }
        (void)esp_audio_simple_player_set_event(s_player, audio_event, NULL);
    }

    strlcpy(s_uri, uri, sizeof(s_uri));
    const esp_gmf_err_t e = esp_audio_simple_player_run(s_player, s_uri, NULL);
    if (e != ESP_GMF_ERR_OK) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "%s: %d", s_uri, (int)e);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t espix_audio_stop(void)
{
    if (s_player == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_audio_simple_player_stop(s_player) == ESP_GMF_ERR_OK ? ESP_OK : ESP_FAIL;
}

/*
 * The audio stack's allocator, moved to PSRAM.
 *
 * esp_audio_codec (prebuilt) and the effects code reach memory through
 * media_lib_malloc/calloc/realloc/free, which are declared *weak* in the
 * components that provide them and default to plain malloc -- internal RAM.
 * Strong definitions win, but they have to live in an object the linker
 * already pulls: a separate archive member is never searched, because the
 * weak definition satisfies the reference first. So they live here, in the
 * object that defines the play entry point.
 *
 * media_lib_caps_malloc_align is deliberately left alone: it carries an
 * explicit capability (DMA/IRAM) and must keep the default behaviour.
 */
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

const char *espix_audio_state(void)
{
    switch (s_state) {
    case ESP_ASP_STATE_RUNNING:  return "playing";
    case ESP_ASP_STATE_PAUSED:   return "paused";
    case ESP_ASP_STATE_STOPPED:  return "stopped";
    case ESP_ASP_STATE_FINISHED: return "finished";
    case ESP_ASP_STATE_ERROR:    return "error";
    default:                     return "idle";
    }
}
