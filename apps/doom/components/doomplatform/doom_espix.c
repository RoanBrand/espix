/*
 * The platform seam: the six functions doomgeneric asks for, over espix_gfx
 * and the RTOS.
 *
 * Nothing here is Doom-specific except the key map. An engine that wants
 * "pixels, keys and milliseconds" is the shape espix_gfx was built for, so the
 * adapter is a conversion loop and a switch -- the whole thing a game needs
 * from espix, and nothing about espix in the game.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "espix_gfx.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "d_event.h"    /* event_t, ev_mouse, D_PostEvent */
#include "doomgeneric.h"
#include "doomkeys.h"
#include "i_video.h"    /* struct color, colors[256] -- global under CMAP256 */
#include "i_sound.h"    /* snd_musicdevice, SNDDEVICE_NONE */

#define DOOM_W 320
#define DOOM_H 200

extern espix_gfx_t         *doom_gfx;
extern espix_gfx_surface_t *doom_surface;

/* The engine's music device, which it defaults to Sound Blaster. Declared here
 * because there is no audio backend on this platform yet. */
extern int snd_musicdevice;

/*
 * Everything the engine's own update path depends on, checked once, here.
 *
 * I_FinishUpdate() walks line_out = DG_ScreenBuffer + x_offset and memcpy's
 * SCREENWIDTH bytes a row, without looking at either -- so a NULL screen buffer
 * (an unchecked malloc) or a bogus offset (s_Fb never filled) arrives as a fault
 * inside memcpy with nothing to say which it was. Upstream calls this hook
 * immediately after allocating that buffer and before D_DoomMain, so it is the
 * one place that can say.
 *
 * Startup only, deliberately: the frame path is DG_DrawFrame(), and a per-frame
 * check or log there would be paid for on every frame of the game.
 */
void DG_Init(void)
{
    /*
     * No music device, because there is no audio backend here to play through.
     *
     * This is not a nicety. snd_musicdevice defaults to SNDDEVICE_SB
     * (i_sound.c), and S_ChangeMusic swaps the intro for its OPL variant when
     * the device is SB or ADLIB: mus_introa, which the shareware WAD does not
     * contain, so the engine exits with "W_GetNumForName: d_introa not found!".
     * -nomusic does not prevent that -- the substitution does not consult it --
     * so the device itself is what has to say none, and this hook runs before
     * D_DoomMain, which is early enough to matter.
     */
    snd_musicdevice = SNDDEVICE_NONE;

    if (DG_ScreenBuffer == NULL) {
        printf("doom: the engine's screen buffer is NULL (%u bytes asked for) "
               "-- not starting\n",
               (unsigned)(DOOMGENERIC_RESX * DOOMGENERIC_RESY * 4));
        fflush(stdout);
        exit(1);
    }

    if (doom_gfx == NULL || doom_surface == NULL) {
        printf("doom: no graphics surface (gfx %p, surface %p) -- not starting\n",
               (void *)doom_gfx, (void *)doom_surface);
        fflush(stdout);
        exit(1);
    }

    printf("doom: screen buffer %p, surface %p\n",
           (void *)DG_ScreenBuffer, (void *)doom_surface);
    fflush(stdout);
}

/*
 * One frame: 8-bit palette indices -> RGB565 into the espix surface, then
 * present. Scaling to the canvas is the PPA's job, not ours.
 */
void DG_DrawFrame(void)
{
    if (doom_gfx == NULL || doom_surface == NULL) {
        return;
    }

    espix_gfx_fb_t fb = espix_gfx_surface_lock(doom_surface);
    const uint8_t *src = (const uint8_t *)DG_ScreenBuffer;

    for (int y = 0; y < DOOM_H; y++) {
        const uint8_t *s = src + (size_t)y * DOOM_W;
        uint16_t      *d = fb.pixels + (size_t)y * fb.stride;

        for (int x = 0; x < DOOM_W; x++) {
            const struct color c = colors[s[x]];
            d[x] = (uint16_t)(((c.r & 0xF8) << 8) | ((c.g & 0xFC) << 3) | (c.b >> 3));
        }
    }

    espix_gfx_surface_unlock(doom_surface);
    espix_gfx_present_surface(doom_gfx, doom_surface);
}

