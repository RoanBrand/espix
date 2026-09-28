/*
 * The desktop, as a client of the display service.
 *
 * Everything here used to live in espix_display, which was doing two jobs at
 * once: owning the screen, and being one of the things on it. The second job is
 * the one that grows -- windows, z-order, focus, decorations -- so it moved
 * out, and the service went back to owning the canvas, the surfaces, the
 * pointer and the backends.
 *
 * The canvas is *not* owned here. It is fetched per call, because the service
 * may take it away -- `display stop` while the desktop owns the screen is
 * refused, but the pointer must not be cached across that.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "espix_desktop.h"
#include "espix_display.h"
#include "espix_kernel.h"
#include "espix_shell.h"
#include "espix_term.h"

#define TAG "desktop"

#define RGB565(r, g, b) \
    ((espix_px_t)((((r) & 0xF8u) << 8) | (((g) & 0xFCu) << 3) | (((b) & 0xF8u) >> 3)))

#define COL_DESKTOP   RGB565(0x2B, 0x30, 0x3A)
#define COL_WIN_BG    RGB565(0x16, 0x1A, 0x20)
#define COL_WIN_EDGE  RGB565(0x4A, 0x54, 0x66)
#define COL_TITLE     RGB565(0x3A, 0x42, 0x52)
#define COL_TITLE_FOC RGB565(0x4C, 0x6E, 0xA8)
#define COL_TITLE_FG  RGB565(0xE8, 0xEC, 0xF2)
#define COL_TEXT_FG   RGB565(0xC8, 0xD8, 0xE8)
#define COL_CLOSE     RGB565(0x8C, 0x30, 0x36)
#define COL_TITLE_BTN RGB565(0x30, 0x38, 0x46)
#define COL_BTN_DIM   RGB565(0x1E, 0x23, 0x2B)

#define TITLE_H 18
#define PAD     4
#define CELL_W  8
#define CELL_H  8

/*
 * The taskbar's geometry, here rather than with its code because the window
 * geometry has to know where the bar is: a window is kept above it, and a drag
 * is stopped by it.
 */
#define TASKBAR_H   26
#define START_X     2
#define START_W     22
#define TRAY_W      64
#define ITEM_H      20
#define BUTTON_MAX  120

#define WIN_MAX 8

struct espix_window {
    int              x, y, w, h;
    uint32_t         seq;       /* creation order, so the taskbar holds still */
    bool             hidden;    /* minimised: still a window, not on the screen */
    char             title[ESPIX_WINDOW_TITLE_MAX];
    espix_surface_t *surf;
    espix_window_draw_fn draw;
    espix_window_key_fn  key;
    espix_window_resize_fn resize;
    espix_window_pointer_fn pointer;
    void            *ctx;
    bool             maxed;     /* filling the work area, with `rest` to go back to */
    espix_rect_t     rest;
};

/* Bottom first, top last: the array *is* the z-order. */
static espix_window_t *s_wins[WIN_MAX];
static int             s_nwin;
static espix_window_t *s_focus;
static uint32_t        s_win_seq;

/*
 * The desktop's own lock, and it is here rather than on the canvas because
 * there is now more than one thing that repaints: the owner's callbacks arrive
 * in the context of whoever posted them -- the RFB task for input, the console
 * task for a claim -- and the clock in the taskbar ticks in the timer task.
 * Three tasks all moving windows and painting the same pixels.
 *
 * Recursive, because a repair nests in a repair: espix_window_damage() repaints
 * a surface and then repairs the region it is in. Held across a whole input
 * event, so a press that opens a window and a tick that redraws the clock cannot
 * interleave, and released for the long ones -- the JPEG decode behind the photo
 * launcher runs with it held, which is why the number in the benchmark matters
 * here too.
 */
static SemaphoreHandle_t s_desk_lock;

/* The last button mask any event carried, and the drag in progress; see the
 * input handler below. Declared here because stopping the desktop has to end
 * both, and it is above the input handler. */
static uint8_t         s_buttons;
static espix_window_t *s_drag;
static espix_window_t *s_press_win;     /* who took the press, for the release */

static void desk_lock(void)
{
    if (s_desk_lock != NULL) {
        xSemaphoreTakeRecursive(s_desk_lock, portMAX_DELAY);
    }
}

static void desk_unlock(void)
{
    if (s_desk_lock != NULL) {
        xSemaphoreGiveRecursive(s_desk_lock);
    }
}

/* ------------------------------------------------------------------ */
/* Cursor                                                              */
/* ------------------------------------------------------------------ */

/*
 * The cursor is the classic left arrow, 12x19, drawn inside a box one pixel
 * larger on every side so its outline has somewhere to live. The box is saved
 * before the sprite is drawn and put back before it moves -- the usual
 * save-under, which is also exactly the composite PPA BLEND will do.
 */
#define CUR_W  12
#define CUR_H  19
#define CUR_BW (CUR_W + 2)
#define CUR_BH (CUR_H + 2)

static const uint16_t s_arrow[CUR_H] = {
    0x800, 0xC00, 0xA00, 0x900, 0x880, 0x840, 0x820, 0x810, 0x808, 0x804,
    0x83E, 0x920, 0xA90, 0xC90, 0x848, 0x048, 0x024, 0x024, 0x018,
};

static uint16_t   s_fill[CUR_BH];
static uint16_t   s_edge[CUR_BH];
static espix_px_t s_under[CUR_BW * CUR_BH];
static bool       s_cursor_on;
static int        s_cx, s_cy;
static bool       s_arrow_built;

static void cursor_build(void)
{
    /* Fill, offset by one so the box has a border to dilate into. */
    for (int by = 0; by < CUR_BH; by++) {
        uint16_t f = 0;
        for (int bx = 0; bx < CUR_BW; bx++) {
            const int ax = bx - 1, ay = by - 1;
            if (ax < 0 || ax >= CUR_W || ay < 0 || ay >= CUR_H) {
                continue;
            }
            if (s_arrow[ay] & (1u << (CUR_W - 1 - ax))) {
                f |= 1u << (CUR_BW - 1 - bx);
            }
        }
        s_fill[by] = f;
    }

    /* Edge: anything not filled that touches a filled pixel. */
    for (int by = 0; by < CUR_BH; by++) {
        uint16_t e = 0;
        for (int bx = 0; bx < CUR_BW; bx++) {
            if (s_fill[by] & (1u << (CUR_BW - 1 - bx))) {
                continue;
            }
            bool near = false;
            for (int dy = -1; dy <= 1 && !near; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    const int nx = bx + dx, ny = by + dy;
                    if (nx < 0 || nx >= CUR_BW || ny < 0 || ny >= CUR_BH) {
                        continue;
                    }
                    if (s_fill[ny] & (1u << (CUR_BW - 1 - nx))) {
                        near = true;
                        break;
                    }
                }
            }
            if (near) {
                e |= 1u << (CUR_BW - 1 - bx);
            }
        }
        s_edge[by] = e;
    }
    s_arrow_built = true;
}

/*
 * Put the cursor at a hotspot, restoring whatever was underneath the old one.
 * The hotspot is the arrow tip, which sits at box offset (1, 1).
 */
/*
 * Undo the cursor: put back what was under it. Separate from drawing it
 * because a repair has to take it off first -- its save-under holds pixels from
 * before the change, so restoring it afterwards would put back a window that
 * has since moved.
 */
static void cursor_hide(void)
{
    espix_canvas_t *c = espix_display_canvas();
    if (c == NULL || !s_cursor_on) {
        return;
    }

    const int cw = espix_canvas_width(c);
    const int ch = espix_canvas_height(c);

    espix_canvas_lock(c);
    espix_px_t *px = espix_canvas_pixels(c);

    for (int by = 0; by < CUR_BH; by++) {
        for (int bx = 0; bx < CUR_BW; bx++) {
            const int cx = s_cx - 1 + bx, cy = s_cy - 1 + by;
            if (cx < 0 || cx >= cw || cy < 0 || cy >= ch) {
                continue;
            }
            px[(size_t)cy * cw + cx] = s_under[by * CUR_BW + bx];
        }
    }
    espix_canvas_damage(c, (espix_rect_t){ s_cx - 1, s_cy - 1, CUR_BW, CUR_BH });
    espix_canvas_unlock(c);

    s_cursor_on = false;
}

/* Save what is under it, then draw it. */
static void cursor_show(int hx, int hy)
{
    espix_canvas_t *c = espix_display_canvas();
    if (c == NULL) {
        return;
    }

    const int cw = espix_canvas_width(c);
    const int ch = espix_canvas_height(c);

    if (hx < 0) { hx = 0; }
    if (hy < 0) { hy = 0; }
    if (hx > cw - 1) { hx = cw - 1; }
    if (hy > ch - 1) { hy = ch - 1; }

    espix_canvas_lock(c);
    espix_px_t *px = espix_canvas_pixels(c);

    s_cx = hx;
    s_cy = hy;

    for (int by = 0; by < CUR_BH; by++) {
        for (int bx = 0; bx < CUR_BW; bx++) {
            const int cx = hx - 1 + bx, cy = hy - 1 + by;
            if (cx < 0 || cx >= cw || cy < 0 || cy >= ch) {
                continue;
            }
            s_under[by * CUR_BW + bx] = px[(size_t)cy * cw + cx];
        }
    }

    for (int by = 0; by < CUR_BH; by++) {
        for (int bx = 0; bx < CUR_BW; bx++) {
            const int cx = hx - 1 + bx, cy = hy - 1 + by;
            if (cx < 0 || cx >= cw || cy < 0 || cy >= ch) {
                continue;
            }
            const uint16_t bit = 1u << (CUR_BW - 1 - bx);
            espix_px_t v;
            if (s_fill[by] & bit) {
                v = 0xFFFF;                 /* white body */
            } else if (s_edge[by] & bit) {
                v = 0x0000;                 /* black outline */
            } else {
                continue;
            }
            px[(size_t)cy * cw + cx] = v;
        }
    }

    espix_canvas_damage(c, (espix_rect_t){ hx - 1, hy - 1, CUR_BW, CUR_BH });
    s_cursor_on = true;
    espix_canvas_unlock(c);
}

/* Move it: undo where it was, then draw it where it is going. */
static void cursor_put(int hx, int hy)
{
    cursor_hide();
    cursor_show(hx, hy);
}

