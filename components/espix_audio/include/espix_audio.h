/*
 * espix audio playback.
 *
 * The engine decodes into whichever sink is registered (espix_audio_sink.h):
 * esp_audio_simple_dec for the codec, our own open()/read() for the source, and
 * a task that keeps the sink fed. It picks the decoder from the file extension,
 * and converts a source to the sink's format -- stereo sinks get a mono source
 * duplicated, because a half-width stream drains a stereo sink at twice the
 * rate. The A2DP source is one sink; an I2S codec attaches the same way.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The PCM a stream or a sink works in.
 *
 * 16-bit signed is the only sample width any current sink takes, and the field
 * is here so a format travels as one value rather than three arguments. It is
 * public because an app's stream and a sink's format are the same description.
 */
typedef struct {
    uint32_t rate;          /* Hz */
    uint8_t  channels;
    uint8_t  bits;          /* 16 today */
} espix_audio_format_t;

/*
 * Play a URI: "http://...", "https://...", or a local path ("/home/esp/x.mp3"
 * or "file:///home/esp/x.mp3"). Requires the A2DP link to be up, because that is
 * what consumes the PCM. Returns immediately; playback runs on its own task.
 */
/*
 * Stop the current playback and wait for its task to go. Used when the sink is
 * going away, so the task does not keep the decoder and its buffers alive with
 * nowhere to play.
 *
 * timeout_ms is how long to give the task to notice; zero waits without a limit,
 * as everywhere else here. False when it was still running when the time ran
 * out, in which case it has still been asked and is on its way.
 */
bool espix_audio_stop_wait(uint32_t timeout_ms);

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

/*
 * Register the benchmark sink (CONFIG_ESPIX_AUDIO_NULL_SINK). It accepts PCM as
 * fast as it arrives and discards it, so a "play" measures the source and the
 * decoder alone. Built only for that measurement; see the Kconfig.
 */
esp_err_t espix_audio_null_sink_register(void);

#ifdef __cplusplus
}
#endif
