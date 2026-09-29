/*
 * espix display service.
 *
 * A framebuffer in PSRAM, one input queue, and a backend that serves the
 * framebuffer over RFB (the protocol every VNC client speaks). The reason the
 * backend exists at all is that a desktop is developable before there is a
 * panel: any VNC client is the monitor. That matters here because the
 * accelerators worth having -- PPA for fill/blit/scale/convert, the JPEG codec
 * for encode, 2D-DMA for moves -- are all testable against a virtual screen,
 * and were otherwise blocked on hardware we do not have.
 *
 * The canvas is RGB565 because that is the format the hardware path wants:
 * PPA converts and scales it, the JPEG encoder eats it, and an RGB panel
 * matches it. A client that asks for something else is converted on the way
 * out (rfb.c) -- which is precisely the CPU fallback PPA will replace.
 *
 * Nothing here is allocated at boot. "vnc start" brings the desktop up and
 * "vnc stop" takes it down, the way Bluetooth and audio are already lazy: a
 * board that never opens a remote screen never pays for one.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The default desktop. 800x600 RGB565 is 960 KiB -- two of them, with the
 * staging copy the server sends from, still leave PSRAM nearly empty. */
#define ESPIX_DISPLAY_W         800
#define ESPIX_DISPLAY_H         600
#define ESPIX_DISPLAY_NAME_MAX  64

/* Most damage rectangles one update can carry. The list coalesces as it fills,
 * so this is a ceiling on work per frame rather than a limit on correctness. */
#define ESPIX_DISPLAY_DAMAGE_MAX 32

/* RGB565. */
typedef uint16_t espix_px_t;

typedef struct {
    int x, y, w, h;
} espix_rect_t;

/* Pending moves a backend will be told about; see espix_canvas_moved(). */
#define ESPIX_DISPLAY_MOVE_MAX 8

/*
 * Pixels that are where they are because they were somewhere else, and where
 * that was.
 *
 * This is what a dragged window is almost entirely made of, and it is worth
 * saying out loud because it is worth more than any amount of drawing faster: a
 * client that already has those pixels can be told to move them, which is
 * sixteen bytes on the wire rather than the hundred kilobytes the same
 * rectangle costs as pixels. RFC 6143 calls it CopyRect.
 */
typedef struct {
    espix_rect_t r;             /* where the pixels are now */
    int          sx, sy;        /* where they were */
} espix_move_t;

typedef struct espix_canvas espix_canvas_t;

/* ------------------------------------------------------------------ */
/* Canvas                                                              */
/* ------------------------------------------------------------------ */

/*
 * An RGB565 surface and the damage list the backend drains. Allocated in
 * PSRAM; NULL if there is not room.
 */
espix_canvas_t *espix_canvas_new(int w, int h, const char *name);
void            espix_canvas_free(espix_canvas_t *c);

/*
 * Replace the canvas's pixel buffer at a new size, in place.
 *
 * The object keeps its identity, so a caller that fetched it before the resize
 * is not left holding freed memory; only the buffer changes. Everything the
 * canvas held is discarded and the whole of it is marked damaged, because a
 * resize is a full repaint from the owner's model.
 */
esp_err_t       espix_canvas_resize(espix_canvas_t *c, int w, int h);

int         espix_canvas_width(const espix_canvas_t *c);
int         espix_canvas_height(const espix_canvas_t *c);
const char *espix_canvas_name(const espix_canvas_t *c);

/*
 * Locking. The desktop task draws, the RFB connection task reads, and every
 * mutation and every read of the pixels happens under this. The server copies
 * out of the canvas under the lock and encodes outside it, so a slow client
 * cannot freeze the cursor.
 */
void        espix_canvas_lock(espix_canvas_t *c);
void        espix_canvas_unlock(espix_canvas_t *c);
/* The holder died still holding it; the lock stops protecting the canvas
 * rather than be force-released onto a freed task handle. See display.c. */
