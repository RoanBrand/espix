/*
 * Bluetooth, espix-shaped.
 *
 * A native layer over the ESP-IDF host stack -- there is no BlueZ and no D-Bus
 * here -- with the names `bluetoothctl` uses on Linux. Phase 1 is the S31
 * (Classic + BLE); the S3's BLE-only NimBLE path comes later.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESPIX_BDA_LEN     6
#define ESPIX_BT_NAME_MAX 64
#define ESPIX_BT_PIN_MAX  16

typedef struct {
    uint8_t bda[ESPIX_BDA_LEN];
    char    name[ESPIX_BT_NAME_MAX];
    bool    bonded;
    bool    connected;      /* A2DP connected; phase 1 */
} espix_bt_dev_t;

/* Bring the host up. Idempotent; returns ESP_ERR_NOT_SUPPORTED without ESPIX_BT. */
esp_err_t espix_bt_init(void);
bool      espix_bt_ready(void);
bool      espix_bt_scanning(void);

/* Classic inquiry for now; BLE scanning arrives with the NimBLE path. */
esp_err_t espix_bt_scan(bool on);

size_t    espix_bt_devices(espix_bt_dev_t *out, size_t n);
esp_err_t espix_bt_info(const uint8_t bda[ESPIX_BDA_LEN], espix_bt_dev_t *out);

/* Pairing and connection. Phase 1 is A2DP source, so pair/connect both mean
 * "bring up the audio link" (bonding follows from it). */
esp_err_t espix_bt_pair(const uint8_t bda[ESPIX_BDA_LEN]);
esp_err_t espix_bt_connect(const uint8_t bda[ESPIX_BDA_LEN]);
esp_err_t espix_bt_disconnect(const uint8_t bda[ESPIX_BDA_LEN]);
esp_err_t espix_bt_remove(const uint8_t bda[ESPIX_BDA_LEN]);
bool      espix_bt_a2d_connected(void);

/*
 * Publish the A2DP source as an audio sink (components/espix_audio). Called
 * once at boot; registration allocates nothing and does not put the stream on
 * the air. The engine sees the sink as disconnected until an A2DP link is up.
 *
 * Returns ESP_ERR_NOT_SUPPORTED when the audio sink is not built.
 */
esp_err_t espix_bt_audio_sink_register(void);

/*
 * SBC quality dial: 0 = mono, bitpool <= 35 (the measured-reliable point on this
 * link); 1 = joint/stereo, <= 35; 2 = joint stereo, <= 52. Applies to the next
 * codec negotiation, so reconnect after changing it.
 */
void espix_bt_set_sbc_quality(int q);
int  espix_bt_sbc_quality(void);

/*
 * AVRCP absolute volume, 0..127. `set` asks the sink to change its own volume;
 * `volume` returns the value the sink last reported, or -1 if it never has (a
 * sink with an analogue knob has no such value). Both are no-ops from the
 * sink's point of view when it does not implement absolute volume.
 */
esp_err_t espix_bt_set_volume(uint8_t v);
int       espix_bt_volume(void);

/*
 * Bring the controller down (and free the PCM ring). `power off` in the shell;
 * also what makes a quality change take effect, since the dial only applies to a
 * new codec negotiation. `connect` calls espix_bt_init() again.
 */
esp_err_t espix_bt_shutdown(void);

/*
 * Pairing policy, for now: a PIN for legacy pairing, and auto-accept for SSP
 * (the "just works" passkey). An interactive agent is later.
 */
esp_err_t espix_bt_set_pin(const char *pin);
const char *espix_bt_pin(void);

const char *espix_bt_bdastr(const uint8_t bda[ESPIX_BDA_LEN], char *buf, size_t len);
esp_err_t   espix_bt_parse_bda(const char *s, uint8_t out[ESPIX_BDA_LEN]);

#ifdef __cplusplus
}
#endif
