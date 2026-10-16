/*
 * The video player: MJPEG, raw (JPEG frames back to back) or in an AVI with
 * PCM audio.
 *
 * One playback is one context, owned by its tasks: the video task reads and
 * decodes with no lock held, the audio task feeds an espix_audio stream, and
 * frames change hands by a pointer swap under s_vlock. The picture is a window
 * overlay -- composited straight from the decoded frame, and handed to a viewer
 * as the file's own JPEG while nothing covers it -- so the controls are a band
 * that shows on pointer motion and hides, with the cursor, when it stops.
 */

#include <ctype.h>
#include <limits.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "espix_audio.h"
#include "espix_audio_sink.h"
#include "espix_desktop.h"
#include "espix_display.h"
#include "espix_kernel.h"
#include "espix_task.h"
#include "espix_widgets.h"

#define TAG "video"

/* A frame that never ends is not a frame. */
#define VIDEO_MAX_FRAME_BYTES (4 << 20)

/* One read covers several frames: the card answers in latency, not bandwidth. */
#define VIDEO_READ_STEP (64 * 1024)
#define VIDEO_MAX_BYTES  (512u << 20)

/* The desktop's frame and taskbar heights, repeated rather than shared. */
#define VIDEO_TITLE_H   18
#define VIDEO_TASKBAR_H 26

/* The control band, over the bottom of the picture while it is shown. */
#define BAND_H  40
#define BTN_W   72
#define BTN_H   24
#define BTN_GAP 6

#define CTL_HIDE_US   (2 * 1000 * 1000)
#define DBLCLICK_US   (400 * 1000)
#define AV_STEP_US    (20 * 1000)
#define AV_A2DP_US    (250 * 1000)      /* Bluetooth sinks buffer far more than a codec */
#define AV_LOCAL_US   (40 * 1000)
#define AV_SLACK_US   (30 * 1000)       /* audio and wall clocks may differ this much */

/* One AVI index entry, made absolute. */
typedef struct {
    uint32_t off;                       /* the chunk's data, in the file */
    uint32_t size;
    uint8_t  kind;                      /* 'v' video, 'a' audio */
} avi_ent_t;

typedef struct vctx {
    char             path[ESPIX_PATH_MAX];
    bool             avi;
    uint32_t         period_us;         /* one frame */

    /* The video task's alone. */
    FILE            *f;
    uint8_t         *buf;
    size_t           buf_cap;
    size_t           off, len;          /* the frame in hand */
    size_t           base, fill;        /* raw MJPEG's read window */
    size_t           beg;
    size_t           vi;                /* AVI: the index entry of the frame in hand */
    espix_surface_t *back;
    uint8_t         *next_jpg;
    size_t           next_len, next_cap;
    int64_t          t0;                /* the clock's origin */

    /* AVI. */
    avi_ent_t       *idx;
    size_t           nidx;
    espix_audio_format_t afmt;
    bool             has_audio;

    /* The audio task's, and the clock it publishes. */
    TaskHandle_t     atask;
    espix_audio_stream_t *st;
    volatile int64_t a_written_us;      /* PCM handed to the stream since the start */
    volatile bool    a_live;            /* the stream is open and being fed */
    volatile uint32_t a_gen;            /* bumped by Restart */

    /* Shared, under s_vlock. */
    espix_surface_t *front;
    uint8_t         *front_jpg;
    size_t           front_len, front_cap;
    espix_surface_t *fit;
    int              fw, fh;
    int              cur;
    volatile bool    quit;
    volatile bool    audio_gone;        /* the audio task has ended */
    TaskHandle_t     task;
} vctx_t;

static espix_window_t   *s_win;
static char              s_path[ESPIX_PATH_MAX];
static vctx_t           *s_ctx;
static SemaphoreHandle_t s_vlock;

static volatile bool     s_playing;
static volatile bool     s_restart;
static volatile int64_t  s_av_off = AV_LOCAL_US;
static char              s_err[96];

/* Measured, for the band and the log. */
static volatile int      s_fps;
static volatile int      s_drops_shown;
static int64_t           s_decode_us;
static volatile int      s_av_ms = INT32_MIN;  /* audio heard minus the frame shown */
static uint32_t          s_dropped, s_done;
static int64_t           s_stat_at;

/* The controls. */
static bool               s_ctl_on;
static bool               s_ptr_in;     /* the pointer is over the window */
static esp_timer_handle_t s_ctl_timer;
static int64_t            s_click_us;

static void vlock(void)
{
    if (s_vlock == NULL) {
        s_vlock = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_vlock, portMAX_DELAY);
}

static void vunlock(void)
{
    xSemaphoreGive(s_vlock);
}

static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
           | ((uint32_t)p[3] << 24);
}

static uint16_t rd16le(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

/* ------------------------------------------------------------------ */
/* The context                                                         */
/* ------------------------------------------------------------------ */

static void ctx_free(vctx_t *v)
{
    if (v == NULL) {
        return;
    }
    if (v->f != NULL) {
        fclose(v->f);
    }
    heap_caps_free(v->buf);
    heap_caps_free(v->idx);
    heap_caps_free(v->next_jpg);
    heap_caps_free(v->front_jpg);
    espix_surface_free(v->back);
    espix_surface_free(v->front);
    espix_surface_free(v->fit);
    free(v);
}

static bool buf_room(vctx_t *v, size_t n)
{
    if (n <= v->buf_cap) {
        return true;
    }
    if (n > VIDEO_MAX_FRAME_BYTES) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "a frame of %u bytes is too large", (unsigned)n);
        return false;
    }
    uint8_t *nb = heap_caps_realloc(v->buf, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (nb == NULL) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "no memory for a %u byte frame", (unsigned)n);
        return false;
    }
    v->buf     = nb;
    v->buf_cap = n;
    return true;
}

