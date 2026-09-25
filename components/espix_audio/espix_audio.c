/*
 * The playback engine: decode a file straight into the A2DP ring.
 *
 * No GMF on the data path. esp_audio_simple_dec parses and decodes, and a task
 * of ours reads, decodes and writes PCM into the PCM ring; the ring's
 * backpressure is what paces playback to the link. The GMF pipeline was tried
 * and put the task's CPU into gmf_core's job/IO/event loop rather than the
 * decoder, which is why it sits this one out.
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

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

#define IN_CHUNK   (32 * 1024)
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
        const size_t sent = espix_bt_audio_write(p + off, n - off);
        if (sent == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        off += sent;
    }
}

/*
 * MP3 decoding. esp_audio_simple_dec's prebuilt Helix decoder is the reference;
 * this is the same decoder built from source with its own per-file
 * optimization flags, which is the only lever for MP3 on this part. Kept as an
 * A/B: see docs/AUDIO.md and the S31 PIE note.
 */
/*
 * Read micro-mp3 in sub-frame chunks. Its decode_direct() path -- taken when
 * the caller's buffer already holds a whole frame -- bounds the slice to that
 * frame, so a frame referencing the bit reservoir cannot reach the earlier main
 * data and comes back MP3_DECODE_ERROR ("a bit-reservoir reference the bounded
 * slice can't reach", in the library's own words). decode_buffered(), taken
 * when a frame spans chunks, keeps the history and decodes those frames. A
 * frame is ~418 bytes at 128 kbps/44.1 kHz, so 256 keeps us in the buffered
 * path.
 */
#define ESPIX_MP3_READ_CHUNK 256

#define ESPIX_MP3_OK                  0
#define ESPIX_MP3_NEED_MORE_DATA      1
#define ESPIX_MP3_STREAM_INFO_READY   2
#define ESPIX_MP3_STREAM_INFO_CHANGED (-5)

void *espix_mp3_open(void);
void  espix_mp3_close(void *h);
int   espix_mp3_decode(void *h, const uint8_t *in, size_t in_len,
                       uint8_t *out, size_t out_len,
                       size_t *consumed, size_t *samples);
int   espix_mp3_sample_rate(void *h);
int   espix_mp3_channels(void *h);
int   espix_mp3_bit_depth(void *h);

