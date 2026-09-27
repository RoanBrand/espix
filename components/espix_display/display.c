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

#define COL_DESKTOP  RGB565(0x2B, 0x30, 0x3A)
#define COL_WIN_BG   RGB565(0x16, 0x1A, 0x20)
#define COL_WIN_EDGE RGB565(0x4A, 0x54, 0x66)
#define COL_TITLE    RGB565(0x3A, 0x42, 0x52)
#define COL_TITLE_FG RGB565(0xE8, 0xEC, 0xF2)
#define COL_TEXT_FG  RGB565(0xC8, 0xD8, 0xE8)

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

static espix_rect_t rect_clip(const espix_canvas_t *c, espix_rect_t r)
{
    int x0 = r.x < 0 ? 0 : r.x;
    int y0 = r.y < 0 ? 0 : r.y;
    int x1 = r.x + r.w > c->w ? c->w : r.x + r.w;
    int y1 = r.y + r.h > c->h ? c->h : r.y + r.h;
    return (espix_rect_t){ x0, y0, x1 - x0, y1 - y0 };
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

void espix_canvas_fill(espix_canvas_t *c, espix_rect_t r, espix_px_t px)
{
    r = rect_clip(c, r);
    for (int y = 0; y < r.h; y++) {
        espix_px_t *row = c->px + (size_t)(r.y + y) * c->w + r.x;
        for (int x = 0; x < r.w; x++) {
            row[x] = px;
        }
    }
    espix_canvas_damage(c, r);
}

void espix_canvas_blit(espix_canvas_t *c, int dst_x, int dst_y,
                       const espix_px_t *src, int src_w, int src_h, int src_stride)
{
    for (int y = 0; y < src_h; y++) {
        const int cy = dst_y + y;
        if (cy < 0 || cy >= c->h) {
            continue;
        }
        for (int x = 0; x < src_w; x++) {
            const int cx = dst_x + x;
            if (cx < 0 || cx >= c->w) {
                continue;
            }
            c->px[(size_t)cy * c->w + cx] = src[(size_t)y * src_stride + x];
        }
    }
    espix_canvas_damage(c, (espix_rect_t){ dst_x, dst_y, src_w, src_h });
}

void espix_canvas_outline(espix_canvas_t *c, espix_rect_t r, espix_px_t px)
{
    espix_canvas_fill(c, (espix_rect_t){ r.x, r.y, r.w, 1 }, px);
    espix_canvas_fill(c, (espix_rect_t){ r.x, r.y + r.h - 1, r.w, 1 }, px);
    espix_canvas_fill(c, (espix_rect_t){ r.x, r.y, 1, r.h }, px);
    espix_canvas_fill(c, (espix_rect_t){ r.x + r.w - 1, r.y, 1, r.h }, px);
}

void espix_canvas_text(espix_canvas_t *c, int x, int y, const char *s,
                       espix_px_t fg, espix_px_t bg)
{
    const int x0 = x;

    for (; *s != '\0'; s++, x += 8) {
        unsigned ch = (unsigned char)*s;
        if (ch > 127) {
            ch = '?';
        }
        const unsigned char *glyph = espix_font8x8[ch];
        for (int row = 0; row < 8; row++) {
            const int cy = y + row;
            if (cy < 0 || cy >= c->h) {
                continue;
            }
            for (int col = 0; col < 8; col++) {
                const int cx = x + col;
                if (cx < 0 || cx >= c->w) {
                    continue;
                }
                c->px[(size_t)cy * c->w + cx] =
                    (glyph[row] & (1u << col)) ? fg : bg;
            }
        }
    }

    espix_canvas_damage(c, (espix_rect_t){ x0, y, (int)(x - x0), 8 });
}

/* ------------------------------------------------------------------ */
/* The desktop                                                         */
/* ------------------------------------------------------------------ */

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

static espix_canvas_t *s_canvas;
static QueueHandle_t   s_input;
static TaskHandle_t    s_desktop_task;
static volatile bool   s_desktop_quit;
static bool            s_up;

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
    espix_canvas_t *c = s_canvas;
    if (c == NULL) {
        return;
    }

    if (hx < 0) { hx = 0; }
    if (hy < 0) { hy = 0; }
    if (hx > c->w - 1) { hx = c->w - 1; }
    if (hy > c->h - 1) { hy = c->h - 1; }

    espix_canvas_lock(c);
    espix_px_t *px = c->px;

    if (s_cursor_on) {
        for (int by = 0; by < CUR_BH; by++) {
            for (int bx = 0; bx < CUR_BW; bx++) {
                const int cx = s_cx - 1 + bx, cy = s_cy - 1 + by;
                if (cx < 0 || cx >= c->w || cy < 0 || cy >= c->h) {
                    continue;
                }
                px[(size_t)cy * c->w + cx] = s_under[by * CUR_BW + bx];
            }
        }
        espix_canvas_damage(c, (espix_rect_t){ s_cx - 1, s_cy - 1, CUR_BW, CUR_BH });
    }

    s_cx = hx;
    s_cy = hy;

    for (int by = 0; by < CUR_BH; by++) {
        for (int bx = 0; bx < CUR_BW; bx++) {
            const int cx = hx - 1 + bx, cy = hy - 1 + by;
            if (cx < 0 || cx >= c->w || cy < 0 || cy >= c->h) {
                continue;
            }
            s_under[by * CUR_BW + bx] = px[(size_t)cy * c->w + cx];
        }
    }

    for (int by = 0; by < CUR_BH; by++) {
        for (int bx = 0; bx < CUR_BW; bx++) {
            const int cx = hx - 1 + bx, cy = hy - 1 + by;
            if (cx < 0 || cx >= c->w || cy < 0 || cy >= c->h) {
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
            px[(size_t)cy * c->w + cx] = v;
        }
    }

    espix_canvas_damage(c, (espix_rect_t){ hx - 1, hy - 1, CUR_BW, CUR_BH });
    s_cursor_on = true;
    espix_canvas_unlock(c);
}

static void text_draw_cell(int row, int col, char ch)
{
    const int x = TEXT_X + col * CELL_W;
    const int y = TEXT_Y + row * CELL_H;
    const char cell[2] = { ch, '\0' };
    espix_canvas_text(s_canvas, x, y, cell, COL_TEXT_FG, COL_WIN_BG);
}

static void text_window_draw(void)
{
    espix_canvas_fill(s_canvas, (espix_rect_t){ WIN_X, WIN_Y, WIN_W, WIN_H }, COL_WIN_BG);
    espix_canvas_fill(s_canvas, (espix_rect_t){ WIN_X, WIN_Y, WIN_W, WIN_TITLE_H }, COL_TITLE);
    espix_canvas_text(s_canvas, WIN_X + WIN_PAD, WIN_Y + (WIN_TITLE_H - 8) / 2,
                      "espix - keyboard and mouse", COL_TITLE_FG, COL_TITLE);
    espix_canvas_outline(s_canvas, (espix_rect_t){ WIN_X, WIN_Y, WIN_W, WIN_H }, COL_WIN_EDGE);
}

/* RFB delivers X11 keysyms; the ASCII range is its own keysym. */
static char keysym_char(uint32_t ks)
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

static void text_key(uint32_t keysym)
{
    const char ch = keysym_char(keysym);

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
        espix_canvas_fill(s_canvas,
                          (espix_rect_t){ TEXT_X, TEXT_Y,
                                          TEXT_COLS * CELL_W, TEXT_ROWS * CELL_H },
                          COL_WIN_BG);
    }
}

