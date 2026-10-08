/*
 * The I2S codec sink (the S31 coreboard's ES8311). espix_i2s.c provides it;
 * main registers it at boot, the way espix_bt registers the A2DP sink.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Publish the "es8311" sink. Idempotent. Returns ESP_OK, or an error if the
 * sink registry is full. The codec itself is opened on the first stream. */
esp_err_t espix_i2s_sink_register(void);

#ifdef __cplusplus
}
#endif