static bool open_file(vctx_t *v)
{
    if (v->f == NULL) {
        v->f = fopen(v->path, "rb");
        if (v->f == NULL) {
            espix_klog(ESPIX_KLOG_WARN, TAG, "%s: %s", v->path, strerror(errno));
            return false;
        }
    }
    return true;
}

static bool read_at(FILE *f, uint32_t off, void *dst, size_t n)
{
    if (fseek(f, (long)off, SEEK_SET) != 0) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "seek to %u: %s", (unsigned)off, strerror(errno));
        return false;
    }
    if (fread(dst, 1, n, f) != n) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "short read at %u", (unsigned)off);
        clearerr(f);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Raw MJPEG: SOI to EOI through a sliding window                      */
/* ------------------------------------------------------------------ */

static bool raw_fill(vctx_t *v)
{
    if (v->fill == v->buf_cap
        && !buf_room(v, v->buf_cap ? v->buf_cap * 2 : VIDEO_READ_STEP * 2)) {
        return false;
    }
    if (fseek(v->f, (long)(v->base + v->fill), SEEK_SET) != 0) {
        return false;
    }
    const size_t got = fread(v->buf + v->fill, 1, v->buf_cap - v->fill, v->f);
    if (got == 0) {
        clearerr(v->f);
        return false;
    }
    v->fill += got;
    return true;
}

static void raw_slide(vctx_t *v, size_t n)
{
    if (n >= v->fill) {
        v->base += v->fill;
        v->fill  = 0;
        return;
    }
    memmove(v->buf, v->buf + n, v->fill - n);
    v->base += n;
    v->fill -= n;
}

static bool raw_frame_at(vctx_t *v, size_t from)
{
    if (!open_file(v)) {
        return false;
    }
    if (from < v->base || from > v->base + v->fill) {
        v->base = from;
        v->fill = 0;
    } else {
        raw_slide(v, from - v->base);
    }

    size_t scan  = 0;
    long   start = -1;

    for (;;) {
        if (start < 0) {
            for (; scan + 1 < v->fill; scan++) {
                if (v->buf[scan] == 0xFF && v->buf[scan + 1] == 0xD8) {
                    start = (long)scan;
                    scan += 2;
                    break;
                }
            }
            if (start > 0) {
                scan -= (size_t)start;
                raw_slide(v, (size_t)start);
                start = 0;
            } else if (start < 0 && v->fill > 1) {
                raw_slide(v, v->fill - 1);
                scan = 0;
            }
        }
        if (start == 0) {
            for (; scan + 1 < v->fill; scan++) {
                if (v->buf[scan] == 0xFF && v->buf[scan + 1] == 0xD9) {
                    v->beg = 0;
                    v->off = v->base;
                    v->len = scan + 2;
                    return true;
                }
            }
        }
        if (!raw_fill(v)) {
            return false;
        }
    }
}

/* ------------------------------------------------------------------ */
/* AVI: RIFF headers and the idx1 index                                */
/* ------------------------------------------------------------------ */

/*
 * Headers and index, read once at open. Only what this player plays: one MJPEG
 * video stream and at most one 16-bit PCM audio stream, indexed by idx1.
 */
