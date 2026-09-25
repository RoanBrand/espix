/*
 * The playback engine: decode a file straight into the A2DP ring.
 *
 * No GMF on the data path. esp_audio_simple_dec parses and decodes, and a task
 * of ours reads, decodes and writes PCM into the PCM ring; the ring's
 * backpressure is what paces playback to the link. The GMF pipeline was tried
 * and put the task's CPU into gmf_core's job/IO/event loop rather than the
 * decoder, which is why it sits this one out.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"

#include "espix_kernel.h"
#include "espix_bt.h"
#include "espix_audio.h"

#define TAG "audio"

#define IN_CHUNK   (16 * 1024)
#define OUT_CHUNK  (16 * 1024)
#define TASK_STACK (6 * 1024)

static TaskHandle_t  s_task;
static volatile bool s_stop;
static volatile bool s_running;
static char          s_uri[200];

/* ------------------------------------------------------------------ */
/* The audio stack's allocator, moved to PSRAM.                         */
/*                                                                      */
/* esp_audio_codec is a prebuilt per-chip archive and reaches memory    */
/* through media_lib_malloc/calloc/realloc/free, declared weak in the   */
/* components that provide them and defaulting to plain malloc --       */
/* internal RAM. Strong definitions win, but they have to live in an    */
/* object the linker already pulls, so they live here, and              */
/* __attribute__((used)) keeps --gc-sections from dropping the ones     */
/* nothing names.                                                       */
/* ------------------------------------------------------------------ */
/*
 * The decoder's own memory goes to internal RAM. Its state and tables are
 * random-access, and PSRAM random access is many times slower: with these in
 * PSRAM the MP3 decode measured ~2900 ms per second of audio (about 2.9x
 * realtime) while read and the ring write were negligible. espix's own chunk
 * buffers are sequential, so those stay in PSRAM below.
 */
#define MEDIA_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define IO_CAPS    (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

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

static esp_audio_simple_dec_type_t type_from_uri(const char *uri)
{
    const char *dot = strrchr(uri, '.');
    if (dot == NULL) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
    }
    if (strcasecmp(dot, ".mp3") == 0) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    }
    if (strcasecmp(dot, ".wav") == 0) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
    }
    if (strcasecmp(dot, ".aac") == 0 || strcasecmp(dot, ".m4a") == 0) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
    }
    if (strcasecmp(dot, ".flac") == 0) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
    }
    return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
}

/*
 * PCM into the ring. A full ring is the sink saying it has enough, so this
 * blocks (yielding) and the decode falls behind the link rather than running
 * ahead of it or dropping samples.
 */
