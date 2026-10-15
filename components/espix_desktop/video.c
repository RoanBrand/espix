/*
 * The video player: an MJPEG file, one JPEG frame after another.
 *
 * MJPEG is not a container and not a codec of its own -- it is JPEG frames
 * written back to back, which is the one shape of video every target here can
 * decode. The S31 and the P4 decode each frame in the JPEG codec; the S3, which
 * has no codec at all, falls back to TJpgDec in the ROM for every frame, and
 * runs accordingly. Nothing in this file decodes anything: reading the frames
 * apart is the whole of it, and espix_image_jpeg_into() does the rest.
 *
 * One playback is one context, owned by its task: the file, the read window and
 * the back buffer are the task's alone, and it reads and decodes without any
 * lock. Only the front frame is shared -- the desktop paints it -- and it
 * changes hands by a pointer swap under s_vlock. Closing the window marks the
 * context finished and the task frees it on its way out, so a close never
 * waits for a read.
 *
 * The window is the video's own size, and the controls are a band drawn over
 * the bottom of it -- so the picture is never scaled to make room for them, and
 * a frame that fits is a blit and nothing else.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"

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
#define VIDEO_MAX_BYTES  (64u << 20)

#define VIDEO_RATE_MIN   1
#define VIDEO_RATE_MAX   30
#define VIDEO_RATE_STEP  4

/* The desktop's frame and taskbar heights, repeated rather than shared. */
#define VIDEO_TITLE_H   18
#define VIDEO_TASKBAR_H 26

/* The control band, over the bottom of the picture. */
#define BAND_H  48
#define BTN_W   60
#define BTN_H   24
#define BTN_GAP 6

typedef struct vctx {
    /* The task's alone. */
    char             path[ESPIX_PATH_MAX];
    FILE            *f;                 /* opened by whichever task reads it */
    uint8_t         *buf;
    size_t           buf_cap;
    size_t           file_len;
    size_t           off, len;          /* the frame in hand */
    size_t           base, fill;        /* the read window: file offset, bytes */
    size_t           beg;               /* the frame's start within it */
    espix_surface_t *back;              /* decoded, not yet shown */

    /* Shared, under s_vlock. */
    espix_surface_t *front;             /* what the window shows */
    espix_surface_t *fit;               /* front scaled to the window, or NULL */
    int              fw, fh;
    int              cur;
    bool             quit;

    TaskHandle_t     task;
} vctx_t;

static espix_window_t   *s_win;
static char              s_path[ESPIX_PATH_MAX];
static vctx_t           *s_ctx;         /* the live playback, under s_vlock */
static SemaphoreHandle_t s_vlock;

static volatile bool     s_playing;
static volatile bool     s_rewind;

/* Raw MJPEG carries no timing; 24 fps is the default the buttons override. */
static volatile int      s_rate = 24;
static char              s_err[96];

/* Measured, for the band and for the log. */
static volatile int      s_frame_ms;
static volatile int      s_fps;
static int64_t           s_decode_us, s_paint_us;
static uint32_t          s_done;
static int64_t           s_stat_at;

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

/* ------------------------------------------------------------------ */
/* The file                                                            */
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
    espix_surface_free(v->back);
    espix_surface_free(v->front);
    espix_surface_free(v->fit);
    free(v);
}

/* Read more of the file onto the end of the window; false at the end or on error. */
static bool video_fill(vctx_t *v)
{
    if (v->fill == v->buf_cap) {
        const size_t ncap = v->buf_cap ? v->buf_cap * 2 : VIDEO_READ_STEP * 2;

        if (ncap > VIDEO_MAX_FRAME_BYTES) {
            espix_klog(ESPIX_KLOG_WARN, TAG, "a frame at %u is over %u bytes",
                       (unsigned)v->base, (unsigned)VIDEO_MAX_FRAME_BYTES);
            return false;
        }
        uint8_t *nb = heap_caps_realloc(v->buf, ncap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (nb == NULL) {
            espix_klog(ESPIX_KLOG_WARN, TAG, "no memory for a %u byte window",
                       (unsigned)ncap);
            return false;
        }
        v->buf     = nb;
        v->buf_cap = ncap;
    }

    if (fseek(v->f, (long)(v->base + v->fill), SEEK_SET) != 0) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "seek to %u: %s",
                   (unsigned)(v->base + v->fill), strerror(errno));
        return false;
    }
    const size_t got = fread(v->buf + v->fill, 1, v->buf_cap - v->fill, v->f);
    if (got == 0) {
        if (ferror(v->f)) {
            espix_klog(ESPIX_KLOG_WARN, TAG, "read at %u: %s",
                       (unsigned)(v->base + v->fill), strerror(errno));
            clearerr(v->f);
        }
        return false;
    }
    v->fill += got;
    return true;
}

