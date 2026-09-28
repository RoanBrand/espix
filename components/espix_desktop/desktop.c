/*
 * The desktop, as a client of the display service.
 *
 * Everything here used to live in espix_display, which was doing two jobs at
 * once: owning the screen, and being one of the things on it. The second job is
 * the one that grows -- windows, z-order, focus, decorations -- so it moved
 * out, and the service went back to owning the canvas, the surfaces, the
 * pointer and the backends.
 *
 * What is here now is the same placeholder content as before: a background, a
 * window that echoes keys so the keyboard round-trip is visibly proven, and the
 * cursor. It is deliberately small, because its job is to make the screen never
 * blank and to give the input path something to prove itself against. The
 * window model goes on top of it.
 *
 * The canvas is *not* owned here. It is fetched per call, because the service
 * may take it away -- `display stop` while the desktop owns the screen is
 * refused, but the pointer must not be cached across that.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "espix_desktop.h"
#include "espix_display.h"
#include "espix_kernel.h"

#define TAG "desktop"

#define RGB565(r, g, b) \
    ((espix_px_t)((((r) & 0xF8u) << 8) | (((g) & 0xFCu) << 3) | (((b) & 0xF8u) >> 3)))

#define COL_DESKTOP  RGB565(0x2B, 0x30, 0x3A)
#define COL_WIN_BG   RGB565(0x16, 0x1A, 0x20)
#define COL_WIN_EDGE RGB565(0x4A, 0x54, 0x66)
#define COL_TITLE    RGB565(0x3A, 0x42, 0x52)
#define COL_TITLE_FG RGB565(0xE8, 0xEC, 0xF2)
#define COL_TEXT_FG  RGB565(0xC8, 0xD8, 0xE8)

/* A window, a cursor, and the grid the keys land in. */
#define WIN_X       40
#define WIN_Y       40
#define WIN_W       480
#define WIN_H       200
#define WIN_TITLE_H 20
#define WIN_PAD     6
#define CELL_W      8
#define CELL_H      8
#define TEXT_COLS   ((WIN_W - 2 * WIN_PAD) / CELL_W)
#define TEXT_ROWS   ((WIN_H - WIN_TITLE_H - 2 * WIN_PAD) / CELL_H)
#define TEXT_X      (WIN_X + WIN_PAD)
#define TEXT_Y      (WIN_Y + WIN_TITLE_H + WIN_PAD)

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

static char s_grid[TEXT_ROWS][TEXT_COLS];
static int  s_trow, s_tcol;

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

static void text_draw_cell(int row, int col, char ch)
{
    const char cell[2] = { ch, '\0' };
    espix_canvas_text(espix_display_canvas(), TEXT_X + col * CELL_W,
                      TEXT_Y + row * CELL_H, cell, COL_TEXT_FG, COL_WIN_BG);
}

static void text_window_draw(void)
{
    espix_canvas_t *c = espix_display_canvas();

    espix_canvas_fill(c, (espix_rect_t){ WIN_X, WIN_Y, WIN_W, WIN_H }, COL_WIN_BG);
    espix_canvas_fill(c, (espix_rect_t){ WIN_X, WIN_Y, WIN_W, WIN_TITLE_H }, COL_TITLE);
    espix_canvas_text(c, WIN_X + WIN_PAD, WIN_Y + (WIN_TITLE_H - 8) / 2,
                      "espix - keyboard and mouse", COL_TITLE_FG, COL_TITLE);
    espix_canvas_outline(c, (espix_rect_t){ WIN_X, WIN_Y, WIN_W, WIN_H }, COL_WIN_EDGE);
}

static void text_key(uint32_t keysym)
{
    const char ch = espix_keysym_char(keysym);

    if (ch == '\r') {
        s_tcol = 0;
        s_trow++;
    } else if (ch == '\b') {
        if (s_tcol > 0) {
            s_tcol--;
        } else if (s_trow > 0) {
            s_trow--;
            s_tcol = TEXT_COLS - 1;
        } else {
            return;
        }
        s_grid[s_trow][s_tcol] = ' ';
        text_draw_cell(s_trow, s_tcol, ' ');
    } else if (ch >= 0x20 && ch < 0x7F) {
        s_grid[s_trow][s_tcol] = ch;
        text_draw_cell(s_trow, s_tcol, ch);
        if (++s_tcol >= TEXT_COLS) {
            s_tcol = 0;
            s_trow++;
        }
    } else {
        return;                     /* modifiers, arrows: not a text window yet */
    }

    if (s_trow >= TEXT_ROWS) {
        /* Clear rather than scroll: this window exists to prove the keyboard,
         * not to be a terminal. */
        s_trow = 0;
        s_tcol = 0;
        memset(s_grid, ' ', sizeof(s_grid));
        espix_canvas_fill(espix_display_canvas(),
                          (espix_rect_t){ TEXT_X, TEXT_Y,
                                          TEXT_COLS * CELL_W, TEXT_ROWS * CELL_H },
                          COL_WIN_BG);
    }
}

/*
 * The whole of the desktop: a background, a window that echoes keys, and the
 * cursor. Repainting from scratch is what ownership means here -- the console
 * does the same -- so this is both the first paint and every repaint.
 */
static void desktop_paint(void)
{
    espix_canvas_t *c = espix_display_canvas();
    if (c == NULL) {
        return;
    }

    espix_canvas_lock(c);
    espix_canvas_fill(c, (espix_rect_t){ 0, 0, espix_canvas_width(c),
                                        espix_canvas_height(c) }, COL_DESKTOP);
    text_window_draw();
    espix_canvas_unlock(c);

    /* Everything under the cursor was just painted over, so the save-under
     * buffer no longer describes what is on the canvas. */
    s_cursor_on = false;

    int px = 0, py = 0;
    espix_display_pointer(&px, &py);
    cursor_put(px, py);
}

static void desktop_input(void *ctx, const espix_input_event_t *ev)
{
    (void)ctx;

    if (ev->kind == ESPIX_INPUT_POINTER) {
        cursor_put(ev->x, ev->y);
    } else if (ev->kind == ESPIX_INPUT_MOTION) {
        /*
         * Applied to where cursor_put() last put it, which is where the cursor
         * actually is. That makes an edge clamp rather than letting the delta
         * accumulate somewhere off-screen -- so pushing into a corner and
         * pulling back moves immediately, the way a mouse does.
         */
        cursor_put(s_cx + ev->x, s_cy + ev->y);
    } else if (ev->kind == ESPIX_INPUT_KEY && ev->down) {
        text_key(ev->keysym);
    }
}

static void desktop_repaint(void *ctx)
{
    (void)ctx;
    desktop_paint();
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
    if (!s_cursor_on && s_fill[0] == 0 && s_fill[1] == 0) {
        cursor_build();
    }
    return espix_display_claim(&s_desktop_screen);
}

void espix_desktop_stop(void)
{
    espix_display_release(&s_desktop_screen);
}