__attribute__((unused)) static void play_mp3(int fd, uint8_t *in, uint8_t *out)
{
    void *mp3 = espix_mp3_open();
    if (mp3 == NULL) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "micro-mp3: cannot create decoder");
        return;
    }

    size_t in_len = 0, in_off = 0;
    bool   info_logged = false;
    uint32_t t_read = 0, t_dec = 0, t_feed = 0, produce = 0;
    uint32_t n_ok = 0, n_need = 0, n_info = 0, n_err = 0;
    int64_t  mark = esp_timer_get_time();

    while (!s_stop) {
        if (in_off >= in_len) {
            const int64_t r0 = esp_timer_get_time();
            const int n = (int)read(fd, in, ESPIX_MP3_READ_CHUNK);
            t_read += (uint32_t)(esp_timer_get_time() - r0);
            if (n <= 0) {
                break;
            }
            in_len = (size_t)n;
            in_off = 0;
        }

        size_t consumed = 0, samples = 0;
        const int64_t d0 = esp_timer_get_time();
        const int r = espix_mp3_decode(mp3, in + in_off, in_len - in_off,
                                       out, OUT_CHUNK, &consumed, &samples);
        t_dec += (uint32_t)(esp_timer_get_time() - d0);
        in_off += consumed;

        if (r == ESPIX_MP3_STREAM_INFO_READY || r == ESPIX_MP3_STREAM_INFO_CHANGED) {
            n_info++;
            if (!info_logged) {
                espix_klog(ESPIX_KLOG_INFO, TAG, "%d Hz, %d ch, %d bits (micro-mp3)",
                           espix_mp3_sample_rate(mp3), espix_mp3_channels(mp3),
                           espix_mp3_bit_depth(mp3));
                info_logged = true;
            }
            continue;
        }
        if (r == ESPIX_MP3_NEED_MORE_DATA) {
            n_need++;
            /* Carry the tail of a partial frame and read more behind it. */
            const size_t rem = in_len - in_off;
            if (rem > 0 && in_off > 0) {
                memmove(in, in + in_off, rem);
            }
            const int n = (int)read(fd, in + rem, ESPIX_MP3_READ_CHUNK - rem);
            if (n <= 0) {
                if (rem == 0) {
                    break;
                }
                in_len = rem;
                in_off = 0;
                continue;
            }
            in_len = rem + (size_t)n;
            in_off = 0;
            continue;
        }
        if (r < 0) {
            if (n_err == 0) {
                espix_klog(ESPIX_KLOG_WARN, TAG,
                           "micro-mp3 first error r=%d consumed=%u in=%u",
                           r, (unsigned)consumed, (unsigned)(in_len - in_off));
            }
            if (n_err < 5 || (n_err % 100) == 0) {
                espix_klog(ESPIX_KLOG_WARN, TAG, "micro-mp3 err#%u r=%d consumed=%u",
                           (unsigned)n_err, r, (unsigned)consumed);
            }
            n_err++;
            /* A bad frame is recoverable; skip at least one byte so it cannot
             * spin on the same input. */
            if (consumed == 0) {
                in_off += 1;
            }
            continue;
        }
        if (r == ESPIX_MP3_OK) {
            n_ok++;
        }
        if (samples > 0) {
            const size_t bytes = samples * (size_t)espix_mp3_channels(mp3) * 2u;
            const int64_t f0 = esp_timer_get_time();
            feed(out, bytes);
            t_feed += (uint32_t)(esp_timer_get_time() - f0);
            produce += (uint32_t)bytes;
        }

        const int64_t now = esp_timer_get_time();
        if (now - mark >= 1000000) {
            espix_klog(ESPIX_KLOG_INFO, TAG,
                       "read %ums decode %ums feed %ums over %ums, %u B produced",
                       (unsigned)(t_read / 1000), (unsigned)(t_dec / 1000),
                       (unsigned)(t_feed / 1000), (unsigned)((now - mark) / 1000),
                       (unsigned)produce);
            t_read = t_dec = t_feed = produce = 0;
            mark = now;
        }
    }

    espix_klog(ESPIX_KLOG_INFO, TAG,
               "micro-mp3 done: %u B fed, ok %u need %u info %u err %u, %u B left",
               (unsigned)produce, (unsigned)n_ok, (unsigned)n_need,
               (unsigned)n_info, (unsigned)n_err, (unsigned)(in_len - in_off));
    espix_mp3_close(mp3);
}

static void audio_task(void *arg)
{
    const char *uri = (const char *)arg;
    int fd = -1;
    uint8_t *in = NULL;
    uint8_t *out = NULL;
    uint8_t *up = NULL;   /* mono -> stereo upmix, so the ring is always stereo */
    int      src_channels = 2;
    esp_audio_simple_dec_handle_t dec = NULL;
    bool info_logged = false;

    /*
     * open()/read(), not stdio. The FILE buffer here is 128 bytes (picolibc's
     * BUFSIZ), so a large fread becomes one read() per 128 bytes and the
     * per-call cost down in littlefs/FAT dominates; and asking stdio for a
     * large buffer puts it in internal RAM, the scarce heap, which starved the
     * next task creation. Reading directly into our own PSRAM chunk keeps the
     * reads large and the internal heap untouched.
     */
    fd = open(uri, O_RDONLY);
    if (fd < 0) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "%s: cannot open", uri);
        goto out;
    }

    const esp_audio_simple_dec_type_t type = type_from_uri(uri);
    if (type == ESP_AUDIO_SIMPLE_DEC_TYPE_NONE) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "%s: no decoder for that extension", uri);
        goto out;
    }

    in = heap_caps_malloc(IN_CHUNK, IO_CAPS);
    out = heap_caps_malloc(OUT_CHUNK, IO_CAPS);
    up = heap_caps_malloc(OUT_CHUNK * 2, IO_CAPS);
    if (in == NULL || out == NULL || up == NULL) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "no buffers");
        goto out;
    }

    espix_klog(ESPIX_KLOG_INFO, TAG, "playing %s", uri);

    /*
     * MP3 stays on esp_audio_simple_dec. esphome/micro-mp3 was wired in as an
     * A/B (same OpenCore decoder, built from source with its own per-file
     * optimization flags) and could not decode this file: every frame came back
     * MP3_DECODE_ERROR. Its decode_direct() bounds the slice to one frame, so a
     * frame referencing the bit reservoir cannot reach the earlier main data,
     * and feeding sub-frame chunks to force decode_buffered() did not help
     * either. The finding is worth reporting upstream; play_mp3() is kept below
     * for that, unused.
     */
