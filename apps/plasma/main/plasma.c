/*
 * espix example app: a full-screen plasma.
 *
 * The point is the ABI, not the picture. This claims the screen, gets a
 * framebuffer, draws into it, presents, reads input and releases -- which is
 * everything a game needs and nothing more.
 *
 * The render target is deliberately smaller than the screen, so the PPA
 * upscale runs on every frame, and it is integer-only so the app links against
 * nothing but the ABI it is testing.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "espix_gfx.h"

/* A renderer's internal resolution and its display size are different things,
 * and the whole point of the surface API is that they can be. */
#define RW 192
#define RH 144

#define KEY_ESC 0xFF1B

static uint16_t rgb565(int r, int g, int b)
{
    if (r < 0) { r = 0; }
    if (r > 255) { r = 255; }
    if (g < 0) { g = 0; }
    if (g > 255) { g = 255; }
    if (b < 0) { b = 0; }
    if (b > 255) { b = 255; }

    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* A triangle wave in 0..255, so the pattern needs no libm -- an app links
 * against the firmware's tables and there is no -lm in its link line. */
static int wave(int a)
{
    a &= 511;
    if (a >= 256) {
        a = 511 - a;
    }
    return a;
}

int app_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    espix_gfx_t *g = espix_gfx_open();
    if (g == NULL) {
        printf("plasma: no display; 'vnc start' or 'display start' first\n");
        return 1;
    }

    espix_gfx_surface_t *s = espix_gfx_surface_new(RW, RH);
    if (s == NULL) {
        printf("plasma: no memory for a %dx%d surface\n", RW, RH);
        espix_gfx_close(g);
        return 1;
    }

    printf("plasma: %dx%d scaled to the screen; Esc or q quits\n", RW, RH);

    int  t   = 0;
    bool run = true;

    while (run) {
        espix_input_event_t ev;

        while (espix_gfx_poll_event(g, &ev)) {
            if (ev.kind == ESPIX_INPUT_KEY && ev.down &&
                (ev.keysym == KEY_ESC || ev.keysym == 'q')) {
                run = false;
            }
        }

        espix_gfx_fb_t fb = espix_gfx_surface_lock(s);

        for (int y = 0; y < fb.h; y++) {
            uint16_t *row = fb.pixels + (size_t)y * fb.stride;

            for (int x = 0; x < fb.w; x++) {
                const int r  = wave(x * 3 + t * 2) + wave(y * 2 - t);
                const int gg = wave(x * 2 + y * 2 + t * 3);
                const int b  = wave(y * 3 - t * 2) + wave(x - t);

                row[x] = rgb565(r / 2, gg, b / 2);
            }
        }

        espix_gfx_surface_unlock(s);

        espix_gfx_present_surface(g, s);
        t++;
    }

    espix_gfx_surface_free(s);
    espix_gfx_close(g);
    printf("plasma: done\n");
    return 0;
}