void        espix_canvas_orphan(espix_canvas_t *c);
espix_px_t *espix_canvas_pixels(espix_canvas_t *c);   /* w*h, RGB565 */

/* Drawing. All clip to the canvas and mark what they touched. */
void espix_canvas_fill(espix_canvas_t *c, espix_rect_t r, espix_px_t px);
void espix_canvas_blit(espix_canvas_t *c, int dst_x, int dst_y,
                       const espix_px_t *src, int src_w, int src_h, int src_stride);
void espix_canvas_outline(espix_canvas_t *c, espix_rect_t r, espix_px_t px);
void espix_canvas_text(espix_canvas_t *c, int x, int y, const char *s,
                       espix_px_t fg, espix_px_t bg);

/* Damage: the rectangles changed since the last drain. */
/*
 * Move the pixels that are in `r` from (sx, sy), and note that they moved.
 *
 * The move is the point and the note is the second half of it. A dragged window
 * is the same pixels somewhere else, so the canvas should slide them rather than
 * repaint them -- which measured 5.7 ms a motion as a fill and a blit, and is
 * well under one as a row-wise memmove. And a *client* that already has those
 * pixels can be told to move them too, which is CopyRect.
 *
 * The destination is not marked as damaged: an accelerated backend sends the
 * copy as the update for those pixels and must not also send them. So a caller
 * has to repair whatever this did *not* cover -- the strips it left behind.
 *
 * The source and the destination must both be inside the canvas.
 */
void   espix_canvas_move(espix_canvas_t *c, espix_rect_t r, int sx, int sy);

/* The note without the move, for a caller that has already painted the pixels. */
void   espix_canvas_moved(espix_canvas_t *c, espix_rect_t r, int sx, int sy);
size_t espix_canvas_move_take(espix_canvas_t *c, espix_move_t *out, size_t max);
void   espix_canvas_move_clear(espix_canvas_t *c);

void   espix_canvas_damage(espix_canvas_t *c, espix_rect_t r);
size_t espix_canvas_damage_take(espix_canvas_t *c, espix_rect_t *out, size_t max);
void   espix_canvas_damage_clear(espix_canvas_t *c);
bool   espix_canvas_damaged(const espix_canvas_t *c);

/* ------------------------------------------------------------------ */
/* Benchmark                                                           */
/* ------------------------------------------------------------------ */

/*
 * What one operation at one size costs, on each path that exists.
 *
 * `us_sw` is the software path -- the only one on the S3, which has no PPA, no
 * 2D-DMA and no JPEG codec at all, and therefore the one every target keeps.
 * `us_hw` is the accelerated path and is zero when there is none, with `hw`
 * naming it. Both are measured on the same board in the same run, because a
 * number from another chip is not a comparison.
 */
typedef struct {
    const char *op;         /* "fill" or "blit" */
    int         w, h;
    uint32_t    iters;
    uint64_t    px_per_iter;/* pixels touched per iteration, for the rate */
    uint32_t    us_sw;      /* the whole run, microseconds */
    uint32_t    us_hw;      /* ...and accelerated; 0 when there is none */
    const char *hw;         /* "PPA FILL", "2D-DMA", ... or NULL */
    bool        hw_ok;      /* the accelerated path produced the right pixels */
} espix_display_bench_t;

#define ESPIX_DISPLAY_BENCH_MAX 16

/* Fills up to `max` rows and returns how many. Allocates its own buffers. */
size_t espix_display_bench(espix_display_bench_t *out, size_t max);

/* ------------------------------------------------------------------ */
/* Surfaces                                                            */
/* ------------------------------------------------------------------ */

/*
 * A window's own pixels.
 *
 * The canvas is what a backend sends; a surface is what a window draws into
 * before the compositor puts it on the canvas. That distinction is the whole
 * reason for it: moving a window becomes a blit rather than a redraw, occlusion
 * and z-order fall out of the order the compositor blits in, and there is
 * something for an accelerator to accelerate -- a single canvas has nothing to
 * copy.
 *
 * Same primitives as the canvas and no damage list: what changed is the
 * compositor's business, and it knows, because it did the blitting.
 */
