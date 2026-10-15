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
 * The file is read whole, once, and the frame offsets are indexed. A frame is
 * then a pointer and a length, which is what the decoder wants, and looping or
 * restarting costs nothing. The cap on both is a listing convenience rather
 * than a belief: a file too big to hold is refused with a reason on the screen
 * rather than half-played.
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
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "espix_desktop.h"
#include "espix_display.h"
#include "espix_kernel.h"
#include "espix_task.h"
#include "espix_widgets.h"

#define TAG "video"

/* Where a frame starts in the file and how long it is. */
typedef struct {
    uint32_t off;
    uint32_t len;
} vframe_t;

/* A frame that never ends is not a frame: the read gives up here rather than
 * growing the buffer for a file that has lost its EOI. */
#define VIDEO_MAX_FRAME_BYTES (4 << 20)

/*
 * How much is read at a time.
 *
 * A frame is eight or nine kilobytes and the card serves a read at about
 * 640 KB/s, which is latency rather than bandwidth -- measured: one frame's
 * read cost fourteen milliseconds of a forty-one millisecond budget, and
 * playback fell from twenty-four frames a second to twenty. Sixty-four
 * kilobytes covers six or seven frames, so the wait is paid once for all of
 * them and the ones that follow are already in memory.
 */
#define VIDEO_READ_STEP (64 * 1024)
#define VIDEO_MAX_BYTES  (64u << 20)

#define VIDEO_RATE_MIN   1
#define VIDEO_RATE_MAX   30
#define VIDEO_RATE_STEP  4

/*
 * The window frame's height and the taskbar's, which are the desktop's
 * geometry rather than the player's -- repeated here so this file does not
 * depend on the order two constants are written in another one.
 */
#define VIDEO_TITLE_H   18
#define VIDEO_TASKBAR_H 26

/* The control band, over the bottom of the picture. */
#define BAND_H  48
#define BTN_W   60
#define BTN_H   24
#define BTN_GAP 6

static espix_window_t  *s_win;
static char             s_path[ESPIX_PATH_MAX];
static FILE            *s_f;                /* open while the video is loaded */
static uint8_t         *s_buf;              /* one frame's bytes, and no more */
static size_t           s_buf_cap;
static size_t           s_file_len;
static size_t           s_off, s_len;       /* where the frame on screen is */
static size_t           s_base, s_fill;     /* the read window: file offset, bytes */
static size_t           s_beg;              /* ...and the frame's own start in it */
static int              s_nframes;          /* how many frames the file holds */

static espix_surface_t *s_frame;            /* decoded, reused every frame */
static espix_surface_t *s_fit;              /* only when it does not fit */
static int              s_fw, s_fh;         /* the video's own size */

static volatile bool    s_playing;
static volatile bool    s_rewind;           /* Stop asks for frame 0 again */

/*
 * Frames a second to aim for.
 *
 * Raw MJPEG carries no timing at all -- there is no field to read -- so the
 * rate has to come from somewhere, and a default is where it comes from today.
 * Twenty-four because that is what the camera, the film and the encoder behind
 * anything anyone actually puts on this board are most likely to have used, and
 * because the decoder is nowhere near the limit: a 640x480 frame costs 5-6 ms
 * of an 83 ms budget at twelve, and 5-6 of 42 at twenty-four. The buttons are
 * the override, and a file that says what it is -- a name, or a sidecar -- is
 * the honest fix when there is a container to put it in.
 */
static volatile int     s_rate = 24;
static int              s_cur;
static char             s_err[96];

/* Measured, for the band and for the log. Written by the task, read by the
 * desktop when it draws the band, so plain ints rather than anything shared. */
static volatile int     s_frame_ms;
static volatile int     s_fps;
static int64_t          s_decode_us, s_paint_us;
static uint32_t         s_done;
static int64_t          s_stat_at;

/* ------------------------------------------------------------------ */
/* The file                                                            */
/* ------------------------------------------------------------------ */

