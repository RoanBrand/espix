/*
 * espix audio playback.
 *
 * The engine decodes straight into the Bluetooth A2DP ring: esp_audio_simple_dec
 * for the codec, our own open()/read() for the source, and a task that keeps the
 * ring fed. It picks the decoder from the file extension, and mono sources are
 * upmixed to stereo because the ring is stereo by contract (the A2DP callback
 * downmixes to the sink's mono SBC frame). An I2S sink will attach the same way.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Play a URI: "http://...", "https://...", or a local path ("/home/esp/x.mp3"
 * or "file:///home/esp/x.mp3"). Requires the A2DP link to be up, because that is
 * what consumes the PCM. Returns immediately; playback runs on its own task.
 */
/*
 * Stop the current playback and wait for its task to go. Used when the sink is
 * going away, so the task does not keep the decoder and its buffers alive with
 * nowhere to play.
 */
void espix_audio_stop_wait(void);

esp_err_t espix_audio_play(const char *uri);

/*
 * Same, but starts the task even with no sink connected and lets it fill the
 * ring until one arrives (`play --wait`). The default refuses, because holding
 * the decoder and its buffers for an unknown wait is worse than an error.
 */
esp_err_t espix_audio_play_wait(const char *uri);

/* Stop the current playback. */
esp_err_t espix_audio_stop(void);

/* Human-readable state, for the command. */
const char *espix_audio_state(void);

#ifdef __cplusplus
}
#endif
