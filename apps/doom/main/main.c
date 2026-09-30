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
static jmp_buf s_fatal_jmp;

static int s_fatal_status;      /* what exit() was asked for */

/*
 * exit() and abort(), the way a program expects them: end this program.
 *
 * espix's are the board's -- the ABI publishes the firmware's, and from an app
 * task they panic and reboot the machine -- which is why a bad WAD rebooted it,
 * and why a timedemo printed its own result and then reset the board. I_Error
 * ends in exit(-1), and every assert ends in abort(); both arrive here.
 *
 * espix cannot give them process semantics yet -- a dead app's heap and fds are
 * not reclaimed, which is the reaper's hard half -- but an app can, and by the
 * same door the quit path already uses: a jump back to app_main, whose return is
 * espix's normal exit. The app's own definition wins over the ABI's, so the
 * engine's calls land here. What was printed before is the reason.
 */
void exit(int status)
{
    fflush(stdout);
    fflush(stderr);
    s_fatal_status = status;
    longjmp(s_fatal_jmp, 1);
}

void abort(void)
{
    exit(-1);
}

void _Exit(int status)
{
    exit(status);
}

static void doom_request_quit(void)
{
    longjmp(s_quit_jmp, 1);
}

/*
 * Where the WAD is looked for, in order.
 *
 * The rootfs first, because that is where the launcher puts what it fetched from
 * the manifest in /etc/apps/doom.conf -- 4 MB of shareware does not belong in
 * the kernel image, and trying espix should not require a flash drive. A stick
 * second: how you bring a different WAD along without reflashing, and the only
 * source on a board with no network.
 */
static const char *const wad_paths[] = {
    /* /bin is for executables, so the data lives under /var/lib. */
    "/var/lib/doom/doom1.wad",
    "/mnt/sda1/doom1.wad",
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

/*
 * The working directory, the WAD and the zone are decided here, and everything
 * the launcher was given after argv[0] is appended to the engine's command line.
 * That is how a repeatable benchmark is run -- "doom -timedemo demo1" -- since
 * the WAD is named here and the engine parses its arguments in one pass
 * wherever they sit.
 */
void app_main(int app_argc, char **app_argv)
{
    /*
     * Armed before anything else the engine can fail in, and for the whole run:
     * a longjmp target is only good while its frame is alive, and app_main does
     * not return until the end.
     */
    if (setjmp(s_fatal_jmp) != 0) {
        printf("doom: %s; leaving the app, not the board\n",
               s_fatal_status == 0 ? "done" : "fatal error");
        fflush(stdout);
        return;
    }

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
        printf("doom: no doom1.wad in /var/lib/doom or on the stick\n");
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
     * Refuse rather than die. I_Error is survivable now (see abort() above), but
     * a zone too small to load a level with would fail *inside* the engine,
     * after the WAD has been read and half the subsystems initialised -- so the
     * check happens here, before any of that. Below 4 MiB the shareware's levels
     * do not fit anyway; between 4 and 6, ask the engine for a zone it can have.
     */
    if (avail < 4) {
        printf("doom: not enough PSRAM (need 4 MiB, have %d) -- not starting\n",
               avail);
        fflush(stdout);
        return;
    }

    char  mb_arg[8];
    char *argv[16] = { "doom", "-iwad", (char *)wad, NULL, NULL, NULL };
    int   argc    = 3;

    if (avail < 6) {
        snprintf(mb_arg, sizeof(mb_arg), "%d", avail);
        argv[argc++] = "-mb";
        argv[argc++] = mb_arg;
        printf("doom: only %d MiB free; starting with a %d MiB zone\n", avail, avail);
    }

    /* The launcher's own arguments, argv[0] aside, leaving room for the NULL. */
    for (int i = 1; i < app_argc && argc + 1 < (int)(sizeof(argv) / sizeof(argv[0]));
         i++) {
        argv[argc++] = app_argv[i];
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
