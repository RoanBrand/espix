/*
 * The widgets themselves. Each one is a few fills and a line of text, and each
 * one is written to be idempotent: drawing a widget twice into the same
 * rectangle leaves the same pixels, so a caller can repaint a whole panel
 * without tracking what changed.
 */

#include <string.h>

#include "espix_widgets.h"

#define PAD_X 8

int espix_wgt_text_w(const char *text)
{
    return (int)strlen(text) * WGT_TEXT_H;
}

bool espix_wgt_hit(espix_rect_t r, int x, int y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

/* Text at the top of its row, or one whole cell down when the row is tall
 * enough to want the space -- never at a half-cell offset. */
static int text_y(espix_rect_t r)
{
    return r.y + (((r.h - WGT_TEXT_H) / 2) & ~7);
}

void espix_wgt_panel(espix_surface_t *s, espix_rect_t r, espix_px_t bg)
{
    espix_surface_fill(s, r, bg);
}

void espix_wgt_heading(espix_surface_t *s, espix_rect_t r, const char *text)
{
    espix_surface_text(s, r.x, text_y(r), text, WGT_HEAD_FG, WGT_PANE);
    espix_surface_fill(s, (espix_rect_t){ r.x, r.y + r.h - 3, r.w, 1 }, WGT_EDGE);
}

void espix_wgt_label(espix_surface_t *s, espix_rect_t r, const char *text, bool on)
{
    espix_surface_text(s, r.x, text_y(r), text, on ? WGT_FG : WGT_FG_DIM,
                       WGT_PANE);
}

void espix_wgt_field(espix_surface_t *s, espix_rect_t r, const char *name,
                     const char *value, bool on)
{
    espix_surface_text(s, r.x, text_y(r), name, on ? WGT_FG : WGT_FG_DIM,
                       WGT_PANE);

    const int vw = espix_wgt_text_w(value);
    int       vx = r.x + r.w - vw;
    if (vx < r.x + PAD_X) {
        vx = r.x + PAD_X;               /* a long value wins the space */
    }
    espix_surface_text(s, vx, text_y(r), value, on ? WGT_FG : WGT_FG_DIM,
                       WGT_PANE);
}

void espix_wgt_button(espix_surface_t *s, espix_rect_t r, const char *text,
                      bool on, bool hot)
{
    const espix_px_t bg = !on ? WGT_BTN_DIM : (hot ? WGT_BTN_HOT : WGT_BTN);
    const espix_px_t fg = on ? WGT_FG : WGT_FG_DIM;
    const int        tw = espix_wgt_text_w(text);

    espix_surface_fill(s, r, bg);
    espix_surface_outline(s, r, on ? WGT_EDGE : WGT_BTN_DIM);
    espix_surface_text(s, r.x + (r.w - tw) / 2, text_y(r), text, fg, bg);
}

/*
 * A ring, and a dot in it when it is the chosen one. Drawn as rows rather than
 * as a glyph, because the font is 8x8 and has no ring in it.
 */
static void radio_dot(espix_surface_t *s, int cx, int cy, bool selected, bool on)
{
    const espix_px_t fg = on ? WGT_FG : WGT_FG_DIM;

    for (int dy = -4; dy <= 4; dy++) {
        int dx = 0;
        while ((dx + 1) * (dx + 1) + dy * dy <= 16) {
            dx++;
        }
        /* Unselected is a ring; selected is that ring filled solid to the
         * edge, in the selected colour -- a five-pixel plus in the middle read
         * as "slightly lighter", which is not a state you can see at a glance. */
        for (int x = -dx; x <= dx; x++) {
            const bool edge = (x == -dx || x == dx || dy == -4 || dy == 4);
            /* x*x, not dx*dx: dx is the row's half-width, so dx*dx+dy*dy is
             * >= 16 in the middle rows and the fill never happened at all. */
            const bool core = selected && (x * x + dy * dy <= 9);
            const bool lit  = core || (selected && edge);

            if (edge || core) {
                espix_surface_fill(s, (espix_rect_t){ cx + x, cy + dy, 1, 1 },
                                   lit ? WGT_SEL_FG : fg);
            }
        }
    }
}

void espix_wgt_radio(espix_surface_t *s, espix_rect_t r, const char *text,
                     bool selected, bool on, bool hot)
{
    if (hot) {
        espix_surface_fill(s, r, WGT_HOT);
    } else {
        espix_surface_fill(s, r, WGT_PANE);
    }

    radio_dot(s, r.x + 8, r.y + r.h / 2, selected, on);
    espix_surface_text(s, r.x + 20, text_y(r), text,
                       on ? (selected ? WGT_SEL_FG : WGT_FG) : WGT_FG_DIM,
                       hot ? WGT_HOT : WGT_PANE);
}

void espix_wgt_listrow(espix_surface_t *s, espix_rect_t r, const char *text,
                       const char *right, bool selected, bool on, bool hot)
{
    const espix_px_t bg = selected ? WGT_SEL : (hot ? WGT_HOT : WGT_PANE);
    const espix_px_t fg = !on ? WGT_FG_DIM : (selected ? WGT_SEL_FG : WGT_FG);
    const int        y  = text_y(r);

    espix_surface_fill(s, r, bg);

    /* Trimmed to the row: a long device name must not run under the column
     * beside it, and a list is where long names live. */
    const int rw = right != NULL ? espix_wgt_text_w(right) + PAD_X : 0;
    int       room = (r.w - PAD_X - rw) / WGT_TEXT_H;
    if (room < 0) {
        room = 0;
    }

    char       label[64];
    const int  n = room < (int)sizeof(label) - 1 ? room : (int)sizeof(label) - 1;
    snprintf(label, sizeof(label), "%.*s", n, text);
    espix_surface_text(s, r.x + PAD_X, y, label, fg, bg);

    if (right != NULL) {
        espix_surface_text(s, r.x + r.w - rw, y, right, fg, bg);
    }
}

void espix_wgt_navrow(espix_surface_t *s, espix_rect_t r, const char *text,
                      bool selected, bool hot)
{
    const espix_px_t bg = selected ? WGT_SEL : (hot ? WGT_HOT : WGT_NAV);

    espix_surface_fill(s, r, bg);
    espix_surface_text(s, r.x + PAD_X, text_y(r), text,
                       selected ? WGT_SEL_FG : WGT_FG, bg);
}

void espix_wgt_rule(espix_surface_t *s, espix_rect_t r)
{
    espix_surface_fill(s, r, WGT_EDGE);
}