/* Drop the window's first `n` bytes, so it starts further into the file. */
static void video_slide(vctx_t *v, size_t n)
{
    if (n == 0) {
        return;
    }
    if (n >= v->fill) {
        v->base += v->fill;
        v->fill  = 0;
        return;
    }
    memmove(v->buf, v->buf + n, v->fill - n);
    v->base += n;
    v->fill -= n;
}

/* The frame (SOI..EOI) at or after `from`, left at the start of a sliding window. */
static bool video_read_frame(vctx_t *v, size_t from)
{
    if (v->f == NULL) {
        v->f = fopen(v->path, "rb");
        if (v->f == NULL) {
            espix_klog(ESPIX_KLOG_WARN, TAG, "%s: %s", v->path, strerror(errno));
            return false;
        }
    }

    if (from < v->base || from > v->base + v->fill) {
        v->base = from;
        v->fill = 0;
    } else {
        video_slide(v, from - v->base);
    }

    size_t scan  = 0;                   /* where the next look starts */
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
                video_slide(v, (size_t)start);
                start = 0;
            } else if (start < 0 && v->fill > 1) {
                video_slide(v, v->fill - 1);    /* keep a byte that may begin one */
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
        if (!video_fill(v)) {
            return false;               /* the end, or a frame cut short */
        }
    }
}

/* Decode the frame in hand into the back buffer. */
static bool video_decode(vctx_t *v)
{
    const uint8_t *p = v->buf + v->beg;

    if (v->back == NULL) {
        v->back = espix_image_jpeg(p, v->len);
        return v->back != NULL;
    }
    return espix_image_jpeg_into(v->back, p, v->len) == ESP_OK;
}

/* Open a file and decode its first frame; no whole-file scan, which cost tens of seconds. */
static vctx_t *video_load(const char *path)
{
    vctx_t *v = calloc(1, sizeof(*v));
    if (v == NULL) {
        snprintf(s_err, sizeof(s_err), "no memory");
        return NULL;
    }

    strlcpy(v->path, path, sizeof(v->path));
    v->f = fopen(path, "rb");
    if (v->f == NULL) {
        snprintf(s_err, sizeof(s_err), "%.32s: %.48s", path, strerror(errno));
        espix_klog(ESPIX_KLOG_WARN, TAG, "%s: %s", path, strerror(errno));
        free(v);
        return NULL;
    }

    fseek(v->f, 0, SEEK_END);
    const long n = ftell(v->f);
    fseek(v->f, 0, SEEK_SET);

    if (n <= 8 || (size_t)n > VIDEO_MAX_BYTES) {
        snprintf(s_err, sizeof(s_err), "%.32s: %ld bytes is out of range", path, n);
        ctx_free(v);
        return NULL;
    }
    v->file_len = (size_t)n;

    if (!video_read_frame(v, 0) || !video_decode(v)) {
        snprintf(s_err, sizeof(s_err), "%.32s: the first frame will not decode", path);
        ctx_free(v);
        return NULL;
    }

    /* A stream belongs to the task that opened it; the player reopens. */
    fclose(v->f);
    v->f = NULL;

    /* The first frame is both buffers' shape; the back gets its own copy. */
    v->front = v->back;
    v->back  = NULL;
    v->fw    = espix_surface_width(v->front);
    v->fh    = espix_surface_height(v->front);

    espix_klog(ESPIX_KLOG_INFO, TAG, "%s: %dx%d, %u bytes", path, v->fw, v->fh,
               (unsigned)v->file_len);
    return v;
}

/* ------------------------------------------------------------------ */
/* Geometry                                                            */
/* ------------------------------------------------------------------ */

static espix_rect_t band_rect(const espix_window_t *w)
{
    const espix_rect_t c = espix_window_content(w);

    return (espix_rect_t){ c.x, c.y + c.h - BAND_H, c.w, BAND_H };
}

/* Right to left: 0 Play/Pause, 1 Stop, 2 slower, 3 faster. */
static espix_rect_t video_btn(const espix_window_t *w, int i)
{
    const espix_rect_t b = band_rect(w);
    const int          x = b.x + b.w - 8 - (i + 1) * BTN_W - i * BTN_GAP;

    return (espix_rect_t){ x, b.y + (BAND_H - BTN_H) / 2, BTN_W, BTN_H };
}

/* Fit the picture to the window, down only; under s_vlock. */
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
    if (fw < 1) { fw = 1; }
    if (fh < 1) { fh = 1; }

    v->fit = espix_surface_new(fw, fh);
    if (v->fit != NULL) {
        (void)espix_surface_scale(v->fit, v->front);
    }
}

