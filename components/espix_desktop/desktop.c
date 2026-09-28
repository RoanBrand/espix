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
static void cursor_put(int hx, int hy)
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

    if (s_cursor_on) {
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
    }

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

/* ------------------------------------------------------------------ */
/* Windows                                                             */
/* ------------------------------------------------------------------ */

/* Defined at the bottom; the drawing gate needs it long before then. */
static const espix_screen_t s_desktop_screen;

static bool desktop_on_screen(void)
{
    return espix_display_owns(&s_desktop_screen);
}

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

static void window_paint(espix_window_t *w)
{
    espix_surface_t *s = w->surf;
    const espix_px_t bar = (w == s_focus) ? COL_TITLE_FOC : COL_TITLE;

    espix_surface_lock(s);

    espix_surface_fill(s, (espix_rect_t){ 0, 0, w->w, w->h }, COL_WIN_BG);
    espix_surface_fill(s, (espix_rect_t){ 0, 0, w->w, TITLE_H }, bar);

    const int tw = (int)strlen(w->title) * CELL_W;
    espix_surface_text(s, (w->w - tw) / 2, (TITLE_H - CELL_H) / 2, w->title,
                       COL_TITLE_FG, bar);
    espix_surface_outline(s, (espix_rect_t){ 0, 0, w->w, w->h }, COL_WIN_EDGE);

    if (w->draw != NULL) {
        w->draw(w, s, w->ctx);
    }

    espix_surface_unlock(s);
}

static void window_blit(espix_window_t *w)
{
    espix_canvas_t *c = espix_display_canvas();
    if (c == NULL) {
        return;
    }
    espix_canvas_lock(c);
    espix_canvas_blit_surface(c, w->x, w->y, w->surf);
    espix_canvas_unlock(c);
}

void espix_window_repaint(espix_window_t *w)
{
    if (w == NULL || w->surf == NULL) {
        return;
    }
    window_paint(w);
    if (desktop_on_screen()) {
        window_blit(w);
    }
}

void espix_desktop_repaint(void)
{
    espix_canvas_t *c = espix_display_canvas();
    if (c == NULL || !desktop_on_screen()) {
        return;
    }

    for (int i = 0; i < s_nwin; i++) {
        window_paint(s_wins[i]);
    }

    espix_canvas_lock(c);
    espix_canvas_fill(c, (espix_rect_t){ 0, 0, espix_canvas_width(c),
                                        espix_canvas_height(c) }, COL_DESKTOP);
    for (int i = 0; i < s_nwin; i++) {
        espix_canvas_blit_surface(c, s_wins[i]->x, s_wins[i]->y, s_wins[i]->surf);
    }
    espix_canvas_unlock(c);

    /* Everything under the cursor was just painted over, so the save-under
     * buffer no longer describes what is on the canvas. */
    s_cursor_on = false;

    int px = 0, py = 0;
    espix_display_pointer(&px, &py);
    cursor_put(px, py);
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

/* The array order is the z-order, so raising is a move to the end. */
static void window_raise_raw(espix_window_t *w)
{
    int i = 0;
    for (; i < s_nwin; i++) {
        if (s_wins[i] == w) {
            break;
        }
    }
    if (i >= s_nwin || i == s_nwin - 1) {
        return;
    }
    for (int j = i; j < s_nwin - 1; j++) {
        s_wins[j] = s_wins[j + 1];
    }
    s_wins[s_nwin - 1] = w;
}

void espix_window_raise(espix_window_t *w)
{
    window_raise_raw(w);
    espix_desktop_repaint();
}

void espix_window_focus(espix_window_t *w)
{
    if (s_focus == w) {
        return;
    }
    s_focus = w;
    espix_desktop_repaint();
}

void espix_window_move(espix_window_t *w, int x, int y)
{
    w->x = x;
    w->y = y;
    /*
     * A full repaint rather than a blit and a repair, because what was
     * underneath the window is no longer known -- that is what occlusion costs,
     * and it is the operation the accelerators exist to make cheap.
     */
    espix_desktop_repaint();
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
    s_focus = hit;
    if (hit != NULL) {
        window_raise_raw(hit);
    }
    espix_desktop_repaint();
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

static void term_draw(espix_window_t *w, espix_surface_t *s, void *ctx)
{
    (void)ctx;
    const espix_rect_t r = espix_window_content(w);

    for (int row = 0; row < TERM_ROWS; row++) {
        char line[TERM_COLS + 1];
        memcpy(line, s_grid[row], TERM_COLS);
        line[TERM_COLS] = '\0';
        espix_surface_text(s, r.x, r.y + row * CELL_H, line, COL_TEXT_FG,
                           COL_WIN_BG);
    }
}

static void term_key(espix_window_t *w, uint32_t keysym, bool down, void *ctx)
{
    (void)ctx;
    if (!down) {
        return;
    }

    const char ch = espix_keysym_char(keysym);

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
    }

    espix_window_repaint(w);
}

/*
 * A second window with nothing in it but a few lines. It is here for the
 * overlap: z-order, focus and the raise that follows the pointer are only
 * visible with two things on the screen.
 */
static espix_window_t *s_about;

static void about_draw(espix_window_t *w, espix_surface_t *s, void *ctx)
{
    (void)ctx;
    const espix_rect_t r = espix_window_content(w);
    static const char *const lines[] = {
        "espix desktop",
        "",
        "focus follows the pointer",
        "and raises what it lands on",
        "",
        "surfaces, composited",
    };

    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        espix_surface_text(s, r.x, r.y + (int)i * CELL_H, lines[i], COL_TEXT_FG,
                           COL_WIN_BG);
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
    }

    s_about = espix_window_new(300, 190, 220, 96, "about");
    if (s_about != NULL) {
        espix_window_set_draw(s_about, about_draw);
    }

    /* The terminal is the one to type at, so it starts in front. */
    if (s_term != NULL) {
        window_raise_raw(s_term);
        s_focus = s_term;
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

static void desktop_input(void *ctx, const espix_input_event_t *ev)
{
    (void)ctx;

    if (ev->kind == ESPIX_INPUT_POINTER) {
        cursor_put(ev->x, ev->y);
        focus_at(ev->x, ev->y);
    } else if (ev->kind == ESPIX_INPUT_MOTION) {
        /*
         * Applied to where cursor_put() last put it, which is where the cursor
         * actually is. That makes an edge clamp rather than letting the delta
         * accumulate somewhere off-screen -- so pushing into a corner and
         * pulling back moves immediately, the way a mouse does.
         */
        cursor_put(s_cx + ev->x, s_cy + ev->y);
        focus_at(s_cx, s_cy);
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