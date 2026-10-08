/*
 * The playback engine: decode a file into the registered sink.
 *
 * No GMF on the data path. esp_audio_simple_dec parses and decodes, and a task
 * of ours reads, decodes and writes PCM to the sink's write(); the sink's
 * backpressure is what paces playback to the link. The GMF pipeline was tried
 * and put the task's CPU into gmf_core's job/IO/event loop rather than the
 * decoder, which is why it sits this one out.
 *
 * The sink itself is chosen through espix_audio_sink.h, so this file names no
 * Bluetooth and links with none.
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
#include "espix_task.h"
#include "espix_audio.h"
#include "espix_audio_sink.h"

#define TAG "audio"

/*
 * The sink registry.
 *
 * Registration happens once, at boot, before the console takes over (see
 * main/espix_main.c), so a plain array and no lock are enough. The first sink
 * registered is the default that "play" uses; choosing among several is a
 * later piece.
 */
#define ESPIX_AUDIO_SINKS_MAX 4

static const espix_audio_sink_ops_t *s_sinks[ESPIX_AUDIO_SINKS_MAX];
static size_t                        s_sink_count;

esp_err_t espix_audio_sink_register(const espix_audio_sink_ops_t *ops)
{
    if (ops == NULL || ops->name == NULL || ops->write == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < s_sink_count; i++) {
        if (s_sinks[i] == ops || strcmp(s_sinks[i]->name, ops->name) == 0) {
            return ESP_OK;          /* already published; idempotent */
        }
    }

    if (s_sink_count >= ESPIX_AUDIO_SINKS_MAX) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "no room for sink '%s'", ops->name);
        return ESP_ERR_NO_MEM;
    }

    s_sinks[s_sink_count++] = ops;
    espix_klog(ESPIX_KLOG_INFO, TAG, "sink '%s' registered", ops->name);
    return ESP_OK;
}

const espix_audio_sink_ops_t *espix_audio_sink_default(void)
{
    return s_sink_count > 0 ? s_sinks[0] : NULL;
}

#if CONFIG_ESPIX_AUDIO_NULL_SINK
/*
 * The benchmark sink. "write" takes everything at once, so the decoder is never
 * paced by a consumer and the telemetry reports the source's and decoder's own
 * rate. Nothing here is for a release -- it is how the decoder is measured with
 * Bluetooth, I2S and the radio out of the picture.
 */
static bool null_connected(void) { return true; }

static espix_audio_format_t null_format(void)
{
    return (espix_audio_format_t){ .rate = 44100, .channels = 2, .bits = 16 };
}

static size_t null_write(const void *pcm, size_t len) { (void)pcm; return len; }
static void   null_start(void)     { }
static void   null_suspend(void)   { }

static const espix_audio_sink_ops_t s_null_sink = {
    .name      = "null",
    .connected = null_connected,
    .format    = null_format,
    .write     = null_write,
    .start     = null_start,
    .suspend   = null_suspend,
};

esp_err_t espix_audio_null_sink_register(void)
{
    return espix_audio_sink_register(&s_null_sink);
}
#endif /* CONFIG_ESPIX_AUDIO_NULL_SINK */

#if !CONFIG_ESPIX_AUDIO

/*
 * The API without the engine.
 *
 * `play` is behind the same option (cmd_play.c) and nothing else needs a
 * decoder -- but the shutdown sequence asks playback to stop and a Bluetooth
 * power-off does too, and a target that has neither should still link. Whole
 * functions rather than a broken engine: nothing here is ever called on such a
 * target, and with every reference gone the codec libraries drop out of the
 * link entirely, which is most of what the option is for.
 */
