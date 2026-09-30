/*
 * Doom on espix -- proof of concept.
 *
 * doomgeneric renders 320x200 8-bit paletted; doom_espix.c converts a frame
 * to RGB565 into an espix_gfx surface and lets the PPA scale it to the canvas,
 * and feeds the engine keys and milliseconds. This file is the entry point and
 * the one policy it needs: where the WAD comes from.
 */

#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "esp_heap_caps.h"
#include "espix_gfx.h"

#include "doomgeneric.h"
#include "i_system.h"

espix_gfx_t         *doom_gfx;
espix_gfx_surface_t *doom_surface;

/*
 * Doom's I_Quit() tears the game down and then, because doomgeneric builds with
 * ORIGCODE undefined (config.h), returns without exiting -- so "quit" left the
 * engine running and the screen unchanged. I_Quit runs its registered exit
 * functions first, so one of ours records the request instead, and the loop
 * below leaves through espix's normal exit path: the canvas and the screen go
 * back the way every other app gives them back, rather than the app calling
 * exit() behind the process machinery.
 */
static jmp_buf s_quit_jmp;

static void doom_request_quit(void)
{
    longjmp(s_quit_jmp, 1);
}

/*
 * Where the WAD is looked for, in order. The stick first: 4 MB of shareware
 * data does not belong in the kernel image, and a flash drive is its natural
 * home. The rootfs is the fallback for a board without one.
 */
static const char *const wad_paths[] = {
    /* Installed with the image: trying espix should not require a flash
     * drive. /bin is for executables, so the data lives under /var/lib. */
    "/var/lib/doom/doom1.wad",
    /* A stick if there is one -- how you bring a different WAD along without
     * reflashing. */
    "/mnt/sda1/doom1.wad",
    "/mnt/sda1/baseq2/doom1.wad",
};

static const char *find_wad(void)
{
    for (size_t i = 0; i < sizeof(wad_paths) / sizeof(wad_paths[0]); i++) {
        FILE *f = fopen(wad_paths[i], "rb");
        if (f != NULL) {
            fclose(f);
            return wad_paths[i];
        }
    }
    return NULL;
}

void app_main(void)
{
    doom_gfx = espix_gfx_open();
    if (doom_gfx == NULL) {
        printf("doom: no display up (start it with 'vnc start')\n");
        return;
    }

    doom_surface = espix_gfx_surface_new(320, 200);
    if (doom_surface == NULL) {
        printf("doom: no surface\n");
        espix_gfx_close(doom_gfx);
        return;
    }

    const char *wad = find_wad();
    if (wad == NULL) {
        printf("doom: no doom1.wad -- put the shareware WAD on the stick\n");
        return;
    }

    printf("doom: starting with %s\n", wad);

    /*
     * The engine asks for a single 6 MiB zone block (i_system.c DEFAULT_RAM),
     * and that request sits right at the edge of the largest block PSRAM has
     * left once the app is loaded -- so it succeeds some boots and aborts
     * others. Report what is actually available before asking.
     */
    int avail = 0;
    for (int mb = 6; mb >= 1; mb--) {
        void *probe = malloc((size_t)mb * 1024 * 1024);
        if (probe != NULL) {
            free(probe);
            avail = mb;
            break;
        }
    }
    printf("doom: psram free %u KB, largest block %d MiB\n",
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024), avail);

    /*
     * Refuse rather than die. The engine's I_Error calls abort(), and on espix
     * an abort in an app takes the whole board down -- so a launch that cannot
     * get its zone reboots the system instead of failing. Below 4 MiB the
     * shareware's levels do not fit anyway, so say so and leave through the
     * normal exit; between 4 and 6, ask the engine for a zone it can have.
     */
    if (avail < 4) {
        printf("doom: not enough PSRAM (need 4 MiB, have %d) -- not starting\n",
               avail);
        fflush(stdout);
        return;
    }

    char  mb_arg[8];
    char *argv[6] = { "doom", "-iwad", (char *)wad, NULL, NULL, NULL };
    int   argc    = 3;

    if (avail < 6) {
        snprintf(mb_arg, sizeof(mb_arg), "%d", avail);
        argv[argc++] = "-mb";
        argv[argc++] = mb_arg;
        printf("doom: only %d MiB free; starting with a %d MiB zone\n", avail, avail);
    }
    fflush(stdout);

    doomgeneric_Create(argc, argv);

    /*
     * Registered AFTER Create, because I_AtExit prepends: this way ours runs
     * before the engine's own shutdown functions, and longjmps past them. Those
     * were written for a process that is about to exit() and they abort when
     * run from a live task -- which is what made "quit" a crash. Nothing after
     * the longjmp is reached.
     */
    I_AtExit(doom_request_quit, false);

    if (setjmp(s_quit_jmp) == 0) {
        for (;;) {
            doomgeneric_Tick();
        }
    }

    /*
     * No espix_gfx_close() here. The engine's I_Quit() teardown and espix's own
     * process teardown both release the screen; a third release from here ran
     * on a canvas that was already gone and aborted.
     */
    printf("doom: quit\n");
    fflush(stdout);
}
