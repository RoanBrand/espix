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
 * PCM for the source to send: 44.1 kHz, stereo, signed 16-bit, as A2DP's SBC
 * encoder expects. `play` decodes into this; the stack pulls it on its own
 * callback. Short writes are the caller's to retry.
 */
/* Accepts as much PCM as the ring has room for and returns that count. The
 * caller must advance by it; a full ring is normal while the sink pulls. */
size_t espix_bt_audio_write(const void *pcm, size_t len);

/*
 * Begin a new stream: drop any PCM left from the last one and re-arm the
 * pre-roll, so the new stream starts with a cushion rather than underrunning.
 */
void espix_bt_audio_start(void);

/*
 * SBC quality dial: 0 = mono, bitpool <= 35 (the measured-reliable point on this
 * link); 1 = joint/stereo, <= 35; 2 = joint stereo, <= 52. Applies to the next
 * codec negotiation, so reconnect after changing it.
 */
void espix_bt_set_sbc_quality(int q);
int  espix_bt_sbc_quality(void);

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
