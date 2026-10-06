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
 * functions first, and those were written for a process about to exit(): run
 * from a live task they abort. So a function of ours records the request
 * instead, and the jump below leaves the loop before any of them runs.
 *
 * This is engine teardown ordering, not process exit, and it is the only jump
 * this app still needs. exit()/abort()/_Exit() used to be overridden here too,
 * because the firmware's exit() ended at IDF's _exit() == abort() and reset the
 * board -- which is why a bad WAD rebooted the machine and a timedemo printed
 * its result and then reset. espix gives them process semantics now, in
 * components/espix_proc/abi_exit.c, so I_Error's exit(-1) and the engine's
 * asserts end the app with a status and leave the board alone.
 */
static jmp_buf s_quit_jmp;

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
     * 320x240, not the 320x200 the engine renders into: Doom's pixels are not
     * square, and 240 is the height that puts them back at 4:3 on a display
     * whose pixels are. The canvas is the app's while it runs and the desktop's
     * resolution comes back when it closes, and the 320x200 surface is scaled
     * to fill this. 320x240 over the 320x200 framebuffer costs the encoder a
     * fifth more rows, and buys the aspect the game was drawn for.
     */
    doom_gfx = espix_gfx_open_mode(320, 240);
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
    /*
     * Ask, do not probe.
     *
     * The probe used to malloc 6, 5, 4 ... MiB until one succeeded: answering
     * the question by *taking* the biggest block and handing it back. Under the
     * arena that is not free -- the successful probe makes the arena carve a
     * region for it, and that region is exactly the size of the probe, so the
     * engine's own zone then neither fits in it nor can see the block it split.
     * espix's heap_caps_get_largest_free_block() answers for this process now
     * (its arenas plus the global pool), so the question can be asked without
     * disturbing the answer.
     */
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    int          avail   = (int)(largest / (1024 * 1024));

    if (avail > 6) {
        avail = 6;      /* the engine's own default; more is not useful here */
    }
    printf("doom: psram free %u KB, largest block %d MiB\n",
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024), avail);

    /*
     * Refuse rather than die. I_Error is survivable now that exit() ends the app, but
     * a zone too small to load a level with would fail *inside* the engine,
     * after the WAD has been read and half the subsystems initialised -- so the
     * check happens here, before any of that. Below 4 MiB the shareware's levels
     * do not fit anyway; between 4 and 6, ask the engine for a zone it can have.
     */
    /*
     * The floor is a judgement, not a fact -- 4 MiB was our port's reading of
     * what a shareware level needs -- so it can be overridden per run:
     * DOOM_MIN_MB=3 doom. The engine's zone is whole MiB (-mb is atoi'd as MiB),
     * so a fractional value here does not reach it.
     */
    const char *min_env = getenv("DOOM_MIN_MB");
    const int   min_mb  = (min_env != NULL && atoi(min_env) > 0) ? atoi(min_env) : 4;

    if (avail < min_mb) {
        printf("doom: not enough PSRAM (need %d MiB, have %d) -- not starting\n",
               min_mb, avail);
        fflush(stdout);
        return;
    }

    char  mb_arg[8];

    /*
     * No sound and no music: nothing on this platform has an audio backend yet.
     * Without these, Doom initialises a Sound Blaster it will never have --
     * i_sound.c's snd_musicdevice defaults to SNDDEVICE_SB -- and S_ChangeMusic
     * then asks the WAD for the OPL intro lump, d_introa, which the shareware
     * file does not contain. The result is "W_GetNumForName: d_introa not
     * found!" and a clean exit through I_Error, which is the last thing that
     * stopped Doom on the S3 and would have stopped the S31 too on a build that
     * enabled the engine's sound module.
     *
     * Declared here rather than by patching the fetched engine, because it is a
     * statement about espix -- and it is two words to remove when the engine has
     * somewhere to send samples (the plan for the S31 is A2DP, and the engine's
     * backends are pluggable).
     */
    char *argv[16] = { "doom", "-iwad", (char *)wad,
                       "-nosound", "-nomusic", NULL, NULL, NULL };
    int   argc    = 5;

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