bool espix_audio_stop_wait(uint32_t timeout_ms)  { (void)timeout_ms; return true; }
esp_err_t espix_audio_play(const char *uri)      { (void)uri; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t espix_audio_play_wait(const char *uri) { (void)uri; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t espix_audio_stop(void)                 { return ESP_ERR_NOT_SUPPORTED; }
const char *espix_audio_state(void)              { return "not built"; }

#else

/*
 * The decoder's input buffer lives in internal RAM, not PSRAM, even though it
 * is the read() target: the codec walks it bit by bit per frame, so its access
 * pattern is random-ish and PSRAM costs there are the same shape that made GMF
 * slow. 8 kB is enough for several frames and affordable in internal; the
 * output buffers stay in PSRAM (the decoder writes them once).
 */
#define IN_CHUNK   (8 * 1024)
/*
 * Sized to an MP3 frame, not to the read: 1152 samples x 2 ch x 2 B = 4608 B is
 * the largest frame any of our sources decodes to, and the buffer is internal
 * (the decoder writes it sample by sample, so PSRAM there costs ~7x). 16 kB was
 * a guess from the original design; the extra 11 kB is internal RAM we do not
 * have. A larger frame grows it through the BUFF_NOT_ENOUGH path, which for
 * WAV will fall back to whatever the heap can give.
 */
#define OUT_CHUNK  (6 * 1024)
/*
 * The mono->stereo scratch. The upmix is stateless and in order, so it runs in
 * pieces this size rather than in one buffer the size of a whole frame: a mono
 * frame is at most 4608 B, and 12 kB held to double it is 12 kB the codec's own
 * state wants.
 */
#define UP_CHUNK   (2 * 1024)
#define TASK_STACK (6 * 1024)

static espix_task_exit_t s_exit;
static volatile bool s_stop;
static volatile bool s_running;

/*
 * Whether the running task's stack came from xTaskCreatePinnedToCoreWithCaps
 * (PSRAM) or the plain xTaskCreatePinnedToCore fallback (internal). It decides
 * how it must be deleted: a WithCaps task deleted with vTaskDelete() leaks its
 * stack, because the idle task only frees what FreeRTOS allocated itself -- and
 * deleting a plain task with vTaskDeleteWithCaps() would double-free. IDF's own
 * comment says as much in idf_additions.c.
 */
static bool s_task_caps;
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
 * Where the codec's own allocations come from.
 *
 * Internal first by default. The decoder's state is random-access and PSRAM
 * random access is many times slower: with it in PSRAM the S31 -- whose codec
 * library has no DSP path -- measured ~1200 ms per second of audio against
 * 161 ms with it internal. The S3's codec uses its LX7 DSP/MAC instructions,
 * which is why it is fast there, so PSRAM may be affordable and gives the
 * internal pool back; ESPIX_AUDIO_CODEC_PSRAM picks that. It is a measurement
 * to confirm, not a default to trust. espix's own chunk buffers are sequential
 * and stay in PSRAM either way.
 */
#define MEDIA_CAPS_INTERNAL (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define IO_CAPS             (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

#if CONFIG_ESPIX_AUDIO_CODEC_PSRAM
#define MEDIA_CAPS     IO_CAPS
#define MEDIA_ALT_CAPS MEDIA_CAPS_INTERNAL
#define MEDIA_ALT_NAME "internal"
#else
#define MEDIA_CAPS     MEDIA_CAPS_INTERNAL
#define MEDIA_ALT_CAPS IO_CAPS
#define MEDIA_ALT_NAME "PSRAM"
#endif

/*
 * A refusal from the preferred pool must not turn a slow decode into no
 * decode, so take the other pool and say so: which pool a decode degraded to
 * is exactly what used to be invisible.
 */
static void *media_fallback(size_t size, bool zeroed)
{
    void *p = zeroed ? heap_caps_calloc(1, size, MEDIA_CAPS)
                     : heap_caps_malloc(size, MEDIA_CAPS);
    if (p == NULL) {
        p = zeroed ? heap_caps_calloc(1, size, MEDIA_ALT_CAPS)
                   : heap_caps_malloc(size, MEDIA_ALT_CAPS);
        if (p != NULL) {
            espix_klog(ESPIX_KLOG_WARN, TAG,
                       "decoder memory: preferred pool refused %u bytes, using "
                       "%s (decode may be slower)", (unsigned)size, MEDIA_ALT_NAME);
        }
    }
    return p;
}

__attribute__((used)) void *media_lib_malloc(size_t size)
{
    return media_fallback(size, false);
}

__attribute__((used)) void *media_lib_calloc(size_t num, size_t size)
{
    return media_fallback(num * size, true);
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

/*
 * Stop the current playback and wait for the task to go.
 *
 * Used when the sink is going away (`bluetoothctl power off`): the task owns the
 * decoder and its buffers, and letting it run on with no sink would hold them
 * for nothing.
 *
 * There is deliberately no decoder reservation here any more. One existed to
 * take the codec's internal memory before Bluetooth fragmented the heap, and it
 * did make the difference between a 430 ms and a 1200 ms decode -- but once the
 * reaper, ota:check and cmd_task stacks moved to PSRAM there was enough
 * contiguous internal to open the decoder at `play` time, and measurement
 * showed the reservation no longer buying anything. Simpler wins.
 */
bool espix_audio_stop_wait(uint32_t timeout_ms)
{
    if (s_exit.task == NULL) {
        return true;                    /* nothing was playing: not a wait */
    }

    s_stop = true;
    return espix_task_exit_wait(&s_exit, timeout_ms);
}

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
        const espix_audio_sink_ops_t *sink = espix_audio_sink_default();
        if (sink == NULL) {
            /*
             * No sink registered yet. Block rather than spin, and rather than
             * consume the source: a "play --wait" on a build whose sink appears
             * later must not race the decoder through the whole file.
             */
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        const size_t sent = sink->write(p + off, n - off);
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

/*
 * One audio buffer: internal first, PSRAM as the graceful fallback.
 *
 * espix expects low memory, so a failure must not be a surprise: the caller gets
 * a NULL and the buffer's name, and frees whatever else it took. Internal is
 * preferred because the decoder walks these per sample -- PSRAM there measured
 * about 7x slower -- but PSRAM is far better than refusing to play, and which
 * buffer degraded is logged so a slow decode is explained rather than a mystery.
 */
#if CONFIG_ESPIX_AUDIO_IO_PSRAM
#define AUDIO_IO_FROM_PSRAM 1
#else
#define AUDIO_IO_FROM_PSRAM 0
#endif

static uint8_t *audio_buf_alloc(const char *name, size_t len, bool *from_psram)
{
    *from_psram = false;

    /*
     * Internal first by default: the decoder walks these per sample, and PSRAM
     * costs there. ESPIX_AUDIO_IO_PSRAM moves them out deliberately, to leave
     * internal for the codec's own random-access state -- measure before
     * trusting it, because the point is realtime rather than fitting.
     */
#if CONFIG_ESPIX_AUDIO_IO_PSRAM
    uint8_t *p = heap_caps_malloc(len, IO_CAPS);
    *from_psram = (p != NULL);
    if (p == NULL) {
        p = heap_caps_malloc(len, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
#else
    uint8_t *p = heap_caps_malloc(len, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (p == NULL) {
        p = heap_caps_malloc(len, IO_CAPS);
        *from_psram = (p != NULL);
    }
#endif

    if (p == NULL) {
        espix_klog(ESPIX_KLOG_ERROR, TAG,
                   "%s buffer: no memory for %u bytes; internal and PSRAM both refused",
                   name, (unsigned)len);
    } else if (*from_psram && !AUDIO_IO_FROM_PSRAM) {
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "%s buffer: internal exhausted, using PSRAM (%u bytes); decode will be slower",
                   name, (unsigned)len);
    }

    return p;
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
    bool owns_dec = true;   /* false when reusing the boot-time reserved handle */
    bool info_logged = false;
    int64_t s_last_yield = esp_timer_get_time();

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

    /*
     * `in` is what the source read fills and the decoder reads; `out` is what
     * it decodes into. Both want internal RAM where the pool allows it, and each
     * degrades to PSRAM on its own rather than failing the playback.
     *
     * `up`, the mono-to-stereo copy, is allocated on the first mono frame
     * instead of here: most sources are stereo and never touch it, and 12 kB of
     * internal RAM held for a case that does not arise is exactly the pressure
     * that makes a stereo MP3 spill.
     */
    bool in_ps = false, out_ps = false, up_ps = false;
    in  = audio_buf_alloc("in",  IN_CHUNK,  &in_ps);
    out = audio_buf_alloc("out", OUT_CHUNK, &out_ps);

    if (in == NULL || out == NULL) {
        char missing[16] = "";
        if (in == NULL)  { strlcat(missing, " in",  sizeof(missing)); }
        if (out == NULL) { strlcat(missing, " out", sizeof(missing)); }

        /* Nothing is left half-allocated: the label below frees both. */
        espix_klog(ESPIX_KLOG_ERROR, TAG,
                   "cannot play %s: not enough memory for the%s audio buffer(s)",
                   uri, missing);
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

    /*
     * Opened per play. The decoder's state and tables want internal RAM, and an
     * earlier version reserved one at boot to get it before Bluetooth
     * fragmented the heap; that stopped being necessary once the idle task
     * stacks moved to PSRAM (enough contiguous internal survives Bluetooth), and
     * measurement confirmed it (161 ms per interval either way).
     */
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
                /*
                 * A mono source into a stereo sink is duplicated, not passed
                 * through: the sink's data callback reads two bytes per sample,
                 * so a mono stream drains its ring at twice the rate and
                 * underruns. The sink names the width it wants.
                 */
                const espix_audio_sink_ops_t *const sink = espix_audio_sink_default();
                const unsigned out_ch = sink != NULL ? sink->format().channels : 2u;
                const int64_t f0 = esp_timer_get_time();
                if (src_channels == 1 && out_ch > 1) {
                    if (up == NULL) {
                        up = audio_buf_alloc("up", UP_CHUNK, &up_ps);
                        if (up == NULL) {
                            espix_klog(ESPIX_KLOG_ERROR, TAG,
                                       "cannot play %s: no memory for the mono "
                                       "upmix buffer", uri);
                            goto out;
                        }
                    }
                    const int16_t *src = (const int16_t *)frame.buffer;
                    int16_t       *dst = (int16_t *)up;
                    const size_t   total = frame.decoded_size / 2;
                    const size_t   per   = UP_CHUNK / 4;   /* stereo frames */
                    size_t         done  = 0;
                    while (done < total && !s_stop) {
                        size_t n = total - done;
                        if (n > per) {
                            n = per;
                        }
                        for (size_t i = 0; i < n; i++) {
                            dst[2 * i]     = src[done + i];
                            dst[2 * i + 1] = src[done + i];
                        }
                        feed((const uint8_t *)dst, n * 4);
                        produce += (uint32_t)(n * 4);
                        done += n;
                    }
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

        /*
         * Yield a little, once every few seconds.
         *
         * While the ring is filling, feed() returns immediately, so this loop
         * never blocks and core 1's idle task starves -- long enough to trip the
         * task watchdog. Once the ring is full feed() blocks on its own and this
         * costs nothing.
         *
         * Twice a second was chosen against a five-second watchdog period. That
         * is a minute now (R-P4.1), so five seconds keeps a 12x margin over the
         * longest idle gap allowed and cuts what this costs the producer by ten
         * -- one tick every five seconds is under 0.2% of it. It is kept rather
         * than removed because audio still exercises the SMP tick, and a starved
         * idle on a core that also runs WiFi is not a thing to discover on a
         * user's board.
         */
        const int64_t ynow = esp_timer_get_time();
        if (ynow - s_last_yield >= 5000000) {
            s_last_yield = ynow;
            vTaskDelay(1);
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
                       "read %ums decode %ums feed %ums over %ums, %u B produced",
                       (unsigned)(t_read / 1000), (unsigned)(t_dec / 1000),
                       (unsigned)(t_feed / 1000), (unsigned)((now - mark) / 1000),
                       (unsigned)produce);
            t_read = t_dec = t_feed = produce = 0;
            mark = now;
        }
    }

out:
    if (owns_dec && dec != NULL) {
        esp_audio_simple_dec_close(dec);
    }
    heap_caps_free(in);
    heap_caps_free(out);
    heap_caps_free(up);
    if (fd >= 0) {
        close(fd);
    }
    /* Nothing left to send: suspend the stream rather than encode silence. */
    const espix_audio_sink_ops_t *const end_sink = espix_audio_sink_default();
    if (end_sink != NULL) {
        end_sink->suspend();
    }

    s_running = false;
    espix_task_exited(&s_exit);
    espix_klog(ESPIX_KLOG_INFO, TAG, "finished");

    if (s_task_caps) {
        vTaskDeleteWithCaps(NULL);      /* frees the PSRAM stack it was given */
    } else {
        vTaskDelete(NULL);
    }
}

static esp_err_t play_common(const char *uri, bool wait)
{
    if (uri == NULL || uri[0] == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * No sink, no playback.
     *
     * This used to start the task anyway and let it fill the ring while waiting
     * for A2DP. That held the decoder and its buffers for as long as the wait
     * lasted -- measured at 38 s -- and made `play` look like it had succeeded
     * when nothing would be heard. A real player refuses instead ("audio open
     * error: No such device"). `play --wait` keeps the old behaviour for a sink
     * that is expected shortly.
     *
     * The controller is not brought up here either: Bluetooth starts from
     * bluetoothctl, and a connected sink is a precondition for playing to it.
     */
    const espix_audio_sink_ops_t *const sink = espix_audio_sink_default();
    if (sink == NULL || !sink->connected()) {
        if (!wait) {
            espix_klog(ESPIX_KLOG_WARN, TAG, "no audio sink; not starting %s", uri);
            return ESP_ERR_INVALID_STATE;
        }
        espix_klog(ESPIX_KLOG_INFO, TAG, "no sink yet; playback waits for one");
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

    if (s_exit.task != NULL) {
        s_stop = true;
        (void)espix_task_exit_wait(&s_exit, 0);
    }
    s_stop = false;
    strlcpy(s_uri, uri, sizeof(s_uri));

    /* New stream: drop the last one's PCM and re-arm the sink's pre-roll, so
     * this one starts with a cushion instead of underrunning. */
    if (sink != NULL) {
        sink->start();
    }

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
    /*
     * s_task_caps is set before each attempt, and the fallback only runs when
     * the caps attempt failed -- so no task is alive to race the flag.
     */
    if (!espix_task_exit_init(&s_exit)) {
        return ESP_ERR_NO_MEM;
    }

    s_task_caps = true;
    if (xTaskCreatePinnedToCoreWithCaps(audio_task, "espix:audio", TASK_STACK, s_uri, 20,
                                        &s_exit.task, 1,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_task_caps = false;
        if (xTaskCreatePinnedToCore(audio_task, "espix:audio", TASK_STACK, s_uri, 20,
                                    &s_exit.task, 1) != pdPASS) {
            s_exit.task = NULL;
            return ESP_FAIL;
        }
    }
    s_running = true;
    return ESP_OK;
}

esp_err_t espix_audio_play(const char *uri)
{
    return play_common(uri, false);
}

esp_err_t espix_audio_play_wait(const char *uri)
{
    return play_common(uri, true);
}

esp_err_t espix_audio_stop(void)
{
    if (s_exit.task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_stop = true;
    return ESP_OK;
}

const char *espix_audio_state(void)
{
    return s_running ? "playing" : "idle";
}

#endif /* CONFIG_ESPIX_AUDIO */