/* ------------------------------------------------------------------ */
/* Windows                                                             */
/* ------------------------------------------------------------------ */

/* Defined at the bottom; the drawing gate needs it long before then. */
static const espix_screen_t s_desktop_screen;

static bool desktop_on_screen(void)
{
    return espix_display_owns(&s_desktop_screen);
}

/* Defined below; damaging a window is a repair of the region it covers. */
static void desktop_repair(espix_rect_t r);

/* The taskbar is painted over the windows, and its menu over the taskbar. */
static void taskbar_paint(espix_canvas_t *c, espix_rect_t r);
static void task_button_damage(espix_window_t *w);
static void window_present(espix_window_t *w);
static void menu_paint(espix_canvas_t *c, espix_rect_t r);
static espix_rect_t menu_rect(void);
static void bar_damage(void);

/* How often a drag lets the screen catch up. See the note where it is used. */
#define DRAG_TICK_US (16 * 1000)

/* A drag in progress, and where the screen last had the window. */
static bool         s_dragging;
static int64_t      s_drag_repair_at;
static espix_rect_t s_drag_shown;

espix_rect_t espix_window_content(const espix_window_t *w)
{
    return (espix_rect_t){ PAD, TITLE_H + PAD,
                           w->w - 2 * PAD, w->h - TITLE_H - 2 * PAD };
}

static bool window_hit(const espix_window_t *w, int x, int y)
{
    return x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + w->h;
}

/* The top window under the pointer, or NULL. The array is the z-order, so the
 * search is from the top down. */
static espix_window_t *window_at(int x, int y)
{
    for (int i = s_nwin - 1; i >= 0; i--) {
        if (!s_wins[i]->hidden && window_hit(s_wins[i], x, y)) {
            return s_wins[i];
        }
    }
    return NULL;
}

/* The topmost window that is actually on the screen, which is what takes the
 * focus when the one wearing it is minimised. */
static espix_window_t *window_top_visible(void)
{
    for (int i = s_nwin - 1; i >= 0; i--) {
        if (!s_wins[i]->hidden) {
            return s_wins[i];
        }
    }
    return NULL;
}

/*
 * Three buttons at the right of the title bar, inboard from the edge in the
 * order every desktop has used for forty years: close outermost, then minimise,
 * then maximise. In surface coordinates, and the same rectangles draw them and
 * hittest them -- a button drawn in one place and pressed in another is the
 * whole class of bug this avoids.
 */
#define WIN_BTN (TITLE_H - 6)
#define WIN_BTN_GAP 4

#define WIN_BTN_MAX   0
#define WIN_BTN_MIN   1
#define WIN_BTN_CLOSE 2

static espix_rect_t window_button_box(const espix_window_t *w, int which)
{
    const int from_right = WIN_BTN_CLOSE - which;

    return (espix_rect_t){ w->w - 3 - (from_right + 1) * WIN_BTN
                                 - from_right * WIN_BTN_GAP,
                           3, WIN_BTN, WIN_BTN };
}

static espix_rect_t window_close_box(const espix_window_t *w)
{
    return window_button_box(w, WIN_BTN_CLOSE);
}

static espix_rect_t window_min_box(const espix_window_t *w)
{
    return window_button_box(w, WIN_BTN_MIN);
}

static bool window_button_hit(const espix_window_t *w, espix_rect_t b,
                              int x, int y)
{
    return x >= w->x + b.x && x < w->x + b.x + b.w &&
           y >= w->y + b.y && y < w->y + b.y + b.h;
}

static bool window_close_hit(const espix_window_t *w, int x, int y)
{
    return window_button_hit(w, window_close_box(w), x, y);
}

static bool window_min_hit(const espix_window_t *w, int x, int y)
{
    return window_button_hit(w, window_min_box(w), x, y);
}

static bool window_max_hit(const espix_window_t *w, int x, int y)
{
    return window_button_hit(w, window_button_box(w, WIN_BTN_MAX), x, y);
}

espix_surface_t *espix_window_surface(espix_window_t *w) { return w->surf; }

static espix_rect_t win_clip(espix_rect_t r, int w, int h)
{
    const int x0 = r.x < 0 ? 0 : r.x;
    const int y0 = r.y < 0 ? 0 : r.y;
    const int x1 = r.x + r.w > w ? w : r.x + r.w;
    const int y1 = r.y + r.h > h ? h : r.y + r.h;
    return (espix_rect_t){ x0, y0, x1 - x0, y1 - y0 };
}

/*
 * The parts of a window that are the desktop's, within `r`: the background, the
 * title bar, the outline. The title is only touched when the region reaches it,
 * and the outline only when the region touches an edge, because redrawing either
 * for a cell in the middle cannot change a pixel.
 */
static void window_frame(espix_window_t *w, espix_rect_t r)
{
    espix_surface_t *s = w->surf;

    espix_surface_fill(s, r, COL_WIN_BG);

    if (r.y < TITLE_H) {
        const espix_px_t bar = (w == s_focus) ? COL_TITLE_FOC : COL_TITLE;
        const int tw = (int)strlen(w->title) * CELL_W;

        espix_surface_fill(s, (espix_rect_t){ 0, 0, w->w, TITLE_H }, bar);
        espix_surface_text(s, (w->w - tw) / 2, (TITLE_H - CELL_H) / 2, w->title,
                           COL_TITLE_FG, bar);

        const espix_rect_t x = window_close_box(w);
        espix_surface_fill(s, x, COL_CLOSE);
        espix_surface_text(s, x.x + 2, x.y + 2, "x", COL_TITLE_FG, COL_CLOSE);

        /* Minimise is a bar and maximise is a box, which is how every desktop
         * has drawn them since before this one existed. */
        const espix_rect_t m = window_min_box(w);
        espix_surface_fill(s, m, COL_TITLE_BTN);
        espix_surface_fill(s, (espix_rect_t){ m.x + 3, m.y + WIN_BTN - 5,
                                              WIN_BTN - 6, 2 }, COL_TITLE_FG);

        const espix_rect_t u = window_button_box(w, WIN_BTN_MAX);
        espix_surface_fill(s, u, COL_TITLE_BTN);
        espix_surface_outline(s, (espix_rect_t){ u.x + 3, u.y + 3, 6, 6 },
                              COL_TITLE_FG);
    }

    if (r.x == 0 || r.y == 0 || r.x + r.w >= w->w || r.y + r.h >= w->h) {
        espix_surface_outline(s, (espix_rect_t){ 0, 0, w->w, w->h }, COL_WIN_EDGE);
    }
}

/* Into the surface only, so a caller can batch it with a compositing change. */
static void window_paint(espix_window_t *w, espix_rect_t r)
{
    espix_surface_lock(w->surf);
    window_frame(w, r);
    if (w->draw != NULL) {
        w->draw(w, w->surf, r, w->ctx);
    }
    espix_surface_unlock(w->surf);
}

void espix_window_damage(espix_window_t *w, espix_rect_t r)
{
    if (w == NULL || w->surf == NULL) {
        return;
    }
    r = win_clip(r, w->w, w->h);
    if (r.w <= 0 || r.h <= 0) {
        return;
    }

    window_paint(w, r);

    /*
     * Then the *region*, from every window that reaches it and in z-order -- not
     * this window's surface alone. Blitting just the damaged window paints over
     * whatever is above it, which is visible the moment a window is focused
     * without being raised: its title bar appears through the window on top of
     * it. The region is one window's worth, so this is still cheap.
     */
    desktop_repair((espix_rect_t){ w->x + r.x, w->y + r.y, r.w, r.h });
}

void espix_window_repaint(espix_window_t *w)
{
    if (w == NULL || w->surf == NULL) {
        return;
    }
    espix_window_damage(w, (espix_rect_t){ 0, 0, w->w, w->h });
}

/*
 * Put the background and every window back within `r`, and nothing else.
 *
 * The surfaces are *not* repainted. They already hold each window's frame and
 * content, and a window changes only when its owner says so -- so a repair is a
 * fill and one blit per window, with no drawing at all. Repainting every
 * surface here is what made a drag pay 8ms of glyphs per window per motion.
 *
 * The region is what makes a drag affordable: a motion only uncovers the union
 * of the window's old and new rectangles, so filling and blitting those instead
 * of the whole canvas is a few times less work for the same picture.
 */
static void desktop_repair_locked(espix_rect_t r)
{
    espix_canvas_t *c = espix_display_canvas();
    if (c == NULL || !desktop_on_screen()) {
        return;
    }

    r = win_clip(r, espix_canvas_width(c), espix_canvas_height(c));
    if (r.w <= 0 || r.h <= 0) {
        return;
    }

    /* Off first: its save-under is pixels from before the change. */
    cursor_hide();

    espix_canvas_lock(c);
    espix_canvas_fill(c, r, COL_DESKTOP);

    for (int i = 0; i < s_nwin; i++) {
        const espix_window_t *w = s_wins[i];
        if (w->hidden) {
            continue;                   /* minimised: not on the screen */
        }
        const int x0 = w->x > r.x ? w->x : r.x;
        const int y0 = w->y > r.y ? w->y : r.y;
        const int x1 = (w->x + w->w) < (r.x + r.w) ? (w->x + w->w) : (r.x + r.w);
        const int y1 = (w->y + w->h) < (r.y + r.h) ? (w->y + w->h) : (r.y + r.h);

        if (x1 <= x0 || y1 <= y0) {
            continue;                   /* does not reach the region */
        }
        espix_canvas_blit_surface_rect(c, w->x, w->y, w->surf,
                                       (espix_rect_t){ x0 - w->x, y0 - w->y,
                                                       x1 - x0, y1 - y0 });
    }
    /* Over the windows, not beside them: a window that reaches the bottom of
     * the screen goes under the bar, which is what makes it a bar. */
    taskbar_paint(c, r);
    menu_paint(c, r);
    espix_canvas_unlock(c);

    int px = 0, py = 0;
    espix_display_pointer(&px, &py);
    cursor_show(px, py);
}

static void desktop_repair(espix_rect_t r)
{
    desk_lock();
    desktop_repair_locked(r);
    desk_unlock();
}

void espix_desktop_repaint(void)
{
    espix_canvas_t *c = espix_display_canvas();
    if (c == NULL) {
        return;
    }
    desktop_repair((espix_rect_t){ 0, 0, espix_canvas_width(c),
                                    espix_canvas_height(c) });
}

