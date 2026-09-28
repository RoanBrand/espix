/*
 * Widgets: the handful of shapes a panel is made of.
 *
 * Deliberately not a toolkit. There is no widget tree, no layout engine and no
 * event loop here -- a widget is a function that draws itself into a rectangle
 * and a rectangle is what a caller hit-tests. What it buys is that a button
 * looks like a button in every panel, including the ones written later, and that
 * "disabled" is one flag rather than a decision each caller makes differently.
 *
 * The drawing is into a surface, which is what a window's content is. Nothing
 * here knows about windows, so a widget can be drawn anywhere there are pixels.
 *
 * Two things every caller needs and none of them should guess: WGT_TEXT_H, and
 * espix_wgt_text_w() for how wide a string will be.
 */

#pragma once

#include <stdbool.h>

#include "espix_display.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One row of text, and the padding a row of them wants. */
/*
 * A row is two text rows tall, and the text sits on the top one. That is not
 * only tidier than centring: an 8-pixel font wants its origins on 8-pixel
 * boundaries, and a row of 20 with the text centred lands every glyph four
 * pixels off the grid -- which is invisible until something tries to read the
 * screen back a cell at a time, and then it is a page of question marks.
 */
#define WGT_TEXT_H 8
#define WGT_ROW_H  16

#define WGT_RGB(r, g, b) \
    ((espix_px_t)((((r) & 0xF8u) << 8) | (((g) & 0xFCu) << 3) | (((b) & 0xF8u) >> 3)))

/* The theme, which is the desktop's, in one place. */
#define WGT_BG       WGT_RGB(0x10, 0x14, 0x1A)
#define WGT_PANE     WGT_RGB(0x16, 0x1A, 0x20)
#define WGT_NAV      WGT_RGB(0x1B, 0x20, 0x28)
#define WGT_EDGE     WGT_RGB(0x3A, 0x42, 0x52)
#define WGT_FG       WGT_RGB(0xC8, 0xD8, 0xE8)
#define WGT_FG_DIM   WGT_RGB(0x74, 0x7E, 0x8C)     /* disabled, and it reads so */
#define WGT_HEAD_FG  WGT_RGB(0xE8, 0xEC, 0xF2)
#define WGT_HOT      WGT_RGB(0x2E, 0x36, 0x44)
#define WGT_SEL      WGT_RGB(0x4C, 0x6E, 0xA8)
#define WGT_SEL_FG   WGT_RGB(0xF0, 0xF4, 0xFA)
#define WGT_BTN      WGT_RGB(0x2A, 0x30, 0x3A)
#define WGT_BTN_HOT  WGT_RGB(0x3A, 0x44, 0x54)
#define WGT_BTN_DIM  WGT_RGB(0x20, 0x24, 0x2C)

/* How wide a string will be, so a caller can centre or right-align one. */
int espix_wgt_text_w(const char *text);

bool espix_wgt_hit(espix_rect_t r, int x, int y);

/* The panel a section sits on: the fill and nothing else. */
void espix_wgt_panel(espix_surface_t *s, espix_rect_t r, espix_px_t bg);

/* A section heading, in the larger colour, and a rule under it. */
void espix_wgt_heading(espix_surface_t *s, espix_rect_t r, const char *text);

/* A line of text. `on` false draws it in the disabled grey. */
void espix_wgt_label(espix_surface_t *s, espix_rect_t r, const char *text, bool on);

/* A line of text with a value on the right, which is what a status panel is. */
void espix_wgt_field(espix_surface_t *s, espix_rect_t r, const char *name,
                     const char *value, bool on);

/* A button. `hot` is the pointer over it; `on` false greys it and the caller
 * is responsible for not acting on it. */
void espix_wgt_button(espix_surface_t *s, espix_rect_t r, const char *text,
                      bool on, bool hot);

/* A radio row: a ring, and a dot in it when it is the chosen one. */
void espix_wgt_radio(espix_surface_t *s, espix_rect_t r, const char *text,
                     bool selected, bool on, bool hot);

/* A row of a list: text, and an optional right-hand column. */
void espix_wgt_listrow(espix_surface_t *s, espix_rect_t r, const char *text,
                       const char *right, bool selected, bool on, bool hot);

/* A row of the left-hand navigation: a list row that fills when it is chosen. */
void espix_wgt_navrow(espix_surface_t *s, espix_rect_t r, const char *text,
                      bool selected, bool hot);

/* A horizontal rule across a rectangle. */
void espix_wgt_rule(espix_surface_t *s, espix_rect_t r);

#ifdef __cplusplus
}
#endif