static void video_unload(void)
{
    heap_caps_free(s_buf);
    espix_surface_free(s_frame);
    espix_surface_free(s_fit);

    if (s_f != NULL) {
        fclose(s_f);
    }

    s_f        = NULL;
    s_buf      = NULL;
    s_buf_cap  = 0;
    s_beg      = 0;
    s_base     = 0;
    s_fill     = 0;
    s_rewind   = false;
    s_frame    = NULL;
    s_fit      = NULL;
    s_file_len = 0;
    s_nframes  = 0;
    s_fw = s_fh = 0;
    s_cur      = 0;
    s_frame_ms = 0;
}

/*
 * Index the frames: SOI to EOI, and on to the next SOI.
 *
 * A byte scan, and a safe one: inside entropy-coded data a 0xFF is written as
 * 0xFF 0x00, so 0xFF 0xD9 appears once, as the end of a frame. Nothing is
 * assumed about what sits between two frames either -- some writers pad, this
 * one does not -- because the scan skips to the next SOI regardless.
 *
 * Read in chunks, and the last byte of each chunk is read again in the next:
 * a marker is two bytes and does not care where the chunk boundary fell. The
 * index itself is a few bytes a frame; it is the *pixels* that are the reason
 * this no longer holds the whole file -- a twenty-second clip cost four
 * megabytes of the thirteen this board has, and left the pool so fragmented
 * that the largest free block fell to 672 KB while it played.
 */
static int video_count(FILE *f, size_t n)
{
    uint8_t buf[4096];
    size_t  base = 0;                   /* the file offset of buf[0] */
    size_t  have = 0;                   /* read, not yet scanned */
    long    start = -1;
    int     k     = 0;

    while (base < n) {
        const size_t got   = fread(buf + have, 1, sizeof(buf) - have, f);
        const size_t total = have + got;

        if (total < 2) {
            break;                      /* nothing more to look at */
        }

        for (size_t i = 0; i + 1 < total; i++) {
            if (buf[i] != 0xFF) {
                continue;
            }
            if (buf[i + 1] == 0xD8) {
                start = (long)(base + i);
            } else if (buf[i + 1] == 0xD9 && start >= 0) {
                k++;
                start = -1;
            }
        }

        if (got == 0 || total < sizeof(buf)) {
            break;                      /* the end of the file */
        }

        /*
         * Carry the last byte into the next read rather than seeking back to
         * it: a marker is two bytes and the read may have split one, and
         * reading on is the difference between one sequential pass and a
         * thousand little ones.
         */
        buf[0] = buf[total - 1];
        base += total - 1;
        have  = 1;
    }
    return k;
}

static bool video_load(const char *path)
{
    video_unload();

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        snprintf(s_err, sizeof(s_err), "%.32s: %.48s", path, strerror(errno));
        espix_klog(ESPIX_KLOG_WARN, TAG, "%s: %s", path, strerror(errno));
        return false;
    }

    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (n <= 8 || (size_t)n > VIDEO_MAX_BYTES) {
        fclose(f);
        snprintf(s_err, sizeof(s_err), "%.32s: %ld bytes is out of range",
                 path, n);
        return false;
    }

    s_file_len = (size_t)n;
    s_nframes  = video_count(f, (size_t)n);
    if (s_nframes == 0) {
        fclose(f);
        video_unload();
        snprintf(s_err, sizeof(s_err), "%.32s: no JPEG frames in it", path);
        return false;
    }

    s_f = f;                            /* read frame by frame as it plays */

    espix_klog(ESPIX_KLOG_INFO, TAG, "%s: %d frames, %u bytes",
               path, s_nframes, (unsigned)s_file_len);
    return true;
}

/*
 * Find the frame at or after an offset, and read it into s_buf.
 *
 * The search and the read are one pass, and that is the point: a frame's bytes
 * have to be in memory for the decoder anyway, so looking for its end while
 * reading it costs nothing that reading it would not have cost. That is what
 * makes an index unnecessary -- and with the index goes the cap on how many
 * frames a file may hold, which was 8,192, or five and a half minutes of
 * twenty-four a second.
 *
 * The buffer grows to fit what it finds and is then reused, so a clip with one
 * large frame pays for it once.
 */
