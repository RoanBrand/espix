/*
 * A source: something that produces PCM. The mirror of espix_audio_sink.h.
 *
 * The ES8311's ADC (the S31 coreboard's microphone) is the first; a network
 * stream or the RF receivers are next. Each provider registers its ops once at
 * boot, and a recorder drives the registered source. As with sinks, the
 * dependency points provider -> engine, so capture does not need Bluetooth or
 * any particular codec.
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
    const char *name;                       /* "es8311", "http": the stable id */

    /* What a person should see -- "S31 coreboard microphone". */
    const char *label;

    /* Is a producer attached right now? Registering is not connecting. */
    bool (*connected)(void);

    /* The PCM this source produces. */
    espix_audio_format_t (*format)(void);

    /*
     * Read up to len bytes and return the count taken. Zero is "nothing yet"
     * rather than an error, which is how a live source paces a reader.
     */
    size_t (*read)(void *pcm, size_t len);

    /* Begin a capture: start the producer and drop anything stale. */
    void (*start)(void);

    /* End a capture: stop the producer rather than let it run on. */
    void (*stop)(void);
} espix_audio_source_ops_t;

/*
 * Publish a source. Idempotent for a name, so a lazy re-init cannot register
 * twice. The first source registered is the default that a recorder uses.
 */
esp_err_t espix_audio_source_register(const espix_audio_source_ops_t *ops);

/* The source a recorder uses, or NULL when nothing is registered. */
const espix_audio_source_ops_t *espix_audio_source_default(void);

/*
 * Choose which source a recorder uses, by its stable name, and report it back.
 * The sink side has had this since there was more than one sink; this is the
 * same thing for the same reason, and an empty name returns to "the first
 * connected one". ESP_ERR_NOT_FOUND if nothing is registered under that name.
 */
esp_err_t espix_audio_source_select(const char *name);
const char *espix_audio_source_selected(void);

/* The registered sources, for a command that has to list them. */
size_t espix_audio_source_list(const espix_audio_source_ops_t **out, size_t max);

#ifdef __cplusplus
}
#endif
