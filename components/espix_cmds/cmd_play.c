/*
 * play: a file or a stream, through the GMF-based player, out the A2DP sink.
 */
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#include "espix_cmds_priv.h"
#include "espix_shell.h"
#include "espix_audio.h"

#if CONFIG_ESPIX_AUDIO

static int cmd_play(espix_session_t *s, int argc, char **argv)
{
    if (argc < 2) {
        espix_eprintf(s, "usage: play <file|url> | play {stop|status}\n");
        return 1;
    }
    if (strcmp(argv[1], "stop") == 0) {
        if (espix_audio_stop() != ESP_OK) {
            espix_eprintf(s, "play: nothing to stop\n");
            return 1;
        }
        return 0;
    }
    if (strcmp(argv[1], "status") == 0) {
        espix_printf(s, "play: %s\n", espix_audio_state());
        return 0;
    }

    const esp_err_t err = espix_audio_play(argv[1]);
    if (err == ESP_ERR_INVALID_STATE) {
        espix_eprintf(s, "play: no A2DP sink connected; pair/connect first\n");
        return 1;
    }
    if (err != ESP_OK) {
        espix_eprintf(s, "play: cannot start '%s'\n", argv[1]);
        return 1;
    }
    espix_printf(s, "play: %s\n", argv[1]);
    return 0;
}

static espix_cmd_t s_play_cmds[] = {
    { .name = "play", .fn = cmd_play,
      .help = "play an audio file or stream to the connected A2DP sink",
      .usage = "play <file|url> | play {stop|status}" },
};

#endif /* CONFIG_ESPIX_AUDIO */

void espix_cmds_register_play(void)
{
#if CONFIG_ESPIX_AUDIO
    espix_cmds_register_table(s_play_cmds,
                              sizeof(s_play_cmds) / sizeof(s_play_cmds[0]));
#endif
}