typedef struct espix_surface espix_surface_t;

/* PSRAM first, internal as a fallback, NULL when neither fits. */
espix_surface_t *espix_surface_new(int w, int h);
void             espix_surface_free(espix_surface_t *s);

int         espix_surface_width(const espix_surface_t *s);
int         espix_surface_height(const espix_surface_t *s);
espix_px_t *espix_surface_pixels(espix_surface_t *s);

/* The compositor and the window's own task both touch these pixels. */
void        espix_surface_lock(espix_surface_t *s);
void        espix_surface_unlock(espix_surface_t *s);

/* Drawing. All clip to the surface. None of them marks anything. */
void espix_surface_fill(espix_surface_t *s, espix_rect_t r, espix_px_t px);
/*
 * Scale the whole of `src` into `dst`, which must already be the size it should
 * end up. Both are RGB565.
 *
 * PPA SRM does it where there is hardware -- the hardware that already does the
 * 1:1 blit, at a size rather than at a copy -- and a nearest-neighbour loop does
 * it where there is not, which on the S3 is every time. The two are the same
 * function on purpose: a caller that fits an image to a window should not have to
 * know which board it is on, and the software path is the one that gets measured
 * against the hardware rather than the other way round.
 */
esp_err_t espix_surface_scale(espix_surface_t *dst, const espix_surface_t *src);

void espix_surface_blit(espix_surface_t *s, int dst_x, int dst_y,
                        const espix_px_t *src, int src_w, int src_h,
                        int src_stride);
void espix_surface_outline(espix_surface_t *s, espix_rect_t r, espix_px_t px);
void espix_surface_text(espix_surface_t *s, int x, int y, const char *str,
                        espix_px_t fg, espix_px_t bg);

/*
 * The compositor's one operation: a surface onto the canvas at (x, y), clipped
 * to the canvas, marking the damage. An accelerator's natural unit, and the
 * call that makes a window move cheap.
 */
void espix_canvas_blit_surface(espix_canvas_t *c, int x, int y,
                               const espix_surface_t *s);

/*
 * A rectangle of one, which is the compositor's operation most of the time.
 *
 * A window whose content changed by one cell should send one cell: redrawing
 * the whole surface and blitting all of it is the difference between a cursor
 * that keeps up and one that does not.
 */
void espix_canvas_blit_surface_rect(espix_canvas_t *c, int x, int y,
                                    const espix_surface_t *s, espix_rect_t r);

/*
 * Scale a surface to fill the canvas -- the whole-canvas case a game wants:
 * render small, and let PPA stretch it to whatever the screen is.
 */
void espix_canvas_scale_surface(espix_canvas_t *c, const espix_surface_t *s);

/* ------------------------------------------------------------------ */
/* Images                                                              */
/* ------------------------------------------------------------------ */

/*
 * Decode a JPEG held in memory into a new surface, or NULL.
 *
 * In memory, because the caller owns the file: reading it is not the display
 * service's job, and decoding is. The software path is TJpgDec in the ROM, which
 * the S3 and the S31 both carry -- so the fallback every target needs costs
 * nothing but the call, and the S3, which has no JPEG block at all, decodes in
 * software because that is all there is. The hardware path is the JPEG codec,
 * which the S31 and the P4 have.
 *
 * The caller frees the surface.
 */
espix_surface_t *espix_image_jpeg(const uint8_t *jpg, size_t len);

/*
 * Both paths on one file, for the benchmark: software, and the codec where there
 * is one, with hw_ok saying whether the two agree -- so a fast wrong answer is
 * not worth more than a slow right one. Fills one row and returns false when the
 * file cannot be decoded at all. Which path espix_image_jpeg() picks is the
 * display service's business; this is the comparison, and it is the only reason
 * the hardware path is reachable other than through a picture on the screen.
 */
