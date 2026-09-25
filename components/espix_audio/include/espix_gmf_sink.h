/*
 * A GMF output IO for the A2DP source.
 *
 * GMF's examples end a pipeline in io_codec_dev, which is I2S. espix's sink is
 * the Bluetooth A2DP source, so it needs its own writer: every PCM payload the
 * pipeline produces is handed to espix_bt_audio_write, and the ring's
 * backpressure is what paces playback.
 */
#pragma once

#include "esp_gmf_io.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Creates the writer and tags it (the pipeline refers to it by that tag). */
esp_gmf_err_t espix_gmf_sink_init(const char *tag, esp_gmf_io_handle_t *io);

#ifdef __cplusplus
}
#endif
