/*
 * The desktop: what is on the screen when something is.
 *
 * A client of the display service, not part of it -- the same relationship the
 * on-screen console has. espix_display owns the canvas, the surfaces, the
 * pointer and the backends; this owns what is drawn on them and claims the
 * screen as an owner when it is asked to.
 *
 * It lives in the kernel image for now, which is a decision about packaging
 * rather than about layering: the same code moves to apps/ once the client
 * surface it draws through has settled, because an ABI is easier to keep than
 * to change.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "espix_display.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Claim the screen with the desktop, or give it back.
 *
 * Claiming takes over rather than being refused, so starting it twice is
 * harmless. ESP_ERR_INVALID_STATE means there is no canvas to claim, which is
 * what `desktop start` as the first command after a boot looks like.
 */
esp_err_t espix_desktop_start(void);
void      espix_desktop_stop(void);

/* ------------------------------------------------------------------ */
/* Windows                                                             */
/* ------------------------------------------------------------------ */

/*
 * A window is a surface and a place to put it.
 *
 * The surface holds the whole window -- frame included -- because the desktop
 * draws the frame and the client draws the content into the same buffer, so
 * compositing is one blit per window and moving one is a blit at a new point
 * rather than a redraw. That is the arrangement an accelerator wants, and it is
 * why the frame is drawn here rather than by whoever owns the window.
 *
 * A client never sees the frame's geometry: espix_window_content() is the
 * rectangle it may draw in, in surface coordinates, and the rest is the
 * desktop's.
 */
typedef struct espix_window espix_window_t;

#define ESPIX_WINDOW_TITLE_MAX 48

/*
 * Draw the content.
 *
 * `r` is the region that needs redrawing, in the surface's coordinates, and
 * only what intersects it has to be drawn -- which is the whole point: a change
 * of one cell should cost one cell, not a window. Called with the surface locked.
 */
typedef void (*espix_window_draw_fn)(espix_window_t *w, espix_surface_t *s,
                                     espix_rect_t r, void *ctx);

/*
 * A key for the focused window. `keysym` is X11, as it is everywhere else here,
 * so a window cannot tell a local keyboard from a viewer's.
 */
typedef void (*espix_window_key_fn)(espix_window_t *w, uint32_t keysym,
                                    bool down, void *ctx);

/*
 * The window changed size, and here is the new content rectangle.
 *
 * Called after the surface has been replaced and before anything is composited,
 * so a client lays itself out once rather than redrawing into a surface whose
 * geometry it has not been told about. A terminal resizes its grid here; a
 * viewer rescales what it is showing.
 */
typedef void (*espix_window_resize_fn)(espix_window_t *w, espix_rect_t content,
                                       void *ctx);

/*
 * A press or a release inside the window's content, in *surface* coordinates --
 * the same ones the draw callback and espix_window_content() use, so a client
 * hit-tests against exactly what it drew. A press on the title bar never arrives
 * here at all.
 *
 * The release is delivered to whichever window took the press, even if the
 * pointer has left it since, which is what a button needs in order to know it
 * was released rather than abandoned.
 */
typedef void (*espix_window_pointer_fn)(espix_window_t *w, int x, int y,
                                        uint8_t buttons, void *ctx);

espix_window_t *espix_window_new(int x, int y, int w, int h, const char *title);
void            espix_window_free(espix_window_t *w);

void espix_window_set_ctx(espix_window_t *w, void *ctx);
void espix_window_set_draw(espix_window_t *w, espix_window_draw_fn fn);
void espix_window_set_key(espix_window_t *w, espix_window_key_fn fn);
void espix_window_set_resize(espix_window_t *w, espix_window_resize_fn fn);
void espix_window_set_pointer(espix_window_t *w, espix_window_pointer_fn fn);

/*
 * A new size, and a new surface to go with it -- because the surface is the
 * window, frame included, so a resize cannot be a bigger blit of a smaller
 * buffer. The content callback is told in between, and the region the window
 * used to cover is put back.
 *
 * False when the surface could not be allocated, in which case nothing moved.
 */
bool espix_window_resize(espix_window_t *w, int x, int y, int width, int height);

espix_surface_t *espix_window_surface(espix_window_t *w);

/* Where the content goes, in the surface's own coordinates. */
espix_rect_t espix_window_content(const espix_window_t *w);

/* Structure changed: the desktop repaints, because what was underneath moved. */
void espix_window_move(espix_window_t *w, int x, int y);
void espix_window_raise(espix_window_t *w);
void espix_window_focus(espix_window_t *w);

/*
 * These pixels changed: redraw them, into the surface and onto the canvas.
 *
 * The window's frame is redrawn too if the region reaches it, so a caller never
 * has to know which parts of a window are the desktop's -- only which parts it
 * changed.
 */
void espix_window_damage(espix_window_t *w, espix_rect_t r);

/* All of it: for a caller that does not track cells, and after a frame change. */
void espix_window_repaint(espix_window_t *w);

/* Everything: background, every window in z-order, the cursor. */
void espix_desktop_repaint(void);

/*
 * The windows the desktop ships with. Each opens its window the first time and
 * brings back the one that is already there after that -- so the caller does not
 * have to know whether it has been opened before, which is the difference
 * between a menu item and a state machine.
 */
espix_window_t *espix_settings_open(void);

/* It has been closed: the desktop forgets it, and so must this. */
void espix_settings_forget(void);
espix_window_t *espix_settings_window(void);

#ifdef __cplusplus
}
#endif