static bool video_read_frame(size_t from)
{
    if (s_f == NULL) {
        return false;
    }

    size_t at;                          /* where to look, within s_buf */

    if (from >= s_base && from < s_base + s_fill) {
        at = from - s_base;             /* in the window already: no read */
    } else {
        s_base = from;
        s_fill = 0;
        at     = 0;
    }

    for (;;) {
        if (s_fill + VIDEO_READ_STEP > s_buf_cap) {
            const size_t ncap = s_buf_cap ? s_buf_cap * 2
                                          : VIDEO_READ_STEP * 2;

            if (ncap > VIDEO_MAX_FRAME_BYTES) {
                return false;           /* no end in sight: not a frame */
            }
            uint8_t *nb = heap_caps_malloc(ncap,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (nb == NULL) {
                return false;
            }
            if (s_buf != NULL) {
                memcpy(nb, s_buf, s_fill);
            }
            heap_caps_free(s_buf);
            s_buf     = nb;
            s_buf_cap = ncap;
        }

        if (s_fill < s_buf_cap) {
            if (fseek(s_f, (long)(s_base + s_fill), SEEK_SET) != 0) {
                return false;
            }
            const size_t got = fread(s_buf + s_fill, 1, s_buf_cap - s_fill, s_f);
            s_fill += got;
            if (got == 0 && s_fill < 2) {
                return false;           /* nothing left to read */
            }
        }

        size_t i     = at;
        long   start = -1;

        while (i + 1 < s_fill) {
            if (s_buf[i] == 0xFF && s_buf[i + 1] == 0xD8) {
                start = (long)i;
                break;
            }
            i++;
        }

        if (start < 0) {
            if (s_fill < s_buf_cap) {
                return false;           /* no frame from here to the end */
            }
            at = s_fill - 1;            /* keep the byte that may begin one */
            continue;
        }

        for (i = (size_t)start + 2; i + 1 < s_fill; i++) {
            if (s_buf[i] == 0xFF && s_buf[i + 1] == 0xD9) {
                s_beg = (size_t)start;
                s_off = s_base + (size_t)start;
                s_len = i + 2 - (size_t)start;
                return true;
            }
        }

        if (s_fill < s_buf_cap) {
            return false;               /* a frame with no end: truncated */
        }
        at = (size_t)start;             /* it runs past the window: read on */
    }
}

/* Decode what video_read_frame() left in s_buf, into s_frame. */
static bool video_decode(void)
{
    const uint8_t *p = s_buf + s_beg;
    const size_t   n = s_len;

    if (s_frame == NULL) {
        s_frame = espix_image_jpeg(p, n);
        if (s_frame == NULL) {
            return false;
        }
        s_fw = espix_surface_width(s_frame);
        s_fh = espix_surface_height(s_frame);
        return true;
    }
    return espix_image_jpeg_into(s_frame, p, n) == ESP_OK;
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

/*
 * Fit the picture to the window, once rather than per frame.
 *
 * Down only, like the viewer: a 160-pixel video blown up to fill a maximised
 * window is a decision nobody asked for, and a scaled frame is a PPA op the
 * player would otherwise pay for every frame. A frame that already fits is
 * blitted from the decoded surface with no scaling at all.
 */
static void video_fit(espix_window_t *w)
{
    espix_surface_free(s_fit);
    s_fit = NULL;

    if (s_frame == NULL) {
        return;
    }

    const espix_rect_t c = espix_window_content(w);

    if (s_fw <= c.w && s_fh <= c.h) {
        return;
    }

    int fw = c.w;
    int fh = (int)((int64_t)s_fh * c.w / s_fw);

    if (fh > c.h) {
        fh = c.h;
        fw = (int)((int64_t)s_fw * c.h / s_fh);
    }
    if (fw < 1) { fw = 1; }
    if (fh < 1) { fh = 1; }

    s_fit = espix_surface_new(fw, fh);
    if (s_fit != NULL) {
        (void)espix_surface_scale(s_fit, s_frame);
    }
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

static void video_draw(espix_window_t *w, espix_surface_t *s, espix_rect_t r,
                       void *ctx)
{
    (void)ctx;

    const espix_rect_t c = espix_window_content(w);
    espix_surface_t   *src = (s_fit != NULL) ? s_fit : s_frame;

    if (src == NULL) {
        if (r.y < c.y + WGT_TEXT_H) {
            espix_surface_text(s, c.x, c.y,
                               s_err[0] != '\0' ? s_err : "no video",
                               WGT_FG, WGT_BG);
        }
        return;
    }

    const int sw = espix_surface_width(src);
    const int sh = espix_surface_height(src);
    const int vx = c.x + (c.w - sw) / 2;
    const int vy = c.y + (c.h - sh) / 2;

    if (vx != c.x || vy != c.y || sw != c.w || sh != c.h) {
        espix_surface_fill(s, c, 0x0000);       /* the letterbox, black */
    }

    /* One PPA SRM call, clipped to the surface, so the whole of it every time
     * is also the cheap way to do it. */
    espix_surface_blit(s, vx, vy, espix_surface_pixels(src), sw, sh, sw);

    const espix_rect_t b = band_rect(w);
    espix_surface_fill(s, b, WGT_PANE);

    char line[64];
    snprintf(line, sizeof(line), "%d fps  %d/%d  %d ms",
             s_fps, s_cur, s_nframes, s_frame_ms);
    espix_wgt_label(s, (espix_rect_t){ b.x + 8, b.y + 12, 320, WGT_ROW_H },
                    line, true);

    espix_wgt_button(s, video_btn(w, 0), s_playing ? "Pause" : "Play",
                     true, false);
    espix_wgt_button(s, video_btn(w, 1), "Stop", true, false);
    espix_wgt_button(s, video_btn(w, 2), "-", true, false);
    espix_wgt_button(s, video_btn(w, 3), "+", true, false);
}

/* Raise or lower the rate and say so on the band. */
static void video_rate(int delta)
{
    int r = s_rate + delta;

    if (r < VIDEO_RATE_MIN) { r = VIDEO_RATE_MIN; }
    if (r > VIDEO_RATE_MAX) { r = VIDEO_RATE_MAX; }
    s_rate = r;
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

static void video_pointer(espix_window_t *w, int x, int y, uint8_t buttons,
                          void *ctx)
{
    (void)ctx;

    if (buttons == 0) {
        return;                         /* act on the press, not the release */
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
    espix_window_repaint(w);
}

static void video_resize(espix_window_t *w, espix_rect_t content, void *ctx)
{
    (void)content;
    (void)ctx;
    video_fit(w);
}

/* ------------------------------------------------------------------ */
/* Playback                                                            */
/* ------------------------------------------------------------------ */

/*
 * The frame rate is the last second, and it is the number that matters: the
 * decode is usually not what holds a video back, and the log below is what says
 * which of the two did.
 */
static void video_stat(void)
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
               "%d of %d at %d fps (target %d): decode %d ms, paint %d ms",
               s_cur, s_nframes, s_fps, s_rate,
               (int)(s_decode_us / n / 1000), (int)(s_paint_us / n / 1000));

    s_decode_us = 0;
    s_paint_us  = 0;
    s_done      = 0;
    s_stat_at   = now;
}

/*
 * The whole of the player, and it owns nothing but the deadline.
 *
 * The desktop's lock is held across the decode and the repaint, which is the
 * same bargain the photo viewer makes with a JPEG and for the same reason: a
 * window can be freed the moment it is closed, and a task that decoded outside
 * the lock and painted inside it would be holding a pointer across the gap.
 * Held, the two orderings are the only two -- the close waits for the frame, or
 * the frame sees no window and stops.
 */
static void video_task(void *arg)
{
    (void)arg;

    int64_t next = esp_timer_get_time();

    while (true) {
        espix_desktop_lock();

        if (s_win == NULL) {
            espix_desktop_unlock();
            return;
        }

        if (!s_playing || s_nframes == 0) {
            espix_desktop_unlock();
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (s_rewind) {
            s_rewind = false;
            s_cur    = 0;
            (void)video_read_frame(0);
        }

        const int     shown = s_cur;
        const int64_t t0    = esp_timer_get_time();
        const bool    ok    = video_decode();
        const int64_t t1    = esp_timer_get_time();

        if (ok) {
            if (s_fit != NULL) {
                (void)espix_surface_scale(s_fit, s_frame);
            }
            espix_window_repaint(s_win);

            /*
             * Read the next frame now rather than when it is shown: the read
             * is the one part of the tick that waits on the card, and this
             * puts it behind the paint instead of in front of the next
             * frame's decode.
             */
            if (video_read_frame(s_off + s_len)) {
                s_cur++;
            } else if (video_read_frame(0)) {
                s_cur = 0;              /* the end: play it again */
            } else {
                s_playing = false;
            }
        } else {
            s_playing = false;
            snprintf(s_err, sizeof(s_err), "frame %d will not decode", shown);
            espix_window_repaint(s_win);
        }

        const int64_t t2 = esp_timer_get_time();
        espix_desktop_unlock();

        s_decode_us += t1 - t0;
        s_paint_us  += t2 - t1;
        s_done++;
        s_frame_ms = (int)((t2 - t0) / 1000);
        video_stat();

        if (!ok) {
            return;
        }

        /* Pace to the target, and do not accumulate a debt when a frame takes
         * longer than its slot -- a video that fell behind should resume at the
         * rate asked for, not at the rate it managed. */
        next += 1000000 / (s_rate > 0 ? s_rate : 1);
        const int64_t now = esp_timer_get_time();

        if (next > now) {
            vTaskDelay(pdMS_TO_TICKS((next - now + 999) / 1000));
        } else {
            next = now;
        }
    }
}

/* ------------------------------------------------------------------ */
/* The window                                                          */
/* ------------------------------------------------------------------ */

/*
 * Load the file and decode the first frame.
 *
 * The decode belongs here rather than in the task, because the window is the
 * video's own size and the size is only known once a frame has been decoded.
 * Sized from anything else -- and the first version sized it from a fallback --
 * the window is the wrong shape and the picture is clipped to it.
 */
static bool video_start(const char *path)
{
    s_err[0]  = '\0';
    s_cur     = 0;
    s_playing = false;

    if (path == NULL || path[0] == '\0') {
        snprintf(s_err, sizeof(s_err), "no file");
        return false;
    }

    strlcpy(s_path, path, sizeof(s_path));

    if (!video_load(path)) {
        return false;
    }
    if (!video_read_frame(0) || !video_decode()) {
        snprintf(s_err, sizeof(s_err), "%.32s: the first frame will not decode",
                 path);
        return false;
    }

    s_playing = true;
    return true;
}

espix_window_t *espix_video_open(const char *path)
{
    if (s_win != NULL) {
        /* The window is up: a new file goes in it, and nothing else happens. */
        if (path != NULL && path[0] != '\0' && strcmp(path, s_path) != 0) {
            video_unload();
            if (video_start(path)) {
                video_fit(s_win);       /* down-scale if the new one is bigger */
            }
        }
        espix_window_repaint(s_win);
        return s_win;
    }

    /* A window either way: one that says why is more use than a click that
     * appears to do nothing. */
    (void)video_start(path);

    int cw = (s_frame != NULL) ? s_fw : 320;
    int ch = (s_frame != NULL) ? s_fh : 120;

    espix_canvas_t *c  = espix_display_canvas();
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
        return NULL;
    }

    espix_window_set_draw(s_win, video_draw);
    espix_window_set_pointer(s_win, video_pointer);
    espix_window_set_key(s_win, video_key);
    espix_window_set_resize(s_win, video_resize);

    video_fit(s_win);

    s_stat_at   = 0;
    s_done      = 0;
    s_decode_us = 0;
    s_paint_us  = 0;
    s_fps       = 0;

    if (espix_kthread_start(&(const espix_kthread_t){
            .name = "espix:video", .fn = video_task, .arg = NULL,
            .stack = 6144, .priority = 3, .core = tskNO_AFFINITY },
            NULL) != ESP_OK) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "no task for the player");
    }

    return s_win;
}

void espix_video_forget(void)
{
    /*
     * Under the lock, because the task paints under it: this is the ordering
     * that makes "the window is gone" true before anything here is freed.
     */
    espix_desktop_lock();

    s_win     = NULL;                   /* its pixels went with the window */
    s_playing = false;
    video_unload();

    espix_desktop_unlock();
}

espix_window_t *espix_video_window(void)
{
    return s_win;
}