espix_window_t *espix_window_new(int x, int y, int w, int h, const char *title)
{
    if (w <= 2 * PAD || h <= TITLE_H + 2 * PAD || s_nwin >= WIN_MAX) {
        return NULL;
    }

    /*
     * Kept in the work area, so nothing is ever created with its title bar under
     * the taskbar and no way to reach it. A window taller than the work area is
     * put at the top and allowed to run under the bar, which is the one case
     * where there is nowhere better to put it.
     */
    espix_canvas_t *c = espix_display_canvas();
    if (c != NULL) {
        const int ww = espix_canvas_width(c);
        const int wh = espix_canvas_height(c) - TASKBAR_H;

        if (x + w > ww) { x = ww - w; }
        if (y + h > wh) { y = wh - h; }
        if (x < 0)      { x = 0; }
        if (y < 0)      { y = 0; }
    }

    espix_window_t *win = calloc(1, sizeof(*win));
    if (win == NULL) {
        return NULL;
    }
    win->surf = espix_surface_new(w, h);
    if (win->surf == NULL) {
        free(win);
        return NULL;
    }

    win->x   = x;
    win->y   = y;
    win->w   = w;
    win->h   = h;
    win->seq = ++s_win_seq;
    snprintf(win->title, sizeof(win->title), "%s",
             title != NULL ? title : "window");

    s_wins[s_nwin++] = win;

    /*
     * Painted once here, so the surface is never the heap's leftovers -- the
     * frame only, since there is no draw callback yet. A window whose content
     * arrives later asks for the whole of itself once with
     * espix_window_repaint().
     */
    window_paint(win, (espix_rect_t){ 0, 0, w, h });
    espix_window_focus(win);
    return win;
}

void espix_window_free(espix_window_t *w)
{
    if (w == NULL) {
        return;
    }
    for (int i = 0; i < s_nwin; i++) {
        if (s_wins[i] != w) {
            continue;
        }
        for (int j = i; j < s_nwin - 1; j++) {
            s_wins[j] = s_wins[j + 1];
        }
        s_wins[--s_nwin] = NULL;
        break;
    }
    if (s_focus == w) {
        s_focus = (s_nwin > 0) ? s_wins[s_nwin - 1] : NULL;
    }
    espix_surface_free(w->surf);
    free(w);
    espix_desktop_repaint();
}

void espix_window_set_ctx(espix_window_t *w, void *ctx) { w->ctx = ctx; }
void espix_window_set_draw(espix_window_t *w, espix_window_draw_fn fn) { w->draw = fn; }
void espix_window_set_key(espix_window_t *w, espix_window_key_fn fn) { w->key = fn; }
void espix_window_set_resize(espix_window_t *w, espix_window_resize_fn fn) { w->resize = fn; }
void espix_window_set_pointer(espix_window_t *w, espix_window_pointer_fn fn) { w->pointer = fn; }

/* A new surface, because the surface is the window -- frame included -- so a
 * bigger window is not a bigger blit of a smaller buffer. */
bool espix_window_resize(espix_window_t *w, int x, int y, int width, int height)
{
    espix_canvas_t *c = espix_display_canvas();

    if (w == NULL || c == NULL || width <= 2 * PAD || height <= TITLE_H + 2 * PAD) {
        return false;
    }
    /* Kept on the screen, like a new one: a window wider than the canvas is a
     * window you cannot see the edge of. */
    if (width > espix_canvas_width(c)) {
        width = espix_canvas_width(c);
    }
    if (height > espix_canvas_height(c) - TASKBAR_H) {
        height = espix_canvas_height(c) - TASKBAR_H;
    }
    if (w->x == x && w->y == y && w->w == width && w->h == height) {
        return true;
    }

    espix_surface_t *surf = espix_surface_new(width, height);
    if (surf == NULL) {
        return false;
    }

    const espix_rect_t was = { w->x, w->y, w->w, w->h };

    desk_lock();
    espix_surface_t *old = w->surf;
    w->surf = surf;
    w->x    = x;
    w->y    = y;
    w->w    = width;
    w->h    = height;

    /* Before anything is drawn into it or composited out of it. */
    if (w->resize != NULL) {
        w->resize(w, espix_window_content(w), w->ctx);
    }

    espix_surface_free(old);
    desk_unlock();

    /* The frame and the content into the new surface, then the union of where it
     * was and where it is -- the first covers the new rectangle, the second the
     * part of the old one it no longer covers. */
    espix_window_repaint(w);

    const espix_rect_t now = { w->x, w->y, w->w, w->h };
    const int x0 = was.x < now.x ? was.x : now.x;
    const int y0 = was.y < now.y ? was.y : now.y;
    const int x1 = (was.x + was.w) > (now.x + now.w) ? (was.x + was.w)
                                                     : (now.x + now.w);
    const int y1 = (was.y + was.h) > (now.y + now.h) ? (was.y + was.h)
                                                     : (now.y + now.h);
    desktop_repair((espix_rect_t){ x0, y0, x1 - x0, y1 - y0 });
    return true;
}

/* The array order is the z-order, so raising is a move to the end. True when
 * the order actually changed, which is what decides whether anything must be
 * recomposited. */
static bool window_raise_raw(espix_window_t *w)
{
    int i = 0;
    for (; i < s_nwin; i++) {
        if (s_wins[i] == w) {
            break;
        }
    }
    if (i >= s_nwin || i == s_nwin - 1) {
        return false;
    }
    for (int j = i; j < s_nwin - 1; j++) {
        s_wins[j] = s_wins[j + 1];
    }
    s_wins[s_nwin - 1] = w;
    return true;
}

void espix_window_raise(espix_window_t *w)
{
    if (w == NULL) {
        return;
    }
    if (window_raise_raw(w)) {
        /*
         * The window's own rectangle, not the canvas.
         *
         * Raising changes what is visible *inside* a window's rectangle and
         * nothing outside it, so repairing the whole screen was paying for a
         * screen to redraw one window. On the wire that is a full frame -- a
         * couple of hundred kilobytes -- which is why clicking anywhere with a
         * window open froze the view for half a second while it arrived, and
         * why clicking with no window open did not.
         */
        desktop_repair((espix_rect_t){ w->x, w->y, w->w, w->h });
    }
}

/* The title bar is the only part of a window whose look follows the focus. */
static espix_rect_t window_title(const espix_window_t *w)
{
    return (espix_rect_t){ 0, 0, w->w, TITLE_H };
}

/*
 * Focus without raising, which is what makes crossing a border cheap: the
 * compositing order has not changed, so nothing has to be recomposited -- two
 * title bars are repainted and that is the whole of it. Raising is the pointer's
 * *press* doing it, below.
 */
static void focus_draw(espix_window_t *was, espix_window_t *hit)
{
    if (was != NULL) {
        espix_window_damage(was, window_title(was));
        task_button_damage(was);
    }
    if (hit != NULL) {
        espix_window_damage(hit, window_title(hit));
        task_button_damage(hit);
    }
}

void espix_window_focus(espix_window_t *w)
{
    if (s_focus == w) {
        return;
    }
    espix_window_t *was = s_focus;
    s_focus = w;
    focus_draw(was, w);
}