static bool avi_parse(vctx_t *v)
{
    uint8_t  h[12];
    uint32_t movi = 0, idx_off = 0, idx_len = 0;
    int      vstream = -1, astream = -1, nstream = 0;

    if (!read_at(v->f, 0, h, 12) || memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "AVI ", 4) != 0) {
        snprintf(s_err, sizeof(s_err), "not an AVI");
        return false;
    }
    const uint32_t riff_end = 8 + rd32le(h + 4);
    uint32_t       off      = 12;

    /* Top level: hdrl, movi, idx1; hdrl's strl lists one level down. */
    while (off + 8 <= riff_end) {
        uint8_t c[12];
        if (!read_at(v->f, off, c, 12)) {
            break;
        }
        const uint32_t sz = rd32le(c + 4);

        if (memcmp(c, "LIST", 4) == 0 && memcmp(c + 8, "hdrl", 4) == 0) {
            uint8_t *hl = malloc(sz);
            if (hl == NULL || !read_at(v->f, off + 12, hl, sz - 4)) {
                free(hl);
                return false;
            }
            for (uint32_t p = 0; p + 8 <= sz - 4; ) {
                const uint32_t csz = rd32le(hl + p + 4);

                if (memcmp(hl + p, "avih", 4) == 0 && csz >= 4) {
                    v->period_us = rd32le(hl + p + 8);
                } else if (memcmp(hl + p, "LIST", 4) == 0 && memcmp(hl + p + 8, "strl", 4) == 0) {
                    const uint8_t *strh = NULL, *strf = NULL;
                    uint32_t       strf_sz = 0;

                    for (uint32_t q = p + 12; q + 8 <= p + 8 + csz; ) {
                        const uint32_t ssz = rd32le(hl + q + 4);
                        if (memcmp(hl + q, "strh", 4) == 0) {
                            strh = hl + q + 8;
                        } else if (memcmp(hl + q, "strf", 4) == 0) {
                            strf = hl + q + 8;
                            strf_sz = ssz;
                        }
                        q += 8 + ssz + (ssz & 1);
                    }
                    if (strh != NULL && memcmp(strh, "vids", 4) == 0 && vstream < 0
                        && (memcmp(strh + 4, "MJPG", 4) == 0 || memcmp(strh + 4, "mjpg", 4) == 0)) {
                        vstream = nstream;
                    } else if (strh != NULL && memcmp(strh, "auds", 4) == 0 && astream < 0
                               && strf != NULL && strf_sz >= 16
                               && rd16le(strf) == 1 && rd16le(strf + 14) == 16) {
                        astream          = nstream;
                        v->afmt.channels = (uint8_t)rd16le(strf + 2);
                        v->afmt.rate     = rd32le(strf + 4);
                        v->afmt.bits     = 16;
                    }
                    nstream++;
                }
                p += 8 + csz + (csz & 1);
            }
            free(hl);
        } else if (memcmp(c, "LIST", 4) == 0 && memcmp(c + 8, "movi", 4) == 0) {
            movi = off + 8;             /* where idx1's offsets count from */
        } else if (memcmp(c, "idx1", 4) == 0) {
            idx_off = off + 8;
            idx_len = sz;
        }
        off += 8 + sz + (sz & 1);
    }

    if (vstream < 0 || movi == 0 || idx_len < 16 || v->period_us == 0) {
        snprintf(s_err, sizeof(s_err), "AVI: no MJPEG stream, or no index");
        return false;
    }

    const size_t n   = idx_len / 16;
    uint8_t     *raw = heap_caps_malloc(idx_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    v->idx = heap_caps_malloc(n * sizeof(avi_ent_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (raw == NULL || v->idx == NULL || !read_at(v->f, idx_off, raw, idx_len)) {
        heap_caps_free(raw);
        return false;
    }

    /* Chunk ids start with the stream number as two digits. */
    const char vid[2] = { (char)('0' + vstream / 10 % 10), (char)('0' + vstream % 10) };
    const char aud[2] = { (char)('0' + (astream < 0 ? 0 : astream) / 10 % 10),
                          (char)('0' + (astream < 0 ? 0 : astream) % 10) };

    /* idx1 offsets are relative to 'movi', except in files that made them absolute. */
    const uint32_t first = rd32le(raw + 8);
    const uint32_t rel   = (first < movi) ? movi : 0;

    for (size_t i = 0; i < n; i++) {
        const uint8_t *e    = raw + i * 16;
        const uint32_t size = rd32le(e + 12);
        uint8_t        kind = 0;

        if (memcmp(e, vid, 2) == 0 && (e[2] == 'd') && size > 0) {
            kind = 'v';
        } else if (astream >= 0 && memcmp(e, aud, 2) == 0 && e[2] == 'w' && size > 0) {
            kind = 'a';
        }
        if (kind != 0) {
            v->idx[v->nidx++] = (avi_ent_t){ rd32le(e + 8) + rel + 8, size, kind };
        }
    }
    heap_caps_free(raw);

    v->has_audio = (astream >= 0);
    return true;
}

/* The next index entry of `kind` at or after `from`, or nidx. */
static size_t avi_find(const vctx_t *v, size_t from, uint8_t kind)
{
    while (from < v->nidx && v->idx[from].kind != kind) {
        from++;
    }
    return from;
}

static bool avi_frame_at(vctx_t *v, size_t i)
{
    i = avi_find(v, i, 'v');
    if (i >= v->nidx || !open_file(v) || !buf_room(v, v->idx[i].size)) {
        return false;
    }
    if (!read_at(v->f, v->idx[i].off, v->buf, v->idx[i].size)) {
        return false;
    }
    v->vi  = i;
    v->beg = 0;
    v->len = v->idx[i].size;
    return true;
}

/* ------------------------------------------------------------------ */
/* Frames, whichever the source                                        */
/* ------------------------------------------------------------------ */

static bool src_first(vctx_t *v)
{
    return v->avi ? avi_frame_at(v, 0) : raw_frame_at(v, 0);
}

static bool src_next(vctx_t *v)
{
    return v->avi ? avi_frame_at(v, v->vi + 1) : raw_frame_at(v, v->off + v->len);
}

/* Whether a viewer's decoder takes the frame alone: sequential, Huffman tables present. */
static bool jpeg_standalone(const uint8_t *p, size_t n)
{
    bool   sof = false, dht = false;
    size_t i   = 2;

    while (i + 4 <= n && p[i] == 0xFF) {
        const uint8_t m = p[i + 1];

        if (m == 0xFF) {
            i++;
            continue;
        }
        if (m == 0xDA) {
            return sof && dht;
        }
        if (m == 0xC0 || m == 0xC1) {
            sof = true;
        } else if (m >= 0xC2 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            return false;
        } else if (m == 0xC4) {
            dht = true;
        }
        i += 2 + (((size_t)p[i + 2] << 8) | p[i + 3]);
    }
    return false;
}

static void keep_jpeg(vctx_t *v)
{
    const uint8_t *p = v->buf + v->beg;

    v->next_len = 0;
    if (!jpeg_standalone(p, v->len)) {
        return;
    }
    if (v->next_cap < v->len) {
        uint8_t *nb = heap_caps_realloc(v->next_jpg, v->len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (nb == NULL) {
            return;
        }
        v->next_jpg = nb;
        v->next_cap = v->len;
    }
    memcpy(v->next_jpg, p, v->len);
    v->next_len = v->len;
}

static void swap_jpeg(vctx_t *v)
{
    uint8_t     *b = v->front_jpg;
    const size_t c = v->front_cap;

    v->front_jpg = v->next_jpg;
    v->front_len = v->next_len;
    v->front_cap = v->next_cap;
    v->next_jpg  = b;
    v->next_cap  = c;
    v->next_len  = 0;
}

/* Width and height from the frame's SOF; false when it has none. */
static bool jpeg_dims(const uint8_t *p, size_t n, int *w, int *h)
{
    size_t i = 2;

    while (i + 9 < n && p[i] == 0xFF) {
        const uint8_t m = p[i + 1];

        if (m == 0xFF) {
            i++;
            continue;
        }
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            *h = (p[i + 5] << 8) | p[i + 6];
            *w = (p[i + 7] << 8) | p[i + 8];
            return *w > 0 && *h > 0;
        }
        if (m == 0xDA) {
            return false;
        }
        i += 2 + (((size_t)p[i + 2] << 8) | p[i + 3]);
    }
    return false;
}

/* A surface the frame decodes straight into: no codec-side copy of a whole frame. */
static espix_surface_t *frame_surface(const uint8_t *p, size_t n)
{
    int w = 0, h = 0;

    if (!jpeg_dims(p, n, &w, &h)) {
        return NULL;
    }
    espix_surface_t *s = espix_surface_new(w, h);
    if (s != NULL && espix_image_jpeg_into(s, p, n) != ESP_OK) {
        espix_surface_free(s);
        s = NULL;
    }
    return s;
}

/* Decode the frame in hand into the back buffer, making one if there is room. */
static bool decode(vctx_t *v)
{
    const uint8_t *p = v->buf + v->beg;

    if (v->back == NULL) {
        v->back = frame_surface(p, v->len);
        return v->back != NULL;
    }
    return espix_image_jpeg_into(v->back, p, v->len) == ESP_OK;
}

/* A raw file's rate from its name ("...-12fps.mjpg"), else 24. */
static uint32_t name_period(const char *path)
{
    const char *p = strstr(path, "fps");

    if (p != NULL) {
        const char *q = p;
        while (q > path && isdigit((unsigned char)q[-1])) {
            q--;
        }
        const int fps = atoi(q);
        if (q < p && fps >= 1 && fps <= 60) {
            return 1000000u / (uint32_t)fps;
        }
    }
    return 1000000u / 24;
}

static bool ends_with(const char *s, const char *ext)
{
    const size_t n = strlen(s), m = strlen(ext);
    return n >= m && strcasecmp(s + n - m, ext) == 0;
}

/* Open a file and decode its first frame. */
static vctx_t *video_load(const char *path)
{
    vctx_t *v = calloc(1, sizeof(*v));
    if (v == NULL) {
        snprintf(s_err, sizeof(s_err), "no memory");
        return NULL;
    }
    strlcpy(v->path, path, sizeof(v->path));
    v->avi = ends_with(path, ".avi");

    if (!open_file(v)) {
        snprintf(s_err, sizeof(s_err), "%.32s: %.48s", path, strerror(errno));
        free(v);
        return NULL;
    }
    fseek(v->f, 0, SEEK_END);
    const long n = ftell(v->f);
    if (n <= 8 || (size_t)n > VIDEO_MAX_BYTES) {
        snprintf(s_err, sizeof(s_err), "%.32s: %ld bytes is out of range", path, n);
        ctx_free(v);
        return NULL;
    }

    if (v->avi) {
        if (!avi_parse(v)) {
            ctx_free(v);
            return NULL;
        }
    } else {
        v->period_us = name_period(path);
    }

    if (!src_first(v) || !decode(v)) {
        snprintf(s_err, sizeof(s_err), "%.32s: the first frame will not decode", path);
        ctx_free(v);
        return NULL;
    }

    /* A stream belongs to the task that reads it. */
    fclose(v->f);
    v->f = NULL;

    keep_jpeg(v);
    swap_jpeg(v);
    v->front = v->back;
    v->back  = NULL;
    v->fw    = espix_surface_width(v->front);
    v->fh    = espix_surface_height(v->front);

    espix_klog(ESPIX_KLOG_INFO, TAG, "%s: %dx%d at %u fps%s", path, v->fw, v->fh,
               (unsigned)(1000000u / v->period_us),
               v->has_audio ? ", PCM audio" : "");
    return v;
}

/* ------------------------------------------------------------------ */
/* Audio                                                               */
/* ------------------------------------------------------------------ */

static int64_t pcm_us(const espix_audio_format_t *f, size_t bytes)
{
    const uint32_t frame = (uint32_t)f->channels * 2;
    return (frame && f->rate) ? (int64_t)bytes * 1000000 / ((int64_t)f->rate * frame) : 0;
}

/* Feeds the AVI's PCM to a stream; writes block while the ring is full. */
static void audio_task(void *arg)
{
    vctx_t  *const v   = arg;
    FILE          *f   = fopen(v->path, "rb");
    uint8_t       *buf = heap_caps_malloc(16 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t         ai  = 0;
    uint32_t       gen = v->a_gen;

    if (f == NULL || buf == NULL) {
        goto out;
    }

    while (!v->quit) {
        if (gen != v->a_gen) {
            gen = v->a_gen;
            if (v->st != NULL) {
                v->a_live = false;
                (void)espix_audio_stream_close(v->st);
                v->st = NULL;
            }
            ai              = 0;
            v->a_written_us = 0;
        }
        if (!s_playing) {
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
            continue;
        }
        if (v->st == NULL) {
            v->st = espix_audio_stream_open(&v->afmt);
            if (v->st == NULL) {
                espix_klog(ESPIX_KLOG_WARN, TAG, "no audio sink; playing silent");
                break;
            }
            v->a_live = true;
        }

        ai = avi_find(v, ai, 'a');
        if (ai >= v->nidx) {
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));   /* the end: wait for Restart */
            continue;
        }
        const avi_ent_t *e = &v->idx[ai];
        const size_t     n = e->size < 16 * 1024 ? e->size : 16 * 1024;

        if (!read_at(f, e->off, buf, n)) {
            break;
        }
        size_t done = 0;
        while (done < n && !v->quit && gen == v->a_gen) {
            done += espix_audio_stream_write(v->st, buf + done, n - done);
        }
        v->a_written_us += pcm_us(&v->afmt, done);
        ai++;
    }

out:
    v->a_live = false;
    if (v->st != NULL) {
        (void)espix_audio_stream_close(v->st);
        v->st = NULL;
    }
    if (f != NULL) {
        fclose(f);
    }
    heap_caps_free(buf);
    v->audio_gone = true;
}

/* Where playback is, in microseconds: wall time, pulled onto the audio's position. */
static int64_t clock_us(vctx_t *v)
{
    int64_t t = esp_timer_get_time() - v->t0;

    if (v->a_live && v->st != NULL && v->a_written_us > 0) {
        const int64_t heard = v->a_written_us - espix_audio_stream_queued_us(v->st) - s_av_off;
        const int64_t diff  = heard - t;

        if (heard > 0 && (diff > AV_SLACK_US || diff < -AV_SLACK_US)) {
            v->t0 -= diff;
            t      = heard;
        }
    }
    return t;
}

/* ------------------------------------------------------------------ */
/* Geometry and drawing                                                */
/* ------------------------------------------------------------------ */

static espix_surface_t *video_src(const vctx_t *v)
{
    if (v == NULL) {
        return NULL;
    }
    return (v->fit != NULL) ? v->fit : v->front;
}

/* The picture's place in the window, centred in the content. Under s_vlock. */
static espix_rect_t video_pic(const espix_window_t *w, const vctx_t *v)
{
    espix_surface_t   *src = video_src(v);
    const espix_rect_t c   = espix_window_content(w);

    if (src == NULL) {
        return (espix_rect_t){ 0, 0, 0, 0 };
    }
    const int sw = espix_surface_width(src);
    const int sh = espix_surface_height(src);

    return (espix_rect_t){ c.x + (c.w - sw) / 2, c.y + (c.h - sh) / 2, sw, sh };
}

/* The controls: the bottom of the content. */
static espix_rect_t band_rect(const espix_window_t *w)
{
    const espix_rect_t c = espix_window_content(w);
    return (espix_rect_t){ c.x, c.y + c.h - BAND_H, c.w, BAND_H };
}

/* 0 Play/Pause, 1 Restart, from the left. */
static espix_rect_t video_btn(const espix_window_t *w, int i)
{
    const espix_rect_t b = band_rect(w);
    return (espix_rect_t){ b.x + 8 + i * (BTN_W + BTN_GAP), b.y + (BAND_H - BTN_H) / 2,
                           BTN_W, BTN_H };
}

/* Fit the picture into the content, down only; under s_vlock. */
static void video_fit_locked(vctx_t *v, espix_window_t *w)
{
    espix_surface_free(v->fit);
    v->fit = NULL;

    if (v->front == NULL || w == NULL) {
        return;
    }
    const espix_rect_t c = espix_window_content(w);
    if (v->fw <= c.w && v->fh <= c.h) {
        return;
    }
    int fw = c.w;
    int fh = (int)((int64_t)v->fh * c.w / v->fw);

    if (fh > c.h) {
        fh = c.h;
        fw = (int)((int64_t)v->fw * c.h / v->fh);
    }
    v->fit = espix_surface_new(fw > 0 ? fw : 1, fh > 0 ? fh : 1);
    if (v->fit != NULL) {
        (void)espix_surface_scale(v->fit, v->front);
    }
}

/* The compositor's call, under the canvas lock: the front frame, straight in. */
static void video_overlay(espix_window_t *w, espix_canvas_t *c, int x, int y,
                          espix_rect_t clip, void *ctx)
{
    (void)w;
    (void)ctx;

    vlock();
    espix_surface_t *src = video_src(s_ctx);
    if (src != NULL) {
        espix_canvas_blit_surface_rect(c, x, y, src,
                                       (espix_rect_t){ clip.x - x, clip.y - y, clip.w, clip.h });
    }
    vunlock();
}

/* The front frame's own JPEG, when it is shown unscaled; under the canvas lock. */
static size_t video_overlay_jpeg(espix_window_t *w, void *ctx, uint8_t *out, size_t cap)
{
    (void)w;
    (void)ctx;

    size_t n = 0;

    vlock();
    const vctx_t *v = s_ctx;
    if (v != NULL && v->fit == NULL && v->front_len > 0) {
        if (out == NULL) {
            n = v->front_len;           /* the size only */
        } else if (v->front_len <= cap) {
            memcpy(out, v->front_jpg, v->front_len);
            n = v->front_len;
        }
    }
    vunlock();
    return n;
}

/* Where the controls cover the picture, while they are shown. */
static espix_rect_t ctl_over(espix_window_t *w)
{
    if (!s_ctl_on) {
        return (espix_rect_t){ 0, 0, 0, 0 };
    }
    vlock();
    const espix_rect_t pic = video_pic(w, s_ctx);
    vunlock();

    const espix_rect_t b  = band_rect(w);
    const int          y0 = b.y > pic.y ? b.y : pic.y;
    const int          y1 = (b.y + b.h) < (pic.y + pic.h) ? (b.y + b.h) : (pic.y + pic.h);
    const int          x0 = b.x > pic.x ? b.x : pic.x;
    const int          x1 = (b.x + b.w) < (pic.x + pic.w) ? (b.x + b.w) : (pic.x + pic.w);

    if (x1 <= x0 || y1 <= y0) {
        return (espix_rect_t){ 0, 0, 0, 0 };   /* the band is in the letterbox */
    }
    return (espix_rect_t){ x0, y0, x1 - x0, y1 - y0 };
}

/* Tell the compositor where the picture and the controls are. Desktop lock held. */
static void video_place(espix_window_t *w)
{
    vlock();
    const espix_rect_t pic = video_pic(w, s_ctx);
    vunlock();

    espix_window_set_overlay(w, pic, pic.w > 0 ? video_overlay : NULL);
    espix_window_set_overlay_jpeg(w, video_overlay_jpeg);
    espix_window_set_overlay_over(w, ctl_over(w));
}

static void video_draw(espix_window_t *w, espix_surface_t *s, espix_rect_t r, void *ctx)
{
    (void)ctx;
    (void)r;

    const espix_rect_t c = espix_window_content(w);

    vlock();
    const bool have = video_src(s_ctx) != NULL;
    const int  cur  = (s_ctx == NULL) ? 0 : s_ctx->cur;
    const bool aud  = (s_ctx != NULL) && s_ctx->has_audio;
    const bool live = (s_ctx != NULL) && s_ctx->a_live;
    vunlock();

    espix_surface_fill(s, c, 0x0000);
    if (!have) {
        espix_surface_text(s, c.x, c.y, s_err[0] != '\0' ? s_err : "no video", WGT_FG, WGT_BG);
        return;
    }
    if (!s_ctl_on) {
        return;
    }

    const espix_rect_t b = band_rect(w);
    char               line[96];

    espix_surface_fill(s, b, WGT_PANE);
    espix_wgt_button(s, video_btn(w, 0), s_playing ? "Pause" : "Play", true, false);
    espix_wgt_button(s, video_btn(w, 1), "Restart", true, false);

    if (aud) {
        snprintf(line, sizeof(line), "%d fps  %d dropped  frame %d  A/V %+d ms%s",
                 s_fps, s_drops_shown, cur, (int)(s_av_off / 1000),
                 live ? "" : "  (no audio)");
    } else {
        snprintf(line, sizeof(line), "%d fps  %d dropped  frame %d", s_fps, s_drops_shown, cur);
    }
    const int tx = video_btn(w, 1).x + BTN_W + 12;
    espix_wgt_label(s, (espix_rect_t){ tx, b.y + (BAND_H - WGT_ROW_H) / 2,
                                       b.x + b.w - tx - 8, WGT_ROW_H }, line, true);
}

/* ------------------------------------------------------------------ */
/* The controls                                                        */
/* ------------------------------------------------------------------ */

static void ctl_hide_desk(void *arg);

static void ctl_timer_fire(void *arg)
{
    (void)arg;
    (void)espix_desktop_post(ctl_hide_desk, NULL);
}

static void ctl_arm(void)
{
    if (s_ctl_timer == NULL) {
        const esp_timer_create_args_t a = { .callback = ctl_timer_fire, .name = "video-ctl" };
        if (esp_timer_create(&a, &s_ctl_timer) != ESP_OK) {
            s_ctl_timer = NULL;
            return;
        }
    }
    (void)esp_timer_stop(s_ctl_timer);
    (void)esp_timer_start_once(s_ctl_timer, CTL_HIDE_US);
}

/* Show the controls (and restart their timeout). Desktop task. */
static void ctl_show(void)
{
    if (s_win == NULL) {
        return;
    }
    ctl_arm();
    if (!s_ctl_on) {
        s_ctl_on = true;
        espix_window_set_overlay_over(s_win, ctl_over(s_win));
        espix_window_damage(s_win, band_rect(s_win));
    }
}

static void ctl_hide(bool conceal)
{
    if (s_win == NULL) {
        return;
    }
    if (s_ctl_on) {
        s_ctl_on = false;
        espix_window_set_overlay_over(s_win, (espix_rect_t){ 0, 0, 0, 0 });
        espix_window_damage(s_win, band_rect(s_win));
    }
    if (conceal) {
        espix_desktop_cursor_conceal();     /* a still cursor would block pass-through */
    }
}

static void ctl_hide_desk(void *arg)
{
    (void)arg;
    ctl_hide(s_ptr_in);
}

static void video_hover(espix_window_t *w, int x, int y, void *ctx)
{
    (void)w;
    (void)ctx;

    s_ptr_in = (x >= 0);
    if (s_ptr_in) {
        ctl_show();
    } else {
        if (s_ctl_timer != NULL) {
            (void)esp_timer_stop(s_ctl_timer);
        }
        ctl_hide(false);
    }
}

static void video_kick(void)
{
    vlock();
    if (s_ctx != NULL) {
        if (s_ctx->task != NULL) {
            xTaskNotifyGive(s_ctx->task);
        }
        if (s_ctx->atask != NULL && !s_ctx->audio_gone) {
            xTaskNotifyGive(s_ctx->atask);
        }
    }
    vunlock();
}

static void video_restart(void)
{
    s_restart = true;
    s_playing = true;
    video_kick();
}

static void video_pointer(espix_window_t *w, int x, int y, uint8_t buttons, void *ctx)
{
    (void)ctx;

    if (buttons == 0) {
        return;
    }
    ctl_show();
    if (espix_wgt_hit(video_btn(w, 0), x, y)) {
        s_playing = !s_playing;
    } else if (espix_wgt_hit(video_btn(w, 1), x, y)) {
        video_restart();
    } else {
        const int64_t now = esp_timer_get_time();
        if (now - s_click_us < DBLCLICK_US) {
            espix_window_set_fullscreen(w, !espix_window_fullscreen(w));
            s_click_us = 0;
        } else {
            s_click_us = now;
        }
        return;
    }
    video_kick();
    espix_window_damage(w, band_rect(w));
}

static void video_key(espix_window_t *w, uint32_t keysym, bool down, void *ctx)
{
    (void)ctx;

    if (!down) {
        return;
    }
    switch (keysym) {
    case 0x20:                          /* space */
        s_playing = !s_playing;
        video_kick();
        break;
    case 'r':
        video_restart();
        break;
    case 'f':
        espix_window_set_fullscreen(w, !espix_window_fullscreen(w));
        break;
    case 0xFF1B:                        /* Escape */
        espix_window_set_fullscreen(w, false);
        break;
    case '[':
        s_av_off -= AV_STEP_US;
        break;
    case ']':
        s_av_off += AV_STEP_US;
        break;
    default:
        return;
    }
    ctl_show();
    espix_window_damage(w, band_rect(w));
}

static void video_resize(espix_window_t *w, espix_rect_t content, void *ctx)
{
    (void)content;
    (void)ctx;

    vlock();
    if (s_ctx != NULL) {
        video_fit_locked(s_ctx, w);
    }
    vunlock();
    video_place(w);
}

/* ------------------------------------------------------------------ */
/* Playback                                                            */
/* ------------------------------------------------------------------ */

static void video_band_desk(void *arg)
{
    (void)arg;
    if (s_win != NULL && s_ctl_on) {
        espix_window_damage(s_win, band_rect(s_win));
    }
}

static void video_stat(int cur)
{
    const int64_t now = esp_timer_get_time();

    if (s_stat_at == 0) {
        s_stat_at = now;
        return;
    }
    const int64_t elapsed = now - s_stat_at;
    if (elapsed < 1000000) {
        return;
    }
    const uint32_t n = s_done ? s_done : 1;

    s_fps         = (int)((int64_t)s_done * 1000000 / elapsed);
    s_drops_shown = (int)s_dropped;
    if (s_av_ms != INT32_MIN) {
        espix_klog(ESPIX_KLOG_INFO, TAG,
                   "frame %d at %d fps, %u dropped: decode %d ms, audio %+d ms from video",
                   cur, s_fps, (unsigned)s_dropped, (int)(s_decode_us / n / 1000), s_av_ms);
    } else {
        espix_klog(ESPIX_KLOG_INFO, TAG, "frame %d at %d fps, %u dropped: decode %d ms",
                   cur, s_fps, (unsigned)s_dropped, (int)(s_decode_us / n / 1000));
    }

    s_dropped   = 0;
    s_decode_us = 0;
    s_done      = 0;
    s_stat_at   = now;
    (void)espix_desktop_post(video_band_desk, NULL);
}

/* A new frame: the picture only. The window may be gone by the time it runs. */
static void video_repaint_desk(void *arg)
{
    (void)arg;

    if (s_win == NULL) {
        return;
    }
    vlock();
    const espix_rect_t pic = video_pic(s_win, s_ctx);
    vunlock();

    if (pic.w > 0) {
        espix_window_damage(s_win, pic);
    } else {
        espix_window_repaint(s_win);
    }
}

static void video_task(void *arg)
{
    vctx_t *const v     = arg;
    int           next  = 1;            /* the index of the frame in hand */
    bool          have  = src_next(v);
    int64_t       pause = 0;
    int64_t       dec_avg = 0;          /* smoothed decode time */

    v->t0 = esp_timer_get_time();

    while (!v->quit) {
        if (s_restart) {
            s_restart = false;
            v->a_gen++;
            have = src_first(v);
            next = 0;
            v->t0 = esp_timer_get_time();
            if (v->atask != NULL) {
                xTaskNotifyGive(v->atask);
            }
        }
        if (!s_playing) {
            if (pause == 0) {
                pause = esp_timer_get_time();
            }
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
            continue;
        }
        if (pause != 0) {
            v->t0 += esp_timer_get_time() - pause;      /* the clock stood still */
            pause  = 0;
        }
        if (!have) {
            video_restart();                            /* the end: play it again */
            continue;
        }

        /* Start early by what a decode takes, so the frame lands when it is due. */
        const int64_t pts = (int64_t)next * v->period_us - dec_avg;
        int64_t       now = clock_us(v);

        if (now < pts - 2000) {
            const int64_t wait = pts - now;
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(((wait < 50000 ? wait : 50000) + 999) / 1000));
            continue;
        }
        /* Behind by more than a frame: skip to the one that is due. */
        while (now + dec_avg - (int64_t)next * v->period_us >= v->period_us
               && (have = src_next(v))) {
            next++;
            s_dropped++;
        }
        if (!have) {
            continue;
        }

        const int64_t t0 = esp_timer_get_time();
        keep_jpeg(v);

        /* No room for a second frame: decode in place, under the lock. */
        const bool fresh   = (v->back == NULL);
        const bool decoded = decode(v);
        const bool single  = fresh && !decoded;

        if (single) {
            vlock();
            const bool ok = espix_image_jpeg_into(v->front, v->buf + v->beg, v->len) == ESP_OK;
            if (ok) {
                swap_jpeg(v);
            }
            vunlock();
            if (!ok) {
                espix_klog(ESPIX_KLOG_WARN, TAG, "frame %d (%u bytes) will not decode",
                           next, (unsigned)v->len);
                have = src_next(v);
                next++;
                continue;
            }
        } else if (!decoded) {
            espix_klog(ESPIX_KLOG_WARN, TAG, "frame %d (%u bytes) will not decode",
                       next, (unsigned)v->len);
            have = src_next(v);
            next++;
            continue;
        }

        vlock();
        if (!single) {
            espix_surface_t *shown = v->front;
            v->front = v->back;
            v->back  = shown;
            swap_jpeg(v);
        }
        if (v->fit != NULL) {
            (void)espix_surface_scale(v->fit, v->front);
        }
        v->cur = next;
        vunlock();

        if (v->a_live && v->st != NULL && v->a_written_us > 0) {
            const int64_t heard = v->a_written_us - espix_audio_stream_queued_us(v->st) - s_av_off;
            s_av_ms = (int)((heard - (int64_t)next * v->period_us) / 1000);
        } else {
            s_av_ms = INT32_MIN;
        }

        (void)espix_desktop_post(video_repaint_desk, NULL);
        const int64_t took = esp_timer_get_time() - t0;
        dec_avg      = (dec_avg * 7 + took) / 8;
        s_decode_us += took;
        s_done++;
        video_stat(next);

        have = src_next(v);
        next++;
    }

    /* The audio task frees nothing of ours, but reads the index: wait for it. */
    while (v->atask != NULL && !v->audio_gone) {
        xTaskNotifyGive(v->atask);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ctx_free(v);
}

/* ------------------------------------------------------------------ */
/* The window                                                          */
/* ------------------------------------------------------------------ */

/* Retire the live context; its task frees it. Desktop lock held. */
static void video_retire(void)
{
    vlock();
    vctx_t      *v    = s_ctx;
    TaskHandle_t task = (v != NULL) ? v->task : NULL;

    s_ctx = NULL;
    if (v != NULL) {
        v->quit = true;
        if (task != NULL) {
            xTaskNotifyGive(task);
        }
        if (v->atask != NULL && !v->audio_gone) {
            xTaskNotifyGive(v->atask);
        }
    }
    vunlock();

    if (v != NULL && task == NULL) {
        ctx_free(v);
    }
}

static bool video_start(const char *path)
{
    s_err[0]  = '\0';
    s_playing = false;
    s_restart = false;

    if (path == NULL || path[0] == '\0') {
        snprintf(s_err, sizeof(s_err), "no file");
        return false;
    }
    strlcpy(s_path, path, sizeof(s_path));

    vctx_t *v = video_load(path);
    if (v == NULL) {
        return false;
    }

    const espix_audio_sink_ops_t *sink = espix_audio_sink_default();
    s_av_off = (sink != NULL && strcmp(sink->name, "a2dp") == 0) ? AV_A2DP_US : AV_LOCAL_US;

    s_stat_at   = 0;
    s_done      = 0;
    s_decode_us = 0;
    s_dropped   = 0;
    s_fps       = 0;
    s_playing   = true;

    if (v->has_audio
        && espix_kthread_start(&(const espix_kthread_t){
               .name = "espix:vaudio", .fn = audio_task, .arg = v, .stack = 4096,
               .priority = 5, .core = tskNO_AFFINITY }, &v->atask) != ESP_OK) {
        v->atask      = NULL;
        v->audio_gone = true;
    }

    vlock();
    s_ctx = v;
    vunlock();

    TaskHandle_t task = NULL;
    if (espix_kthread_start(&(const espix_kthread_t){
            .name = "espix:video", .fn = video_task, .arg = v, .stack = 6144,
            .priority = 3, .core = tskNO_AFFINITY }, &task) != ESP_OK || task == NULL) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "no task for the player");
        s_playing = false;
        return true;
    }
    vlock();
    v->task = task;
    vunlock();
    return true;
}

