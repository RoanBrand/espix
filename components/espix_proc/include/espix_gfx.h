/*
 * Graphics and input, for loadable apps.
 *
 * This is the surface a program draws on when it wants the whole screen rather
 * than a terminal: claim it, get a linear RGB565 framebuffer, draw, present,
 * poll input, release. It is deliberately not a widget toolkit -- a game, a
 * video player and a paint program all want "pixels, and the keys", and none of
 * them wants espix's window model.
 *
 * The shape is the one every software renderer already expects: a width, a
 * height, a row stride in pixels, and a pointer. That is what makes an engine
 * portable onto espix without an adapter, and it is why the types here are
 * plain C rather than espix's canvas object.
 *
 * What a screen is: the display service has exactly one owner at a time (the
 * console, the desktop, or this). espix_gfx_open() claims it, and
 * espix_gfx_close() releases it, at which point whatever owned it before -- or
 * the console -- comes back. A program that exits without closing does NOT
 * release: nothing does that for it, so whatever claims the screen next is what
 * takes it back, and a program that wants the console back should close.
 *
 * RGB565, not 888: the canvas is 565 end to end, PPA scales it, and the RFB
 * encoder converts it to whatever the viewer asked for. A surface that is not
 * the screen's size is scaled to fill it on present, which is how a renderer
 * keeps a small internal resolution.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "espix_display.h"      /* espix_input_event_t, espix_rect_t */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct espix_gfx espix_gfx_t;

/* A locked, drawable buffer. stride is in pixels, not bytes. */
typedef struct {
    int       w, h, stride;
    uint16_t *pixels;
} espix_gfx_fb_t;

/* ------------------------------------------------------------------ */
/* The screen                                                          */
/* ------------------------------------------------------------------ */

/*
 * Claim the screen. Returns NULL when there is no display up -- an app run
 * before 'vnc start' or 'display start' has nowhere to draw, and saying so is
 * better than drawing into nothing.
 *
 * espix_gfx_open_mode() is the same claim with a video mode: full_w and full_h
 * are the canvas the app wants to itself. A full-screen game names its own
 * size, and the desktop's resolution comes back when it closes. 0,0 -- what
 * espix_gfx_open() passes -- means "leave the canvas as it is", which is right
 * for an app that draws into a small surface and is happy for the display to
 * scale it up.
 *
 * The size is a claim, not a request: one the display will not take is refused
 * and logged, and the app draws at whatever it got.
 */
espix_gfx_t *espix_gfx_open(void);
espix_gfx_t *espix_gfx_open_mode(int full_w, int full_h);

/* Release the screen. Idempotent; NULL is a no-op. */
void         espix_gfx_close(espix_gfx_t *g);

/*
 * The screen's pixels, locked, at its native size. Pair every lock with
 * espix_gfx_present(), which marks what changed and unlocks -- or with
 * espix_gfx_unlock() to release it having drawn nothing.
 */
espix_gfx_fb_t espix_gfx_lock(espix_gfx_t *g);
void           espix_gfx_present(espix_gfx_t *g);
void           espix_gfx_present_rect(espix_gfx_t *g, int x, int y, int w, int h);
void           espix_gfx_unlock(espix_gfx_t *g);

/* ------------------------------------------------------------------ */
/* Off-screen surfaces                                                 */
/* ------------------------------------------------------------------ */

/*
 * A render target that is not the screen, so a program can draw at its own
 * resolution and let the hardware stretch it. PSRAM, RGB565.
 */
typedef struct espix_gfx_surface espix_gfx_surface_t;

espix_gfx_surface_t *espix_gfx_surface_new(int w, int h);
void                 espix_gfx_surface_free(espix_gfx_surface_t *s);
espix_gfx_fb_t       espix_gfx_surface_lock(espix_gfx_surface_t *s);
void                 espix_gfx_surface_unlock(espix_gfx_surface_t *s);

/*
 * Scale the whole surface to fill the screen, and mark it changed. PPA SRM
 * where there is one, a nearest-neighbour loop where there is not.
 */
void espix_gfx_present_surface(espix_gfx_t *g, const espix_gfx_surface_t *s);

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

/*
 * One pending event, or false when there is none. Never blocks: a game loop
 * polls this once a frame and gets on with drawing.
 *
 * POINTER events carry absolute canvas coordinates and the button mask; MOTION
 * carries a delta; KEY carries an X11 keysym (see espix_keysym_char() for the
 * character a key stands for).
 */
bool espix_gfx_poll_event(espix_gfx_t *g, espix_input_event_t *ev);

#ifdef __cplusplus
}
#endif