/* The overlap of two rectangles, which is empty when they do not meet. */
static espix_rect_t rect_meet(espix_rect_t a, espix_rect_t b)
{
    const int x0 = a.x > b.x ? a.x : b.x;
    const int y0 = a.y > b.y ? a.y : b.y;
    const int x1 = (a.x + a.w) < (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    const int y1 = (a.y + a.h) < (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);

    return (espix_rect_t){ x0, y0, x1 - x0, y1 - y0 };
}

/* `r` without the part of it inside `hole`, as at most four rectangles. */
static size_t rect_cut(espix_rect_t r, espix_rect_t hole, espix_rect_t *out)
{
    if (hole.w <= 0 || hole.h <= 0 ||
        hole.x >= r.x + r.w || hole.x + hole.w <= r.x ||
        hole.y >= r.y + r.h || hole.y + hole.h <= r.y) {
        out[0] = r;
        return 1;
    }

    size_t n = 0;

    if (hole.y > r.y) {
        out[n++] = (espix_rect_t){ r.x, r.y, r.w, hole.y - r.y };
    }
    if (hole.y + hole.h < r.y + r.h) {
        out[n++] = (espix_rect_t){ r.x, hole.y + hole.h, r.w,
                                   r.y + r.h - (hole.y + hole.h) };
    }

    const int y0 = hole.y > r.y ? hole.y : r.y;
    const int y1 = (hole.y + hole.h) < (r.y + r.h) ? (hole.y + hole.h)
                                                   : (r.y + r.h);

    if (hole.x > r.x) {
        out[n++] = (espix_rect_t){ r.x, y0, hole.x - r.x, y1 - y0 };
    }
    if (hole.x + hole.w < r.x + r.w) {
        out[n++] = (espix_rect_t){ hole.x + hole.w, y0,
                                   r.x + r.w - (hole.x + hole.w), y1 - y0 };
    }
    return n;
}

void espix_window_move(espix_window_t *w, int x, int y)
{
    if (w == NULL || (w->x == x && w->y == y)) {
        return;
    }

    /*
     * Only what the old and new positions cover. What was underneath the window
     * is not known -- that is what occlusion costs -- but it does not have to be
     * known *everywhere*, which is the difference between repairing two window-
     * sized rectangles and repairing the canvas.
     */
    const espix_rect_t was = { w->x, w->y, w->w, w->h };
    const espix_rect_t now = { x, y, w->w, w->h };
    const bool         topmost = (s_nwin > 0 && s_wins[s_nwin - 1] == w);

    w->x = x;
    w->y = y;

    /* In a drag, motions inside one tick are one motion: the window has moved
     * and the screen has not been told yet. See DRAG_TICK_US. */
    if (s_dragging && topmost &&
        esp_timer_get_time() - s_drag_repair_at < DRAG_TICK_US) {
        return;
    }

    /* Where the screen has the window -- during a drag, where it was when we
     * last caught up rather than where it was a moment ago. Measuring the move
     * from there is what keeps the copy and the strips from overlapping. */
    const espix_rect_t from = (s_dragging && topmost) ? s_drag_shown : was;

    if (s_dragging && topmost) {
        s_drag_repair_at = esp_timer_get_time();
        s_drag_shown     = now;
    }

    const int x0 = from.x < now.x ? from.x : now.x;
    const int y0 = from.y < now.y ? from.y : now.y;
    const int x1 = (from.x + from.w) > (now.x + now.w) ? (from.x + from.w)
                                                       : (now.x + now.w);
    const int y1 = (from.y + from.h) > (now.y + now.h) ? (from.y + from.h)
                                                       : (now.y + now.h);

    const espix_rect_t box = { x0, y0, x1 - x0, y1 - y0 };
    const espix_rect_t ov  = rect_meet(from, now);
    const espix_rect_t dst = { ov.x + (x - from.x), ov.y + (y - from.y),
                               ov.w, ov.h };

    espix_canvas_t *c = espix_display_canvas();

    /*
     * The fast path, and the one a drag always takes: the press raised the
     * window, so it is on top, the overlap holds nothing but its own pixels, and
     * sliding those across is both correct and about ten times cheaper than
     * painting them again.
     *
     * Which was the whole of the problem. Repainting that rectangle is a fill of
     * the union and a blit of the window over it -- measured at 5.7 ms a motion,
     * with a motion arriving every few milliseconds, so the window could not keep
     * up with the pointer. That is what still slow meant.
     */
    /*
     * Painted first, and then noted, which is the order that matters.
     *
     * There is a faster version of this that slides the pixels across the canvas
     * with a memmove and repairs only the strips, and it is *not* used, because
     * it is wrong: it moves the pixels and records nothing, so the client is told
     * about the strips and not about the rectangle between them, and a dragged
     * window smears until something else repaints it -- which is exactly what
     * chasing the drag around with other windows looked like.
     *
     * It also would not have been faster. Measured: 5.8 ms a motion either way,
     * because both are 600 KB of PSRAM traffic and that, not the drawing, is the
     * wall. So the note below is the part worth keeping: a client that can be
     * told to move its own pixels is the only reason a drag is affordable, and
     * that part works.
     */
    desktop_repair(box);

    if (c != NULL && ov.w > 0 && ov.h > 0) {
        espix_canvas_lock(c);
        espix_canvas_moved(c, dst, ov.x, ov.y);
        espix_canvas_unlock(c);
    }
    (void)rect_cut;
}

/*
 * Focus follows the pointer, and the focused window comes to the front. That is
 * a policy, not a law -- click-to-focus is the other one -- and this is the one
 * that needs no button state, which is the piece the input path does not carry
 * yet. Raising only when the focus *changes* keeps a full repaint off the
 * pointer-move path.
 */
static void focus_at(int x, int y)
{
    espix_window_t *hit = window_at(x, y);

    if (hit == s_focus) {
        return;
    }

    espix_window_t *was = s_focus;
    s_focus = hit;

    /*
     * Two title bars and nothing else, because focusing reorders nothing --
     * raising is the pointer's *press* doing that, below. So crossing a window
     * edge costs two title bars rather than a recomposite, which is the
     * difference between a pointer that feels continuous and one that does not.
     */
    focus_draw(was, hit);
}

/* ------------------------------------------------------------------ */
/* The launcher square, and the window it opens                       */
/* ------------------------------------------------------------------ */

/*
 * A hardcoded path, deliberately. There is no file browser and no launcher, and
 * the point of this is the smallest thing that proves the shape: something on
 * the desktop, clicked with the mouse, that puts a window on the screen. A path
 * from somewhere else is one line, once there is a somewhere else.
 */
#define PHOTO_PATH "/home/esp/test.jpg"

static espix_window_t  *s_img_win;
static espix_surface_t *s_img;          /* decoded once, then kept */
static bool             s_img_tried;

/*
 * Read the file and decode it, once. The whole file is in memory before the
 * decode because that is what the decoder takes -- reading it is not the display
 * service's job -- and because a JPEG is read forwards, backwards and twice.
 */
static bool photo_load(void)
{
    if (s_img != NULL) {
        return true;
    }
    if (s_img_tried) {
        return false;             /* said why once already */
    }
    s_img_tried = true;

    FILE *f = fopen(PHOTO_PATH, "rb");
    if (f == NULL) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "%s: %s", PHOTO_PATH, strerror(errno));
        return false;
    }

    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *buf = NULL;
    if (n > 4 && n < (4 << 20)) {
        buf = heap_caps_malloc((size_t)n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buf != NULL && fread(buf, 1, (size_t)n, f) != (size_t)n) {
            heap_caps_free(buf);
            buf = NULL;
        }
    }
    fclose(f);

    if (buf == NULL) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "%s: cannot read %ld bytes",
                   PHOTO_PATH, n);
        return false;
    }

    const int64_t t0 = esp_timer_get_time();
    s_img = espix_image_jpeg(buf, (size_t)n);
    const int64_t t1 = esp_timer_get_time();
    heap_caps_free(buf);

    if (s_img == NULL) {
        return false;
    }

    espix_klog(ESPIX_KLOG_INFO, TAG, "%s: %dx%d decoded in %lld ms",
               PHOTO_PATH, espix_surface_width(s_img),
               espix_surface_height(s_img),
               (long long)((t1 - t0) / 1000));
    return true;
}

/*
 * Fitting the picture to the window.
 *
 * Three rules, and they are the ones an image viewer has: scaled down if it is
 * too big, left alone if it is not, and centred either way on a neutral
 * background. Never scaled *up* -- a 64-pixel icon blown up to fill a maximised
 * window is a decision nobody asked for, and "it fits, so show it" is what a
 * viewer is for.
 *
 * Done once, into a surface of its own, rather than on every repaint: a drag
 * repaints a window several times a second and would otherwise rescale a
 * photograph each time. PPA SRM with the scale factors set where there is
 * hardware, and a nearest-neighbour loop where there is not.
 */
static espix_surface_t *s_img_fit;

static void photo_fit(espix_window_t *w)
{
    if (s_img == NULL) {
        return;
    }

    const espix_rect_t box = espix_window_content(w);
    const int          iw  = espix_surface_width(s_img);
    const int          ih  = espix_surface_height(s_img);

    int fw = iw;
    int fh = ih;

    if (fw > box.w || fh > box.h) {
        /* One factor for both axes, or the picture is stretched. */
        if ((int64_t)box.w * ih <= (int64_t)box.h * iw) {
            fw = box.w;
            fh = (int)((int64_t)ih * box.w / iw);
        } else {
            fh = box.h;
            fw = (int)((int64_t)iw * box.h / ih);
        }
    }
    if (fw < 1) { fw = 1; }
    if (fh < 1) { fh = 1; }

    espix_surface_free(s_img_fit);
    s_img_fit = espix_surface_new(fw, fh);
    if (s_img_fit == NULL) {
        return;                     /* photo_draw() says so on the screen */
    }
    (void)espix_surface_scale(s_img_fit, s_img);
}

static void photo_resize(espix_window_t *w, espix_rect_t content, void *ctx)
{
    (void)content;
    (void)ctx;
    photo_fit(w);
}

/* The neutral around a picture that does not fill its window. */
#define COL_PHOTO_BG RGB565(0x50, 0x54, 0x5A)

static void photo_draw(espix_window_t *w, espix_surface_t *s, espix_rect_t r,
                       void *ctx)
{
    (void)ctx;
    const espix_rect_t c = espix_window_content(w);

    if (s_img == NULL || s_img_fit == NULL) {
        if (r.y < c.y + CELL_H) {
            espix_surface_text(s, c.x, c.y, "no image", COL_TEXT_FG, COL_WIN_BG);
        }
        return;
    }

    /* Only what the region reaches, so a drag pays for the pixels it uncovers
     * rather than for the window. */
    const int bx0 = c.x > r.x ? c.x : r.x;
    const int by0 = c.y > r.y ? c.y : r.y;
    const int bx1 = (c.x + c.w) < (r.x + r.w) ? (c.x + c.w) : (r.x + r.w);
    const int by1 = (c.y + c.h) < (r.y + r.h) ? (c.y + c.h) : (r.y + r.h);

    if (bx1 > bx0 && by1 > by0) {
        espix_surface_fill(s, (espix_rect_t){ bx0, by0, bx1 - bx0, by1 - by0 },
                           COL_PHOTO_BG);
    }

    const int fw = espix_surface_width(s_img_fit);
    const int fh = espix_surface_height(s_img_fit);

    /* op_blit() clips it to the surface, so the whole picture every time is
     * right as well as cheap -- it is one PPA SRM call. */
    espix_surface_blit(s, c.x + (c.w - fw) / 2, c.y + (c.h - fh) / 2,
                       espix_surface_pixels(s_img_fit), fw, fh, fw);
}

static void photo_open(void)
{
    if (s_img_win == NULL) {
        const bool got = photo_load();

        /* A window either way: one that says why there is no picture is more
         * use than a click that appears to do nothing. */
        const int iw = got ? espix_surface_width(s_img) : 160;
        const int ih = got ? espix_surface_height(s_img) : 24;

        /*
         * No bigger than the work area, whatever the picture is. A 4000-pixel
         * photograph asked for a window 4000 pixels wide, which cannot be shown
         * and whose edges cannot be reached; the picture fits itself to whatever
         * it gets instead.
         */
        espix_canvas_t *c  = espix_display_canvas();
        const int       cw = (c != NULL) ? espix_canvas_width(c) : iw;
        const int       ch = (c != NULL) ? espix_canvas_height(c) - TASKBAR_H : ih;

        int ww = iw + 2 * PAD;
        int wh = ih + TITLE_H + 2 * PAD;
        if (ww > cw - 16) { ww = cw - 16; }
        if (wh > ch - 16) { wh = ch - 16; }

        s_img_win = espix_window_new(120, 120, ww, wh, "photo");
        if (s_img_win == NULL) {
            return;
        }
        espix_window_set_draw(s_img_win, photo_draw);
        espix_window_set_resize(s_img_win, photo_resize);
        photo_fit(s_img_win);
        espix_window_repaint(s_img_win);
    }

    window_present(s_img_win);
}

/* ------------------------------------------------------------------ */
/* The windows that ship with it                                      */
/* ------------------------------------------------------------------ */

