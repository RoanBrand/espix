/*
 * The platform seam: the six functions doomgeneric asks for, over espix_gfx
 * and the RTOS.
 *
 * Nothing here is Doom-specific except the key map. An engine that wants
 * "pixels, keys and milliseconds" is the shape espix_gfx was built for, so the
 * adapter is a conversion loop and a switch -- the whole thing a game needs
 * from espix, and nothing about espix in the game.
 */

#include <ctype.h>
#include <stddef.h>
#include <stdint.h>

#include "espix_gfx.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "doomgeneric.h"
#include "doomkeys.h"
#include "i_video.h"    /* struct color, colors[256] -- global under CMAP256 */

#define DOOM_W 320
#define DOOM_H 200

extern espix_gfx_t         *doom_gfx;
extern espix_gfx_surface_t *doom_surface;

void DG_Init(void)
{
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
 * rest of the printable range is the character, uppercased, which is what
 * Doom's default bindings expect for letters and the number row.
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
    case 0xFFE1: case 0xFFE2: return KEY_RSHIFT;
    case 0xFFE3: case 0xFFE4: return KEY_RCTRL;
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
 * Every key event, appended to a file, so "the key did nothing" can be told
 * apart from "the key never arrived" without a serial console. Remove once the
 * input path is trusted.
 */
static void trace_key(uint32_t ks, unsigned char k, int down)
{
    FILE *f = fopen("/tmp/doom.keys", "a");

    if (f != NULL) {
        fprintf(f, "sym %06x -> %02x %s\n", (unsigned)ks, k, down ? "down" : "up");
        fclose(f);
    }
}

int DG_GetKey(int *pressed, unsigned char *key)
{
    if (doom_gfx == NULL) {
        return 0;
    }

    espix_input_event_t ev;
    while (espix_gfx_poll_event(doom_gfx, &ev)) {
        if (ev.kind != ESPIX_INPUT_KEY) {
            continue;   /* the POC is keyboard-only */
        }
        const unsigned char k = keysym_to_doom(ev.keysym);
        trace_key(ev.keysym, k, ev.down ? 1 : 0);
        if (k == 0) {
            continue;
        }
        *pressed = ev.down ? 1 : 0;
        *key     = k;
        return 1;
    }
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