/* What is shown: the fitted copy when there is one. Under s_vlock. */
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
                                       (espix_rect_t){ clip.x - x, clip.y - y,
                                                       clip.w, clip.h });
    }
    vunlock();
}

/* Tell the compositor where the picture is now. Under the desktop lock. */
static void video_place(espix_window_t *w)
{
    vlock();
    const espix_rect_t pic = video_pic(w, s_ctx);
    vunlock();

    espix_window_set_overlay(w, pic, pic.w > 0 ? video_overlay : NULL);
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

/* The surface: letterbox and controls. The picture is the overlay's. */
static void video_draw(espix_window_t *w, espix_surface_t *s, espix_rect_t r,
                       void *ctx)
{
    (void)ctx;
    (void)r;

    const espix_rect_t c = espix_window_content(w);

    vlock();
    const bool have = video_src(s_ctx) != NULL;
    const int  cur  = (s_ctx == NULL) ? 0 : s_ctx->cur;
    vunlock();

    espix_surface_fill(s, c, 0x0000);
    if (!have) {
        espix_surface_text(s, c.x, c.y, s_err[0] != '\0' ? s_err : "no video",
                           WGT_FG, WGT_BG);
        return;
    }

    const espix_rect_t b = band_rect(w);
    espix_surface_fill(s, b, WGT_PANE);

    char line[64];
    snprintf(line, sizeof(line), "%d fps  frame %d  %d ms", s_fps, cur, s_frame_ms);
    espix_wgt_label(s, (espix_rect_t){ b.x + 8, b.y + 12, 320, WGT_ROW_H },
                    line, true);

    espix_wgt_button(s, video_btn(w, 0), s_playing ? "Pause" : "Play", true, false);
    espix_wgt_button(s, video_btn(w, 1), "Stop", true, false);
    espix_wgt_button(s, video_btn(w, 2), "-", true, false);
    espix_wgt_button(s, video_btn(w, 3), "+", true, false);
}

static void video_rate(int delta)
{
    int r = s_rate + delta;

    if (r < VIDEO_RATE_MIN) { r = VIDEO_RATE_MIN; }
    if (r > VIDEO_RATE_MAX) { r = VIDEO_RATE_MAX; }
    s_rate = r;
}

/* Wake the task so a play, a stop or a close is acted on now. */
static void video_kick(void)
{
    vlock();
    if (s_ctx != NULL && s_ctx->task != NULL) {
        xTaskNotifyGive(s_ctx->task);
    }
    vunlock();
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

static void video_pointer(espix_window_t *w, int x, int y, uint8_t buttons,
                          void *ctx)
{
    (void)ctx;

    if (buttons == 0) {
        return;
    }

    if (espix_wgt_hit(video_btn(w, 0), x, y)) {
        s_playing = !s_playing;
    } else if (espix_wgt_hit(video_btn(w, 1), x, y)) {
        s_playing = false;
        s_rewind  = true;
    } else if (espix_wgt_hit(video_btn(w, 2), x, y)) {
        video_rate(-VIDEO_RATE_STEP);
    } else if (espix_wgt_hit(video_btn(w, 3), x, y)) {
        video_rate(+VIDEO_RATE_STEP);
    } else {
        return;
    }
    video_kick();
    espix_window_repaint(w);
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
        break;
    case 's':
        s_playing = false;
        s_rewind  = true;
        break;
    case '-':
        video_rate(-VIDEO_RATE_STEP);
        break;
    case '+':
    case '=':
        video_rate(+VIDEO_RATE_STEP);
        break;
    default:
        return;
    }
    video_kick();
    espix_window_repaint(w);
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

static void video_band_desk(void *arg);

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

    s_fps = (int)((int64_t)s_done * 1000000 / elapsed);

    espix_klog(ESPIX_KLOG_INFO, TAG,
               "frame %d at %d fps (target %d): decode %d ms, swap %d ms",
               cur, s_fps, s_rate,
               (int)(s_decode_us / n / 1000), (int)(s_paint_us / n / 1000));

    s_decode_us = 0;
    s_paint_us  = 0;
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

/* The numbers on the band changed. */
static void video_band_desk(void *arg)
{
    (void)arg;

    if (s_win != NULL) {
        espix_window_damage(s_win, band_rect(s_win));
    }
}

static void video_task(void *arg)
{
    vctx_t *const v = arg;
    int64_t       next = esp_timer_get_time();

    for (;;) {
        vlock();
        const bool quit = v->quit;
        vunlock();

        if (quit) {
            break;
        }
        if (!s_playing) {
            /* Paused: sleep until poked. */
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
            next = esp_timer_get_time();
            continue;
        }

        if (s_rewind) {
            s_rewind = false;
            vlock();
            v->cur = 0;
            vunlock();
            (void)video_read_frame(v, 0);
        }

        const int64_t t0 = esp_timer_get_time();
        const bool    ok = video_decode(v);
        const int64_t t1 = esp_timer_get_time();

        if (!ok) {
            s_playing = false;
            espix_klog(ESPIX_KLOG_WARN, TAG, "frame at %u (%u bytes) will not decode",
                       (unsigned)v->off, (unsigned)v->len);
            snprintf(s_err, sizeof(s_err), "a frame will not decode");
            (void)espix_desktop_post(video_repaint_desk, NULL);
            continue;
        }

        vlock();
        espix_surface_t *shown = v->front;
        v->front = v->back;
        v->back  = shown;
        if (v->fit != NULL) {
            (void)espix_surface_scale(v->fit, v->front);
        }
        v->cur++;
        const int cur = v->cur;
        vunlock();

        (void)espix_desktop_post(video_repaint_desk, NULL);
        const int64_t t2 = esp_timer_get_time();

        /* The read waits on the card; it goes behind the paint, not before it. */
        if (!video_read_frame(v, v->off + v->len)) {
            if (video_read_frame(v, 0)) {
                vlock();
                v->cur = 0;
                vunlock();
            } else {
                espix_klog(ESPIX_KLOG_WARN, TAG, "no frame after %u, nor at 0",
                           (unsigned)(v->off + v->len));
                s_playing = false;
            }
        }

        s_decode_us += t1 - t0;
        s_paint_us  += t2 - t1;
        s_done++;
        s_frame_ms = (int)((esp_timer_get_time() - t0) / 1000);
        video_stat(cur);

        /* Pace to the target without accumulating a debt. */
        next += 1000000 / (s_rate > 0 ? s_rate : 1);
        const int64_t now = esp_timer_get_time();

        if (next > now) {
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS((next - now + 999) / 1000));
        } else {
            next = now;
        }
    }

    ctx_free(v);
}

/* ------------------------------------------------------------------ */
/* The window                                                          */
/* ------------------------------------------------------------------ */

/* Retire the live context; its task frees it. Under the desktop lock. */
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
    }
    vunlock();

    if (v != NULL && task == NULL) {
        ctx_free(v);                    /* never had a task */
    }
}