/*
 * A terminal window: a real shell session, drawn by the same espix_term the
 * on-screen console uses. It is the reason this is a desktop rather than a
 * background -- and the difference between a window that echoes keys and one
 * you can work in.
 *
 * The task is the terminal's, not the desktop's: a shell blocks in read_line
 * for as long as nobody types, so running one on the input path would freeze the
 * desktop. Input arrives from the display's callback and is pushed into the
 * terminal's queue; output arrives on this task and paints the window.
 */
#define TERM_COLS 76
#define TERM_ROWS 28

static espix_window_t *s_term;          /* the window */
static espix_term_t   *s_shell;         /* the terminal inside it */
static TaskHandle_t    s_term_task;
static uint32_t        s_term_gen;

/* The rectangle one character cell occupies, in surface coordinates. */
static espix_rect_t term_cell(const espix_window_t *w, int row, int col)
{
    const espix_rect_t c = espix_window_content(w);
    return (espix_rect_t){ c.x + col * CELL_W, c.y + row * CELL_H, CELL_W, CELL_H };
}

/* The grid's size is the terminal's, not the constant it was created with:
 * maximising the window makes it a different shape. */
static espix_rect_t term_grid_rect(const espix_window_t *w)
{
    const espix_rect_t c    = espix_window_content(w);
    const int          cols = s_shell != NULL ? espix_term_cols(s_shell) : TERM_COLS;
    const int          rows = s_shell != NULL ? espix_term_rows(s_shell) : TERM_ROWS;

    return (espix_rect_t){ c.x, c.y, cols * CELL_W, rows * CELL_H };
}

/*
 * The terminal's view, into the window's surface.
 *
 * Three locks, in one order, each there for a different reason. The desktop's
 * own lock is what makes the window pointer mean anything: the window can be
 * closed from the input task while this task is halfway through a row, and the
 * close takes the same lock before it clears the pointer. The surface lock is
 * what stops this interleaving with the compositor reading the surface -- and it
 * is released before the damage, because damaging a window repaints it, which
 * takes the surface lock again, and that one is not recursive.
 */
static void term_view_row(void *ctx, int row, const char *cells, int len)
{
    (void)ctx;

    desk_lock();
    if (s_term != NULL && s_shell != NULL) {
        char line[ESPIX_TERM_MAX_COLS + 1];

        if (len > ESPIX_TERM_MAX_COLS) {
            len = ESPIX_TERM_MAX_COLS;
        }
        memcpy(line, cells, (size_t)len);
        line[len] = '\0';

        espix_window_t    *w = s_term;
        espix_surface_t   *s = espix_window_surface(w);
        const espix_rect_t c = espix_window_content(w);

        espix_surface_lock(s);
        espix_surface_text(s, c.x, c.y + row * CELL_H, line, COL_TEXT_FG,
                           COL_WIN_BG);
        espix_surface_unlock(s);

        espix_window_damage(w, (espix_rect_t){ c.x, c.y + row * CELL_H,
                                               espix_term_cols(s_shell) * CELL_W,
                                               CELL_H });
    }
    desk_unlock();
}

static void term_view_cell(void *ctx, int row, int col, char ch)
{
    (void)ctx;

    desk_lock();
    if (s_term != NULL && s_shell != NULL) {
        const char cell[2] = { ch, '\0' };

        espix_window_t    *w = s_term;
        espix_surface_t   *s = espix_window_surface(w);
        const espix_rect_t c = espix_window_content(w);

        espix_surface_lock(s);
        espix_surface_text(s, c.x + col * CELL_W, c.y + row * CELL_H, cell,
                           COL_TEXT_FG, COL_WIN_BG);
        espix_surface_unlock(s);

        espix_window_damage(w, term_cell(w, row, col));
    }
    desk_unlock();
}

static void term_view_clear(void *ctx)
{
    (void)ctx;

    desk_lock();
    if (s_term != NULL && s_shell != NULL) {
        espix_window_t  *w = s_term;
        espix_surface_t *s = espix_window_surface(w);
        const espix_rect_t g = term_grid_rect(w);

        espix_surface_lock(s);
        espix_surface_fill(s, g, COL_WIN_BG);
        espix_surface_unlock(s);

        espix_window_damage(w, g);
    }
    desk_unlock();
}

static const espix_term_view_t s_term_view = {
    .ctx   = NULL,
    .cell  = term_view_cell,
    .row   = term_view_row,
    .clear = term_view_clear,
};

/*
 * Redrawing from the model, for a window being composited. The surface lock is
 * already held by the caller and is not recursive, so this reads the grid
 * through espix_term's own accessors rather than through the view.
 */
static void term_draw(espix_window_t *w, espix_surface_t *s, espix_rect_t r,
                      void *ctx)
{
    (void)ctx;
    if (s_shell == NULL) {
        return;
    }
    const espix_rect_t c    = espix_window_content(w);
    const int          cols = espix_term_cols(s_shell);
    const int          rows = espix_term_rows(s_shell);

    for (int row = 0; row < rows; row++) {
        const int y = c.y + row * CELL_H;
        if (y + CELL_H <= r.y || y >= r.y + r.h) {
            continue;
        }
        const char *cells = espix_term_row_text(s_shell, row);
        if (cells == NULL) {
            continue;
        }
        char line[ESPIX_TERM_MAX_COLS + 1];
        memcpy(line, cells, (size_t)cols);
        line[cols] = '\0';
        espix_surface_text(s, c.x, y, line, COL_TEXT_FG, COL_WIN_BG);
    }
}

/* A bigger window is a bigger grid. Nothing tells the shell: it writes and the
 * grid wraps at whatever width it now has, which is what a terminal does. */
static void term_resize(espix_window_t *w, espix_rect_t content, void *ctx)
{
    (void)w;
    (void)ctx;

    if (s_shell != NULL) {
        (void)espix_term_resize(s_shell, content.w / CELL_W,
                                content.h / CELL_H);
    }
}

static void term_key(espix_window_t *w, uint32_t keysym, bool down, void *ctx)
{
    (void)w;
    (void)ctx;

    if (s_shell != NULL) {
        espix_term_key(s_shell, keysym, down);
    }
}

static void term_task(void *arg)
{
    const uint32_t gen = (uint32_t)(uintptr_t)arg;
    espix_term_t  *t   = s_shell;

    if (t != NULL) {
        espix_session_t *s = espix_term_session(t);

        s->name = "term0";
        /* The esp account, like the VNC console: a window on a screen someone
         * reached over the network is not the serial cable. */
        s->uid  = 1000;
        s->gid  = 1000;
        strlcpy(s->user, "esp", sizeof(s->user));
        strlcpy(s->home, "/home/esp", sizeof(s->home));

        espix_term_run(t);
    }

    /* This task is the only thing that knows it has stopped touching the grid,
     * so it owns the freeing -- the same bargain the console makes. */
    espix_term_free(t);

    desk_lock();
    if (gen == s_term_gen) {
        s_term_task = NULL;
    }
    desk_unlock();

    vTaskDeleteWithCaps(NULL);
}

