/*
 * espix audio playback.
 *
 * This is deliberately thin: the engine is Espressif's GMF-based simple player
 * (esp_audio_simple_player), which knows the URI schemes, picks the decoder from
 * the file extension and converts bit depth / channels / rate. espix supplies the
 * one thing the player cannot know: where the PCM goes. Here that is the
 * Bluetooth A2DP source, and it is where an I2S sink will later attach too.
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
esp_err_t espix_audio_play(const char *uri);

/* Stop the current playback. */
esp_err_t espix_audio_stop(void);

/* Human-readable state, for the command. */
const char *espix_audio_state(void);

#ifdef __cplusplus
}
#endif