/* Load a file into a fresh context and start its task. */
static bool video_start(const char *path)
{
    s_err[0]  = '\0';
    s_playing = false;
    s_rewind  = false;

    if (path == NULL || path[0] == '\0') {
        snprintf(s_err, sizeof(s_err), "no file");
        return false;
    }
    strlcpy(s_path, path, sizeof(s_path));

    vctx_t *v = video_load(path);
    if (v == NULL) {
        return false;
    }

    vlock();
    s_ctx = v;
    vunlock();

    s_stat_at   = 0;
    s_done      = 0;
    s_decode_us = 0;
    s_paint_us  = 0;
    s_fps       = 0;
    s_playing   = true;

    TaskHandle_t task = NULL;
    if (espix_kthread_start(&(const espix_kthread_t){
            .name = "espix:video", .fn = video_task, .arg = v, .stack = 6144,
            .priority = 3, .core = tskNO_AFFINITY }, &task) != ESP_OK
        || task == NULL) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "no task for the player");
        s_playing = false;
        return true;                    /* the first frame still shows */
    }
    vlock();
    v->task = task;
    vunlock();
    return true;
}

espix_window_t *espix_video_open(const char *path)
{
    if (s_win != NULL) {
        /* The window is up: a new file goes in it. */
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
    int cw = (s_ctx != NULL) ? s_ctx->fw : 320;
    int ch = (s_ctx != NULL) ? s_ctx->fh : 120;
    vunlock();

    espix_canvas_t *c     = espix_display_canvas();
    const int       cwmax = (c != NULL) ? espix_canvas_width(c) : cw + 2 * 8;
    const int       chmax = (c != NULL) ? espix_canvas_height(c) - VIDEO_TASKBAR_H
                                        : ch + VIDEO_TITLE_H + 2 * 8;

    int ww = cw + 2 * 4;
    int wh = ch + VIDEO_TITLE_H + 2 * 4;

    if (ww > cwmax - 16) { ww = cwmax - 16; }
    if (wh > chmax - 16) { wh = chmax - 16; }
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

    vlock();
    if (s_ctx != NULL) {
        video_fit_locked(s_ctx, s_win);
    }
    vunlock();
    video_place(s_win);
    espix_window_repaint(s_win);
    return s_win;
}

void espix_video_forget(void)
{
    s_win     = NULL;                   /* its pixels went with the window */
    s_playing = false;
    s_path[0] = '\0';
    video_retire();
}

espix_window_t *espix_video_window(void)
{
    return s_win;
}