static void term_start(void)
{
    s_term = espix_window_new(40, 40,
                              TERM_COLS * CELL_W + 2 * PAD,
                              TERM_ROWS * CELL_H + TITLE_H + 2 * PAD,
                              "terminal");
    if (s_term == NULL) {
        return;
    }

    s_shell = espix_term_new(&s_term_view, TERM_COLS, TERM_ROWS, "esp");
    if (s_shell == NULL) {
        espix_window_free(s_term);
        s_term = NULL;
        return;
    }

    espix_window_set_draw(s_term, term_draw);
    espix_window_set_key(s_term, term_key);
    espix_window_set_resize(s_term, term_resize);

    /*
     * A new window's *surface* is painted by espix_window_new, and the canvas is
     * not: the first frame shows the title bar (focus did that) and nothing
     * else until something composites the window. On the first start the claim
     * repaints everything, which hid that -- but a terminal reopened from the
     * start menu got no such repaint and showed its title bar and its text
     * cells, with no background and no border, until it was moved.
     */
    espix_window_repaint(s_term);

    void *const gen = (void *)(uintptr_t)++s_term_gen;

    if (xTaskCreateWithCaps(term_task, "espix:term", 8192, gen, 4, &s_term_task,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        (void)xTaskCreateWithCaps(term_task, "espix:term", 8192, gen, 4,
                                  &s_term_task,
                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (s_term_task == NULL) {
        espix_term_free(s_shell);
        s_shell = NULL;
    }
}

/*
 * Stop the shell and take the window with it.
 *
 * The window pointer is cleared under the desktop's lock before the window is
 * freed, and the view checks it under that same lock -- so a task halfway
 * through painting a row either finishes first or sees NULL and draws nothing.
 * Without that ordering this is a window freed out from under the task that is
 * drawing into its surface.
 *
 * The terminal itself is the task's to free, and it may take a moment: it is
 * blocked in read_line, which notices the stop within its 100 ms poll.
 */
static void term_stop(void)
{
    espix_term_t *t;

    desk_lock();
    t = s_shell;
    s_shell = NULL;
    if (s_term != NULL) {
        espix_window_t *w = s_term;
        s_term = NULL;
        espix_window_free(w);
    }
    desk_unlock();

    if (t == NULL) {
        return;
    }

    espix_term_stop(t);

    /*
     * And then let it go: nothing here waits for the task, because there is
     * nothing left that needs it to have finished. The window is already gone,
     * the view checks both pointers under the lock, so the task cannot touch a
     * freed surface -- it only has its own grid to free, which it does on its
     * way out.
     *
     * It is worth saying why, because the obvious thing is to wait and the
     * obvious thing is wrong twice over. This runs from the input handler, which
     * is inside the desktop's lock -- and that lock is recursive, so unlocking
     * here does not release it. A wait would be a two-second hold on the lock
     * that the task needs to finish, every time a terminal window is closed.
     */
}

/* Open the terminal, or bring back the one that is already there. Closing a
 * window should not be the end of the application in it. */
static void term_open(void)
{
    if (s_term == NULL) {
        term_start();
    }
    window_present(s_term);
}

/*
 * A second window with nothing in it but a few lines. It is here for the
 * overlap: z-order, focus and the raise that follows the pointer are only
 * visible with two things on the screen.
 */
static espix_window_t *s_about;

static void about_draw(espix_window_t *w, espix_surface_t *s, espix_rect_t r,
                       void *ctx)
{
    (void)ctx;
    const espix_rect_t c = espix_window_content(w);
    static const char *const lines[] = {
        "espix desktop",
        "",
        "focus follows the pointer",
        "and raises what it lands on",
        "",
        "surfaces, composited",
    };

    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        const int y = c.y + (int)i * CELL_H;
        if (y + CELL_H <= r.y || y >= r.y + r.h) {
            continue;                   /* not in the region */
        }
        espix_surface_text(s, c.x, y, lines[i], COL_TEXT_FG, COL_WIN_BG);
    }
}

/* ------------------------------------------------------------------ */
/* The taskbar                                                         */
/* ------------------------------------------------------------------ */

/*
 * A strip along the bottom, because that is where a desktop has kept the
 * launcher, the windows and the time since before this one existed.
 *
 * The round mark on the launcher is *drawn*, not copied. Espressif's own logo is
 * a trademark, and Apache-2.0 -- the licence the file lives under -- says in as
 * many words that it grants no rights to trade marks or product names. So this
 * is ours: a disc with a letter on it, in the colour anybody associates with
 * this silicon. Putting the real one there is a decision for whoever ships this,
 * not a line to slip in here.
 */
#define COL_BAR      RGB565(0x18, 0x1C, 0x24)
#define COL_BAR_EDGE RGB565(0x3A, 0x42, 0x52)
#define COL_BTN      RGB565(0x2A, 0x30, 0x3A)
#define COL_TRAY     RGB565(0x12, 0x15, 0x1B)
#define COL_TRAY_FG  RGB565(0xD0, 0xDE, 0xEC)
#define COL_MENU     RGB565(0x22, 0x28, 0x32)
#define COL_MENU_HOT RGB565(0x4C, 0x6E, 0xA8)
#define COL_LOGO     RGB565(0xE0, 0x2E, 0x32)
#define COL_LOGO_FG  RGB565(0xFF, 0xFF, 0xFF)

static char               s_clock[8];       /* "HH:MM", or "--:--" until set */
static bool               s_menu;
static int                s_menu_hot = -1;
static esp_timer_handle_t s_clock_timer;

static bool in_rect(espix_rect_t r, int x, int y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static bool rects_overlap(espix_rect_t a, espix_rect_t b)
{
    return a.x < b.x + b.w && b.x < a.x + a.w &&
           a.y < b.y + b.h && b.y < a.y + a.h;
}

/* The bar's strip of canvas, and the line windows stop at. */
static espix_rect_t bar_rect(void)
{
    espix_canvas_t *c = espix_display_canvas();
    if (c == NULL) {
        return (espix_rect_t){ 0, 0, 0, 0 };
    }
    return (espix_rect_t){ 0, espix_canvas_height(c) - TASKBAR_H,
                           espix_canvas_width(c), TASKBAR_H };
}

static int work_bottom(void)
{
    espix_canvas_t *c = espix_display_canvas();
    return (c == NULL) ? 0 : espix_canvas_height(c) - TASKBAR_H;
}

static espix_rect_t start_rect(void)
{
    const espix_rect_t b = bar_rect();
    return (espix_rect_t){ START_X, b.y + 2, START_W, TASKBAR_H - 4 };
}

static espix_rect_t tray_rect(void)
{
    espix_canvas_t    *c = espix_display_canvas();
    const espix_rect_t b = bar_rect();
    const int          w = (c == NULL) ? 0 : espix_canvas_width(c);
    return (espix_rect_t){ w - TRAY_W, b.y, TRAY_W, TASKBAR_H };
}

static const char *const s_menu_items[] = { "photo", "terminal", "settings",
                                           "about" };
#define MENU_N ((int)(sizeof(s_menu_items) / sizeof(s_menu_items[0])))

static espix_rect_t menu_rect(void)
{
    const espix_rect_t b = bar_rect();
    const int          h = MENU_N * ITEM_H + 6;
    return (espix_rect_t){ START_X, b.y - h, 132, h };
}

/*
 * A filled circle, one scanline at a time, which is all a canvas of rectangles
 * needs for one. It is 20-odd fills, once per full repaint, on a 22-pixel disc.
 */
static void disc_paint(espix_canvas_t *c, int cx, int cy, int rad, espix_px_t col)
{
    for (int dy = -rad; dy <= rad; dy++) {
        int dx = 0;
        while ((dx + 1) * (dx + 1) + dy * dy <= rad * rad) {
            dx++;
        }
        espix_canvas_fill(c, (espix_rect_t){ cx - dx, cy + dy, 2 * dx + 1, 1 },
                          col);
    }
}

static void start_paint(espix_canvas_t *c)
{
    const espix_rect_t b  = start_rect();
    const espix_px_t   bg = s_menu ? COL_TITLE_FOC : COL_BTN;
    const int          cx = b.x + b.w / 2;
    const int          cy = b.y + b.h / 2;

    espix_canvas_fill(c, b, bg);
    espix_canvas_outline(c, b, COL_BAR_EDGE);

    disc_paint(c, cx, cy, b.h / 2 - 2, COL_LOGO);
    /* The 8x8 cell sits inside the disc at this size, so the glyph's background
     * is the disc's colour and there is no square left over. */
    espix_canvas_text(c, cx - CELL_W / 2, cy - CELL_H / 2, "e",
                      COL_LOGO_FG, COL_LOGO);
}

static void tray_paint(espix_canvas_t *c)
{
    const espix_rect_t t = tray_rect();

    espix_canvas_fill(c, t, COL_TRAY);
    espix_canvas_fill(c, (espix_rect_t){ t.x, t.y, 1, t.h }, COL_BAR_EDGE);
    espix_canvas_text(c, t.x + (TRAY_W - 5 * CELL_W) / 2,
                      t.y + (TASKBAR_H - CELL_H) / 2, s_clock,
                      COL_TRAY_FG, COL_TRAY);
}

/*
 * One button per window, between the launcher and the tray. The geometry is
 * derived here once and read back for the hit test, because a button that is
 * drawn in one place and pressed in another is the whole class of bug this
 * avoids.
 */
/*
 * The windows in the order they were opened, which is the order the bar lists
 * them in -- not the z-order they are stacked in. A button that moves the moment
 * you click it is a button you have to find again, and it moves every time a
 * window is raised.
 */
static int task_order(espix_window_t **out)
{
    int n = 0;

    for (int i = 0; i < s_nwin; i++) {
        out[n++] = s_wins[i];
    }
    for (int i = 1; i < n; i++) {               /* insertion sort, and n <= 8 */
        espix_window_t *w = out[i];
        int             j = i - 1;

        while (j >= 0 && out[j]->seq > w->seq) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = w;
    }
    return n;
}

static bool task_at(int i, espix_rect_t *out, espix_window_t **win)
{
    espix_canvas_t *c = espix_display_canvas();
    espix_window_t *list[WIN_MAX];

    if (c == NULL || i < 0 || i >= task_order(list)) {
        return false;
    }

    const espix_rect_t s  = start_rect();
    const int          x1 = espix_canvas_width(c) - TRAY_W - 6;
    int                x  = s.x + s.w + 6;

    for (int k = 0; k <= i; k++) {
        espix_window_t *w  = list[k];
        int             tw = (int)strlen(w->title) * CELL_W + 16;

        if (tw > BUTTON_MAX) { tw = BUTTON_MAX; }
        if (x + tw > x1)     { tw = x1 - x; }
        if (tw < 24) {
            return false;
        }
        if (k == i) {
            if (out != NULL) {
                *out = (espix_rect_t){ x, s.y, tw, s.h };
            }
            if (win != NULL) {
                *win = w;
            }
            return true;
        }
        x += tw + 4;
    }
    return false;
}

static void taskbar_paint(espix_canvas_t *c, espix_rect_t r)
{
    const espix_rect_t b = bar_rect();

    if (c == NULL || b.h <= 0 || !rects_overlap(b, r)) {
        return;
    }

    const espix_rect_t d = rect_meet(b, r);

    espix_canvas_fill(c, d, COL_BAR);
    if (d.y == b.y) {
        espix_canvas_fill(c, (espix_rect_t){ d.x, b.y, d.w, 1 }, COL_BAR_EDGE);
    }

    if (rects_overlap(start_rect(), r)) {
        start_paint(c);
    }
    if (rects_overlap(tray_rect(), r)) {
        tray_paint(c);
    }

    for (int i = 0; i < s_nwin; i++) {
        espix_rect_t    t;
        espix_window_t *w;

        if (!task_at(i, &t, &w) || !rects_overlap(t, r)) {
            continue;
        }
        const espix_px_t bg  = w->hidden ? COL_BTN_DIM
                             : (w == s_focus) ? COL_TITLE_FOC : COL_BTN;
        const int        fit = (t.w - 16) / CELL_W;
        char             label[17];

        snprintf(label, sizeof(label), "%.*s", fit > 0 ? fit : 0, w->title);
        espix_canvas_fill(c, t, bg);
        espix_canvas_text(c, t.x + 8, t.y + (t.h - CELL_H) / 2, label,
                          COL_TITLE_FG, bg);
    }
}

/*
 * Repair the one task button that belongs to `w`.
 *
 * A focus change repaints two title bars and has to repaint the two buttons that
 * show which window is focused -- otherwise the bar goes on claiming the old
 * one is, which is what it did: move the pointer off a window and onto the
 * desktop and the title bar went grey while its button stayed lit.
 *
 * One button some eighty pixels wide rather than the whole bar, because this is
 * on the pointer path.
 */
static void task_button_damage(espix_window_t *w)
{
    for (int i = 0; i < s_nwin; i++) {
        espix_rect_t    t;
        espix_window_t *at;

        if (task_at(i, &t, &at) && at == w) {
            desktop_repair(t);
            return;
        }
    }
}

static int menu_hot(void)
{
    if (!s_menu || s_buttons != 0) {
        return -1;
    }
    const espix_rect_t m = menu_rect();
    if (!in_rect(m, s_cx, s_cy)) {
        return -1;
    }
    const int i = (s_cy - (m.y + 3)) / ITEM_H;
    return (i >= 0 && i < MENU_N) ? i : -1;
}

static void menu_paint(espix_canvas_t *c, espix_rect_t r)
{
    if (!s_menu || c == NULL) {
        return;
    }
    const espix_rect_t m = menu_rect();
    if (!rects_overlap(m, r)) {
        return;
    }

    espix_canvas_fill(c, m, COL_MENU);
    espix_canvas_outline(c, m, COL_BAR_EDGE);

    for (int i = 0; i < MENU_N; i++) {
        const espix_rect_t it = { m.x + 1, m.y + 3 + i * ITEM_H,
                                  m.w - 2, ITEM_H };
        const espix_px_t   bg = (i == s_menu_hot) ? COL_MENU_HOT : COL_MENU;

        espix_canvas_fill(c, it, bg);
        espix_canvas_text(c, it.x + 10, it.y + (ITEM_H - CELL_H) / 2,
                          s_menu_items[i], COL_TITLE_FG, bg);
    }
}

static void bar_damage(void)
{
    espix_canvas_t *c = espix_display_canvas();
    if (c == NULL) {
        return;
    }
    const espix_rect_t m = menu_rect();
    const int          h = espix_canvas_height(c);
    const int          y = m.y < work_bottom() ? m.y : work_bottom();

    desktop_repair((espix_rect_t){ 0, y, espix_canvas_width(c), h - y });
}

static void term_open(void);

static void menu_activate(int i)
{
    s_menu = false;

    if (strcmp(s_menu_items[i], "photo") == 0) {
        photo_open();
    } else if (strcmp(s_menu_items[i], "about") == 0) {
        if (s_about == NULL) {
            s_about = espix_window_new(300, 190, 220, 96, "about");
            if (s_about != NULL) {
                espix_window_set_draw(s_about, about_draw);
                espix_window_repaint(s_about);
            }
        }
        window_present(s_about);
    } else if (strcmp(s_menu_items[i], "terminal") == 0) {
        term_open();
    } else if (strcmp(s_menu_items[i], "settings") == 0) {
        window_present(espix_settings_open());
    }
    bar_damage();
}

/*
 * The clock, from the system clock the kernel already keeps -- the one SNTP
 * sets. Before that it reads 1970, and printing 00:00 with a straight face
 * would be worse than saying nothing, so it says nothing.
 */
static void clock_text(char *out, size_t len)
{
    const time_t now = time(NULL);
    struct tm    tm;

    if (now < 1600000000 || localtime_r(&now, &tm) == NULL) {
        snprintf(out, len, "--:--");
        return;
    }
    strftime(out, len, "%H:%M", &tm);
}

/*
 * Once a minute the bar changes, and once a minute this repaints the 64-pixel
 * column it changed in. The tick is every five seconds so the minute is never
 * more than five seconds late; the string comparison is what keeps it to one
 * repair rather than twelve.
 */
static void clock_tick(void *arg)
{
    (void)arg;

    char now[8];
    clock_text(now, sizeof(now));

    if (strcmp(now, s_clock) == 0 || !desktop_on_screen()) {
        return;
    }
    snprintf(s_clock, sizeof(s_clock), "%s", now);

    espix_canvas_t *c = espix_display_canvas();
    if (c != NULL) {
        desk_lock();
        desktop_repair_locked(tray_rect());
        desk_unlock();
    }
}

/*
 * Minimise: the window stops being composited and stays in the list, and its
 * task button is where it comes back from. That is the whole reason the taskbar
 * was worth having before this existed -- a minimised window with nowhere to go
 * is a lost window.
 */
static void window_minimize(espix_window_t *w)
{
    if (w == NULL || w->hidden) {
        return;
    }

    const espix_rect_t was_drawn = { w->x, w->y, w->w, w->h };
    const bool         had_focus = (s_focus == w);

    w->hidden = true;

    if (had_focus) {
        /* Cleared first, because espix_window_focus() compares against it and
         * would otherwise decide there is nothing to do. */
        s_focus = NULL;
        espix_window_focus(window_top_visible());
    }

    desktop_repair(was_drawn);          /* what it was covering comes back */
    bar_damage();                       /* and its button changes colour */
}

static void window_restore(espix_window_t *w)
{
    if (w == NULL || !w->hidden) {
        return;
    }

    w->hidden = false;
    window_raise_raw(w);
    espix_window_focus(w);

    /* The whole screen, because a window that was not being composited could
     * have been anywhere -- and the repair is a fill and a blit per window, so
     * it is the cheap kind of full repaint. */
    espix_desktop_repaint();
    bar_damage();
}

/*
 * Maximise: the work area, or wherever it was.
 *
 * No rescaling is involved in the window itself -- a maximised window is a
 * bigger surface, which is a new buffer and not a stretched one. What the
 * *content* does about it is the content's business, and it is told: the terminal
 * resizes its grid and the viewer refits its picture. A window whose content has
 * no opinion simply gets more room.
 */
static void window_toggle_max(espix_window_t *w)
{
    espix_canvas_t *c = espix_display_canvas();

    if (w == NULL || c == NULL) {
        return;
    }

    if (w->maxed) {
        if (espix_window_resize(w, w->rest.x, w->rest.y, w->rest.w, w->rest.h)) {
            w->maxed = false;
        }
        return;
    }

    const espix_rect_t rest = { w->x, w->y, w->w, w->h };

    if (espix_window_resize(w, 0, 0, espix_canvas_width(c),
                            espix_canvas_height(c) - TASKBAR_H)) {
        w->rest  = rest;
        w->maxed = true;
    }
}

/*
 * Bring a window to the front, unhiding it if it was minimised.
 *
 * The one way anything opens a window on request: the menu, the launcher and the
 * taskbar all want this, and every one of them that forgot the unhiding made a
 * minimised window impossible to get back through that door.
 */
static void window_present(espix_window_t *w)
{
    if (w == NULL) {
        return;
    }
    if (w->hidden) {
        window_restore(w);
        return;
    }
    espix_window_focus(w);
    espix_window_raise(w);
}

/*
 * Close a window. Everything that holds a pointer to one has to forget it here,
 * or the next thing that looks it up -- a task button, the menu, a repaint --
 * is looking at freed memory. The terminal is the awkward case: its window is
 * freed by term_stop(), which is the only thing that knows the shell task has
 * stopped drawing into the surface.
 */
static void window_close(espix_window_t *w)
{
    if (w == NULL) {
        return;
    }
    if (w == s_term) {
        term_stop();                    /* takes the window with it */
        return;
    }
    if (w == s_img_win) {
        s_img_win = NULL;
    }
    if (w == s_about) {
        s_about = NULL;
    }
    if (w == espix_settings_window()) {
        espix_settings_forget();
    }
    espix_window_free(w);
}

static void windows_create(void)
{
    /* The window, the terminal inside it and the shell that runs in that. */
    term_start();

    s_about = espix_window_new(300, 190, 220, 96, "about");
    if (s_about != NULL) {
        espix_window_set_draw(s_about, about_draw);
        espix_window_repaint(s_about);
    }

    /*
     * The terminal is the one to type at, so it starts in front and focused.
     * Through espix_window_focus() rather than by assigning s_focus, because
     * creating the about window focused it -- and that painted the about's title
     * bar as the focused one and the terminal's as the unfocused one, which is
     * the state the pixels would otherwise still show.
     */
    if (s_term != NULL) {
        window_raise_raw(s_term);
        /* Not `s_focus = s_term`: that leaves the about window wearing the
         * focused title bar it was given when it was created. Going through
         * espix_window_focus() repaints both, because it knows which one is
         * losing the focus. */
        espix_window_focus(s_term);
    }
}

static void windows_destroy(void)
{
    /* Before the windows, not with them: the shell task is drawing into the
     * terminal window's surface, and term_stop() is what makes it stop. */
    term_stop();

    while (s_nwin > 0) {
        espix_window_t *w = s_wins[s_nwin - 1];
        espix_surface_free(w->surf);
        free(w);
        s_wins[--s_nwin] = NULL;
    }
    s_focus = NULL;
    s_term  = NULL;
    s_about = NULL;

    /*
     * The viewer's window is in that list too, and it used to be the one
     * pointer out of it that nobody cleared -- so a stop followed by a click on
     * the launcher took a Store access fault in espix_surface_lock, on a window
     * that had been freed a second earlier.
     */
    s_img_win = NULL;

    /*
     * And the picture goes with the windows it was for. "Decode once and keep
     * it" means once per session: holding a few hundred kilobytes of PSRAM for
     * a window that no longer exists is the opposite of what stopping is for,
     * and the codec decodes it again in 12 ms.
     */
    espix_surface_free(s_img);
    s_img = NULL;
    espix_surface_free(s_img_fit);
    s_img_fit   = NULL;
    s_img_tried = false;

    /* A drag that was in progress ends with the window it was holding. */
    s_drag    = NULL;
    s_buttons = 0;
}

/* ------------------------------------------------------------------ */
/* The screen owner                                                    */
/* ------------------------------------------------------------------ */

/*
 * A drag in progress. `s_buttons` is the last mask any event carried, because
 * only POINTER events have one: a motion report is not where a mouse's buttons
 * live, so the press and the release have to be recognised as changes in this
 * rather than looked for on the event that moves the window.
 */
static int            s_grab_x, s_grab_y;   /* where in the window it was grabbed */

/*
 * How often a drag lets the screen catch up.
 *
 * A mouse reports far more often than a screen changes, and every motion used to
 * cost a full repair -- 5.7 ms of a motion that costs seven, which is how a mouse
 * outruns a desktop. So motions arriving inside one tick of the last repair do
 * nothing but record where the window now is, and the next tick moves it there in
 * one go. Sixty a second, as a screen does.
 *
 * And it is not only the cost. Coalescing makes the copy one *net* move from
 * where the client last saw the window, which is what makes the geometry work
 * out: the strips sent for the space it left cannot land inside the rectangle the
 * copy reads from, because both are measured from the same old position. One
 * motion at a time is the only case that is correct, and this makes every case
 * one motion.
 */
static void drag_begin(espix_window_t *w, int x, int y)
{
    s_drag   = w;
    s_grab_x = x - w->x;
    s_grab_y = y - w->y;

    s_drag_repair_at = 0;               /* the first motion always repairs */
    s_drag_shown     = (espix_rect_t){ w->x, w->y, w->w, w->h };
    s_dragging       = true;
}

static void drag_end(void)
{
    if (s_drag == NULL) {
        return;                     /* a click, not a drag: nothing moved */
    }

    const espix_rect_t last  = s_drag_shown;
    const bool         moved = (s_drag->x != s_drag_shown.x ||
                                s_drag->y != s_drag_shown.y);

    s_drag     = NULL;
    s_dragging = false;

    /*
     * Catch up completely, and "completely" is the word that was missing.
     *
     * Inside a drag the window and the screen are allowed to differ -- that is
     * the whole of the coalescing -- so at the end there are *three* places that
     * matter: where the screen last drew it, where it has been moved to since,
     * and the space in between that was never drawn at all. Repairing only the
     * first leaves the window's pixels on the desktop; repairing only the last
     * leaves the window undrawn, which is a side of it chopped off in the
     * direction it was going. One full repair covers all three, once, at the end
     * of a drag, where 7 ms does not matter.
     */
    (void)last;
    if (moved) {
        espix_desktop_repaint();
    }
}

static void drag_to(int x, int y)
{
    /*
     * Kept on the screen, allowing the window to be pushed off all but a strip
     * of its title bar -- which is what makes it recoverable. A window dragged
     * entirely out of sight is a window you cannot get back without a command.
     */
    espix_canvas_t *c = espix_display_canvas();
    int nx = x - s_grab_x;
    int ny = y - s_grab_y;

    if (c != NULL) {
        const int cw = espix_canvas_width(c);
        const int ch = espix_canvas_height(c);

        if (nx + s_drag->w < 32) { nx = 32 - s_drag->w; }
        if (nx > cw - 32)        { nx = cw - 32; }
        if (ny < 0)              { ny = 0; }
        /* Stopped by the bar, not by the screen: the title bar stays reachable
         * and the taskbar stays a thing windows go under rather than through. */
        if (ny > ch - TASKBAR_H - TITLE_H) { ny = ch - TASKBAR_H - TITLE_H; }
    }

    espix_window_move(s_drag, nx, ny);
}

static void desktop_input_locked(void *ctx, const espix_input_event_t *ev)
{
    (void)ctx;

    if (ev->kind == ESPIX_INPUT_POINTER) {
        const uint8_t was = s_buttons;
        s_buttons = ev->buttons;

        cursor_put(ev->x, ev->y);

        if (was != 0 && s_buttons == 0) {
            /* To whoever took the press, even if the pointer has left it: a
             * button that never hears a release is a button stuck down. */
            if (s_press_win != NULL && s_press_win->pointer != NULL) {
                s_press_win->pointer(s_press_win, ev->x - s_press_win->x,
                                     ev->y - s_press_win->y, 0, s_press_win->ctx);
            }
            s_press_win = NULL;
            drag_end();                 /* the button came up */
            return;
        }
        if (was == 0 && s_buttons != 0) {
            /*
             * The bar is drawn over everything, so it is pressed before anything
             * under it -- and while the menu is open it swallows the click, so
             * that a click anywhere else closes it rather than pressing whatever
             * was behind it. That is the order a menu has, and getting it wrong
             * is a menu you cannot dismiss without choosing something.
             */
            if (s_menu) {
                const espix_rect_t m = menu_rect();

                if (in_rect(m, ev->x, ev->y)) {
                    const int i = (ev->y - (m.y + 3)) / ITEM_H;
                    if (i >= 0 && i < MENU_N) {
                        menu_activate(i);
                    }
                } else {
                    s_menu = false;
                    s_menu_hot = -1;
                    bar_damage();
                }
                return;
            }
            if (in_rect(start_rect(), ev->x, ev->y)) {
                s_menu = true;
                s_menu_hot = -1;
                bar_damage();
                return;
            }
            for (int i = 0; i < s_nwin; i++) {
                espix_rect_t    t;
                espix_window_t *w;

                if (task_at(i, &t, &w) && in_rect(t, ev->x, ev->y)) {
                    if (w->hidden) {
                        window_restore(w);
                    } else if (w == s_focus) {
                        /* Pressing the button of the window you are looking at
                         * puts it away, as every taskbar does. */
                        window_minimize(w);
                    } else {
                        espix_window_focus(w);
                        espix_window_raise(w);
                    }
                    return;
                }
            }
            /*
             * Close before focus, and on the window under the pointer rather
             * than on the focused one: the button belongs to the window it is
             * drawn on, so pressing it must close that one even when it was not
             * the focused window.
             */
            espix_window_t *titled = window_at(ev->x, ev->y);
            if (titled != NULL && window_close_hit(titled, ev->x, ev->y)) {
                window_close(titled);
                return;
            }
            if (titled != NULL && window_min_hit(titled, ev->x, ev->y)) {
                window_minimize(titled);
                return;
            }
            if (titled != NULL && window_max_hit(titled, ev->x, ev->y)) {
                window_toggle_max(titled);
                return;
            }
            focus_at(ev->x, ev->y);     /* focus first, so the title is right */
            if (s_focus != NULL) {
                /* The press is what raises, which is why a border crossing does
                 * not have to recomposite anything. */
                espix_window_raise(s_focus);

                const espix_rect_t c = espix_window_content(s_focus);

                if (ev->x >= s_focus->x + c.x &&
                    ev->x <  s_focus->x + c.x + c.w &&
                    ev->y >= s_focus->y + c.y &&
                    ev->y <  s_focus->y + c.y + c.h) {
                    /* In the content, so it is the client's: a panel of buttons
                     * is not a place to start dragging a window from. */
                    if (s_focus->pointer != NULL) {
                        const uint8_t b = ev->buttons;
                        s_focus->pointer(s_focus, ev->x - s_focus->x,
                                         ev->y - s_focus->y, b, s_focus->ctx);
                    }
                } else if (ev->y >= s_focus->y && ev->y < s_focus->y + TITLE_H) {
                    drag_begin(s_focus, ev->x, ev->y);
                }
            }
            return;
        }

        /* Held: either following the pointer or dragging what it is holding. */
        if (s_drag != NULL) {
            drag_to(ev->x, ev->y);
        } else if (s_buttons == 0) {
            focus_at(ev->x, ev->y);
        }
    } else if (ev->kind == ESPIX_INPUT_MOTION) {
        /*
         * Applied to where cursor_put() last put it, which is where the cursor
         * actually is. That makes an edge clamp rather than letting the delta
         * accumulate somewhere off-screen -- so pushing into a corner and
         * pulling back moves immediately, the way a mouse does.
         */
        cursor_put(s_cx + ev->x, s_cy + ev->y);

        /* Only when it changes: a highlight that repaints on every report of a
         * mouse crossing a menu is 132x66 of canvas per motion. */
        if (s_menu) {
            const int hot = menu_hot();
            if (hot != s_menu_hot) {
                s_menu_hot = hot;
                desktop_repair(menu_rect());
            }
        }

        if (s_drag != NULL) {
            drag_to(s_cx, s_cy);
        } else if (s_buttons == 0) {
            focus_at(s_cx, s_cy);
        }
    } else if (ev->kind == ESPIX_INPUT_KEY && s_focus != NULL &&
               s_focus->key != NULL) {
        s_focus->key(s_focus, ev->keysym, ev->down, s_focus->ctx);
    }
}

/*
 * What input costs, reported every hundred events.
 *
 * Here because a drag that is still slow after the bytes were cut 37-fold is a
 * question about *this*, not about the wire -- and three guesses at it had
 * already cost three flashes. The numbers say which half is the problem: if a
 * motion is milliseconds here, the repair is the cost and the fix is to move
 * pixels instead of repainting them; if it is microseconds here and the frame
 * task is the slow one, the answer is somewhere else entirely.
 */
static struct {
    uint32_t n;
    uint64_t us;
    int64_t  worst;
} s_input_stat[4];

static void input_stat(const espix_input_event_t *ev, int64_t us)
{
    const int kind = ((int)ev->kind >= 0 && (int)ev->kind < 4) ? (int)ev->kind : 3;

    s_input_stat[kind].n++;
    s_input_stat[kind].us += (uint64_t)us;
    if (us > s_input_stat[kind].worst) {
        s_input_stat[kind].worst = us;
    }
    if (s_input_stat[kind].n < 100) {
        return;
    }
    espix_klog(ESPIX_KLOG_INFO, TAG,
               "input kind %d: 100 events, mean %lld us, worst %lld us",
               kind, (long long)(s_input_stat[kind].us / 100),
               (long long)s_input_stat[kind].worst);
    s_input_stat[kind].n     = 0;
    s_input_stat[kind].us    = 0;
    s_input_stat[kind].worst = 0;
}

static void desktop_input(void *ctx, const espix_input_event_t *ev)
{
    const int64_t t0 = esp_timer_get_time();

    desk_lock();
    desktop_input_locked(ctx, ev);
    desk_unlock();

    input_stat(ev, esp_timer_get_time() - t0);
}

static void desktop_repaint(void *ctx)
{
    (void)ctx;
    espix_desktop_repaint();
}

static const espix_screen_t s_desktop_screen = {
    .name    = "desktop",
    .input   = desktop_input,
    .repaint = desktop_repaint,
};

esp_err_t espix_desktop_start(void)
{
    if (espix_display_canvas() == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_arrow_built) {
        cursor_build();
    }
    if (s_desk_lock == NULL) {
        s_desk_lock = xSemaphoreCreateRecursiveMutex();
    }
    if (s_clock_timer == NULL) {
        clock_text(s_clock, sizeof(s_clock));

        const esp_timer_create_args_t args = {
            .callback = clock_tick,
            .name     = "taskbar",
        };
        if (esp_timer_create(&args, &s_clock_timer) == ESP_OK) {
            esp_timer_start_periodic(s_clock_timer, 5 * 1000 * 1000);
        } else {
            /* A bar with a clock that never moves is better than no bar. */
            s_clock_timer = NULL;
        }
    }
    if (s_nwin == 0) {
        windows_create();
    }
    return espix_display_claim(&s_desktop_screen);
}

void espix_desktop_stop(void)
{
    /* Stopped before the canvas goes, so the tick cannot repaint into a display
     * that is on its way down. */
    if (s_clock_timer != NULL) {
        esp_timer_stop(s_clock_timer);
        esp_timer_delete(s_clock_timer);
        s_clock_timer = NULL;
    }

    espix_display_release(&s_desktop_screen);

    /*
     * Freed on stop, not kept for next time: a board that never opens the
     * desktop should not be paying for its surfaces, which is the same argument
     * the console makes.
     */
    windows_destroy();
    s_cursor_on = false;
    s_menu      = false;
    s_menu_hot  = -1;
}