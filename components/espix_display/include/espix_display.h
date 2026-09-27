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
espix_px_t *espix_canvas_pixels(espix_canvas_t *c);   /* w*h, RGB565 */

/* Drawing. All clip to the canvas and mark what they touched. */
void espix_canvas_fill(espix_canvas_t *c, espix_rect_t r, espix_px_t px);
void espix_canvas_blit(espix_canvas_t *c, int dst_x, int dst_y,
                       const espix_px_t *src, int src_w, int src_h, int src_stride);
void espix_canvas_outline(espix_canvas_t *c, espix_rect_t r, espix_px_t px);
void espix_canvas_text(espix_canvas_t *c, int x, int y, const char *s,
                       espix_px_t fg, espix_px_t bg);

/* Damage: the rectangles changed since the last drain. */
void   espix_canvas_damage(espix_canvas_t *c, espix_rect_t r);
size_t espix_canvas_damage_take(espix_canvas_t *c, espix_rect_t *out, size_t max);
void   espix_canvas_damage_clear(espix_canvas_t *c);
bool   espix_canvas_damaged(const espix_canvas_t *c);

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
    ESPIX_INPUT_POINTER = 0,
    ESPIX_INPUT_KEY,
} espix_input_kind_t;

typedef struct {
    espix_input_kind_t kind;
    uint16_t           x, y;      /* POINTER: absolute, canvas coordinates */
    uint8_t            buttons;   /* POINTER: RFB button mask */
    uint32_t           keysym;    /* KEY: an X11 keysym, as RFB delivers */
    bool               down;      /* KEY */
} espix_input_event_t;

/* Post an event from a source. Never blocks; a full queue drops the event. */
void espix_display_input(const espix_input_event_t *ev);

/* ------------------------------------------------------------------ */
/* Service                                                             */
/* ------------------------------------------------------------------ */

/* The desktop canvas, or NULL when the display is down. */
espix_canvas_t *espix_display_canvas(void);
bool            espix_display_ready(void);

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