espix_window_t *espix_video_open(const char *path)
{
    if (s_win != NULL) {
        if (path != NULL && path[0] != '\0' && strcmp(path, s_path) != 0) {
            video_retire();
            if (video_start(path)) {
                vlock();
                if (s_ctx != NULL) {
                    video_fit_locked(s_ctx, s_win);
                }
                vunlock();
            }
            video_place(s_win);
        }
        espix_window_repaint(s_win);
        return s_win;
    }

    (void)video_start(path);

    vlock();
    const int pw = (s_ctx != NULL) ? s_ctx->fw : 320;
    const int ph = (s_ctx != NULL) ? s_ctx->fh : 120;
    vunlock();

    espix_canvas_t *c  = espix_display_canvas();
    const int       cw = (c != NULL) ? espix_canvas_width(c) : pw;
    const int       ch = (c != NULL) ? espix_canvas_height(c) : ph;

    int  ww = pw + 2 * 4;
    int  wh = ph + VIDEO_TITLE_H + 2 * 4;
    /* Too big for a window but not for the screen: fullscreen, so it is not scaled. */
    const bool fs = (ww > cw || wh > ch - VIDEO_TASKBAR_H) && pw <= cw && ph <= ch;

    if (ww > cw) { ww = cw; }
    if (wh > ch - VIDEO_TASKBAR_H) { wh = ch - VIDEO_TASKBAR_H; }
    if (ww < 240) { ww = 240; }
    if (wh < 140) { wh = 140; }

    s_win = espix_window_new(40, 40, ww, wh, "video");
    if (s_win == NULL) {
        video_retire();
        return NULL;
    }
    espix_window_set_draw(s_win, video_draw);
    espix_window_set_pointer(s_win, video_pointer);
    espix_window_set_key(s_win, video_key);
    espix_window_set_resize(s_win, video_resize);
    espix_window_set_hover(s_win, video_hover);

    vlock();
    if (s_ctx != NULL) {
        video_fit_locked(s_ctx, s_win);
    }
    vunlock();
    video_place(s_win);
    if (fs) {
        espix_window_set_fullscreen(s_win, true);
    }
    espix_window_repaint(s_win);
    return s_win;
}

void espix_video_forget(void)
{
    s_win     = NULL;
    s_playing = false;
    s_ctl_on  = false;
    s_ptr_in  = false;
    s_path[0] = '\0';
    if (s_ctl_timer != NULL) {
        (void)esp_timer_stop(s_ctl_timer);
    }
    video_retire();
}

espix_window_t *espix_video_window(void)
{
    return s_win;
}
