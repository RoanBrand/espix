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
#include "espix_audio_sink.h"

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
    const char *sink = NULL;

    while (argi < argc && strncmp(argv[argi], "--", 2) == 0) {
        if (strcmp(argv[argi], "--wait") == 0) {
            wait = true;
            argi += 1;
        } else if (strcmp(argv[argi], "--sink") == 0 && argi + 1 < argc) {
            sink = argv[argi + 1];
            argi += 2;
        } else {
            break;
        }
    }

    /*
     * Which sink, for this playback. It sets the system's chosen sink rather
     * than attaching the request to the stream, because one producer at a time
     * is what this can express today: per-stream sinks need the mixer to stop
     * assuming a single output. The distinction is real and this is the honest
     * half of it -- the other half is a mixer that groups streams by sink.
     */
    if (sink != NULL && espix_audio_sink_select(sink) != ESP_OK) {
        espix_eprintf(s, "play: no sink called '%s'\n", sink);
        return 1;
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

/*
 * A pipe: a source connected to a sink and left connected -- microphone to
 * headset, radio to speaker.
 *
 * "add" takes the source and, optionally, the sink. The sink has to be the one
 * the engine would use, because the mixer still has a single output: naming a
 * different one is refused with that reason rather than quietly sending the
 * microphone to the wrong place.
 */
static int cmd_pipe(espix_session_t *s, int argc, char **argv)
{
    if (argc == 1 || strcmp(argv[1], "list") == 0) {
        espix_audio_pipe_info_t p[ESPIX_AUDIO_PIPES_MAX];
        const size_t n = espix_audio_pipe_list(p, ESPIX_AUDIO_PIPES_MAX);
        if (n == 0) {
            espix_printf(s, "no pipes\n");
            return 0;
        }
        for (size_t i = 0; i < n; i++) {
            espix_printf(s, "%d  %s -> %s  %s\n", p[i].id, p[i].source, p[i].sink,
                         p[i].running ? "running" : "stopping");
        }
        return 0;
    }
    if (strcmp(argv[1], "add") == 0) {
        const char *const src = argc > 2 ? argv[2] : NULL;
        const char *const snk = argc > 3 ? argv[3] : NULL;
        const int id = espix_audio_pipe_add(src, snk);
        if (id < 0) {
            espix_eprintf(s, "pipe: could not open it; see dmesg for why\n");
            return 1;
        }
        espix_printf(s, "pipe %d: %s -> %s\n", id,
                     src != NULL ? src : "default", snk != NULL ? snk : "default");
        return 0;
    }
    if (strcmp(argv[1], "del") == 0 && argc > 2) {
        if (espix_audio_pipe_del(atoi(argv[2])) != ESP_OK) {
            espix_eprintf(s, "pipe: no pipe %s\n", argv[2]);
            return 1;
        }
        return 0;
    }
    espix_eprintf(s, "usage: pipe [list | add <source> [sink] | del <id>]\n");
    return 1;
}

/*
 * Which sink the engine would use, and why.
 *
 * "Preferred while connected, otherwise the first connected one" is a rule you
 * cannot see from the shell, and not seeing it is how a test tone can go to the
 * onboard codec while the speaker across the room hears nothing and gets
 * blamed.
 */
static int cmd_sink(espix_session_t *s, int argc, char **argv)
{
    const espix_audio_sink_ops_t *list[8];
    const size_t n = espix_audio_sink_list(list, sizeof(list) / sizeof(list[0]));
    const char  *sel = espix_audio_sink_selected();
    const espix_audio_sink_ops_t *def = espix_audio_sink_default();

    if (argc >= 2) {
        const esp_err_t err = espix_audio_sink_select(argv[1]);
        if (err != ESP_OK) {
            espix_eprintf(s, "sink: %s: %s\n", argv[1], esp_err_to_name(err));
            return 1;
        }
        espix_printf(s, "sink: preferred is now '%s' (used while it is connected)\n",
                     argv[1]);
        return 0;
    }

    for (size_t i = 0; i < n; i++) {
        const espix_audio_sink_ops_t *o = list[i];
        const bool is_def = (o == def);
        const bool is_sel = (sel != NULL && strcmp(sel, o->name) == 0);
        espix_printf(s, "  %c %-8s %-26s %s\n",
                     is_def ? '*' : is_sel ? '+' : ' ',
                     o->name, o->label ? o->label : "-",
                     o->connected() ? "connected" : "not connected");
    }
    espix_printf(s, "preferred %s, would use %s   (* = would use, + = preferred)\n",
                 sel ? sel : "(none)", def ? def->name : "(none)");
    return 0;
}

static espix_cmd_t s_play_cmds[] = {
    { .name = "sink", .fn = cmd_sink,
      .help = "list the audio sinks, or make one the preferred output",
      .usage = "sink [name]" },
    { .name = "play", .fn = cmd_play,
      .help = "play an audio file or stream to the connected A2DP sink",
      .usage = "play [--wait] [--sink <name>] <file|url> | play {stop|status}" },
    { .name = "volume", .fn = cmd_volume,
      .help = "get or set the master output volume (0-100)",
      .usage = "volume [0-100]" },
    { .name = "pipe", .fn = cmd_pipe,
      .help = "connect a source to a sink and leave it connected",
      .usage = "pipe [list | add <source> [sink] | del <id>]" },
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
