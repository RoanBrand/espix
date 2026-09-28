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

/* Draw the content. Called with the surface locked, in surface coordinates. */
typedef void (*espix_window_draw_fn)(espix_window_t *w, espix_surface_t *s,
                                     void *ctx);

/*
 * A key for the focused window. `keysym` is X11, as it is everywhere else here,
 * so a window cannot tell a local keyboard from a viewer's.
 */
typedef void (*espix_window_key_fn)(espix_window_t *w, uint32_t keysym,
                                    bool down, void *ctx);

espix_window_t *espix_window_new(int x, int y, int w, int h, const char *title);
void            espix_window_free(espix_window_t *w);

void espix_window_set_ctx(espix_window_t *w, void *ctx);
void espix_window_set_draw(espix_window_t *w, espix_window_draw_fn fn);
void espix_window_set_key(espix_window_t *w, espix_window_key_fn fn);

espix_surface_t *espix_window_surface(espix_window_t *w);

/* Where the content goes, in the surface's own coordinates. */
espix_rect_t espix_window_content(const espix_window_t *w);

/* Structure changed: the desktop repaints, because what was underneath moved. */
void espix_window_move(espix_window_t *w, int x, int y);
void espix_window_raise(espix_window_t *w);
void espix_window_focus(espix_window_t *w);

/* Content changed: this window is redrawn and blitted, and nothing else is. */
void espix_window_repaint(espix_window_t *w);

/* Everything: background, every window in z-order, the cursor. */
void espix_desktop_repaint(void);

#ifdef __cplusplus
}
#endif