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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_heap_caps.h"

#include "espix_desktop.h"
#include "espix_display.h"
#include "espix_kernel.h"

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

#define TITLE_H 18
#define PAD     4
#define CELL_W  8
#define CELL_H  8

#define WIN_MAX 8

struct espix_window {
    int              x, y, w, h;
    char             title[ESPIX_WINDOW_TITLE_MAX];
    espix_surface_t *surf;
    espix_window_draw_fn draw;
    espix_window_key_fn  key;
    void            *ctx;
};

/* Bottom first, top last: the array *is* the z-order. */
static espix_window_t *s_wins[WIN_MAX];
static int             s_nwin;
static espix_window_t *s_focus;

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

espix_rect_t espix_window_content(const espix_window_t *w)
{
    return (espix_rect_t){ PAD, TITLE_H + PAD,
                           w->w - 2 * PAD, w->h - TITLE_H - 2 * PAD };
}

static bool window_hit(const espix_window_t *w, int x, int y)
{
    return x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + w->h;
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
static void desktop_repair(espix_rect_t r)
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
    espix_canvas_unlock(c);

    int px = 0, py = 0;
    espix_display_pointer(&px, &py);
    cursor_show(px, py);
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

    espix_window_t *win = calloc(1, sizeof(*win));
    if (win == NULL) {
        return NULL;
    }
    win->surf = espix_surface_new(w, h);
    if (win->surf == NULL) {
        free(win);
        return NULL;
    }

    win->x = x;
    win->y = y;
    win->w = w;
    win->h = h;
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
    if (window_raise_raw(w)) {
        espix_desktop_repaint();
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
    }
    if (hit != NULL) {
        espix_window_damage(hit, window_title(hit));
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

    w->x = x;
    w->y = y;

    const int x0 = was.x < now.x ? was.x : now.x;
    const int y0 = was.y < now.y ? was.y : now.y;
    const int x1 = (was.x + was.w) > (now.x + now.w) ? (was.x + was.w)
                                                      : (now.x + now.w);
    const int y1 = (was.y + was.h) > (now.y + now.h) ? (was.y + was.h)
                                                      : (now.y + now.h);

    desktop_repair((espix_rect_t){ x0, y0, x1 - x0, y1 - y0 });
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
    espix_window_t *hit = NULL;
    for (int i = s_nwin - 1; i >= 0; i--) {
        if (window_hit(s_wins[i], x, y)) {
            hit = s_wins[i];
            break;
        }
    }
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
/* The windows that ship with it                                      */
/* ------------------------------------------------------------------ */

/*
 * A terminal that echoes keys. It exists to prove the keyboard round trip
 * visibly, and it is the reason this is a desktop rather than a background.
 */
#define TERM_COLS 56
#define TERM_ROWS 20

static char s_grid[TERM_ROWS][TERM_COLS];
static int  s_trow, s_tcol;
static espix_window_t *s_term;

/* The rectangle one character cell occupies, in surface coordinates. */
static espix_rect_t term_cell(const espix_window_t *w, int row, int col)
{
    const espix_rect_t c = espix_window_content(w);
    return (espix_rect_t){ c.x + col * CELL_W, c.y + row * CELL_H, CELL_W, CELL_H };
}

static espix_rect_t rect_union(espix_rect_t a, espix_rect_t b)
{
    const int x0 = a.x < b.x ? a.x : b.x;
    const int y0 = a.y < b.y ? a.y : b.y;
    const int x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w;
    const int y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    return (espix_rect_t){ x0, y0, x1 - x0, y1 - y0 };
}

/*
 * Only the cells the region reaches. A keystroke reaches one, a backspace two,
 * and a whole-window repaint all of them -- so this is where drawing a window
 * stops costing a window.
 */
static void term_draw(espix_window_t *w, espix_surface_t *s, espix_rect_t r,
                      void *ctx)
{
    (void)ctx;
    const espix_rect_t c = espix_window_content(w);

    int row0 = (r.y - c.y) / CELL_H;
    int row1 = (r.y + r.h - 1 - c.y) / CELL_H;
    int col0 = (r.x - c.x) / CELL_W;
    int col1 = (r.x + r.w - 1 - c.x) / CELL_W;

    if (row0 < 0) { row0 = 0; }
    if (col0 < 0) { col0 = 0; }
    if (row1 > TERM_ROWS - 1) { row1 = TERM_ROWS - 1; }
    if (col1 > TERM_COLS - 1) { col1 = TERM_COLS - 1; }

    for (int row = row0; row <= row1; row++) {
        char line[TERM_COLS + 1];
        const int n = col1 - col0 + 1;

        memcpy(line, &s_grid[row][col0], (size_t)n);
        line[n] = '\0';
        espix_surface_text(s, c.x + col0 * CELL_W, c.y + row * CELL_H, line,
                           COL_TEXT_FG, COL_WIN_BG);
    }
}

static void term_key(espix_window_t *w, uint32_t keysym, bool down, void *ctx)
{
    (void)ctx;
    if (!down) {
        return;
    }

    const char ch = espix_keysym_char(keysym);
    const int  was_row = s_trow, was_col = s_tcol;

    if (ch == '\r') {
        s_tcol = 0;
        s_trow++;
    } else if (ch == '\b') {
        if (s_tcol > 0) {
            s_tcol--;
        } else if (s_trow > 0) {
            s_trow--;
            s_tcol = TERM_COLS - 1;
        } else {
            return;
        }
        s_grid[s_trow][s_tcol] = ' ';
    } else if (ch >= 0x20 && ch < 0x7F) {
        s_grid[s_trow][s_tcol] = ch;
        if (++s_tcol >= TERM_COLS) {
            s_tcol = 0;
            s_trow++;
        }
    } else {
        return;                     /* modifiers, arrows: not a text window yet */
    }

    if (s_trow >= TERM_ROWS) {
        /* Clear rather than scroll: this window exists to prove the keyboard,
         * not to be a terminal. */
        s_trow = 0;
        s_tcol = 0;
        memset(s_grid, ' ', sizeof(s_grid));
        espix_window_repaint(w);
        return;
    }

    /*
     * The cell it was in and the cell it moved to. A keystroke touches one and
     * a backspace two; a wrap touches the end of one row and the start of the
     * next. Two cells is the most a key can change, so this is the whole of it.
     */
    espix_window_damage(w, rect_union(term_cell(w, was_row, was_col),
                                      term_cell(w, s_trow, s_tcol)));
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

static void windows_create(void)
{
    s_term = espix_window_new(40, 40,
                              TERM_COLS * CELL_W + 2 * PAD,
                              TERM_ROWS * CELL_H + TITLE_H + 2 * PAD,
                              "terminal");
    if (s_term != NULL) {
        espix_window_set_draw(s_term, term_draw);
        espix_window_set_key(s_term, term_key);
        memset(s_grid, ' ', sizeof(s_grid));
        s_trow = s_tcol = 0;
        /* The surface exists but was painted frame-only at creation, so the
         * content is asked for once, here. */
        espix_window_repaint(s_term);
    }

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
    while (s_nwin > 0) {
        espix_window_t *w = s_wins[s_nwin - 1];
        espix_surface_free(w->surf);
        free(w);
        s_wins[--s_nwin] = NULL;
    }
    s_focus = NULL;
    s_term  = NULL;
    s_about = NULL;
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
static uint8_t        s_buttons;
static espix_window_t *s_drag;
static int            s_grab_x, s_grab_y;   /* where in the window it was grabbed */

static void drag_begin(espix_window_t *w, int x, int y)
{
    s_drag   = w;
    s_grab_x = x - w->x;
    s_grab_y = y - w->y;
}

static void drag_end(void)
{
    s_drag = NULL;
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
        if (ny > ch - TITLE_H)   { ny = ch - TITLE_H; }
    }

    espix_window_move(s_drag, nx, ny);
}

static void desktop_input(void *ctx, const espix_input_event_t *ev)
{
    (void)ctx;

    if (ev->kind == ESPIX_INPUT_POINTER) {
        const uint8_t was = s_buttons;
        s_buttons = ev->buttons;

        cursor_put(ev->x, ev->y);

        if (was != 0 && s_buttons == 0) {
            drag_end();                 /* the button came up */
            return;
        }
        if (was == 0 && s_buttons != 0) {
            focus_at(ev->x, ev->y);     /* focus first, so the title is right */
            if (s_focus != NULL) {
                /* The press is what raises, which is why a border crossing does
                 * not have to recomposite anything. */
                espix_window_raise(s_focus);
                if (ev->y >= s_focus->y && ev->y < s_focus->y + TITLE_H) {
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
    if (s_nwin == 0) {
        windows_create();
    }
    return espix_display_claim(&s_desktop_screen);
}

void espix_desktop_stop(void)
{
    espix_display_release(&s_desktop_screen);

    /*
     * Freed on stop, not kept for next time: a board that never opens the
     * desktop should not be paying for its surfaces, which is the same argument
     * the console makes.
     */
    windows_destroy();
    s_cursor_on = false;
}