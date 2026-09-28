/*
 * The desktop: a canvas, an input queue, and the little bit of drawing that
 * makes a remote screen obviously working -- a background, a cursor, and a
 * window that echoes keys.
 *
 * Every pixel here is drawn by the CPU, on purpose. This is the reference the
 * accelerators get measured against: PPA FILL replaces espix_canvas_fill, PPA
 * SRM/BLEND replaces the blit and the cursor composite, and the encoder in
 * rfb.c is where the JPEG codec goes. Keeping the CPU path correct and slow
 * is what makes "is the hardware actually faster" an answerable question.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"

#include "espix_display.h"
#include "espix_kernel.h"

#define TAG "display"

/* The font table in font8x8.c. Bit 0 of a row byte is the leftmost pixel. */
extern const unsigned char espix_font8x8[128][8];

#define RGB565(r, g, b) \
    ((espix_px_t)((((r) & 0xF8u) << 8) | (((g) & 0xFCu) << 3) | (((b) & 0xF8u) >> 3)))

/*
 * What the canvas is cleared to when nothing owns it. Deliberately not a
 * desktop colour: the service has no opinion about what a desktop looks like,
 * and this is only what a viewer sees in the gap between one owner releasing
 * and the next claiming -- which is normally the console, and is normally
 * never seen at all.
 */
#define COL_BG RGB565(0x10, 0x12, 0x16)

/* ------------------------------------------------------------------ */
/* Canvas                                                              */
/* ------------------------------------------------------------------ */

struct espix_canvas {
    int              w, h;
    char             name[ESPIX_DISPLAY_NAME_MAX];
    espix_px_t      *px;          /* w * h, RGB565, PSRAM */
    SemaphoreHandle_t lock;
    espix_rect_t     damage[ESPIX_DISPLAY_DAMAGE_MAX];
    int              ndamage;
};

/*
 * A surface is the same pixels without the backend's bookkeeping: no name, no
 * damage list. The stride is carried separately even though it is the width
 * today, because every accelerator wants it as its own argument and pretending
 * otherwise is how a padded buffer becomes a rewrite later.
 */
struct espix_surface {
    int               w, h, stride;
    espix_px_t       *px;
    SemaphoreHandle_t lock;
};

static espix_rect_t rect_clip_wh(espix_rect_t r, int w, int h)
{
    int x0 = r.x < 0 ? 0 : r.x;
    int y0 = r.y < 0 ? 0 : r.y;
    int x1 = r.x + r.w > w ? w : r.x + r.w;
    int y1 = r.y + r.h > h ? h : r.y + r.h;
    return (espix_rect_t){ x0, y0, x1 - x0, y1 - y0 };
}

static espix_rect_t rect_clip(const espix_canvas_t *c, espix_rect_t r)
{
    return rect_clip_wh(r, c->w, c->h);
}

static espix_rect_t rect_union(espix_rect_t a, espix_rect_t b)
{
    int x0 = a.x < b.x ? a.x : b.x;
    int y0 = a.y < b.y ? a.y : b.y;
    int x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w;
    int y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    return (espix_rect_t){ x0, y0, x1 - x0, y1 - y0 };
}

/* Touching counts as overlapping: two rectangles that share an edge are one
 * update, not two. */
static bool rect_touches(espix_rect_t a, espix_rect_t b)
{
    return !(b.x > a.x + a.w || b.x + b.w < a.x ||
             b.y > a.y + a.h || b.y + b.h < a.y);
}