bool espix_image_bench(espix_display_bench_t *row, const uint8_t *jpg, size_t len);

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

/*
 * One event queue, many sources: today the RFB connection, later USB HID and
 * touch. The desktop consumes the queue and knows nothing about where an
 * event came from -- which is the whole reason a remote mouse works before a
 * local one exists.
 */
typedef enum {
    /* A viewer saying where the pointer *is*. Absolute, canvas coordinates. */
    ESPIX_INPUT_POINTER = 0,

    /*
     * A local device saying how far it *moved*. Relative, because that is what
     * a mouse reports -- and because the cursor's position has to live in
     * exactly one place. The desktop owns it, so a local mouse and a remote
     * one move the same cursor instead of each keeping its own idea of where it
     * is and fighting.
     */
    ESPIX_INPUT_MOTION,

    ESPIX_INPUT_KEY,
} espix_input_kind_t;

typedef struct {
    espix_input_kind_t kind;

    /* POINTER: absolute. MOTION: a delta. Signed, because a mouse goes left. */
    int16_t            x, y;
    /*
     * POINTER: the RFB button mask, and the only event that carries one.
     *
     * MOTION does not, because a mouse's motion report is not where its buttons
     * live -- so an owner that cares about a drag holds this from the last
     * POINTER event and treats a change to or from zero as the press and the
     * release. Both sources deliver it that way: RFB in the event itself, and a
     * local mouse as a POINTER event when its button byte changes.
     */
    uint8_t            buttons;
    uint32_t           keysym;    /* KEY: an X11 keysym, as RFB delivers */
    bool               down;      /* KEY */
} espix_input_event_t;

/* Post an event from a source. Never blocks. */
void espix_display_input(const espix_input_event_t *ev);

/* The pointer's position, which the service owns because two sources feed it:
 * a viewer says where it is, a local mouse says how far it moved. Rendering it
 * is the owner's business -- a text console has no arrow. */
void espix_display_pointer(int *x, int *y);

/*
 * The character an X11 keysym stands for, or 0 for keys with no text meaning.
 * Here rather than in each source so that a viewer, a local keyboard and the
 * console cannot disagree about what a key produces.
 */
char espix_keysym_char(uint32_t keysym);

/* ------------------------------------------------------------------ */
/* The screen's owner                                                  */
/* ------------------------------------------------------------------ */

/*
 * The screen has exactly one owner. There is no window list, no focus and no
 * arbitration: ownership means "whose model is rendered to the canvas", so each
 * side keeps its own state and a switch is a repaint. That is what lets a
 * console keep running, invisibly, while a desktop is in front of it.
 *
 * Callbacks run in the context of whoever posted the input -- the RFB
 * connection task -- so they must be quick and must not block.
 */
typedef struct {
    const char *name;
    void (*input)(void *ctx, const espix_input_event_t *ev);
    void (*repaint)(void *ctx);   /* redraw the whole canvas from your model */
    /*
     * The canvas changed size. Re-clamp whatever absolute coordinates you hold
     * and repaint. Optional: a screen that lays itself out relative to the
     * canvas every time needs nothing here. Called with the canvas unlocked.
     */
    void (*resized)(void *ctx);
    void *ctx;
} espix_screen_t;

esp_err_t   espix_display_claim(const espix_screen_t *screen);
void        espix_display_release(const espix_screen_t *screen);
const char *espix_display_owner(void);   /* "" when the default content is up */

/*
 * Is this screen the one being rendered?
 *
 * Claiming gives a screen the canvas; it does not take it away from anyone
 * else, because a screen that has been taken over keeps its own model and
 * simply stops being drawn. So a screen that draws checks this first -- the
 * console does, and without it output from a console that is no longer on the
 * screen is painted over whatever replaced it.
 */
bool        espix_display_owns(const espix_screen_t *screen);

