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
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "esp_asrc.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"

#include "freertos/stream_buffer.h"

#include "espix_kernel.h"
#include "espix_task.h"
#include "espix_audio.h"
#include "espix_audio_sink.h"
#include "espix_audio_source.h"

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

/* A UI's choice, honoured while that sink is registered. */
static char s_sink_want[32];

size_t espix_audio_sink_list(const espix_audio_sink_ops_t **out, size_t max)
{
    size_t n = 0;
    for (size_t i = 0; i < s_sink_count && n < max; i++) {
        out[n++] = s_sinks[i];
    }
    return n;
}

esp_err_t espix_audio_sink_select(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        s_sink_want[0] = '\0';
        return ESP_OK;
    }
    for (size_t i = 0; i < s_sink_count; i++) {
        if (strcmp(s_sinks[i]->name, name) == 0) {
            strlcpy(s_sink_want, name, sizeof(s_sink_want));
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

const char *espix_audio_sink_selected(void)
{
    return s_sink_want;
}

esp_err_t espix_audio_sink_volume(int percent)
{
    const espix_audio_sink_ops_t *sink = espix_audio_sink_default();
    if (sink == NULL || sink->set_volume == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return sink->set_volume(percent);
}

int espix_audio_sink_volume_get(void)
{
    const espix_audio_sink_ops_t *sink = espix_audio_sink_default();
    return (sink != NULL && sink->get_volume != NULL) ? sink->get_volume() : -1;
}

const espix_audio_sink_ops_t *espix_audio_sink_default(void)
{
    /*
     * A chosen sink wins while it is registered -- the point of choosing. With
     * no choice, the first *connected* sink rather than simply the first: the
     * A2DP sink exists from boot but is only connected once a link is up, and a
     * local codec is always there, so a board with both plays to Bluetooth when
     * it is linked and to the speaker when it is not, with nothing set.
     */
    if (s_sink_want[0] != '\0') {
        for (size_t i = 0; i < s_sink_count; i++) {
            if (strcmp(s_sinks[i]->name, s_sink_want) == 0 &&
                (s_sinks[i]->connected == NULL || s_sinks[i]->connected())) {
                return s_sinks[i];
            }
        }
        /*
         * Chosen but not connected: fall through rather than return it. A
         * selection that silences the board reads as a broken player, and the
         * list already says which sink is connected.
         */
    }
    for (size_t i = 0; i < s_sink_count; i++) {
        if (s_sinks[i]->connected == NULL || s_sinks[i]->connected()) {
            return s_sinks[i];
        }
    }
    return s_sink_count > 0 ? s_sinks[0] : NULL;
}

/*
 * The source registry, the mirror of the sink one and for the same reason: a
 * recorder drives whatever is registered, and the providers are components that
 * depend on this one, never the other way round.
 */
#define ESPIX_AUDIO_SOURCES_MAX 2

static const espix_audio_source_ops_t *s_sources[ESPIX_AUDIO_SOURCES_MAX];
static size_t                          s_source_count;

/* The chosen source, by name; empty means "first connected". Same shape as
 * s_sink_want, and read by espix_audio_source_default() below. */
static char s_source_want[32];

esp_err_t espix_audio_source_register(const espix_audio_source_ops_t *ops)
{
    if (ops == NULL || ops->name == NULL || ops->read == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < s_source_count; i++) {
        if (s_sources[i] == ops || strcmp(s_sources[i]->name, ops->name) == 0) {
            return ESP_OK;          /* already published; idempotent */
        }
    }

    if (s_source_count >= ESPIX_AUDIO_SOURCES_MAX) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "no room for source '%s'", ops->name);
        return ESP_ERR_NO_MEM;
    }

    s_sources[s_source_count++] = ops;
    espix_klog(ESPIX_KLOG_INFO, TAG, "source '%s' registered", ops->name);
    return ESP_OK;
}

const espix_audio_source_ops_t *espix_audio_source_default(void)
{
    /*
     * A chosen source wins while it is connected, and falls through when it is
     * not -- a selection that captures nothing reads as a broken recorder, and
     * the list already says which source is connected. Same rule as the sink.
     */
    if (s_source_want[0] != 0) {
        for (size_t i = 0; i < s_source_count; i++) {
            if (strcmp(s_sources[i]->name, s_source_want) == 0 &&
                (s_sources[i]->connected == NULL || s_sources[i]->connected())) {
                return s_sources[i];
            }
        }
    }
    for (size_t i = 0; i < s_source_count; i++) {
        if (s_sources[i]->connected == NULL || s_sources[i]->connected()) {
            return s_sources[i];
        }
    }
    return s_source_count > 0 ? s_sources[0] : NULL;
}

esp_err_t espix_audio_source_select(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        s_source_want[0] = '\0';
        return ESP_OK;
    }
    for (size_t i = 0; i < s_source_count; i++) {
        if (strcmp(s_sources[i]->name, name) == 0) {
            strlcpy(s_source_want, name, sizeof(s_source_want));
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

const char *espix_audio_source_selected(void)
{
    return s_source_want;
}

size_t espix_audio_source_list(const espix_audio_source_ops_t **out, size_t max)
{
    const size_t n = (s_source_count < max) ? s_source_count : max;
    for (size_t i = 0; i < n; i++) {
        out[i] = s_sources[i];
    }
    return n;
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
    .label     = "Benchmark (discard)",
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
 * A stop pressed while a stream is still opening. stream_open() spends tens of
 * milliseconds in the codec's own open before s_stream exists, and a Stop
 * landing in that window used to find nothing and be lost -- which is exactly
 * "I click Test, then Stop, and it does not stop". The flag is cleared by
 * whatever starts a tone, not by the stop, so it cannot be lost.
 */
static volatile bool s_stop_requested;

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
/*
 * The master output volume, 0-100. It is a PCM gain applied here so that it is
 * the same for every sink; 100 is a pass-through and skips the multiply. A
 * sink's own hardware volume -- the ES8311 register, an A2DP link's absolute
 * volume -- is a per-sink control on top of this, not a replacement for it.
 */
static int s_volume = 100;

esp_err_t espix_audio_set_volume(int percent)
{
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }
    s_volume = percent;
    return ESP_OK;
}

int espix_audio_get_volume(void)
{
    return s_volume;
}

static void feed_ex(uint8_t *p, size_t n, volatile bool *stop)
{
    if (s_volume < 100) {
        /* Q15: the naive form divides per sample, which is per-sample CPU. */
        const int32_t g = (int32_t)s_volume * 32768 / 100;
        int16_t *smp = (int16_t *)p;
        for (size_t i = 0; i < n / 2; i++) {
            int32_t v = ((int32_t)smp[i] * g) >> 15;
            if (v > 32767) {
                v = 32767;
            } else if (v < -32768) {
                v = -32768;
            }
            smp[i] = (int16_t)v;
        }
    }

    size_t off = 0;
    while (off < n && !*stop) {
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

/* The engine's own feed: the decoder's stop flag. */
static espix_audio_stream_t *s_play_stream;
static bool                  s_wait;   /* play --wait; the open happens later now */

/*
 * The decoder's PCM goes into a stream, like every other producer's.
 *
 * It used to call the sink directly, which is the whole reason a stream and a
 * play could not coexist: two things drove one sink and neither knew about the
 * other. Now play is a client of the server exactly as an app is, and the
 * mixing, the master gain and the sink's lifecycle are the mixer's business.
 */
static void feed(uint8_t *p, size_t n)
{
    size_t off = 0;

    while (off < n && !s_stop && s_play_stream != NULL) {
        const size_t w = espix_audio_stream_write(s_play_stream, p + off,
                                                  n - off);
        if (w == 0) {
            /*
             * A full ring is not a stop: the mixer has not made room yet. The
             * same zero-means-stop confusion silenced the test tone once.
             */
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        off += w;
    }
}

/*
 * An app's PCM stream: a ring in PSRAM and a task that drains it.
 *
 * The point is that the app and the sink never wait on each other. The app
 * blocks in stream_write only while the ring is full; the task blocks in
 * xStreamBufferReceive only while it is empty. Neither spins. The task carries
 * a PSRAM stack like `play`'s and announces its exit through espix_task_exit,
 * so close() blocks on that rather than watching a handle.
 *
 * One producer at a time for now: a stream and `play` are mutually exclusive,
 * because there is one default sink and no mixer yet.
 */
/*
 * Eight blocks, not sixty-four. 256 KB is 1.5 seconds of audio at 44.1 kHz
 * stereo, and a buffer that size cannot pace its producer: it absorbs a second
 * and a half of overproduction before the writer ever blocks, so the only brake
 * on an app is whatever it does to pace itself -- and when that is wrong, as it
 * was, the audio simply runs fast. It also puts the latency of anything played
 * through it at up to that figure. Sized for the jitter a realtime path needs
 * rather than for throughput, the ring itself becomes the clock.
 */
#define STREAM_RING  (8 * 4 * 1024)
#define STREAM_CHUNK (4 * 1024)
#define STREAM_STACK (4 * 1024)

/*
 * The mixer.
 *
 * One task for every stream rather than one per stream: N apps writing PCM,
 * each summed at its own gain into one block, clipped once, into the one sink.
 * That is the whole of the "server" -- Linux calls it PipeWire's graph, and the
 * deliberate simplification (AUDIO.md) is that there is no graph: every stream
 * routes to the default sink, and the only conversion is whatever the sink
 * negotiated.
 *
 * Lazy like the rest of the engine: the task exists only while at least one
 * stream is open, and the sink is started by the first and suspended by the
 * last. An idle mixer does not feed the sink at all -- "drain 180224 B/s,
 * ring 0 B" while nothing was playing was measured once already, and it is the
 * shape of a link saturating itself on its own silence.
 */
#define STREAM_MAX   4
#define MIX_FRAMES   1024               /* ~23 ms at 44.1 kHz */
#define MIX_BYTES    (MIX_FRAMES * 4)   /* S16 stereo */
#define MIX_GAIN_ONE 32768              /* per-stream gain is Q15 of 0..100 */

/* Frames per generated-tone block; 512 is ~12 ms at 44.1 kHz. */
#define TONE_FRAMES  512

/* Silence between the left/right test's segments; see where it is used. */
#define TONE_GAP_MS  150
#define TONE_HZ      440
#define TONE_LEVEL   12000
/*
 * A sine from the "magic circle": a quadrature pair rotated once per sample by
 * k = 2 sin(pi f / fs). Two multiplies and two adds, no table and no trig --
 * which matters here, because the obvious table costs a kilobyte of the RAM
 * this board is always short of, and sin() per sample costs tens of cycles of
 * soft double math 44100 times a second. There is a CORDIC peripheral, but it
 * computes trig of an angle you have rather than running one: the angle update
 * is the cheap part, and a register round-trip per sample is not.
 */
typedef struct {
    int32_t x;          /* in phase  */
    int32_t y;          /* quadrature */
    int32_t k;          /* 2 sin(pi f / fs), Q15 */
} tone_osc_t;

static void tone_osc_init(tone_osc_t *o, int freq_hz, int rate)
{
    o->x = TONE_LEVEL;
    o->y = 0;
    o->k = (int32_t)(2.0 * sin(M_PI * (double)freq_hz / (double)rate) * 32768.0);
}

static inline int16_t tone_osc_next(tone_osc_t *o)
{
    o->x -= (o->y * o->k) >> 15;
    o->y += (o->x * o->k) >> 15;
    return (int16_t)o->x;
}

/*
 * Defined below, beside the converter they wrap. Declared here because a stream
 * may own one and the stream functions come first.
 */
struct audio_asrc;
static struct audio_asrc *audio_asrc_new(const espix_audio_format_t *src,
                                         const espix_audio_format_t *dst);
static void audio_asrc_free(struct audio_asrc *a);
static bool audio_asrc_run(struct audio_asrc *a, uint32_t in_samples,
                           uint32_t *out_samples);

/*
 * The rates the ASRC converts between, from the datasheet: 8, 16, 32, 44.1 and
 * 48 kHz, and nothing else. It is not a general resampler -- Doom's DMX effects
 * are 11 kHz and can never go through it -- so this is what the negotiation
 * checks before promising a conversion, rather than opening one that cannot
 * work and failing with no reason attached.
 */
static bool asrc_rate_supported(uint32_t hz)
{
    return hz == 8000 || hz == 16000 || hz == 32000 || hz == 44100 || hz == 48000;
}

/* The converter itself, complete here because a stream writes through its
 * members; the functions that open one are defined below. */
typedef struct audio_asrc {
    esp_asrc_handle_t handle;
    uint8_t  *in;
    uint8_t  *out;
    uint32_t  in_size;
    uint32_t  out_size;
    uint32_t  out_max_samples;
    uint16_t  in_frame_bytes;
    uint16_t  out_frame_bytes;
} audio_asrc_t;

struct espix_audio_stream {
    StreamBufferHandle_t sb;
    espix_audio_format_t fmt;         /* what the owner asked for */
    espix_audio_format_t ring_fmt;    /* what the ring holds: the sink's */
    struct audio_asrc   *asrc;        /* non-NULL when those two differ */
    size_t               out_pending; /* converted, not yet taken by the ring */
    size_t               out_off;
    int                  volume;      /* 0-100, this stream's own level */
    bool                 mute;        /* kept apart from volume, so unmute restores */
    volatile bool        stop;        /* no further writes; the owner will close */
    void                *owner;       /* the task that opened it; see the reaper */
};

/*
 * The open streams, and the one task that mixes them. A fixed table rather than
 * a list: the number is small, the lookup is a scan of four, and a stream that
 * cannot be opened is a clear answer instead of an allocation that might fail
 * later.
 */
static espix_audio_stream_t *s_streams[STREAM_MAX];
static espix_task_exit_t     s_mixer_exit;
static volatile bool         s_mixer_stop;
static volatile bool         s_mixer_up;

static int stream_slot_free(void)
{
    for (int i = 0; i < STREAM_MAX; i++) {
        if (s_streams[i] == NULL) {
            return i;
        }
    }
    return -1;
}

static bool stream_any_open(void)
{
    for (int i = 0; i < STREAM_MAX; i++) {
        if (s_streams[i] != NULL) {
            return true;
        }
    }
    return false;
}

static size_t stream_count(void)
{
    size_t n = 0;
    for (int i = 0; i < STREAM_MAX; i++) {
        if (s_streams[i] != NULL) {
            n++;
        }
    }
    return n;
}

/*
 * Sum one stream into the mix, at its own gain, clipping as we go.
 *
 * Clipped here rather than at the end on purpose: a stream that is too loud
 * should distort itself, not everything else in the mix. The master gain is
 * applied later, in feed_ex(), where it stays a single multiply over the sum.
 */
static void mix_add(int16_t *out, const int16_t *in, size_t frames, int volume)
{
    const int32_t g = (int32_t)volume * MIX_GAIN_ONE / 100;

    for (size_t i = 0; i < frames * 2; i++) {
        int32_t v = in[i];
        if (g != MIX_GAIN_ONE) {
            v = (v * g) >> 15;
        }
        v += out[i];
        out[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
    }
}

static void mixer_task(void *arg)
{
    (void)arg;
    int16_t *out = malloc(MIX_BYTES);
    int16_t *in  = malloc(MIX_BYTES);

    /*
     * End to end instrumentation, and it is the wall clock or it is nothing.
     * A loop that sleeps correctly and a loop that spins report the same *work*,
     * which is what made the last two rounds guesswork.
     *
     * full/part is the number that matters here: "A full block, always" below
     * pads with silence, and if the producers hand over less than MIX_BYTES the
     * sink is being fed silence it did not ask for -- heard as the audio
     * stretching and doubling over itself.
     */
#if CONFIG_ESPIX_AUDIO_TRACE
    int64_t  t_win  = esp_timer_get_time();
    int64_t  t_recv = 0;
    int64_t  t_feed = 0;
    uint64_t fed    = 0;
    uint32_t iters  = 0, full = 0, part = 0;
#endif

    while (out != NULL && in != NULL && !s_mixer_stop) {
        memset(out, 0, MIX_BYTES);

        /*
         * A full block, always. A stream with nothing ready contributes silence
         * for this block rather than shrinking it: the others must not be held
         * back by one app that is between sounds, and the sink wants a steady
         * block size either way.
         */
#if CONFIG_ESPIX_AUDIO_TRACE
        const int64_t t_r0 = esp_timer_get_time();
#endif
        bool any = false;
        for (int i = 0; i < STREAM_MAX; i++) {
            espix_audio_stream_t *st = s_streams[i];
            if (st == NULL) {
                continue;
            }
            /*
             * A timeout, and it is the fix, not a tweak.
             *
             * The ring is created with STREAM_CHUNK as its trigger level --
             * which is what unblocks a *waiting* receiver -- and 0 as the
             * timeout means never waiting, so the level never applied and this
             * read whatever happened to be there. A producer writing blocks
             * smaller than MIX_BYTES then had its audio laid into the front of
             * each output block with silence behind it: half of every block was
             * gap, which is heard as the sound stretching and doubling over
             * itself, and it is why the sink's callback reported the ring dry
             * on a third of its calls while the link itself ran at exactly
             * realtime.
             *
             * Waiting for the level is what makes the producer's block size its
             * own business. The timeout is what keeps a genuine gap a gap: four
             * ticks is comfortably longer than the block period of anything
             * here, and a stream that really has nothing still contributes
             * silence rather than holding the others back.
             */
            const size_t n = xStreamBufferReceive(st->sb, in, MIX_BYTES,
                                                  pdMS_TO_TICKS(4));
            if (n == 0) {
                continue;
            }
            any = true;
#if CONFIG_ESPIX_AUDIO_TRACE
            if (n < MIX_BYTES) {
                part++;
            } else {
                full++;
            }
#endif
            mix_add(out, in, n / 4, st->mute ? 0 : st->volume);
        }
#if CONFIG_ESPIX_AUDIO_TRACE
        t_recv += esp_timer_get_time() - t_r0;
#endif

        if (!any) {
            /*
             * Nothing to play. Do not feed the sink -- that is what keeps an
             * idle link from streaming silence -- and wake soon enough that the
             * first write after a lull is not held up by the poll.
             */
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

#if CONFIG_ESPIX_AUDIO_TRACE
        const int64_t t_f0 = esp_timer_get_time();
#endif
        feed_ex((uint8_t *)out, MIX_BYTES, &s_mixer_stop);
#if CONFIG_ESPIX_AUDIO_TRACE
        t_feed += esp_timer_get_time() - t_f0;
#endif

#if CONFIG_ESPIX_AUDIO_TRACE
        fed += MIX_BYTES;
        if (++iters >= 64) {
            const int64_t  wall = esp_timer_get_time() - t_win;
            const uint64_t rate = (wall > 0) ? fed * 1000000ULL / (uint64_t)wall : 0;
            espix_klog(ESPIX_KLOG_INFO, TAG,
                       "mix %u blk: %lld ms wall, recv %lld feed %lld, "
                       "%llu B (%llu B/s), full %u part %u",
                       (unsigned)iters, (long long)(wall / 1000),
                       (long long)(t_recv / 1000), (long long)(t_feed / 1000),
                       (unsigned long long)fed, (unsigned long long)rate,
                       (unsigned)full, (unsigned)part);
            t_win  = esp_timer_get_time();
            t_recv = 0;
            t_feed = 0;
            fed    = 0;
            iters  = 0;
            full   = 0;
            part   = 0;
        }
#endif
    }

    free(out);
    free(in);
    espix_task_exited(&s_mixer_exit);
    vTaskDeleteWithCaps(NULL);
}

espix_audio_stream_t *espix_audio_stream_open(const espix_audio_format_t *fmt)
{
    if (fmt == NULL || fmt->channels == 0 || fmt->bits != 16) {
        return NULL;
    }
    /*
     * `play` still feeds the sink itself rather than opening a stream, so the
     * two cannot run at once: they would both drive the same sink and neither
     * would know. Moving play onto this API is what removes the check.
     */
    const int slot = stream_slot_free();
    if (slot < 0) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "stream: all %d are open", STREAM_MAX);
        return NULL;
    }
    const espix_audio_sink_ops_t *sink = espix_audio_sink_default();
    if (sink == NULL) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "stream: no sink registered");
        return NULL;
    }
    if (!sink->connected()) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "stream: sink '%s' is not connected",
                   sink->name);
        return NULL;
    }

    espix_audio_stream_t *st = calloc(1, sizeof(*st));
    if (st == NULL) {
        return NULL;
    }
    st->fmt      = *fmt;
    st->ring_fmt = sink->format();
    st->volume   = 100;

    /*
     * Rate negotiation, cheapest answer first.
     *
     * Same format: nothing to do. Different, but the sink can adopt it: ask,
     * because forwarding is free and resampling is not -- which is the whole
     * reason a sink is allowed to say it can. Otherwise the stream converts,
     * and it converts at its own edge: the ring still holds the sink's format,
     * so the mixer is untouched and one stream's rate never reaches another's.
     */
    if (fmt->rate != st->ring_fmt.rate || fmt->channels != st->ring_fmt.channels) {
        /*
         * Adopting is only safe while nothing else is playing. The mixer sums
         * every stream at one rate -- the sink's -- so changing it under a
         * stream that is already open would reinterpret that stream's samples.
         * With none open the change is free; with any, this stream converts.
         */
        if (sink->set_format != NULL && stream_count() == 0 && sink->set_format(fmt)) {
            st->ring_fmt = *fmt;
            espix_klog(ESPIX_KLOG_INFO, TAG,
                       "stream: sink '%s' adopts %u Hz, %u ch",
                       sink->name, (unsigned)fmt->rate, (unsigned)fmt->channels);
        } else if (!asrc_rate_supported(fmt->rate) ||
                   !asrc_rate_supported(st->ring_fmt.rate)) {
            espix_klog(ESPIX_KLOG_WARN, TAG,
                       "stream: cannot convert %u Hz to %u Hz; the ASRC does "
                       "8/16/32/44.1/48 kHz only, so the producer must match "
                       "the sink",
                       (unsigned)fmt->rate, (unsigned)st->ring_fmt.rate);
            vStreamBufferDeleteWithCaps(st->sb);
            free(st);
            return NULL;
        } else {
            st->asrc = audio_asrc_new(fmt, &st->ring_fmt);
            if (st->asrc == NULL) {
                espix_klog(ESPIX_KLOG_WARN, TAG,
                           "stream: cannot convert %u Hz to %u Hz",
                           (unsigned)fmt->rate, (unsigned)st->ring_fmt.rate);
                vStreamBufferDeleteWithCaps(st->sb);
                free(st);
                return NULL;
            }
            espix_klog(ESPIX_KLOG_INFO, TAG,
                       "stream: converting %u Hz -> %u Hz for sink '%s'",
                       (unsigned)fmt->rate, (unsigned)st->ring_fmt.rate, sink->name);
        }
    }
    /*
     * Whose it is, by task handle rather than by pid: the engine has no
     * business knowing what a pid is, and the reaper already hands the dying
     * task to whoever needs it -- see espix_pm_task_gone for the same shape.
     */
    st->owner  = (void *)xTaskGetCurrentTaskHandle();
    st->sb     = xStreamBufferCreateWithCaps(STREAM_RING, STREAM_CHUNK, MALLOC_CAP_SPIRAM);
    if (st->sb == NULL) {
        free(st);
        return NULL;
    }

    /*
     * In the table before anything can see it, and before the sink starts: a
     * Stop landing inside the sink's own open has to find this stream, which is
     * the race that made the desktop's Stop button do nothing.
     */
    s_streams[slot] = st;

    if (s_mixer_up) {
        return st;                      /* already mixing; nothing else to do */
    }

    /*
     * First stream: the sink starts, then the mixer. In that order, so the
     * codec's own open (an I2C register sequence, tens of milliseconds) is not
     * inside the mixer's first block.
     */
    if (!espix_task_exit_init(&s_mixer_exit)) {
        s_streams[slot] = NULL;
        vStreamBufferDeleteWithCaps(st->sb);
        free(st);
        return NULL;
    }
    s_mixer_stop = false;
    s_mixer_up   = true;
    sink->start();

    if (xTaskCreatePinnedToCoreWithCaps(mixer_task, "espix:mix", STREAM_STACK, NULL, 20,
                                        &s_mixer_exit.task, 1,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        if (xTaskCreatePinnedToCore(mixer_task, "espix:mix", STREAM_STACK, NULL, 20,
                                    &s_mixer_exit.task, 1) != pdPASS) {
            s_mixer_exit.task = NULL;
            s_mixer_up        = false;
            s_streams[slot]   = NULL;
            vStreamBufferDeleteWithCaps(st->sb);
            free(st);
            return NULL;
        }
    }
    return st;
}

size_t espix_audio_stream_write(espix_audio_stream_t *st, const void *pcm, size_t len)
{
    if (st == NULL || pcm == NULL || len == 0) {
        return 0;
    }
    /*
     * Stopped from elsewhere: say so with a zero rather than accept more. The
     * writer's loop ends on it, which is what lets the *owner* close the
     * stream -- closing it here instead would free it while this call is still
     * inside, and that use-after-free is exactly how a stopped tone kept
     * playing.
     */
    if (st->stop) {
        return 0;
    }
    /*
     * Blocks while the ring is full -- that is the backpressure, and how an app
     * paces itself to the sink. A timeout rather than portMAX_DELAY so the stop
     * above is noticed within it; the count actually taken comes back either
     * way, and looping on a short write is the caller's.
     */
    if (st->asrc == NULL) {
        return xStreamBufferSend(st->sb, pcm, len, pdMS_TO_TICKS(1000));
    }

    /*
     * Converting. The ring holds the sink's rate, so what goes into it is not
     * what the owner passed in.
     *
     * Converted-but-untaken bytes are pushed before anything else: the ASRC's
     * output buffer is the only place they exist, so converting more would
     * overwrite them, and telling the caller to retry would convert its input
     * twice. A zero there is the ordinary backpressure -- nothing was consumed,
     * so the same bytes come back and are converted once.
     */
    struct audio_asrc *const a = st->asrc;

    while (st->out_pending > 0) {
        const size_t w = xStreamBufferSend(st->sb, (uint8_t *)a->out + st->out_off,
                                           st->out_pending, pdMS_TO_TICKS(1000));
        if (w == 0) {
            return 0;
        }
        st->out_off     += w;
        st->out_pending -= w;
    }

    size_t chunk = (len > a->in_size) ? a->in_size : len;
    chunk -= chunk % a->in_frame_bytes;             /* whole frames only */
    if (chunk == 0) {
        return 0;
    }

    memcpy(a->in, pcm, chunk);

    uint32_t out_samples = a->out_max_samples;
    if (!audio_asrc_run(a, (uint32_t)(chunk / a->in_frame_bytes), &out_samples)) {
        return 0;
    }

    st->out_off     = 0;
    st->out_pending = (size_t)out_samples * a->out_frame_bytes;

    const size_t w = xStreamBufferSend(st->sb, a->out, st->out_pending,
                                       pdMS_TO_TICKS(1000));
    st->out_off     = w;
    st->out_pending -= w;

    /* The caller counts in its own bytes: this much in, whatever it became. */
    return chunk;
}

esp_err_t espix_audio_stream_close(espix_audio_stream_t *st)
{
    if (st == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    st->stop = true;                    /* no further writes from the owner */

    /*
     * Let the mixer finish what is queued before the row goes. The ring holds
     * up to a second and a half of audio, and dropping it cuts the tail off
     * everything short -- the bug fixed once already as "a 3-second tone that
     * finishes in one second and is never heard".
     */
    for (int i = 0; i < 500 && s_mixer_up && !xStreamBufferIsEmpty(st->sb); i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    for (int i = 0; i < STREAM_MAX; i++) {
        if (s_streams[i] == st) {
            s_streams[i] = NULL;
        }
    }
    vStreamBufferDeleteWithCaps(st->sb);
    audio_asrc_free(st->asrc);
    free(st);

    if (stream_any_open()) {
        return ESP_OK;                  /* other apps are still playing */
    }

    /*
     * Last one out. The mixer goes first, then the sink is allowed to finish:
     * an A2DP link left STREAMING with nothing behind it pulls the full rate
     * out of an empty ring -- "drain 180224 B/s, ring 0 B" while nothing was
     * playing -- and suspending it flushes whatever it still holds, so the
     * drain comes before the suspend for the same reason it always did.
     */
    s_mixer_stop = true;
    if (s_mixer_up) {
        (void)espix_task_exit_wait(&s_mixer_exit, 0);
        s_mixer_up = false;
    }

    const espix_audio_sink_ops_t *sink = espix_audio_sink_default();
    if (sink != NULL) {
        if (sink->drain != NULL) {
            sink->drain();
        }
        sink->suspend();
    }
    return ESP_OK;
}

/*
 * A stream's own level. Master is the engine's; this is the app's, and they
 * multiply -- which is what keeps an app's mix from changing the system volume.
 */
esp_err_t espix_audio_stream_volume(espix_audio_stream_t *st, int percent)
{
    if (st == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }
    st->volume = percent;
    return ESP_OK;
}

esp_err_t espix_audio_stream_mute(espix_audio_stream_t *st, bool on)
{
    if (st == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    st->mute = on;
    return ESP_OK;
}

/*
 * Close whatever a dying task left open.
 *
 * Without this a killed player keeps its stream, its ring and its slot for
 * good: the mixer goes on mixing a producer that will never write again, the
 * sink never suspends because "a stream is open", and four kills exhaust the
 * table. It is the same failure espix_gfx_recover() exists to prevent for the
 * canvas lock, and it is called from the same place.
 */
void espix_audio_streams_recover(void *task)
{
    if (task == NULL) {
        return;
    }
    for (int i = 0; i < STREAM_MAX; i++) {
        espix_audio_stream_t *st = s_streams[i];
        if (st != NULL && st->owner == task) {
            espix_klog(ESPIX_KLOG_WARN, TAG,
                       "closing a stream its owner died holding");
            (void)espix_audio_stream_close(st);
        }
    }
}

/*
 * A generated tone, written through the stream API above. `ch` is which
 * channel carries it: 0 left, 1 right, 2 both. That is what makes a stereo
 * check possible -- one second each way says more than a level meter.
 */
/* Frames of silence, written the same way a segment is. */
static void tone_silence(espix_audio_stream_t *st, int frames, int16_t *buf)
{
    memset(buf, 0, (size_t)TONE_FRAMES * 4);
    while (frames > 0 && !s_stop_requested) {
        const int    n     = frames > TONE_FRAMES ? TONE_FRAMES : frames;
        const size_t bytes = (size_t)n * 4;
        size_t       off   = 0;
        while (off < bytes) {
            if (st->stop || s_stop_requested) {
                return;
            }
            const size_t w = espix_audio_stream_write(st, (uint8_t *)buf + off,
                                                      bytes - off);
            if (w == 0) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            off += w;
        }
        frames -= n;
    }
}

static void tone_play(espix_audio_stream_t *st, int frames, tone_osc_t *osc,
                      int ch, int16_t *buf)
{
    /*
     * Ramp every segment's ends. Switching channels is a step -- one side goes
     * from full scale to zero and the other from zero to full scale inside a
     * single sample -- and a step is a click: heard as an abrupt cut, and as a
     * right-channel tone that seems to start late. A few milliseconds turns it
     * into a move, and costs nothing on a tone.
     */
    const int total = frames;
    const int ramp  = (total < 2048) ? total / 4 : 352;      /* ~8 ms at 44.1k */
    int       done  = 0;

    while (frames > 0 && !s_stop_requested) {
        int n = frames > TONE_FRAMES ? TONE_FRAMES : frames;

        for (int i = 0; i < n; i++) {
            int32_t v = tone_osc_next(osc);
            if (ramp > 0) {
                const int pos = done + i;
                int32_t   g   = 32768;
                if (pos < ramp) {
                    g = (int32_t)((int64_t)32768 * pos / ramp);
                } else if (pos >= total - ramp) {
                    g = (int32_t)((int64_t)32768 * (total - pos) / ramp);
                }
                v = (v * g) >> 15;
            }
            buf[2 * i]     = (ch == 1) ? 0 : (int16_t)v;
            buf[2 * i + 1] = (ch == 0) ? 0 : (int16_t)v;
        }
        done += n;
        size_t off = 0;
        const size_t bytes = (size_t)n * 4;
        while (off < bytes) {
            if (st->stop || s_stop_requested) {
                return;                 /* stopped: the owner closes it */
            }
            const size_t w = espix_audio_stream_write(st, (uint8_t *)buf + off,
                                                      bytes - off);
            if (w == 0) {
                /*
                 * A full ring, not a stop. stream_write's wait timed out because
                 * the sink has not made room yet -- which is what an A2DP link
                 * still waking out of sniff looks like, and it takes about a
                 * second and a half. Give up the write, never the tone: reading
                 * this as "closed under us" is why a test tone was silent.
                 */
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            off += w;
        }
        frames -= n;
    }
}

esp_err_t espix_audio_tone(int freq_hz, int seconds, bool left_right)
{
    if (freq_hz <= 0 || freq_hz > 20000) {
        return ESP_ERR_INVALID_ARG;
    }
    s_stop_requested = false;       /* a new tone is not a stopped one */
    const espix_audio_format_t fmt = { 44100, 2, 16 };
    espix_audio_stream_t *st = espix_audio_stream_open(&fmt);
    if (st == NULL) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "tone: no stream (no sink, or busy)");
        return ESP_ERR_INVALID_STATE;       /* no sink, or one is busy */
    }
    espix_klog(ESPIX_KLOG_INFO, TAG, "tone: %d Hz, %d s, %s", freq_hz, seconds,
               left_right ? "left/right/both" : "both");

    int16_t *buf = malloc(TONE_FRAMES * 2 * sizeof(int16_t));
    if (buf == NULL) {
        (void)espix_audio_stream_close(st);
        return ESP_ERR_NO_MEM;
    }

    tone_osc_t  osc;
    const int   one_s = (int)fmt.rate;

    tone_osc_init(&osc, freq_hz, (int)fmt.rate);

    if (left_right) {
        /*
         * Timed and logged per segment. The three are identical in code -- same
         * frame count, same generator, same path -- so if one comes out
         * audibly shorter the number that proves it belongs here, not in a
         * guess about SBC joint stereo or the far end's rendering.
         */
        /*
         * The order is left, right, both, and the channel is what each segment
         * drives. (This was swapped once, deliberately, to place a fault: the
         * short segment turned out to follow the *position* and not the
         * channel, which ruled out the sink's rendering and put it here.)
         */
        static const char *const seg[3] = {
            "left channel", "right channel", "both channels"
        };
        static const int seg_ch[3] = { 0, 1, 2 };
        /*
         * A gap between them, and it is a diagnostic as much as a courtesy:
         * without one the three run into each other and the boundary is a guess
         * by ear. With one, "the middle tone is short" is a statement about the
         * audio rather than about a transition nobody can hear where it lands.
         */
        const int gap = (int)fmt.rate * TONE_GAP_MS / 1000;
        for (int i = 0; i < 3; i++) {
            const int ch = seg_ch[i];
            const int64_t t0 = esp_timer_get_time();
            tone_play(st, one_s, &osc, ch, buf);
            espix_klog(ESPIX_KLOG_INFO, TAG, "tone: %s, %lld ms",
                       seg[i], (long long)((esp_timer_get_time() - t0) / 1000));
            if (i < 2) {
                tone_silence(st, gap, buf);
            }
        }
    } else {
        tone_play(st, seconds * one_s, &osc, 2, buf);
    }

    free(buf);
    const esp_err_t err = espix_audio_stream_close(st);
    espix_klog(ESPIX_KLOG_INFO, TAG, "tone: done (%s) requested %d",
               esp_err_to_name(err), (int)s_stop_requested);
    return err;
}

/*
 * The same, without blocking the caller -- for a UI, whose input handler cannot
 * stand still for three seconds while a tone plays. A task of its own, on a
 * PSRAM stack, announcing nothing because there is nobody waiting.
 */
typedef struct {
    int  freq_hz;
    int  seconds;
    bool left_right;
} tone_args_t;

static void tone_task(void *arg)
{
    tone_args_t a = *(tone_args_t *)arg;
    free(arg);

    /*
     * Checked before the tone starts, because a Stop can land between this task
     * being created and it getting here -- the same race the stream's early
     * publication covers further in.
     */
    if (!s_stop_requested) {
        (void)espix_audio_tone(a.freq_hz, a.seconds, a.left_right);
    }
    vTaskDeleteWithCaps(NULL);
}

esp_err_t espix_audio_tone_async(int freq_hz, int seconds, bool left_right)
{
    tone_args_t *a = malloc(sizeof(*a));
    if (a == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_stop_requested = false;
    a->freq_hz     = freq_hz;
    a->seconds     = seconds;
    a->left_right  = left_right;

    if (xTaskCreateWithCaps(tone_task, "espix:tone", STREAM_STACK, a, 4,
                            NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        free(a);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
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

/*
 * Sample-rate conversion, for when a source does not match the sink's rate.
 *
 * The engine has no resampler of its own; this is esp_asrc, which is the S31's
 * hardware ASRC and an optimized software path where there is none. It can
 * convert channels too, but the engine's own mono duplication already covers
 * the channel-only case, so this opens only when the rate differs.
 *
 * The decoded frame is copied into the ASRC's own input buffer because the
 * hardware path asks for cache-line alignment and the codec's decode target is
 * not required to meet it. One audio task runs at a time, so this is file
 * scope rather than passed around.
 */


static audio_asrc_t s_asrc;

static void asrc_close(audio_asrc_t *a)
{
    if (a->handle != NULL) {
        esp_asrc_close(a->handle);
        a->handle = NULL;
    }
    heap_caps_free(a->in);
    heap_caps_free(a->out);
    a->in = a->out = NULL;
}

static bool asrc_open(audio_asrc_t *a, const esp_asrc_aud_info_t *src,
                      const esp_asrc_aud_info_t *dst, size_t in_bytes_max)
{
    audio_asrc_t fresh = { 0 };
    esp_asrc_buffer_alignment_t al = { 0 };
    esp_asrc_cfg_t cfg = {
        .src_info   = *src,
        .dest_info  = *dst,
        .perf_type  = ESP_ASRC_PERF_TYPE_AUTO,
        .complexity = 3,
        .timeout_ms = -1,
    };

    if (esp_asrc_open(&cfg, &fresh.handle) != ESP_ASRC_ERR_OK) {
        return false;
    }
    esp_asrc_get_bytes_per_sample(fresh.handle, &fresh.in_frame_bytes,
                                  &fresh.out_frame_bytes);
    if (fresh.in_frame_bytes == 0 || fresh.out_frame_bytes == 0) {
        asrc_close(&fresh);
        return false;
    }

    /*
     * Output for one input chunk: the input's sample count times the rate
     * ratio, plus margin for the ASRC's fractional residual. A non-integer
     * ratio gives a slightly varying count per call, and the actual one comes
     * back from esp_asrc_process.
     */
    const uint64_t in_samples  = in_bytes_max / fresh.in_frame_bytes;
    const uint64_t out_samples = (in_samples * dst->sample_rate) / src->sample_rate + 64;
    const uint32_t out_bytes   = (uint32_t)out_samples * fresh.out_frame_bytes;

    esp_asrc_get_buffer_alignment(&al);
    fresh.in  = esp_asrc_align_alloc(in_bytes_max, al.inbuf_addr_align,
                                     al.inbuf_size_align, &fresh.in_size);
    fresh.out = esp_asrc_align_alloc(out_bytes, al.outbuf_addr_align,
                                     al.outbuf_size_align, &fresh.out_size);
    if (fresh.in == NULL || fresh.out == NULL) {
        asrc_close(&fresh);
        return false;
    }
    fresh.out_max_samples = fresh.out_size / fresh.out_frame_bytes;

    *a = fresh;
    return true;
}

/*
 * A converter a stream owns, and the lock that lets several of them exist.
 *
 * The file-scoped instance above was enough while the decoder was the only user;
 * streams made the peripheral shared, because each can convert at its own edge
 * on its own thread. The lock is held across esp_asrc_process and nothing else
 * -- the conversion is short and the call is what must not interleave.
 */
static SemaphoreHandle_t s_asrc_lock;

struct audio_asrc *audio_asrc_new(const espix_audio_format_t *src,
                                  const espix_audio_format_t *dst)
{
    const esp_asrc_aud_info_t si = { .sample_rate = src->rate,
                                     .bits_per_sample = src->bits,
                                     .channel = src->channels };
    const esp_asrc_aud_info_t di = { .sample_rate = dst->rate,
                                     .bits_per_sample = dst->bits,
                                     .channel = dst->channels };
    audio_asrc_t *a = calloc(1, sizeof(*a));

    if (a == NULL) {
        return NULL;
    }
    if (!asrc_open(a, &si, &di, STREAM_CHUNK)) {
        free(a);
        return NULL;
    }
    return (struct audio_asrc *)a;
}

static void audio_asrc_free(struct audio_asrc *a)
{
    if (a != NULL) {
        asrc_close((audio_asrc_t *)a);
        free(a);
    }
}

static bool audio_asrc_run(struct audio_asrc *a, uint32_t in_samples,
                           uint32_t *out_samples)
{
    audio_asrc_t *const c = (audio_asrc_t *)a;

    if (s_asrc_lock == NULL) {
        s_asrc_lock = xSemaphoreCreateMutex();
        if (s_asrc_lock == NULL) {
            return false;
        }
    }
    if (xSemaphoreTake(s_asrc_lock, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    const esp_err_t e = esp_asrc_process(c->handle, c->in, in_samples,
                                         c->out, out_samples);
    xSemaphoreGive(s_asrc_lock);
    return e == ESP_ASRC_ERR_OK;
}

/* ------------------------------------------------------------- capture -- */

typedef struct espix_audio_capture {
    const espix_audio_source_ops_t *src;
    espix_audio_format_t            got;         /* what read() returns */
    struct audio_asrc              *asrc;        /* when that is not the source's */
    size_t                          out_pending; /* converted, not yet taken */
    size_t                          out_off;
} espix_audio_capture_t;

static espix_audio_capture_t *s_capture;

espix_audio_capture_t *espix_audio_capture_open(const espix_audio_format_t *want,
                                                espix_audio_format_t *got)
{
    if (s_capture != NULL) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "capture: already open");
        return NULL;
    }
    const espix_audio_source_ops_t *src = espix_audio_source_default();
    if (src == NULL || !src->connected()) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "capture: no source");
        return NULL;
    }
    const espix_audio_format_t src_fmt = src->format();
    if (src_fmt.rate == 0 || src_fmt.channels == 0) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "capture: '%s' reports no format", src->name);
        return NULL;
    }

    espix_audio_capture_t *cap = calloc(1, sizeof(*cap));
    if (cap == NULL) {
        return NULL;
    }
    cap->src = src;
    cap->got = src_fmt;

    if (want != NULL &&
        (want->rate != src_fmt.rate || want->channels != src_fmt.channels)) {
        if (!asrc_rate_supported(want->rate) || !asrc_rate_supported(src_fmt.rate)) {
            espix_klog(ESPIX_KLOG_WARN, TAG,
                       "capture: cannot convert %u Hz to %u Hz; the ASRC does "
                       "8/16/32/44.1/48 kHz only",
                       (unsigned)src_fmt.rate, (unsigned)want->rate);
            free(cap);
            return NULL;
        }
        cap->asrc = audio_asrc_new(&src_fmt, want);
        if (cap->asrc == NULL) {
            espix_klog(ESPIX_KLOG_WARN, TAG, "capture: cannot convert");
            free(cap);
            return NULL;
        }
        cap->got = *want;
    }

    src->start();
    s_capture = cap;
    if (got != NULL) {
        *got = cap->got;
    }
    espix_klog(ESPIX_KLOG_INFO, TAG, "capture: %s at %u Hz, %u ch",
               src->name, (unsigned)cap->got.rate, (unsigned)cap->got.channels);
    return cap;
}

size_t espix_audio_capture_read(espix_audio_capture_t *cap, void *pcm, size_t len)
{
    if (cap == NULL || pcm == NULL || len == 0) {
        return 0;
    }

    /* The source's own format: the caller's buffer is handed straight over. */
    if (cap->asrc == NULL) {
        for (int tries = 0; tries < 20; tries++) {
            const size_t n = cap->src->read(pcm, len);
            if (n > 0) {
                return n;
            }
            vTaskDelay(pdMS_TO_TICKS(5));       /* pace, do not spin */
        }
        return 0;
    }

    /*
     * Converting, with the same care the write path takes in the other
     * direction: one conversion fills the ASRC's output buffer, and the caller
     * may ask for less than that, so what is left over is kept rather than
     * recomputed or dropped.
     */
    struct audio_asrc *const a = cap->asrc;

    if (cap->out_pending == 0) {
        size_t in_bytes = 0;
        for (int tries = 0; tries < 20 && in_bytes < a->in_size; tries++) {
            const size_t n = cap->src->read(a->in + in_bytes, a->in_size - in_bytes);
            if (n == 0) {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            in_bytes += n;
        }
        in_bytes -= in_bytes % a->in_frame_bytes;
        if (in_bytes == 0) {
            return 0;
        }

        uint32_t out_samples = a->out_max_samples;
        if (!audio_asrc_run(a, (uint32_t)(in_bytes / a->in_frame_bytes), &out_samples)) {
            return 0;
        }
        if (out_samples == 0) {
            return 0;
        }
        cap->out_off     = 0;
        cap->out_pending = (size_t)out_samples * a->out_frame_bytes;
    }

    size_t take = cap->out_pending;
    if (take > len) {
        take = len;
    }
    memcpy(pcm, a->out + cap->out_off, take);
    cap->out_off     += take;
    cap->out_pending -= take;
    return take;
}

esp_err_t espix_audio_capture_close(espix_audio_capture_t *cap)
{
    if (cap == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cap->src != NULL) {
        cap->src->stop();
    }
    audio_asrc_free(cap->asrc);
    if (s_capture == cap) {
        s_capture = NULL;
    }
    free(cap);
    return ESP_OK;
}

/* ----------------------------------------------------------------- pipes -- */

typedef struct {
    int                             id;
    const espix_audio_source_ops_t *src;
    const espix_audio_sink_ops_t   *snk;
    volatile bool                   stop;
    volatile bool                   done;
    TaskHandle_t                    task;
} espix_audio_pipe_t;

static espix_audio_pipe_t s_pipes[ESPIX_AUDIO_PIPES_MAX];
static int                 s_pipe_next_id = 1;

#define PIPE_CHUNK 2048

static void pipe_task(void *arg)
{
    espix_audio_pipe_t *const p = arg;

    uint8_t *buf = malloc(PIPE_CHUNK);
    if (buf == NULL) {
        p->done = true;
        p->task = NULL;
        vTaskDeleteWithCaps(NULL);
        return;
    }

    /*
     * The capture's format, not one of our choosing. One end states the format
     * and the stream's edge reconciles it with the sink, so a pipe carries no
     * conversion of its own: a 16 kHz microphone into a 44.1 kHz codec takes
     * exactly the negotiation every other producer gets.
     */
    espix_audio_format_t got = { 0, 0, 0 };
    espix_audio_capture_t *cap = espix_audio_capture_open(NULL, &got);
    if (cap == NULL) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "pipe %d: cannot capture", p->id);
        goto out;
    }

    espix_audio_stream_t *st = espix_audio_stream_open(&got);
    if (st == NULL) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "pipe %d: cannot play", p->id);
        (void)espix_audio_capture_close(cap);
        goto out;
    }
    espix_klog(ESPIX_KLOG_INFO, TAG, "pipe %d: %s -> %s, %u Hz",
               p->id, p->src->name, p->snk->name, (unsigned)got.rate);

    while (!p->stop) {
        const size_t n = espix_audio_capture_read(cap, buf, PIPE_CHUNK);
        if (n == 0) {
            continue;                   /* capture_read paces; nothing yet */
        }
        size_t off = 0;
        while (off < n && !p->stop) {
            const size_t w = espix_audio_stream_write(st, buf + off, n - off);
            if (w == 0) {
                break;                  /* stopped, or the ring is closing */
            }
            off += w;
        }
    }

    (void)espix_audio_stream_close(st);
    (void)espix_audio_capture_close(cap);

out:
    free(buf);
    p->done = true;
    p->task = NULL;
    vTaskDeleteWithCaps(NULL);
}

int espix_audio_pipe_add(const char *source, const char *sink)
{
    const espix_audio_source_ops_t *src = espix_audio_source_default();

    if (source != NULL && source[0] != 0) {
        const espix_audio_source_ops_t *list[ESPIX_AUDIO_SOURCES_MAX];
        const size_t n = espix_audio_source_list(list, ESPIX_AUDIO_SOURCES_MAX);
        src = NULL;
        for (size_t i = 0; i < n; i++) {
            if (strcmp(list[i]->name, source) == 0) {
                src = list[i];
            }
        }
    }
    if (src == NULL || (src->connected != NULL && !src->connected())) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "pipe: no source '%s'",
                   source != NULL ? source : "(default)");
        return -1;
    }

    const espix_audio_sink_ops_t *snk = espix_audio_sink_default();
    if (snk == NULL) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "pipe: no sink");
        return -1;
    }
    if (sink != NULL && sink[0] != 0 && strcmp(snk->name, sink) != 0) {
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "pipe: the mixer has one output and it is '%s', not '%s' -- "
                   "select it with 'sink %s' first", snk->name, sink, sink);
        return -1;
    }

    for (int i = 0; i < ESPIX_AUDIO_PIPES_MAX; i++) {
        espix_audio_pipe_t *const p = &s_pipes[i];
        if (p->id != 0) {
            continue;
        }
        memset(p, 0, sizeof(*p));
        p->id  = s_pipe_next_id++;
        p->src = src;
        p->snk = snk;

        if (xTaskCreatePinnedToCoreWithCaps(pipe_task, "espix:pipe", STREAM_STACK,
                                            p, 20, &p->task, 1,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS &&
            xTaskCreatePinnedToCore(pipe_task, "espix:pipe", STREAM_STACK,
                                    p, 20, &p->task, 1) != pdPASS) {
            p->id = 0;
            espix_klog(ESPIX_KLOG_WARN, TAG, "pipe: no task");
            return -1;
        }
        return p->id;
    }
    espix_klog(ESPIX_KLOG_WARN, TAG, "pipe: all %d in use", ESPIX_AUDIO_PIPES_MAX);
    return -1;
}