espix_canvas_t *espix_canvas_new(int w, int h, const char *name)
{
    if (w <= 0 || h <= 0) {
        return NULL;
    }

    espix_canvas_t *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        return NULL;
    }

    /* PSRAM first: the whole point of a virtual screen is that it is big.
     * Internal is a fallback only because a small canvas may fit. */
    c->px = heap_caps_malloc((size_t)w * h * sizeof(espix_px_t),
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (c->px == NULL) {
        c->px = heap_caps_malloc((size_t)w * h * sizeof(espix_px_t),
                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (c->px == NULL) {
        free(c);
        return NULL;
    }

    c->lock = xSemaphoreCreateMutex();
    if (c->lock == NULL) {
        heap_caps_free(c->px);
        free(c);
        return NULL;
    }

    c->w = w;
    c->h = h;
    snprintf(c->name, sizeof(c->name), "%s", name != NULL ? name : "canvas");
    return c;
}

void espix_canvas_free(espix_canvas_t *c)
{
    if (c == NULL) {
        return;
    }
    if (c->lock != NULL) {
        vSemaphoreDelete(c->lock);
    }
    heap_caps_free(c->px);
    free(c);
}

int         espix_canvas_width(const espix_canvas_t *c)  { return c->w; }
int         espix_canvas_height(const espix_canvas_t *c) { return c->h; }
const char *espix_canvas_name(const espix_canvas_t *c)   { return c->name; }

void espix_canvas_lock(espix_canvas_t *c)   { xSemaphoreTake(c->lock, portMAX_DELAY); }
void espix_canvas_unlock(espix_canvas_t *c) { xSemaphoreGive(c->lock); }

espix_px_t *espix_canvas_pixels(espix_canvas_t *c) { return c->px; }

/* ------------------------------------------------------------------ */
/* Damage                                                              */
/* ------------------------------------------------------------------ */

void espix_canvas_damage(espix_canvas_t *c, espix_rect_t r)
{
    r = rect_clip(c, r);
    if (r.w <= 0 || r.h <= 0) {
        return;
    }

    /*
     * Fold into anything it touches, and keep folding: growing into one
     * rectangle can bring it into contact with another. The list is short, so
     * restarting the scan is cheaper than being clever.
     */
    bool folded = true;
    while (folded) {
        folded = false;
        for (int i = 0; i < c->ndamage; i++) {
            if (rect_touches(c->damage[i], r)) {
                r = rect_union(c->damage[i], r);
                c->damage[i] = c->damage[--c->ndamage];
                folded = true;
                break;
            }
        }
    }

    if (c->ndamage < ESPIX_DISPLAY_DAMAGE_MAX) {
        c->damage[c->ndamage++] = r;
        return;
    }

    /* Full: one box around everything is the cheapest correct answer. */
    espix_rect_t all = r;
    for (int i = 0; i < c->ndamage; i++) {
        all = rect_union(all, c->damage[i]);
    }
    c->damage[0] = all;
    c->ndamage = 1;
}

size_t espix_canvas_damage_take(espix_canvas_t *c, espix_rect_t *out, size_t max)
{
    size_t n = 0;
    while (c->ndamage > 0 && n < max) {
        out[n++] = c->damage[--c->ndamage];
    }
    return n;
}

void espix_canvas_damage_clear(espix_canvas_t *c) { c->ndamage = 0; }

bool espix_canvas_damaged(const espix_canvas_t *c) { return c->ndamage > 0; }

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

/*
 * The two operations an accelerator can do, and the software that stands in for
 * each.
 *
 * This is the seam the surface design exists for. `op_fill` and `op_blit` are
 * what PPA FILL and PPA SRM or 2D-DMA replace, and the CPU loop underneath is
 * not a fallback in the apologetic sense -- it is the S3's only implementation,
 * since the S3 has none of those blocks, and every target keeps it so the two
 * can be measured against each other on one board rather than across two.
 *
 * Both take the destination as pixels and geometry rather than as a canvas or a
 * surface, because both are called with either.
 */
static void op_fill(espix_px_t *px, int w, int h, int stride, espix_rect_t r,
                    espix_px_t v)
{
    r = rect_clip_wh(r, w, h);
    for (int y = 0; y < r.h; y++) {
        espix_px_t *row = px + (size_t)(r.y + y) * stride + r.x;
        for (int x = 0; x < r.w; x++) {
            row[x] = v;
        }
    }
}

static void op_blit(espix_px_t *px, int w, int h, int stride,
                    int dst_x, int dst_y, const espix_px_t *src,
                    int src_w, int src_h, int src_stride)
{
    for (int y = 0; y < src_h; y++) {
        const int cy = dst_y + y;
        if (cy < 0 || cy >= h) {
            continue;
        }
        for (int x = 0; x < src_w; x++) {
            const int cx = dst_x + x;
            if (cx < 0 || cx >= w) {
                continue;
            }
            px[(size_t)cy * stride + cx] = src[(size_t)y * src_stride + x];
        }
    }
}

/* Text is glyph work, not a copy: there is nothing here for an accelerator. */
static void px_text(espix_px_t *px, int w, int h, int stride, int x, int y,
                    const char *s, espix_px_t fg, espix_px_t bg)
{
    for (; *s != '\0'; s++, x += 8) {
        unsigned ch = (unsigned char)*s;
        if (ch > 127) {
            ch = '?';
        }
        const unsigned char *glyph = espix_font8x8[ch];
        for (int row = 0; row < 8; row++) {
            const int cy = y + row;
            if (cy < 0 || cy >= h) {
                continue;
            }
            for (int col = 0; col < 8; col++) {
                const int cx = x + col;
                if (cx < 0 || cx >= w) {
                    continue;
                }
                px[(size_t)cy * stride + cx] =
                    (glyph[row] & (1u << col)) ? fg : bg;
            }
        }
    }
}

static void px_outline(espix_px_t *px, int w, int h, int stride, espix_rect_t r,
                       espix_px_t v)
{
    op_fill(px, w, h, stride, (espix_rect_t){ r.x, r.y, r.w, 1 }, v);
    op_fill(px, w, h, stride, (espix_rect_t){ r.x, r.y + r.h - 1, r.w, 1 }, v);
    op_fill(px, w, h, stride, (espix_rect_t){ r.x, r.y, 1, r.h }, v);
    op_fill(px, w, h, stride, (espix_rect_t){ r.x + r.w - 1, r.y, 1, r.h }, v);
}

void espix_canvas_fill(espix_canvas_t *c, espix_rect_t r, espix_px_t px)
{
    r = rect_clip(c, r);
    op_fill(c->px, c->w, c->h, c->w, r, px);
    espix_canvas_damage(c, r);
}

void espix_canvas_blit(espix_canvas_t *c, int dst_x, int dst_y,
                       const espix_px_t *src, int src_w, int src_h, int src_stride)
{
    op_blit(c->px, c->w, c->h, c->w, dst_x, dst_y, src, src_w, src_h, src_stride);
    espix_canvas_damage(c, (espix_rect_t){ dst_x, dst_y, src_w, src_h });
}

void espix_canvas_outline(espix_canvas_t *c, espix_rect_t r, espix_px_t px)
{
    px_outline(c->px, c->w, c->h, c->w, r, px);
    espix_canvas_damage(c, r);
}

void espix_canvas_text(espix_canvas_t *c, int x, int y, const char *s,
                       espix_px_t fg, espix_px_t bg)
{
    const int x0 = x;

    px_text(c->px, c->w, c->h, c->w, x, y, s, fg, bg);
    for (; *s != '\0'; s++) {
        x += 8;
    }

    espix_canvas_damage(c, (espix_rect_t){ x0, y, x - x0, 8 });
}

void espix_canvas_blit_surface(espix_canvas_t *c, int x, int y,
                               const espix_surface_t *s)
{
    if (s == NULL || s->px == NULL) {
        return;
    }
    op_blit(c->px, c->w, c->h, c->w, x, y, s->px, s->w, s->h, s->stride);
    espix_canvas_damage(c, (espix_rect_t){ x, y, s->w, s->h });
}

/* ------------------------------------------------------------------ */
/* Surfaces                                                            */
/* ------------------------------------------------------------------ */

espix_surface_t *espix_surface_new(int w, int h)
{
    if (w <= 0 || h <= 0) {
        return NULL;
    }

    espix_surface_t *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        return NULL;
    }

    s->px = heap_caps_malloc((size_t)w * h * sizeof(espix_px_t),
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s->px == NULL) {
        s->px = heap_caps_malloc((size_t)w * h * sizeof(espix_px_t),
                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (s->px == NULL) {
        free(s);
        return NULL;
    }

    s->lock = xSemaphoreCreateMutex();
    if (s->lock == NULL) {
        heap_caps_free(s->px);
        free(s);
        return NULL;
    }

    s->w = w;
    s->h = h;
    s->stride = w;
    return s;
}

void espix_surface_free(espix_surface_t *s)
{
    if (s == NULL) {
        return;
    }
    if (s->lock != NULL) {
        vSemaphoreDelete(s->lock);
    }
    heap_caps_free(s->px);
    free(s);
}

int         espix_surface_width(const espix_surface_t *s)  { return s->w; }
int         espix_surface_height(const espix_surface_t *s) { return s->h; }
espix_px_t *espix_surface_pixels(espix_surface_t *s)       { return s->px; }

void espix_surface_lock(espix_surface_t *s)   { xSemaphoreTake(s->lock, portMAX_DELAY); }
void espix_surface_unlock(espix_surface_t *s) { xSemaphoreGive(s->lock); }

void espix_surface_fill(espix_surface_t *s, espix_rect_t r, espix_px_t px)
{
    op_fill(s->px, s->w, s->h, s->stride, r, px);
}

void espix_surface_blit(espix_surface_t *s, int dst_x, int dst_y,
                        const espix_px_t *src, int src_w, int src_h,
                        int src_stride)
{
    op_blit(s->px, s->w, s->h, s->stride, dst_x, dst_y, src, src_w, src_h,
            src_stride);
}

void espix_surface_outline(espix_surface_t *s, espix_rect_t r, espix_px_t px)
{
    px_outline(s->px, s->w, s->h, s->stride, r, px);
}

void espix_surface_text(espix_surface_t *s, int x, int y, const char *str,
                        espix_px_t fg, espix_px_t bg)
{
    px_text(s->px, s->w, s->h, s->stride, x, y, str, fg, bg);
}

/* ------------------------------------------------------------------ */
/* The service                                                         */
/* ------------------------------------------------------------------ */

static espix_canvas_t *s_canvas;
static bool            s_up;

/* The one owner of the screen, or NULL when nothing does. */
static const espix_screen_t *s_owner;

/* Pointer position; see espix_display_pointer(). */
static int s_ptr_x, s_ptr_y;

/* What a viewer gets when nothing owns the screen. */
static espix_display_default_t s_default;
static bool                    s_default_up;

/*
 * Whether a viewer is attached at all -- which is a different question from
 * whether the viewer started the default content, and conflating the two is
 * what lost the console: start a desktop from the serial console, attach a
 * viewer to it, and `desktop stop` fell back to a plain canvas rather than a
 * shell, because this viewer had never started a console.
 */
static bool                    s_viewer;

/* RFB delivers X11 keysyms; the ASCII range is its own keysym. Exported so
 * every input source and the console agree on what a key means. */
char espix_keysym_char(uint32_t ks)
{
    if (ks >= 0x20 && ks <= 0x7E) {
        return (char)ks;
    }
    switch (ks) {
    case 0xFF0D:                    /* Return */
    case 0xFF8D: return '\r';     /* KP_Enter */
    case 0xFF08: return '\b';     /* BackSpace */
    default:     return 0;
    }
}

void espix_display_input(const espix_input_event_t *ev)
{
    if (ev == NULL || !s_up) {
        return;
    }

    if (ev->kind == ESPIX_INPUT_POINTER) {
        s_ptr_x = ev->x;
        s_ptr_y = ev->y;
    } else if (ev->kind == ESPIX_INPUT_MOTION) {
        s_ptr_x += ev->x;
        s_ptr_y += ev->y;
    }

    /*
     * Clamped here, because this is where the position is owned -- and because
     * a delta pushed against an edge accumulates without ever leaving it. A
     * local mouse held at the right-hand border adds to s_ptr_x for as long as
     * it is pushed; the cursor stops at the border, which is the owner's
     * clamping, and the number underneath does not.
     *
     * That number is handed out again by espix_display_pointer(), and
     * espix_input_event_t.x is 16 bits -- so a pointer event built from a
     * position that has run past 32767 wraps, and the cursor lands somewhere
     * unrelated the next time a button is pressed. Which is exactly what it
     * looked like: push the pointer into an edge, click, and it jumps.
     */
    if (s_ptr_x < 0)                   { s_ptr_x = 0; }
    if (s_ptr_y < 0)                   { s_ptr_y = 0; }
    if (s_ptr_x > ESPIX_DISPLAY_W - 1) { s_ptr_x = ESPIX_DISPLAY_W - 1; }
    if (s_ptr_y > ESPIX_DISPLAY_H - 1) { s_ptr_y = ESPIX_DISPLAY_H - 1; }

    /*
     * Dispatched in the poster's context rather than through a queue of this
     * service's own. The owner is the only consumer, so a queue would buy a
     * task and a copy for nothing -- and it would make the round trip
     * asynchronous, which is exactly what the old desktop task needed a
     * priority above the RFB task to paper over.
     *
     * With no owner the event is dropped rather than given to anything. The
     * desktop used to be this service's own fallback consumer and is a client
     * now, so there is nobody here to hand it to -- and the position above has
     * already moved, which is the part the service owns.
     */
    if (s_owner != NULL) {
        s_owner->input(s_owner->ctx, ev);
    }
}

void espix_display_pointer(int *x, int *y)
{
    if (x != NULL) {
        *x = s_ptr_x;
    }
    if (y != NULL) {
        *y = s_ptr_y;
    }
}

esp_err_t espix_display_claim(const espix_screen_t *screen)
{
    if (!s_up || screen == NULL || screen->input == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /*
     * Takeover, not refusal. A new owner is what `startx` does to a console:
     * the old one is not destroyed and not consulted -- it keeps its own state
     * and simply stops being the thing that is rendered. Refusing instead made
     * "run the desktop from the console" impossible, which is the one place
     * you would most want to do it from.
     */
    if (s_owner != NULL && s_owner != screen) {
        espix_klog(ESPIX_KLOG_INFO, TAG, "screen owner: %s -> %s",
                   s_owner->name, screen->name);
    } else {
        espix_klog(ESPIX_KLOG_INFO, TAG, "screen owner: %s", screen->name);
    }

    s_owner = screen;
    if (screen->repaint != NULL) {
        screen->repaint(screen->ctx);
    }
    return ESP_OK;
}

void espix_display_release(const espix_screen_t *screen)
{
    if (s_owner != screen) {
        return;
    }

    s_owner = NULL;

    /*
     * Whatever a viewer should see when nothing owns the screen. The console
     * re-claims itself if it is still running -- which is what makes
     * "desktop stop" bring it back -- and the built-in content is the floor
     * when there is no console either.
     */
    if (s_default.start != NULL && (s_default_up || s_viewer)) {
        if (s_default.start() == ESP_OK) {
            s_default_up = true;
            return;
        }
    }

    /*
     * Nothing to fall back to, so the floor is a plain background rather than
     * content. The desktop is a client now, like the console, so the service
     * has nothing of its own to draw.
     */
    espix_klog(ESPIX_KLOG_INFO, TAG, "screen owner: none");
    if (s_canvas != NULL) {
        espix_canvas_lock(s_canvas);
        espix_canvas_fill(s_canvas,
                          (espix_rect_t){ 0, 0, ESPIX_DISPLAY_W, ESPIX_DISPLAY_H },
                          COL_BG);
        espix_canvas_unlock(s_canvas);
    }
}

const char *espix_display_owner(void)
{
    return s_owner != NULL ? s_owner->name : "";
}

bool espix_display_owns(const espix_screen_t *screen)
{
    return screen != NULL && s_owner == screen;
}

void espix_display_set_default(const espix_display_default_t *def)
{
    if (def != NULL) {
        s_default = *def;
    } else {
        memset(&s_default, 0, sizeof(s_default));
    }
}

void espix_display_viewer_attached(void)
{
    /*
     * Only when nothing else owns the screen. A desktop that is already
     * running is what the viewer should see -- starting a console over it
     * would be the server second-guessing the user.
     */
    s_viewer = true;

    if (s_owner == NULL && s_default.start != NULL) {
        if (s_default.start() == ESP_OK) {
            s_default_up = true;
        }
    }
}

void espix_display_viewer_detached(void)
{
    s_viewer = false;

    if (s_default_up && s_default.stop != NULL) {
        s_default.stop();
        s_default_up = false;
    }
}

/* ------------------------------------------------------------------ */
/* Service                                                             */
/* ------------------------------------------------------------------ */

espix_canvas_t *espix_display_canvas(void) { return s_canvas; }
bool            espix_display_ready(void)  { return s_up; }

static esp_err_t display_up(void)
{
    if (s_up) {
        return ESP_OK;
    }

    s_canvas = espix_canvas_new(ESPIX_DISPLAY_W, ESPIX_DISPLAY_H, "espix");
    if (s_canvas == NULL) {
        espix_klog(ESPIX_KLOG_ERROR, TAG,
                   "cannot allocate a %dx%d canvas", ESPIX_DISPLAY_W, ESPIX_DISPLAY_H);
        return ESP_ERR_NO_MEM;
    }

    /*
     * No task and no queue of this service's own. Input is dispatched in the
     * poster's context and the owner draws, so a task would buy nothing but a
     * copy per event and a priority puzzle -- which is exactly what the old
     * desktop task needed one to paper over.
     */
    s_ptr_x = ESPIX_DISPLAY_W / 3;
    s_ptr_y = ESPIX_DISPLAY_H / 3;

    s_up = true;

    /*
     * The whole screen is damage once, so a client that asks for an
     * incremental update before it has ever seen a frame still gets one. Well
     * behaved clients ask for a full update first and this is simply dropped.
     */
    espix_canvas_lock(s_canvas);
    espix_canvas_damage(s_canvas,
                        (espix_rect_t){ 0, 0, ESPIX_DISPLAY_W, ESPIX_DISPLAY_H });
    espix_canvas_unlock(s_canvas);

    /*
     * Cleared rather than painted: what goes on the screen is an owner's
     * business and there is none yet. A viewer that attaches gets the console;
     * one that attaches while something already owns the screen gets that.
     */
    espix_canvas_lock(s_canvas);
    espix_canvas_fill(s_canvas,
                      (espix_rect_t){ 0, 0, ESPIX_DISPLAY_W, ESPIX_DISPLAY_H },
                      COL_BG);
    espix_canvas_unlock(s_canvas);

    espix_klog(ESPIX_KLOG_INFO, TAG, "display %dx%d up", ESPIX_DISPLAY_W, ESPIX_DISPLAY_H);
    return ESP_OK;
}

static void display_down(void)
{
    if (!s_up) {
        return;
    }

    /*
     * An owner that outlived its canvas would be holding a pointer to freed
     * memory, and its own teardown is what releases it -- so say so rather than
     * guessing at a teardown from here.
     */
    if (s_owner != NULL) {
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "display stopped while %s still owned the screen",
                   s_owner->name);
        s_owner = NULL;
    }

    if (s_canvas != NULL) {
        espix_canvas_free(s_canvas);
        s_canvas = NULL;
    }

    s_up = false;
}

esp_err_t espix_display_start(void) { return display_up(); }
void      espix_display_stop(void)  { display_down(); }

/* Defined in rfb.c: the RFB listener is a backend of this service. */
esp_err_t espix_display_rfb_listen(uint16_t port);
void      espix_display_rfb_stop(void);

esp_err_t espix_display_vnc_start(uint16_t port)
{
    if (port == 0) {
        port = 5900;
    }

    /* Already serving: a second start is only an error if it disagrees about
     * the port, and in neither case may it disturb the desktop underneath. */
    if (espix_display_vnc_running()) {
        return espix_display_vnc_port() == port ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

    /*
     * The backend needs a canvas; the canvas does not need the backend. Brought
     * up here for convenience -- "vnc start" on a cold board should just work
     * -- but only taken back down on the failure path if this call is what
     * brought it up.
     */
    const bool      was_up = s_up;
    const esp_err_t err    = display_up();
    if (err != ESP_OK) {
        return err;
    }

    const esp_err_t lerr = espix_display_rfb_listen(port);
    if (lerr != ESP_OK) {
        if (!was_up) {
            display_down();
        }
        return lerr;
    }
    return ESP_OK;
}

void espix_display_vnc_stop(void)
{
    /*
     * The backend only: the canvas outlives it, which is the whole point of the
     * split. "display stop" is what frees the memory, and the command says so
     * rather than leaving someone to wonder where two megabytes went.
     */
    espix_display_rfb_stop();
}