static void desktop_task(void *arg)
{
    (void)arg;
    espix_input_event_t ev;

    while (!s_desktop_quit) {
        if (xQueueReceive(s_input, &ev, pdMS_TO_TICKS(200)) != pdTRUE) {
            continue;
        }
        if (ev.kind == ESPIX_INPUT_POINTER) {
            cursor_put(ev.x, ev.y);
        } else if (ev.kind == ESPIX_INPUT_KEY && ev.down) {
            text_key(ev.keysym);
        }
    }

    s_desktop_task = NULL;
    vTaskDeleteWithCaps(NULL);          /* frees the PSRAM stack it was given */
}

void espix_display_input(const espix_input_event_t *ev)
{
    if (s_input == NULL || ev == NULL) {
        return;
    }
    (void)xQueueSend(s_input, ev, 0);   /* full queue drops the event */
}

/* ------------------------------------------------------------------ */
/* Service                                                             */
/* ------------------------------------------------------------------ */

espix_canvas_t *espix_display_canvas(void) { return s_canvas; }
bool            espix_display_ready(void)  { return s_up; }

static esp_err_t desktop_up(void)
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

    s_input = xQueueCreate(32, sizeof(espix_input_event_t));
    if (s_input == NULL) {
        espix_canvas_free(s_canvas);
        s_canvas = NULL;
        return ESP_ERR_NO_MEM;
    }

    cursor_build();
    s_desktop_quit = false;
    s_cursor_on = false;
    s_trow = s_tcol = 0;
    memset(s_grid, ' ', sizeof(s_grid));

    espix_canvas_lock(s_canvas);
    espix_canvas_fill(s_canvas,
                      (espix_rect_t){ 0, 0, ESPIX_DISPLAY_W, ESPIX_DISPLAY_H },
                      COL_DESKTOP);
    text_window_draw();
    espix_canvas_unlock(s_canvas);

    cursor_put(ESPIX_DISPLAY_W / 3, ESPIX_DISPLAY_H / 3);

    /*
     * Outranks the RFB task (4) on purpose. When a pointer event arrives the
     * RFB task posts it and then turns round to send the damage -- if the
     * desktop task could not preempt, the damage would not be there yet and
     * the cursor would move only on the next poll timeout, a quarter of a
     * second later. Priority makes the round trip synchronous.
     */
    if (xTaskCreateWithCaps(desktop_task, "espix:desk", 4096, NULL, 5,
                            &s_desktop_task, MALLOC_CAP_SPIRAM) != pdPASS) {
        (void)xTaskCreateWithCaps(desktop_task, "espix:desk", 4096, NULL, 5,
                                  &s_desktop_task,
                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (s_desktop_task == NULL) {
        vQueueDelete(s_input);
        s_input = NULL;
        espix_canvas_free(s_canvas);
        s_canvas = NULL;
        return ESP_ERR_NO_MEM;
    }

    /*
     * The whole screen is damage once, so a client that asks for an
     * incremental update before it has ever seen a frame still gets one. Well
     * behaved clients ask for a full update first and this is simply dropped.
     */
    espix_canvas_lock(s_canvas);
    espix_canvas_damage(s_canvas,
                        (espix_rect_t){ 0, 0, ESPIX_DISPLAY_W, ESPIX_DISPLAY_H });
    espix_canvas_unlock(s_canvas);

    s_up = true;
    espix_klog(ESPIX_KLOG_INFO, TAG, "desktop %dx%d up", ESPIX_DISPLAY_W, ESPIX_DISPLAY_H);
    return ESP_OK;
}

static void desktop_down(void)
{
    if (!s_up) {
        return;
    }

    /*
     * Ask the task to stop and wait for it to clear its own handle: a task
     * created with xTaskCreateWithCaps must be deleted with the matching
     * vTaskDeleteWithCaps, and the only place that can safely do that is the
     * task itself. Deleting it from here would leak its PSRAM stack.
     */
    if (s_desktop_task != NULL) {
        s_desktop_quit = true;
        for (int i = 0; i < 100 && s_desktop_task != NULL; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    if (s_input != NULL) {
        vQueueDelete(s_input);
        s_input = NULL;
    }
    if (s_canvas != NULL) {
        espix_canvas_free(s_canvas);
        s_canvas = NULL;
    }

    s_cursor_on = false;
    s_up = false;
}

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

    const esp_err_t err = desktop_up();
    if (err != ESP_OK) {
        return err;
    }

    const esp_err_t lerr = espix_display_rfb_listen(port);
    if (lerr != ESP_OK) {
        desktop_down();
        return lerr;
    }
    return ESP_OK;
}

void espix_display_vnc_stop(void)
{
    espix_display_rfb_stop();
    desktop_down();
}