esp_err_t espix_audio_pipe_del(int id)
{
    for (int i = 0; i < ESPIX_AUDIO_PIPES_MAX; i++) {
        espix_audio_pipe_t *const p = &s_pipes[i];
        if (p->id != id) {
            continue;
        }
        p->stop = true;
        for (int t = 0; t < 400 && !p->done; t++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        p->id = 0;
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

size_t espix_audio_pipe_list(espix_audio_pipe_info_t *out, size_t max)
{
    size_t n = 0;
    for (int i = 0; i < ESPIX_AUDIO_PIPES_MAX && n < max; i++) {
        if (s_pipes[i].id == 0) {
            continue;
        }
        out[n].id      = s_pipes[i].id;
        out[n].source  = s_pipes[i].src->name;
        out[n].sink    = s_pipes[i].snk->name;
        out[n].running = !s_pipes[i].done;
        n++;
    }
    return n;
}

static void audio_task(void *arg)
{
    bool in_ps = false, out_ps = false;
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
    unsigned src_rate = 0;
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
                        src_rate     = info.sample_rate;
                    }
                    info_logged = true;

                    /*
                     * The stream, at the source's own format. The engine
                     * converts it to the sink's at this stream's edge, or the
                     * sink adopts it, or it is refused with a reason -- which is
                     * the negotiation's business, not this decoder's.
                     */
                    const espix_audio_format_t src_fmt = {
                        src_rate, (uint8_t)src_channels, 16
                    };
                    for (int tries = 0; s_play_stream == NULL; tries++) {
                        s_play_stream = espix_audio_stream_open(&src_fmt);
                        if (s_play_stream != NULL || !s_wait || tries >= 100) {
                            break;
                        }
                        vTaskDelay(pdMS_TO_TICKS(100));  /* --wait: ~10 s */
                    }
                    if (s_play_stream == NULL) {
                        espix_klog(ESPIX_KLOG_WARN, TAG,
                                   "play: no stream (no sink, busy, or a rate "
                                   "nothing can convert)");
                        goto out;
                    }
                }
                /*
                 * Straight to the stream: whatever rate and channel count the
                 * file turns out to be, this is what is written, and the
                 * stream's edge is where it becomes the sink's. The resample
                 * and the mono upmix that used to be here were that same job
                 * done a second time, in the wrong layer.
                 */
                const int64_t f0 = esp_timer_get_time();
                feed(frame.buffer, frame.decoded_size);
                produce += frame.decoded_size;
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
    asrc_close(&s_asrc);
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
    /*
     * Closed by the decoder's own task, which is what lets it drain: everything
     * this play wrote is still in the ring, and the mixer needs to consume it
     * before the sink is suspended. Freeing the stream any earlier throws the
     * tail away.
     */
    if (s_play_stream != NULL) {
        (void)espix_audio_stream_close(s_play_stream);
        s_play_stream = NULL;
    }
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

    /*
     * The stream is opened by audio_task, once the decoder has said what the
     * file's format actually is -- not here, and not at the sink's rate.
     *
     * That is the arrangement Linux settled on, and it keeps conversion in one
     * place: aplay writes at the file's rate and lets ALSA's plug convert,
     * pw-play does the same and lets the graph insert a resampler. The producer
     * streams what it has; the layer below reconciles it with the hardware. The
     * engine's stream edge does that now, so this no longer resamples anything
     * itself, and the upmix that used to live here is gone with it.
     */
    s_wait = wait;

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
        (void)espix_audio_stream_close(s_play_stream);
        s_play_stream = NULL;
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
            (void)espix_audio_stream_close(s_play_stream);
            s_play_stream = NULL;
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
    bool did = false;

    espix_klog(ESPIX_KLOG_INFO, TAG, "stop: streams %d task %p requested %d",
               (int)stream_count(), (void *)s_exit.task, (int)s_stop_requested);
    s_stop_requested = true;

    /*
     * An app's stream is not the decoder, so it does not see s_stop -- the
     * desktop's Stop button did nothing to a tone for exactly that reason.
     * This *asks* the stream to stop rather than closing it: whoever opened it
     * is the one inside stream_write, and freeing it from here would be a
     * use-after-free. The writer's loop ends on the zero that write() then
     * returns, and its owner closes it.
     */
    for (int i = 0; i < STREAM_MAX; i++) {
        if (s_streams[i] != NULL) {
            s_streams[i]->stop = true;
            did = true;
        }
    }
    if (s_exit.task != NULL) {
        s_stop = true;
        did = true;
    }
    return did ? ESP_OK : ESP_ERR_INVALID_STATE;
}

const char *espix_audio_state(void)
{
    if (s_running) {
        return "playing";
    }
    return stream_any_open() ? "streaming" : "idle";
}

#endif /* CONFIG_ESPIX_AUDIO */