void DG_SleepMs(uint32_t ms)
{
    /* A zero delay still has to yield, or the idle task never gets a turn. */
    vTaskDelay(pdMS_TO_TICKS(ms ? ms : 1));
}

uint32_t DG_GetTicksMs(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/*
 * X11 keysym -- what RFB and the console both deliver -- to Doom key code. The
 * rest of the printable range is the character as sent, which is what Doom's
 * default bindings expect for letters and the number row.
 */
static unsigned char keysym_to_doom(uint32_t ks)
{
    switch (ks) {
    case 0xFF51: return KEY_LEFTARROW;
    case 0xFF52: return KEY_UPARROW;
    case 0xFF53: return KEY_RIGHTARROW;
    case 0xFF54: return KEY_DOWNARROW;
    case 0xFF0D: return KEY_ENTER;
    case 0xFF1B: return KEY_ESCAPE;
    case 0xFF09: return KEY_TAB;
    case 0xFF08: return KEY_BACKSPACE;
    /*
     * Shift, ctrl and alt keep their engine meanings: run, fire, and strafe.
     * Ctrl is KEY_FIRE and not KEY_RCTRL -- doomgeneric's scancode table says
     * so in a comment, and its SDL port does the same. KEY_RCTRL is a different
     * value, and sending it left the fire key dead.
     */
    case 0xFFE1: case 0xFFE2: return KEY_RSHIFT;
    case 0xFFE3: case 0xFFE4: return KEY_FIRE;
    case 0xFFE9: case 0xFFEA: return KEY_RALT;
    case 0xFFBE: return KEY_F1;
    case 0xFFBF: return KEY_F2;
    case 0xFFC0: return KEY_F3;
    case 0xFFC1: return KEY_F4;
    case 0xFFC2: return KEY_F5;
    case 0xFFC3: return KEY_F6;
    case 0xFFC4: return KEY_F7;
    case 0xFFC5: return KEY_F8;
    case 0xFFC6: return KEY_F9;
    case 0xFFC7: return KEY_F10;
    case 0xFFC8: return KEY_F11;
    case 0xFFC9: return KEY_F12;
    case 0x20:   return KEY_USE;    /* space: open doors, press switches */
    /*
     * Comma and period are Doom's own strafe keys, and the engine's defaults
     * name the codes KEY_STRAFE_L and KEY_STRAFE_R rather than the characters --
     * a rename doomgeneric made without remapping any platform. So the keys
     * have to be translated here or strafing with them does nothing at all.
     */
    case 0x2C:   return KEY_STRAFE_L;
    case 0x2E:   return KEY_STRAFE_R;
    case 0xFF13: return KEY_PAUSE;
    default:     break;
    }

    /*
     * As sent, not uppercased. Doom's config binds keys by the character the
     * platform reports, and its defaults are lower case ('y' confirms the quit
     * prompt); TranslateKey passes the value straight through, so folding case
     * here made 'y' arrive as 'Y' and nothing matched.
     */
    if (ks >= 0x21 && ks < 0x7F) {
        return (unsigned char)ks;
    }
    return 0;
}

/*
 * The mouse, held between calls because the engine only ever asks for keys.
 *
 * doomgeneric has one input hook -- DG_GetKey -- and I_GetEvent() drains it in a
 * while loop. There is no DG_GetMouse, and the engine's own SDL mouse case is
 * commented out, so the way in is the one that commented-out code used: build an
 * ev_mouse and hand it to D_PostEvent() from here. G_Responder() takes data1 as
 * the button bitmask and data2/data3 as the deltas, which is how a left click
 * becomes fire and a horizontal movement becomes a turn without either the
 * engine or this file knowing much about the other.
 *
 * espix delivers two shapes for one device and both are handled: a viewer says
 * where the pointer *is* (absolute, from RFB), a local mouse says how far it
 * *moved* (relative, HID). The first is differenced here, so the engine sees
 * deltas either way. An absolute source has the limitation an absolute source
 * has -- pushing the pointer against an edge stops producing movement, so a
 * turn stops there too -- and a local mouse has no such edge.
 */
#define MOUSE_GAIN 4

static int     s_mouse_x, s_mouse_y;    /* the last position a pointer event gave */
static bool    s_mouse_run;             /* ...and whether it continued one */
static int     s_mouse_dx, s_mouse_dy;  /* accumulated since the last post */
static uint8_t s_mouse_buttons;         /* as of the last POINTER event */
static uint8_t s_mouse_sent;            /* ...and what the engine was told */

static int mouse_delta(int d)
{
    /*
     * A gain, and a blunt one. The engine scales by (mouseSensitivity+5)/10 --
     * 1 at the default -- and then does angleturn -= mousex*8, which was
     * calibrated against a mouse reporting tens of counts per poll. A USB mouse
     * reports a handful per report, which turns about three degrees a second
     * unscaled. This is the number to change if it feels wrong; there is no
     * menu for it and no config file is read (see below).
     */
    return d * MOUSE_GAIN;
}

static void mouse_post(void)
{
    if (s_mouse_dx == 0 && s_mouse_dy == 0 && s_mouse_buttons == s_mouse_sent) {
        return;
    }

    event_t ev = { 0 };
    ev.type  = ev_mouse;
    ev.data1 = s_mouse_buttons;
    ev.data2 = mouse_delta(s_mouse_dx);

    /*
     * Vertical movement is reported as nothing, deliberately.
     *
     * The engine's mouse-forward -- forward += mousey, with no threshold -- is
     * the only thing it does with data3, and vanilla Doom expected a deliberate
     * push of a *relative* device. This platform's pointers are mostly absolute:
     * a viewer's cursor drifts vertically whenever it is moved horizontally, so
     * every turn would also walk the player. The engine's novert option would be
     * the answer if anything read it; it is declared as a config variable and
     * never consulted, and no config is read here anyway (the config directory is
     * the process's working directory, and the quit path skips M_SaveDefaults).
     * So the axis is dropped in the one place that can drop it, and the arrow
     * keys remain how you walk.
     */
    ev.data3 = 0;

    D_PostEvent(&ev);

    s_mouse_dx = s_mouse_dy = 0;
    s_mouse_sent = s_mouse_buttons;
}

int DG_GetKey(int *pressed, unsigned char *key)
{
    if (doom_gfx == NULL) {
        return 0;
    }

    espix_input_event_t ev;
    while (espix_gfx_poll_event(doom_gfx, &ev)) {
        if (ev.kind == ESPIX_INPUT_POINTER) {
            /*
             * A position is evidence of movement only when it continues a run
             * of positions.
             *
             * An absolute device -- a viewer -- sends one for every motion it
             * sees, so the difference between two of them *is* the movement. A
             * relative device sends MOTION for the movement and a POINTER only
             * when its buttons change, carrying wherever the pointer has got to
             * by then. Differencing *that* against the last button event
             * reports the whole distance travelled since the last click as one
             * delta: turn with a local mouse, click, and the view snapped to a
             * new direction. Which is exactly what it did.
             */
            if (s_mouse_run) {
                s_mouse_dx += ev.x - s_mouse_x;
                s_mouse_dy += ev.y - s_mouse_y;
            }
            s_mouse_x       = ev.x;
            s_mouse_y       = ev.y;
            s_mouse_buttons = ev.buttons;
            s_mouse_run     = true;
            continue;
        }
        if (ev.kind == ESPIX_INPUT_MOTION) {
            /* Already a delta, and unbounded by any edge. It also ends a run of
             * positions: whatever a POINTER says next is a place, not a move. */
            s_mouse_dx += ev.x;
            s_mouse_dy += ev.y;
            s_mouse_run = false;
            continue;
        }
        if (ev.kind != ESPIX_INPUT_KEY) {
            continue;
        }

        const unsigned char k = keysym_to_doom(ev.keysym);
        if (k == 0) {
            continue;
        }
        *pressed = ev.down ? 1 : 0;
        *key     = k;
        return 1;
    }

    /* No key left in the queue, which is where the mouse gets its turn: one
     * event per call, and the engine is at the bottom of the loop. */
    mouse_post();
    return 0;
}

void DG_SetWindowTitle(const char *title)
{
    (void)title;
}

/*
 * The engine expects a C library it does not always have. Doomgeneric uses
 * system() twice: to probe for a file browser it could spawn, and to raise a
 * graphical error box. A device has neither, so the honest answer is "no" --
 * and providing it here rather than in the ABI keeps "an app may run a shell
 * command" a decision espix has not made.
 */
int system(const char *command)
{
    (void)command;
    return -1;
}