static void feed(const uint8_t *p, size_t n)
{
    size_t off = 0;
    while (off < n && !s_stop) {
        if (espix_bt_audio_write(p + off, n - off) == ESP_OK) {
            off = n;
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

static void audio_task(void *arg)
{
    const char *uri = (const char *)arg;
    FILE *f = NULL;
    uint8_t *in = NULL;
    uint8_t *out = NULL;
    esp_audio_simple_dec_handle_t dec = NULL;
    bool info_logged = false;

    f = fopen(uri, "rb");
    if (f == NULL) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "%s: cannot open", uri);
        goto out;
    }

    const esp_audio_simple_dec_type_t type = type_from_uri(uri);
    if (type == ESP_AUDIO_SIMPLE_DEC_TYPE_NONE) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "%s: no decoder for that extension", uri);
        goto out;
    }

    esp_audio_simple_dec_cfg_t cfg = {
        .dec_type      = type,
        .use_frame_dec = false,
    };
    if (esp_audio_simple_dec_open(&cfg, &dec) != ESP_AUDIO_ERR_OK) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "cannot open decoder %d", (int)type);
        goto out;
    }

    in = heap_caps_malloc(IN_CHUNK, IO_CAPS);
    out = heap_caps_malloc(OUT_CHUNK, IO_CAPS);
    if (in == NULL || out == NULL) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "no buffers");
        goto out;
    }

    espix_klog(ESPIX_KLOG_INFO, TAG, "playing %s", uri);

    /* Where does the time go? Read, decode, and the ring write are timed
     * separately and reported once a second; guessing from watchdog symbols
     * has not worked. */
    uint32_t t_read = 0, t_dec = 0, t_feed = 0, produce = 0;
    int64_t  mark = esp_timer_get_time();

    while (!s_stop) {
        const int64_t r0 = esp_timer_get_time();
        const int n = (int)fread(in, 1, IN_CHUNK, f);
        t_read += (uint32_t)(esp_timer_get_time() - r0);
        if (n <= 0) {
            break;
        }
        esp_audio_simple_dec_raw_t raw = {
            .buffer = in,
            .len    = (uint32_t)n,
            .eos    = (n < IN_CHUNK),
        };
        while (raw.len > 0 && !s_stop) {
            esp_audio_simple_dec_out_t frame = {
                .buffer = out,
                .len    = OUT_CHUNK,
            };
            const int64_t d0 = esp_timer_get_time();
            const esp_audio_err_t e = esp_audio_simple_dec_process(dec, &raw, &frame);
            t_dec += (uint32_t)(esp_timer_get_time() - d0);
            if (e == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                uint8_t *nb = heap_caps_realloc(out, frame.needed_size, IO_CAPS);
                if (nb == NULL) {
                    espix_klog(ESPIX_KLOG_ERROR, TAG, "cannot grow the out buffer");
                    goto out;
                }
                out = nb;
                continue;
            }
            if (e != ESP_AUDIO_ERR_OK) {
                espix_klog(ESPIX_KLOG_ERROR, TAG, "decode error %d", (int)e);
                break;
            }
            if (frame.decoded_size > 0) {
                if (!info_logged) {
                    esp_audio_simple_dec_info_t info = { 0 };
                    if (esp_audio_simple_dec_get_info(dec, &info) == ESP_AUDIO_ERR_OK) {
                        espix_klog(ESPIX_KLOG_INFO, TAG, "%u Hz, %u ch, %u bits",
                                   (unsigned)info.sample_rate, (unsigned)info.channel,
                                   (unsigned)info.bits_per_sample);
                    }
                    info_logged = true;
                }
                const int64_t f0 = esp_timer_get_time();
                feed(frame.buffer, frame.decoded_size);
                t_feed += (uint32_t)(esp_timer_get_time() - f0);
                produce += frame.decoded_size;
            }
            raw.len -= raw.consumed;
            raw.buffer += raw.consumed;
        }
        if (raw.eos) {
            break;
        }

        const int64_t now = esp_timer_get_time();
        if (now - mark >= 1000000) {
            espix_klog(ESPIX_KLOG_INFO, TAG,
                       "read %ums decode %ums feed %ums, %u B/s produced",
                       (unsigned)(t_read / 1000), (unsigned)(t_dec / 1000),
                       (unsigned)(t_feed / 1000), (unsigned)produce);
            t_read = t_dec = t_feed = produce = 0;
            mark = now;
        }
    }

out:
    if (dec != NULL) {
        esp_audio_simple_dec_close(dec);
    }
    heap_caps_free(in);
    heap_caps_free(out);
    if (f != NULL) {
        fclose(f);
    }
    s_running = false;
    s_task = NULL;
    espix_klog(ESPIX_KLOG_INFO, TAG, "finished");
    vTaskDelete(NULL);
}

esp_err_t espix_audio_play(const char *uri)
{
    if (uri == NULL || uri[0] == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!espix_bt_ready()) {
        /* play may be the first command; bring the controller up here. The
         * sink is not required yet -- the ring holds playback until A2DP
         * connects. */
        const esp_err_t e = espix_bt_init();
        if (e != ESP_OK) {
            return e;
        }
    }
    if (!espix_bt_a2d_connected()) {
        espix_klog(ESPIX_KLOG_INFO, TAG, "no sink yet; playback waits for A2DP");
    }

    static bool registered;
    if (!registered) {
        /* The simple decoder's own default set is WAV/M4A/TS/OGG; MP3 lives
         * in the advanced registry, and it is the one the simple decoder
         * delegates to for MP3 (without this: "Decoder MP3 not registered"). */
        esp_audio_dec_register_default();
        esp_audio_simple_dec_register_default();
        registered = true;
    }

    if (s_task != NULL) {
        s_stop = true;
        while (s_task != NULL) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    s_stop = false;
    strlcpy(s_uri, uri, sizeof(s_uri));

    /*
     * The stack is deliberately in internal RAM. A decode task's stack is
     * touched on every call, local and return, and PSRAM makes that path many
     * times slower -- the version of this task whose stack was in PSRAM burned
     * CPU 1 flat but only produced about half realtime. The buffers below are
     * still PSRAM; only the stack is internal, and a few KB is affordable.
     */
    if (xTaskCreatePinnedToCore(audio_task, "audio", TASK_STACK, s_uri, 20,
                                &s_task, 1) != pdPASS) {
        s_task = NULL;
        return ESP_FAIL;
    }
    s_running = true;
    return ESP_OK;
}

esp_err_t espix_audio_stop(void)
{
    if (s_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_stop = true;
    return ESP_OK;
}

const char *espix_audio_state(void)
{
    return s_running ? "playing" : "idle";
}