/*
 * What a viewer gets when nothing else owns the screen -- the on-screen
 * console. Registered by main, which is the only place that knows both the
 * display and the shell, and started only while a viewer is attached, so a
 * headless board pays for none of it.
 */
typedef struct {
    esp_err_t (*start)(void);   /* claim the screen and begin drawing */
    void      (*stop)(void);    /* release it and stop */
} espix_display_default_t;

/*
 * Whatever a viewer gets when nothing owns the screen -- the console, today.
 * The service does not draw it: it asks, and if the answer is no it clears the
 * canvas and says so.
 */
void espix_display_set_default(const espix_display_default_t *def);
void espix_display_viewer_attached(void);
void espix_display_viewer_detached(void);

/* ------------------------------------------------------------------ */
/* Service                                                             */
/* ------------------------------------------------------------------ */

/* The desktop canvas, or NULL when the display is down. */
espix_canvas_t *espix_display_canvas(void);
bool            espix_display_ready(void);

/*
 * Resize the screen, live. Reallocates the canvas, marks it damaged, and tells
 * the current owner so it can re-lay out. A viewer keeps its connection: the
 * RFB backend reports the new size with the ExtendedDesktopSize pseudo-encoding
 * and reallocates its staging copy. Fails with ESP_ERR_NO_MEM if the new buffer
 * does not fit, leaving the current size in place.
 */
esp_err_t       espix_display_resize(int w, int h);

/* Whether a size is one this service will take. Shared so the command, the
 * settings panel and the config file cannot disagree about the range. */
bool            espix_display_size_ok(int w, int h);

/*
 * Bring the desktop up, and take it down.
 *
 * Independent of any backend, because the canvas is what every output shares: a
 * panel needs it with no network at all, local input needs somewhere to land,
 * and a VNC session comes and goes underneath. So this is the only thing that
 * frees the canvas; stopping a backend is not.
 */
esp_err_t espix_display_start(void);
void      espix_display_stop(void);

/*
 * Bring the desktop up and listen for RFB clients on port. Idempotent:
 * starting an already-running server with the same port is a no-op, with a
 * different one it is an error.
 */
esp_err_t espix_display_vnc_start(uint16_t port);
void      espix_display_vnc_stop(void);

bool        espix_display_vnc_running(void);
uint16_t    espix_display_vnc_port(void);
int         espix_display_vnc_clients(void);
const char *espix_display_vnc_peer(void);   /* "192.168.1.5:52344", or "" */
/* What the last client offered, which decides whether a drag can be copies. */
const char *espix_display_vnc_encodings(void);

/*
 * The password VNC authentication falls back to when none has been set.
 *
 * Public on purpose. It exists so that a client which insists on a password --
 * macOS Screen Sharing, which will not connect without one -- works with no
 * setup at all, not to keep anyone out. Change it with `vnc password`, or turn
 * authentication off entirely with `vnc nopassword`.
 */
#define ESPIX_DISPLAY_VNC_DEFAULT_PASSWORD "espix"

/*
 * VNC authentication (RFB security type 2).
 *
 * The scheme is a single DES challenge and is weak by any modern measure: it
 * proves the client knows the password and protects nothing afterwards. It is
 * here for compatibility, because macOS Screen Sharing insists on a password
 * and against security type None waits for a challenge that never comes until
 * it gives up. Only the first eight characters are used, which is a property of
 * VNC rather than of this implementation.
 *
 * When a key exists the 3.3 handshake dictates authentication and the 3.7+
 * handshake offers both, None first -- so a viewer happy without a password
 * keeps working without one. That means the password is a compatibility
 * mechanism rather than a security boundary until the list is narrowed to
 * authentication alone.
 */
void espix_display_vnc_set_password(const char *pw);   /* NULL turns it off */
bool espix_display_vnc_auth_required(void);
bool espix_display_vnc_auth_is_default(void);

#ifdef __cplusplus
}
#endif