#if 0
    if (type == ESP_AUDIO_SIMPLE_DEC_TYPE_MP3) {
        play_mp3(fd, in, out);
        goto out;
    }
#endif

    esp_audio_simple_dec_cfg_t cfg = {
        .dec_type      = type,
        .use_frame_dec = false,
    };
    if (esp_audio_simple_dec_open(&cfg, &dec) != ESP_AUDIO_ERR_OK) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "cannot open decoder %d", (int)type);
        goto out;
    }

    /* Where does the time go? Read, decode, and the ring write are timed
     * separately and reported once a second; guessing from watchdog symbols
     * has not worked. */
    uint32_t t_read = 0, t_dec = 0, t_feed = 0, produce = 0;
    int64_t  mark = esp_timer_get_time();

    while (!s_stop) {
        const int64_t r0 = esp_timer_get_time();
        const int n = (int)read(fd, in, IN_CHUNK);
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
                        src_channels = info.channel;
                    }
                    info_logged = true;
                }
                const int64_t f0 = esp_timer_get_time();
                if (src_channels == 1) {
                    /*
                     * The ring is stereo by contract: the A2DP data callback
                     * downmixes stereo to the sink's (mono) SBC frame. Feeding
                     * a mono file through unchanged made that callback read two
                     * bytes per sample and drain the ring at twice the rate, so
                     * a mono source underran. Duplicate each sample instead.
                     */
                    const int16_t *src = (const int16_t *)frame.buffer;
                    int16_t       *dst = (int16_t *)up;
                    const size_t   n = frame.decoded_size / 2;
                    for (size_t i = 0; i < n; i++) {
                        dst[2 * i]     = src[i];
                        dst[2 * i + 1] = src[i];
                    }
                    feed(up, n * 4);
                    produce += (uint32_t)(n * 4);
                } else {
                    feed(frame.buffer, frame.decoded_size);
                    produce += frame.decoded_size;
                }
                t_feed += (uint32_t)(esp_timer_get_time() - f0);
            }
            raw.len -= raw.consumed;
            raw.buffer += raw.consumed;
        }
        if (raw.eos) {
            break;
        }

        const int64_t now = esp_timer_get_time();
        if (now - mark >= 1000000) {
            /*
             * DEBUG, not INFO, on purpose. klog's ring is asynchronous, but its
             * console echo is not: it does fprintf + fflush to a line-buffered
             * tty, which is a blocking UART write of ~8-10 ms at 115200. An INFO
             * line here fired every 2-3 s during playback and was audible as a
             * burst of static. DEBUG stays in the ring, so dmesg still has it.
             */
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
    heap_caps_free(up);
    if (fd >= 0) {
        close(fd);
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
     * PSRAM first, internal as a fallback.
     *
     * This stack was pinned to internal because an earlier PSRAM-stack build
     * underran badly -- but that was before the feed() accounting bug was
     * found, so the stack's memory was never actually the variable. Internal is
     * the scarce pool, and a 6 kB block there is exactly what made `play`
     * intermittently fail with ESP_FAIL when the heap was momentarily
     * exhausted. This task's working set is small, so try PSRAM and fall back
     * if it is not available.
     */
    if (xTaskCreatePinnedToCoreWithCaps(audio_task, "audio", TASK_STACK, s_uri, 20,
                                        &s_task, 1,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS &&
        xTaskCreatePinnedToCore(audio_task, "audio", TASK_STACK, s_uri, 20,
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
