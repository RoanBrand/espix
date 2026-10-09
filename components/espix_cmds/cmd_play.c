/*
 * play: a file or a stream, through the GMF-based player, out the A2DP sink.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "sdkconfig.h"

#include "espix_cmds_priv.h"
#include "espix_shell.h"
#include "espix_audio.h"

#if CONFIG_ESPIX_AUDIO

static int cmd_play(espix_session_t *s, int argc, char **argv)
{
    /*
     * --wait keeps the old queue-and-wait: start anyway and fill the ring until
     * a sink arrives. Without it a missing sink is an error, as it is for any
     * other player.
     */
    bool wait = false;
    int  argi = 1;

    if (argc > 1 && strcmp(argv[1], "--wait") == 0) {
        wait = true;
        argi = 2;
    }

    if (argc <= argi) {
        espix_eprintf(s, "usage: play [--wait] <file|url> | play {stop|status}\n");
        return 1;
    }
    if (strcmp(argv[argi], "stop") == 0) {
        if (espix_audio_stop() != ESP_OK) {
            espix_eprintf(s, "play: nothing to stop\n");
            return 1;
        }
        return 0;
    }
    if (strcmp(argv[argi], "status") == 0) {
        espix_printf(s, "play: %s\n", espix_audio_state());
        return 0;
    }

    /*
     * A URL passes through untouched; a path is resolved against the shell's
     * cwd first. GMF's IO scoring accepts a leading "/" or a scheme, so a bare
     * "test.mp3" is neither and comes back as "invalid URI". Resolving here also
     * lets a missing file fail before any player state is built.
     */
    char path[320];
    const char *uri = argv[argi];

    if (strstr(argv[argi], "://") == NULL) {
        if (!espix_cmd_path(s, argv[argi], path, sizeof(path))) {
            return 1;
        }
        struct stat st;
        if (stat(path, &st) != 0) {
            espix_eprintf(s, "play: %s: no such file\n", argv[argi]);
            return 1;
        }
        uri = path;
    }

    const esp_err_t err = wait ? espix_audio_play_wait(uri) : espix_audio_play(uri);
    if (err == ESP_ERR_INVALID_STATE) {
        espix_eprintf(s, "play: no audio sink available "
                        "(connect one, or use --wait)\n");
        return 1;
    }
    if (err != ESP_OK) {
        espix_eprintf(s, "play: cannot start '%s': %s\n", argv[argi],
                      esp_err_to_name(err));
        return 1;
    }
    espix_printf(s, "play: %s\n", uri);
    return 0;
}

static int cmd_volume(espix_session_t *s, int argc, char **argv)
{
    if (argc > 1) {
        const int pct = atoi(argv[1]);
        if (pct < 0 || pct > 100) {
            espix_eprintf(s, "volume: 0-100\n");
            return 1;
        }
        espix_audio_set_volume(pct);
    }
    espix_printf(s, "volume: %d%%\n", espix_audio_get_volume());
    return 0;
}

/*
 * A generated tone through the app stream API -- the shortest end-to-end test
 * of it, and useful for setting a level or finding a speaker. The "lr" form
 * plays one second on the left, one on the right and one on both, which is how
 * stereo is actually confirmed rather than assumed.
 */
static int cmd_tone(espix_session_t *s, int argc, char **argv)
{
    const int  freq = argc > 1 ? atoi(argv[1]) : 440;
    const int  secs = argc > 2 ? atoi(argv[2]) : 3;
    const bool lr   = argc > 3 && strcmp(argv[3], "lr") == 0;

    if (freq <= 0 || freq > 20000 || secs <= 0 || secs > 60) {
        espix_eprintf(s, "usage: tone [freq 20-20000] [seconds 1-60] [lr]\n");
        return 1;
    }

    const esp_err_t err = espix_audio_tone(freq, secs, lr);
    if (err != ESP_OK) {
        espix_eprintf(s, "tone: %s\n", esp_err_to_name(err));
        return 1;
    }
    espix_printf(s, "tone: %d Hz, %s\n", freq,
                 lr ? "left, right, both" : "both channels");
    return 0;
}

static espix_cmd_t s_play_cmds[] = {
    { .name = "play", .fn = cmd_play,
      .help = "play an audio file or stream to the connected A2DP sink",
      .usage = "play [--wait] <file|url> | play {stop|status}" },
    { .name = "volume", .fn = cmd_volume,
      .help = "get or set the master output volume (0-100)",
      .usage = "volume [0-100]" },
    { .name = "tone", .fn = cmd_tone,
      .help = "play a generated tone through the app stream API",
      .usage = "tone [freq] [seconds] [lr]   (lr: left, right, both)" },
};

#endif /* CONFIG_ESPIX_AUDIO */

void espix_cmds_register_play(void)
{
#if CONFIG_ESPIX_AUDIO
    espix_cmds_register_table(s_play_cmds,
                              sizeof(s_play_cmds) / sizeof(s_play_cmds[0]));
#endif
}
