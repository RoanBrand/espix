/*
 * A sink: something that consumes PCM. The audio engine knows only this.
 *
 * The A2DP source is a sink today; an I2S codec (the S31 coreboard's ES8311)
 * and the RF transmitters are the next. Each provider -- espix_bt, an I2S
 * component -- registers its ops once at boot, and espix_audio.c drives the
 * registered sink. The dependency points provider -> engine, never the
 * reverse, so audio builds and links with no Bluetooth at all.
 *
 * This header is for the firmware's own components. Apps do not include it;
 * they see the stream API in espix_audio.h.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "espix_audio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;                       /* "a2dp", "i2s0", "fm" */

    /* Is a consumer attached right now? Registering does not mean connected:
     * the A2DP sink exists from boot but reports false until a link is up. */
    bool (*connected)(void);

    /* The PCM this sink consumes. The engine converts to it. */
    espix_audio_format_t (*format)(void);

    /*
     * Accept up to len bytes and return the count taken. A partial write is the
     * normal backpressure, not an error: the caller advances by the count. (An
     * earlier version retried the whole chunk, re-sending bytes the ring had
     * already consumed, which was the harsh noise in the first A2DP engine.)
     */
    size_t (*write)(const void *pcm, size_t len);

    /* Begin a new stream: drop stale PCM and re-arm the pre-roll. */
    void (*start)(void);

    /* End a stream: stop the consumer rather than let it run on silence. */
    void (*suspend)(void);

    /*
     * The sink's own volume, 0-100, or NULL when it has none. This is the
     * device's control -- the ES8311's register, an A2DP link's absolute
     * volume -- and is separate from the engine's master PCM gain: this one
     * costs nothing when it is the device doing the attenuation.
     */
    esp_err_t (*set_volume)(int percent);
    int       (*get_volume)(void);
} espix_audio_sink_ops_t;

/*
 * Publish a sink. Idempotent for a name, so a lazy re-init cannot register
 * twice. The first sink registered is the default that `play` uses.
 */
esp_err_t espix_audio_sink_register(const espix_audio_sink_ops_t *ops);

/* The sink `play` uses, or NULL when nothing is registered (S3 with no codec). */
const espix_audio_sink_ops_t *espix_audio_sink_default(void);

/* The registered sinks, for a UI to list. Returns how many were written. */
size_t espix_audio_sink_list(const espix_audio_sink_ops_t **out, size_t max);

/*
 * Prefer a sink by name, or clear the preference with NULL (or ""). The
 * preference is honoured while that sink is registered; without one the engine
 * keeps picking the first connected sink on its own.
 */
esp_err_t espix_audio_sink_select(const char *name);

/* The preferred sink's name, or "" when there is none. */
const char *espix_audio_sink_selected(void);

#ifdef __cplusplus
}
#endif
