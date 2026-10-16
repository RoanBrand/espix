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
 * The master output volume, 0-100, applied as a PCM gain by the engine so that
 * it is the same whatever the sink is. 100 is a pass-through. This is the
 * global level; a sink's own hardware volume is separate and on top.
 */
esp_err_t espix_audio_set_volume(int percent);
int       espix_audio_get_volume(void);

/*
 * The default sink's own volume, 0-100, as against the master PCM gain above.
 * This is the device's control -- the ES8311's register, an A2DP link's
 * absolute volume -- so it is the one that costs no CPU and works when the
 * master is a pass-through. ESP_ERR_NOT_SUPPORTED when the sink has none, and
 * -1 from the getter.
 */
esp_err_t espix_audio_sink_volume(int percent);
int       espix_audio_sink_volume_get(void);

/*
 * A generated tone through the stream API: a test signal, and the shortest
 * end-to-end exercise of the stream. With left_right, one second goes to the
 * left channel, one to the right and one to both -- which is how a listener
 * confirms stereo rather than guessing from a level. `seconds` is otherwise
 * the length. The _async form returns at once and plays on its own task, for a
 * UI whose input handler cannot block.
 */
esp_err_t espix_audio_tone(int freq_hz, int seconds, bool left_right);
esp_err_t espix_audio_tone_async(int freq_hz, int seconds, bool left_right);

/*
 * A PCM stream the app writes itself, for audio it generates rather than
 * decodes -- a game's sound effects, a synthesiser. The app blocks in
 * stream_write only while the engine's ring is full; the engine drains it to
 * the sink on its own PSRAM task, so neither side waits on the other. One
 * stream at a time for now, and mutually exclusive with play().
 */
typedef struct espix_audio_stream espix_audio_stream_t;

/* NULL when there is no connected sink, or every slot is taken. Several streams
 * may be open at once and are mixed together; the decoder (play) is still
 * exclusive with them, because it drives the sink itself. */
espix_audio_stream_t *espix_audio_stream_open(const espix_audio_format_t *fmt);

/* Blocks while the ring is full. Returns the bytes taken; a short write is the
 * caller's to loop on. */
size_t espix_audio_stream_write(espix_audio_stream_t *st, const void *pcm, size_t len);

/*
 * This stream's own level, 0-100, and its mute.
 *
 * Separate from the engine's master volume so an app's mix cannot change the
 * system volume, and the two multiply. Mute is a bit rather than volume 0, so
 * unmuting restores what the app had asked for instead of guessing at it.
 */
esp_err_t espix_audio_stream_volume(espix_audio_stream_t *st, int percent);
esp_err_t espix_audio_stream_mute(espix_audio_stream_t *st, bool on);

/* Let the mixer finish what is queued, then free the ring. The sink is drained
 * and suspended only when the last stream closes. Blocks while it waits. */
esp_err_t espix_audio_stream_close(espix_audio_stream_t *st);

/* Audio written but not yet taken by the sink, in microseconds: what a player
 * subtracts from what it wrote to know what has been heard. */
uint32_t espix_audio_stream_queued_us(espix_audio_stream_t *st);

/* ------------------------------------------------------------- capture -- */

/*
 * Reading a source, the mirror of a stream: open, read, close.
 *
 * No task and no ring behind it, deliberately. A capture client reads on its own
 * thread and the source paces it -- read() returns zero when the producer has
 * nothing yet -- which is what ALSA's capture read() does and what keeps this
 * the size it is. One client at a time: with no fan-out there is no way to give
 * two readers the same samples, so the second is refused rather than quietly
 * handed half of them.
 */
typedef struct espix_audio_capture espix_audio_capture_t;

/*
 * Open the default source. want may be NULL for whatever the source produces; a
 * different rate or channel count is converted with the ASRC when the pair is
 * one it supports. *got, when not NULL, says what read() will actually return.
 */
espix_audio_capture_t *espix_audio_capture_open(const espix_audio_format_t *want,
                                                espix_audio_format_t *got);

/* Up to len bytes. Zero means nothing ready yet, not end of stream. */
size_t espix_audio_capture_read(espix_audio_capture_t *cap, void *pcm, size_t len);

esp_err_t espix_audio_capture_close(espix_audio_capture_t *cap);

/* ----------------------------------------------------------------- pipes -- */

/*
 * A pipe is a source connected to a sink and left connected: microphone to
 * headset, radio to speaker. Internally it is nothing new -- the task behind it
 * is a capture client and a stream, which is what a recorder and a player are --
 * but it is a thing the system can be asked to hold open, which neither of those
 * is.
 */
#define ESPIX_AUDIO_PIPES_MAX 2

typedef struct {
    int         id;
    const char *source;
    const char *sink;
    bool        running;
} espix_audio_pipe_info_t;

/*
 * Open a pipe from a named source to a named sink, either of which may be NULL
 * for the current default. Returns its id, or -1 with the reason logged. The
 * sink has to be the one the engine would use: the mixer still has a single
 * output, so a pipe to a different one is refused rather than silently sent to
 * the wrong place.
 */
int espix_audio_pipe_add(const char *source, const char *sink);

/* Close a pipe by id. Blocks until its task has stopped. */
esp_err_t espix_audio_pipe_del(int id);

size_t espix_audio_pipe_list(espix_audio_pipe_info_t *out, size_t max);

/*
 * Register the benchmark sink (CONFIG_ESPIX_AUDIO_NULL_SINK). It accepts PCM as
 * fast as it arrives and discards it, so a "play" measures the source and the
 * decoder alone. Built only for that measurement; see the Kconfig.
 */
esp_err_t espix_audio_null_sink_register(void);

/*
 * Close every stream a dying task still holds. Called by the process reaper
 * with the task handle, not by apps: a killed player must not keep its ring,
 * its mixer slot or the sink alive behind it. See espix_gfx_recover for the
 * same idea applied to the canvas lock.
 */
void espix_audio_streams_recover(void *task);

#ifdef __cplusplus
}
#endif
