/*
 * Bluetooth, espix-shaped: a native layer over esp_bt with the names
 * bluetoothctl uses on Linux. Phase 1 is the S31 (Classic + BLE), with the
 * A2DP source as the audio output.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#include "espix_kernel.h"
#include <math.h>

#include "espix_bt.h"

#define TAG      "bt"
#define DEV_MAX  32
/*
 * The PCM ring, in PSRAM. Sized for slack, not for the frame: at 44.1 kHz
 * stereo the sink takes ~180 kB/s, so 64 kB was only ~350 ms and the ring could
 * be seen dipping to zero whenever the producer paused for a read or a decode
 * burst -- audible as padded-silence artefacts. PSRAM is the free pool here.
 */
#define PCM_BUF  (256 * 1024)

#if CONFIG_ESPIX_BT

#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_a2dp_legacy_api.h"
#include "esp_avrc_api.h"

static espix_bt_dev_t s_devs[DEV_MAX];
static size_t         s_dev_count;
static bool           s_inited;
static bool           s_scanning;
static char           s_pin[ESPIX_BT_PIN_MAX] = "0000";

static StreamBufferHandle_t s_pcm;        /* decoded PCM, waiting for the stack */
static esp_timer_handle_t   s_retry;      /* re-issues a failed A2DP connect */
static uint8_t              s_target[ESPIX_BDA_LEN];
static bool                 s_want_connect;
static unsigned             s_retries;
static StaticStreamBuffer_t s_pcm_cb;     /* its control block (internal RAM) */
static uint8_t             *s_pcm_storage;/* its storage (PSRAM) */
static bool                 s_a2d_connected;
static uint8_t              s_connected_bda[ESPIX_BDA_LEN];

/*
 * Whether the A2DP stream is on the air. START and SUSPEND are sent only on
 * transitions: two `play`s in quick succession otherwise had the first one's
 * SUSPEND and the second one's START cross, and Bluedroid logged "un-acked
 * a2dp cmd: 3" and stalled the ring while it sorted itself out.
 */
static bool                 s_streaming;

/* A zero address turns up in connection-state events for "no device"; it is
 * not a device and must not enter the list (it showed as 00:00:...). */
static bool bda_valid(const uint8_t bda[ESPIX_BDA_LEN])
{
    for (int i = 0; i < ESPIX_BDA_LEN; i++) {
        if (bda[i] != 0) {
            return true;
        }
    }
    return false;
}

static int dev_find(const uint8_t bda[ESPIX_BDA_LEN])
{
    for (size_t i = 0; i < s_dev_count; i++) {
        if (memcmp(s_devs[i].bda, bda, ESPIX_BDA_LEN) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void dev_upsert(const uint8_t bda[ESPIX_BDA_LEN], const char *name)
{
    if (!bda_valid(bda)) {
        return;
    }
    int i = dev_find(bda);
    if (i < 0) {
        if (s_dev_count >= DEV_MAX) {
            return;
        }
        i = (int)s_dev_count++;
        memset(&s_devs[i], 0, sizeof(s_devs[i]));
        memcpy(s_devs[i].bda, bda, ESPIX_BDA_LEN);
    }
    if (name != NULL && name[0] != '\0') {
        strlcpy(s_devs[i].name, name, sizeof(s_devs[i].name));
    }
}

static void mark_bonded(void)
{
    esp_bd_addr_t list[DEV_MAX];
    int n = esp_bt_gap_get_bond_device_num();

    if (n <= 0) {
        return;
    }
    if (n > DEV_MAX) {
        n = DEV_MAX;
    }
    if (esp_bt_gap_get_bond_device_list(&n, list) != ESP_OK) {
        return;
    }
    for (int i = 0; i < n; i++) {
        dev_upsert((const uint8_t *)list[i], NULL);
        const int j = dev_find((const uint8_t *)list[i]);
        if (j >= 0) {
            s_devs[j].bonded = true;
        }
    }
}

/* ---- GAP: discovery, pairing ---- */

static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT: {
        char name[ESPIX_BT_NAME_MAX] = {0};
        for (int i = 0; i < param->disc_res.num_prop; i++) {
            if (param->disc_res.prop[i].type == ESP_BT_GAP_DEV_PROP_BDNAME) {
                strlcpy(name, (const char *)param->disc_res.prop[i].val, sizeof(name));
            }
        }
        dev_upsert((const uint8_t *)param->disc_res.bda, name);
        break;
    }
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        s_scanning = (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED);
        espix_klog(ESPIX_KLOG_INFO, TAG, "discovery %s",
                   s_scanning ? "started" : "stopped");
        break;
    case ESP_BT_GAP_PIN_REQ_EVT: {
        esp_bt_pin_code_t pin = {0};
        const size_t len = strlen(s_pin);
        memcpy(pin, s_pin, len < sizeof(pin) ? len : sizeof(pin));
        (void)esp_bt_gap_pin_reply(param->pin_req.bda, true, (uint8_t)len, pin);
        espix_klog(ESPIX_KLOG_INFO, TAG, "pin requested; replied '%s'", s_pin);
        break;
    }
    case ESP_BT_GAP_CFM_REQ_EVT:
        (void)esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        espix_klog(ESPIX_KLOG_INFO, TAG, "ssp %06" PRIu32 " auto-accepted",
                   param->cfm_req.num_val);
        break;
    case ESP_BT_GAP_KEY_NOTIF_EVT:
        espix_klog(ESPIX_KLOG_INFO, TAG, "ssp passkey %06" PRIu32,
                   param->key_notif.passkey);
        break;
    case ESP_BT_GAP_KEY_REQ_EVT:
        (void)esp_bt_gap_ssp_passkey_reply(param->key_req.bda, true, 0);
        break;
    case ESP_BT_GAP_AUTH_CMPL_EVT: {
        const int i = dev_find((const uint8_t *)param->auth_cmpl.bda);
        if (i >= 0 && param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            s_devs[i].bonded = true;
            if (param->auth_cmpl.device_name[0] != 0) {
                strlcpy(s_devs[i].name, (const char *)param->auth_cmpl.device_name,
                        sizeof(s_devs[i].name));
            }
        }
        espix_klog(param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS ? ESPIX_KLOG_INFO
                                                                 : ESPIX_KLOG_WARN,
                   TAG, "pairing: %s", esp_err_to_name(param->auth_cmpl.stat));
        break;
    }
    default:
        break;
    }
}

/* ---- A2DP source ---- */

/*
 * The stack pulls PCM here, in the SBC frame size it needs; the play command
 * fills the buffer. An empty buffer yields silence rather than a stall, so a
 * connected speaker does not decide the link is dead between tracks.
 */
static const char *sbc_freq_str(uint8_t f)
{
    switch (f) {
    case 0: return "16000";
    case 1: return "32000";
    case 2: return "44100";
    case 3: return "48000";
    default: return "?";
    }
}

/*
 * The CIE channel-mode field is a *bitmask*, not an index: MONO 0x8, DUAL 0x4,
 * STEREO 0x2, JOINT 0x1 (esp_a2dp_api.h). The old 0-3 mapping printed "?" for
 * every real value, which hid which mode was actually negotiated -- exactly the
 * thing to know when a per-ear artefact is being chased.
 */
static const char *sbc_ch_str(uint8_t c)
{
    switch (c) {
    case 0x8: return "mono";
    case 0x4: return "dual";
    case 0x2: return "stereo";
    case 0x1: return "joint";
    default:  return "?";
    }
}

/*
 * Drain accounting. The PCM ring is only ever pulled from here, so bytes per
 * second is the sink's real consumption rate: at 48 kHz stereo it should read
 * ~192000 B/s, at 44.1 kHz ~176400 B/s. A lower number, or a ring that keeps
 * reading empty, is the stutter.
 */
static uint32_t s_drain_bytes;
static uint32_t s_drain_calls;
static uint32_t s_short_calls;
static uint32_t s_short_bytes;
static int64_t  s_drain_mark_us;

/*
 * The negotiated stream is MONO (see the preferred codec config in a2d_cb), so
 * the encoder asks for mono frames while the ring holds stereo PCM. Downmix
 * here rather than converting on the feed side: the callback already runs per
 * frame, and this keeps the ring's format a plain stereo PCM stream.
 */
static bool     s_mono;
static int16_t  s_dm[1024];   /* stereo samples for up to 512 mono samples */

/*
 * Pre-roll.
 *
 * A play that starts with an empty ring underruns until the producer has built
 * a cushion, because the producer only runs slightly ahead of realtime: a mono
 * tone measured 34 short calls (17 kB) in its first moments, heard as gaps,
 * then short 0 once the ring was deep. Until the cushion exists, hand the sink
 * silence rather than a partly-filled frame -- it is already playing, so
 * silence is inaudible where a gap is not. Reset for each play.
 */
#define PREROLL_BYTES (64 * 1024)
static bool     s_prerolled;

static int32_t a2d_data_cb(uint8_t *data, int32_t len)
{
    if (data == NULL || len <= 0) {
        return 0;
    }

    if (!s_prerolled) {
        if (xStreamBufferBytesAvailable(s_pcm) < PREROLL_BYTES) {
            memset(data, 0, (size_t)len);
            s_drain_bytes += (uint32_t)len;
            s_drain_calls++;
            return (int32_t)len;
        }
        s_prerolled = true;
    }

    size_t got;
    if (s_mono) {
        const size_t need = (size_t)len * 2;
        const size_t want = (need <= sizeof(s_dm)) ? need : sizeof(s_dm);
        got = xStreamBufferReceive(s_pcm, s_dm, want, 0);
        if (got < want) {
            memset((uint8_t *)s_dm + got, 0, want - got);
            s_short_calls++;
            s_short_bytes += (uint32_t)(want - got);
        }
        int16_t *out = (int16_t *)data;
        const int n = len >> 1;
        for (int i = 0; i < n; i++) {
            out[i] = (int16_t)(((int32_t)s_dm[2 * i] + (int32_t)s_dm[2 * i + 1]) / 2);
        }
        got = (size_t)len;
    } else {
        got = xStreamBufferReceive(s_pcm, data, (size_t)len, 0);
        if (got < (size_t)len) {
            memset(data + got, 0, (size_t)len - got);
            s_short_calls++;
            s_short_bytes += (uint32_t)((size_t)len - got);
        }
    }

    s_drain_bytes += (uint32_t)len;
    s_drain_calls++;
    const int64_t now = esp_timer_get_time();
    if (s_drain_mark_us == 0) {
        s_drain_mark_us = now;
    } else if (now - s_drain_mark_us >= 1000000) {
        /*
         * DEBUG, not INFO. The comment said so while the call said INFO, which is
         * why this spammed the console once a second: klog's ring is asynchronous
         * but its console echo is not, and this line has no business on the
         * terminal unless someone is debugging the drain. It stays in the ring
         * for dmesg.
         */
        espix_klog(ESPIX_KLOG_DEBUG, TAG,
                   "drain %u B/s, %u calls/s, ring %u B, short %u calls/%u B",
                   (unsigned)s_drain_bytes, (unsigned)s_drain_calls,
                   (unsigned)xStreamBufferBytesAvailable(s_pcm),
                   (unsigned)s_short_calls, (unsigned)s_short_bytes);
        s_drain_bytes = 0;
        s_drain_calls = 0;
        s_short_calls = 0;
        s_short_bytes = 0;
        s_drain_mark_us = now;
    }
    return len;
}

/* Minimal AVRCP controller callback: its existence is what A2DP requires, and
 * the connection state is worth a line in the log. */
/*
 * AVRCP absolute volume.
 *
 * A sink with a digital volume (the Q45) changes it itself and reports the new
 * value; we can also set it. The register-notification command is one-shot, so
 * it is re-armed after every report -- miss that and the sink's later changes
 * are silent. A sink whose volume is an analogue knob (the HD3) simply never
 * reports, which is why the value stays unknown there and setting it does
 * nothing audible.
 */
static uint8_t s_sink_volume;
static bool    s_sink_volume_known;
static uint8_t s_rc_tl;

/*
 * The volume we present as an AVRCP target, 0..127, and whether it means
 * anything yet. A sink whose buttons are not local (the Q45) sends passthrough
 * volume-up/down to the *source*, expecting the source to hold the volume and
 * tell it back -- and it does not move its own volume while it waits. With no
 * TG those commands had nowhere to land, which is why its buttons did nothing.
 */
static uint8_t s_tg_volume = 64;
static bool    s_tg_volume_valid;

static void avrc_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param)
{
    switch (event) {
    case ESP_AVRC_TG_CONNECTION_STATE_EVT:
        espix_klog(ESPIX_KLOG_INFO, TAG, "avrc tg %s",
                   param->conn_stat.connected ? "connected" : "disconnected");
        break;

    case ESP_AVRC_TG_PASSTHROUGH_CMD_EVT:
        /* Act on the press; ignore the release that follows it. */
        if (param->psth_cmd.key_state != ESP_AVRC_PT_CMD_STATE_PRESSED) {
            break;
        }
        if (param->psth_cmd.key_code == ESP_AVRC_PT_CMD_VOL_UP ||
            param->psth_cmd.key_code == ESP_AVRC_PT_CMD_VOL_DOWN) {
            const int step = (param->psth_cmd.key_code == ESP_AVRC_PT_CMD_VOL_UP) ? 5 : -5;
            int v = (int)s_tg_volume + step;
            v = (v < 0) ? 0 : (v > 127 ? 127 : v);

            /* There is no PCM gain yet, so the audible level is the sink's own
             * absolute volume: set that, which is the effect the button asked
             * for. When a mixer arrives this becomes the gain instead. */
            (void)espix_bt_set_volume((uint8_t)v);
            espix_klog(ESPIX_KLOG_INFO, TAG, "sink volume %d/127 (its buttons)", v);
        }
        break;

    case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT:
        /* The sink setting *our* volume -- it is the CT for this direction. */
        s_tg_volume       = param->set_abs_vol.volume;
        s_tg_volume_valid = true;
        espix_klog(ESPIX_KLOG_DEBUG, TAG, "sink set our volume to %u/127",
                   (unsigned)s_tg_volume);
        break;

    default:
        break;
    }
}

static uint8_t rc_next_tl(void)
{
    s_rc_tl = (uint8_t)((s_rc_tl + 1) & 0x0f);   /* 0..15, consecutive differ */
    return s_rc_tl;
}

static void avrc_arm_volume(void)
{
    const esp_err_t e = esp_avrc_ct_send_register_notification_cmd(rc_next_tl(),
                                                                  ESP_AVRC_RN_VOLUME_CHANGE, 0);
    if (e != ESP_OK) {
        /*
         * If this fails, the sink's later changes cannot reach us and the value
         * stays unknown forever -- so say so rather than leaving it a mystery.
         */
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "cannot register for volume notifications: %s",
                   esp_err_to_name(e));
    }
}

esp_err_t espix_bt_set_volume(uint8_t v)
{
    if (v > 0x7f) {
        v = 0x7f;
    }

    s_tg_volume       = v;
    s_tg_volume_valid = true;

    return esp_avrc_ct_send_set_absolute_volume_cmd(rc_next_tl(), v);
}

int espix_bt_volume(void)
{
    if (s_sink_volume_known) {
        return (int)s_sink_volume;      /* the sink told us */
    }
    if (s_tg_volume_valid) {
        return (int)s_tg_volume;        /* the value we last set */
    }
    return -1;
}

static void avrc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    switch (event) {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
        espix_klog(ESPIX_KLOG_INFO, TAG, "avrc %s",
                   param->conn_stat.connected ? "connected" : "disconnected");
        if (param->conn_stat.connected) {
            avrc_arm_volume();
        } else {
            s_sink_volume_known = false;
        }
        break;

    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
        /*
         * DEBUG, not INFO: a chatty sink (the Q45 reports more than volume) would
         * otherwise fill the console. It is in dmesg, which is where to look when
         * asking whether a sink reports at all.
         */
        espix_klog(ESPIX_KLOG_DEBUG, TAG, "avrc notify %d",
                   (int)param->change_ntf.event_id);
        if (param->change_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
            s_sink_volume       = param->change_ntf.event_parameter.volume;
            s_sink_volume_known = true;
            espix_klog(ESPIX_KLOG_INFO, TAG, "sink volume %u/127",
                       (unsigned)s_sink_volume);
            avrc_arm_volume();      /* one-shot: ask for the next change */
        }
        break;

    default:
        espix_klog(ESPIX_KLOG_DEBUG, TAG, "avrc ct event %d", (int)event);
        break;
    }
}

/*
 * A2DP source connections habitually fail the first several times with HCI
 * Page Timeout (st 0x4) and then succeed -- issue #15913 has the same log on a
 * plain ESP32. IDF's example retries until connected; so does espix now, or a
 * single connect would look like a permanent failure.
 */
static void retry_connect(void *arg)
{
    (void)arg;
    if (s_want_connect && s_retries < 40) {
        s_retries++;
        espix_klog(ESPIX_KLOG_INFO, TAG, "a2dp connect retry %u", s_retries);
        (void)esp_a2d_source_connect(s_target);
    }
}

/*
 * Which SBC configuration to ask the sink for.
 *
 * This is a *policy over the sink's capabilities*, not a capability limit. The
 * sink advertises what it can decode (the Q45: every sample rate, every channel
 * mode, bitpool 2-52); what espix should send is the intersection of that with
 * what espix can encode, de-rated to what the link has been *measured* to hold.
 *
 * ESPIX_BT_SBC_QUALITY is that de-rating dial:
 *   0  mono, bitpool <= 35   the measured-reliable point on this link, and what
 *                            IDF's stock a2dp_source example picks; plays clean.
 *   1  joint stereo, <= 35
 *   2  joint stereo, <= 52   the sink's full advertised bitpool
 * Raise it by testing, one step at a time -- which is the point of keeping
 * capability and measured-reliable separate.
 */
/*
 * Runtime rather than a #define: the quality dial is the thing to A/B by ear
 * when chasing an artefact, and rebuilding for each setting is a wasted cycle.
 * `bluetoothctl quality [0|1|2]` sets it; it applies to the next codec
 * negotiation, so reconnect (or re-run `bluetoothctl connect`) after changing.
 */
/*
 * Default q2: joint stereo, bitpool <= 52 -- the full budget the sink offers,
 * and the setting that sounded right on the Q45.
 *
 * It was q0/q1 out of caution, after one measurement showed the audio task at
 * 77% with q2. A repeat measurement over many intervals showed ~43%, the same as
 * q1 (decode ~430 ms per ~1020 ms), so that 77% was a transient -- the first
 * sample after a codec change, or SSH traffic in that window -- and not a
 * property of q2. The link is the only thing it really spends.
 *
 * A mono source is still sent as two channels (the ring is stereo by contract,
 * so the difference channel is empty); this dial is about the link, not the file.
 */
static int s_sbc_quality = 2;

void espix_bt_set_sbc_quality(int q)
{
    s_sbc_quality = (q < 0) ? 0 : (q > 2 ? 2 : q);
}

int espix_bt_sbc_quality(void)
{
    return s_sbc_quality;
}

static esp_err_t a2d_pick_pref_mcc(const esp_a2d_mcc_t *caps, esp_a2d_mcc_t *out)
{
    if (caps->type != ESP_A2D_MCT_SBC) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const esp_a2d_cie_sbc_t *c = &caps->cie.sbc_info;

    memset(out, 0, sizeof(*out));
    out->type = ESP_A2D_MCT_SBC;
    esp_a2d_cie_sbc_t *p = &out->cie.sbc_info;

    /* No resampler yet, so 44.1 kHz or nothing. */
    if (!(c->samp_freq & ESP_A2D_SBC_CIE_SF_44K)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    p->samp_freq = ESP_A2D_SBC_CIE_SF_44K;

    /* Best channels this level is willing to send and the sink can take. */
    if (s_sbc_quality >= 1 && (c->ch_mode & ESP_A2D_SBC_CIE_CH_MODE_JOINT_STEREO)) {
        p->ch_mode = ESP_A2D_SBC_CIE_CH_MODE_JOINT_STEREO;
    } else if (s_sbc_quality >= 1 && (c->ch_mode & ESP_A2D_SBC_CIE_CH_MODE_STEREO)) {
        p->ch_mode = ESP_A2D_SBC_CIE_CH_MODE_STEREO;
    } else if (c->ch_mode & ESP_A2D_SBC_CIE_CH_MODE_MONO) {
        p->ch_mode = ESP_A2D_SBC_CIE_CH_MODE_MONO;
    } else {
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Where more is better, take the best the sink offers. */
    p->block_len    = (c->block_len    & ESP_A2D_SBC_CIE_BLOCK_LEN_16)        ? ESP_A2D_SBC_CIE_BLOCK_LEN_16
                    : (c->block_len    & ESP_A2D_SBC_CIE_BLOCK_LEN_12)        ? ESP_A2D_SBC_CIE_BLOCK_LEN_12
                    : c->block_len;
    p->num_subbands = (c->num_subbands & ESP_A2D_SBC_CIE_NUM_SUBBANDS_8)      ? ESP_A2D_SBC_CIE_NUM_SUBBANDS_8
                    : c->num_subbands;
    p->alloc_mthd   = (c->alloc_mthd   & ESP_A2D_SBC_CIE_ALLOC_MTHD_LOUDNESS) ? ESP_A2D_SBC_CIE_ALLOC_MTHD_LOUDNESS
                    : c->alloc_mthd;

    /* Bitpool: inside the sink's range and under this level's cap. */
    const uint8_t cap = (s_sbc_quality >= 2) ? 52 : 35;
    p->min_bitpool = (c->min_bitpool < 2) ? 2 : c->min_bitpool;
    p->max_bitpool = (c->max_bitpool > cap) ? cap : c->max_bitpool;
    if (p->max_bitpool < p->min_bitpool) {
        p->max_bitpool = p->min_bitpool;
    }
    return ESP_OK;
}

static void a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
        const esp_a2d_connection_state_t st = param->conn_stat.state;
        const int i = dev_find((const uint8_t *)param->conn_stat.remote_bda);
        if (i >= 0) {
            s_devs[i].connected = (st == ESP_A2D_CONNECTION_STATE_CONNECTED);
        }
        if (st == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            s_a2d_connected = true;
            memcpy(s_connected_bda, param->conn_stat.remote_bda, ESPIX_BDA_LEN);
            s_want_connect = false;
            s_retries = 0;
            espix_klog(ESPIX_KLOG_INFO, TAG, "a2dp connected; checking source");
            (void)esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
        } else if (st == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            s_a2d_connected = false;
            s_streaming     = false;
            espix_klog(ESPIX_KLOG_INFO, TAG, "a2dp disconnected");
            if (s_want_connect && s_retry != NULL) {
                (void)esp_timer_start_once(s_retry, 2 * 1000 * 1000);
            }
        }
        break;
    }
    case ESP_A2D_AUDIO_CFG_EVT: {
        const esp_a2d_mcc_t *m = &param->audio_cfg.mcc;
        if (m->type == ESP_A2D_MCT_SBC) {
            espix_klog(ESPIX_KLOG_INFO, TAG,
                       "sink cfg: SBC %s Hz, %s, subbands %u, block %u, bitpool %u-%u",
                       sbc_freq_str(m->cie.sbc_info.samp_freq),
                       sbc_ch_str(m->cie.sbc_info.ch_mode),
                       (unsigned)m->cie.sbc_info.num_subbands,
                       (unsigned)m->cie.sbc_info.block_len,
                       (unsigned)m->cie.sbc_info.min_bitpool,
                       (unsigned)m->cie.sbc_info.max_bitpool);
        } else {
            espix_klog(ESPIX_KLOG_INFO, TAG, "sink cfg: codec type 0x%x",
                       (unsigned)m->type);
        }
        break;
    }
    case ESP_A2D_REPORT_SNK_CODEC_CAPS_EVT: {
        /*
         * The stock a2dp_source example sets a preferred codec config here,
         * and it is load-bearing: with the sink's defaults (joint stereo,
         * bitpool up to 52/53) this link delivers a garbled stream that the
         * sink plays as harsh noise, while the example's mono + bitpool<=35
         * config is clean. Matched to the example's values.
         */
        const esp_a2d_conn_hdl_t h = param->a2d_report_snk_codec_caps_stat.conn_hdl;
        const esp_a2d_mcc_t *caps = &param->a2d_report_snk_codec_caps_stat.mcc;

        esp_a2d_mcc_t pref;
        const esp_err_t picked = a2d_pick_pref_mcc(caps, &pref);
        if (picked != ESP_OK) {
            espix_klog(ESPIX_KLOG_WARN, TAG,
                       "sink offers nothing this engine can encode (type 0x%x)",
                       (unsigned)caps->type);
            break;
        }
        const esp_err_t e = esp_a2d_source_set_pref_mcc(h, &pref);
        s_mono = (e == ESP_OK) && (pref.cie.sbc_info.ch_mode == ESP_A2D_SBC_CIE_CH_MODE_MONO);
        espix_klog(e == ESP_OK ? ESPIX_KLOG_INFO : ESPIX_KLOG_WARN, TAG,
                   "preferred mcc: %s (q%d, %s, bitpool %u-%u)",
                   esp_err_to_name(e), s_sbc_quality,
                   s_mono ? "mono" : "stereo",
                   (unsigned)pref.cie.sbc_info.min_bitpool,
                   (unsigned)pref.cie.sbc_info.max_bitpool);
        break;
    }
    case ESP_A2D_MEDIA_CTRL_ACK_EVT:
        if (param->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY &&
            param->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
            /*
             * Suspend rather than start: a source that starts here streams
             * continuously, so an idle connection had Bluedroid encoding silence
             * once a second forever -- measured at 8% of a core on BTC_TASK, plus
             * the link airtime, plus a drain log line a second. The stream is
             * started by `play` (espix_bt_audio_start) and suspended again when
             * playback finishes, which is what a phone does.
             */
            (void)esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND);
            s_streaming = false;
        }
        break;
    default:
        break;
    }
}

esp_err_t espix_bt_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_bt_controller_init(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_bluedroid_init();
    if (err != ESP_OK) {
        return err;
    }
    err = esp_bluedroid_enable();
    if (err != ESP_OK) {
        return err;
    }

    (void)esp_bt_gap_register_callback(gap_cb);
    (void)esp_bt_gap_set_device_name(CONFIG_ESPIX_BT_NAME);
    (void)esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

    /*
     * PSRAM for the PCM: 32 KB of contiguous internal RAM is not there once
     * Bluetooth, WiFi and Ethernet have taken theirs, and PSRAM is where
     * audio belongs on this chip anyway.
     */
    s_pcm_storage = heap_caps_malloc(PCM_BUF, MALLOC_CAP_SPIRAM);
    if (s_pcm_storage == NULL) {
        s_pcm_storage = malloc(PCM_BUF);
    }
    if (s_pcm_storage == NULL) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "no PCM buffer");
        return ESP_ERR_NO_MEM;
    }
    s_pcm = xStreamBufferCreateStatic(PCM_BUF, 1, s_pcm_storage, &s_pcm_cb);
    if (s_pcm == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const esp_timer_create_args_t retry_args = {
        .callback = retry_connect,
        .name     = "bt_retry",
    };
    err = esp_timer_create(&retry_args, &s_retry);
    if (err != ESP_OK) {
        return err;
    }

    /*
     * AVRCP first. Bluedroid refuses the A2DP link with "A2DP Enable without
     * AVRC" when the controller half is missing -- exactly the
     * BTA_AV_OPEN_EVT::FAILED the speaker produced.
     */
    err = esp_avrc_ct_init();
    if (err != ESP_OK) {
        return err;
    }
    (void)esp_avrc_ct_register_callback(avrc_ct_cb);

    /*
     * And the target half, which is what lets a sink's own buttons reach us.
     * A sink whose volume is not local sends passthrough volume-up/down to the
     * source and waits for it to hold the level; with CT alone those commands
     * had nowhere to land.
     */
    err = esp_avrc_tg_init();
    if (err != ESP_OK) {
        return err;
    }
    (void)esp_avrc_tg_register_callback(avrc_tg_cb);

    /*
     * No AVRCP target. A source is an AVRCP controller; enabling the target
     * role too is what espix did and the stock a2dp_source example does not,
     * and it is the last structural difference between them. CT alone is what
     * a source needs.
     */
    err = esp_a2d_source_init();
    if (err != ESP_OK) {
        return err;
    }
    (void)esp_a2d_register_callback(a2d_cb);
    (void)esp_a2d_source_register_data_callback(a2d_data_cb);

    s_inited = true;
    espix_klog(ESPIX_KLOG_INFO, TAG, "up as '%s' (a2dp source)", CONFIG_ESPIX_BT_NAME);
    return ESP_OK;
}

/*
 * Take the controller down, mirroring espix_bt_init(). Needed for two things:
 * a `power off` that is not a reboot, and a guaranteed fresh codec negotiation
 * -- the SBC quality only applies to a new negotiation, and a plain A2DP
 * disconnect leaves the sink free to re-establish with the old config.
 */
esp_err_t espix_bt_shutdown(void)
{
    if (!s_inited) {
        return ESP_OK;
    }

    /* Stop wanting the link first: the retry timer would otherwise reconnect
     * into a stack that is being torn down. */
    s_want_connect = false;
    if (s_retry != NULL) {
        (void)esp_timer_stop(s_retry);
    }

    (void)esp_a2d_source_deinit();
    (void)esp_avrc_ct_deinit();
    (void)esp_bluedroid_disable();
    (void)esp_bluedroid_deinit();
    (void)esp_bt_controller_disable();
    (void)esp_bt_controller_deinit();

    if (s_retry != NULL) {
        esp_timer_delete(s_retry);
        s_retry = NULL;
    }
    if (s_pcm_storage != NULL) {
        heap_caps_free(s_pcm_storage);
        s_pcm_storage = NULL;
        s_pcm = NULL;
    }

    s_inited = false;
    s_a2d_connected = false;

    espix_klog(ESPIX_KLOG_INFO, TAG, "powered off");
    return ESP_OK;
}

bool espix_bt_ready(void)         { return s_inited; }
bool espix_bt_scanning(void)      { return s_scanning; }
bool espix_bt_a2d_connected(void) { return s_a2d_connected; }

esp_err_t espix_bt_scan(bool on)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (on) {
        s_dev_count = 0;
        return esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 0x30, 0);
    }
    return esp_bt_gap_cancel_discovery();
}

size_t espix_bt_devices(espix_bt_dev_t *out, size_t n)
{
    size_t count = 0;
    if (s_inited) {
        mark_bonded();
    }
    for (size_t i = 0; i < s_dev_count && count < n; i++) {
        out[count++] = s_devs[i];
    }
    return count;
}

esp_err_t espix_bt_info(const uint8_t bda[ESPIX_BDA_LEN], espix_bt_dev_t *out)
{
    if (s_inited) {
        mark_bonded();
    }
    const int i = dev_find(bda);
    if (i < 0) {
        return ESP_ERR_NOT_FOUND;
    }
    *out = s_devs[i];
    return ESP_OK;
}

/* For a speaker, pairing and connecting are the same act: bonding follows the
 * A2DP link. pair exists because bluetoothctl has it and people reach for it. */
esp_err_t espix_bt_pair(const uint8_t bda[ESPIX_BDA_LEN])
{
    return espix_bt_connect(bda);
}

esp_err_t espix_bt_connect(const uint8_t bda[ESPIX_BDA_LEN])
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Already connected to this one: re-requesting it is not a harmless no-op.
     * Bluedroid's A2DP state machine is in "started", so it drops the request
     * with "btc_av_state_started_handler : unhandled event:BTC_AV_CONNECT_REQ_EVT"
     * and nothing happens -- which reads as the command being broken. This is
     * also the common case when the sink connects by itself, since a bonded
     * sink re-opens the link unprompted.
     */
    if (s_a2d_connected && memcmp(s_connected_bda, bda, ESPIX_BDA_LEN) == 0) {
        return ESP_OK;
    }

    dev_upsert(bda, NULL);
    memcpy(s_target, bda, ESPIX_BDA_LEN);
    s_want_connect = true;
    s_retries      = 0;
    return esp_a2d_source_connect((uint8_t *)bda);
}

esp_err_t espix_bt_disconnect(const uint8_t bda[ESPIX_BDA_LEN])
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    s_want_connect = false;
    if (s_retry != NULL) {
        (void)esp_timer_stop(s_retry);
    }
    return esp_a2d_source_disconnect((uint8_t *)bda);
}

esp_err_t espix_bt_remove(const uint8_t bda[ESPIX_BDA_LEN])
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = esp_bt_gap_remove_bond_device((uint8_t *)bda);
    const int i = dev_find(bda);
    if (i >= 0 && err == ESP_OK) {
        s_devs[i].bonded = false;
    }
    return err;
}

/*
 * Returns the number of bytes the ring accepted, not an error code.
 *
 * xStreamBufferSend takes what fits and reports how much, so a full ring (the
 * normal state while playing) accepts part of a chunk. Reporting that as an
 * error and letting the caller retry the whole chunk re-sends the bytes that
 * were already consumed, and the stream desynchronises -- clean for the first
 * moment, then harsh noise. The caller must advance by exactly this count.
 * The short blocking timeout waits for space; the caller paces on it.
 */
void espix_bt_audio_start(void)
{
    s_prerolled = false;
    if (s_pcm != NULL) {
        (void)xStreamBufferReset(s_pcm);
    }

    /* The stream is suspended at connect; this is what puts it on the air. */
    if (s_a2d_connected && !s_streaming) {
        (void)esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
        s_streaming = true;
    }
}

void espix_bt_audio_suspend(void)
{
    if (s_a2d_connected && s_streaming) {
        (void)esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND);
        s_streaming = false;
    }
}

size_t espix_bt_audio_write(const void *pcm, size_t len)
{
    if (!s_inited || s_pcm == NULL) {
        return 0;
    }
    return xStreamBufferSend(s_pcm, pcm, len, pdMS_TO_TICKS(20));
}

esp_err_t espix_bt_set_pin(const char *pin)
{
    if (pin == NULL || strlen(pin) > ESPIX_BT_PIN_MAX - 1) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(s_pin, pin, sizeof(s_pin));
    return ESP_OK;
}

const char *espix_bt_pin(void) { return s_pin; }

#else  /* !CONFIG_ESPIX_BT */

esp_err_t espix_bt_init(void)                 { return ESP_ERR_NOT_SUPPORTED; }
bool      espix_bt_ready(void)                { return false; }
bool      espix_bt_scanning(void)             { return false; }
bool      espix_bt_a2d_connected(void)        { return false; }
esp_err_t espix_bt_scan(bool on)              { (void)on; return ESP_ERR_NOT_SUPPORTED; }
size_t    espix_bt_devices(espix_bt_dev_t *o, size_t n) { (void)o; (void)n; return 0; }
esp_err_t espix_bt_info(const uint8_t b[ESPIX_BDA_LEN], espix_bt_dev_t *o)
                                              { (void)b; (void)o; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t espix_bt_pair(const uint8_t b[ESPIX_BDA_LEN])       { (void)b; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t espix_bt_connect(const uint8_t b[ESPIX_BDA_LEN])    { (void)b; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t espix_bt_disconnect(const uint8_t b[ESPIX_BDA_LEN]) { (void)b; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t espix_bt_remove(const uint8_t b[ESPIX_BDA_LEN])     { (void)b; return ESP_ERR_NOT_SUPPORTED; }
size_t    espix_bt_audio_write(const void *p, size_t n)       { (void)p; (void)n; return 0; }
esp_err_t espix_bt_set_pin(const char *p)     { (void)p; return ESP_ERR_NOT_SUPPORTED; }
const char *espix_bt_pin(void)                { return ""; }

#endif /* CONFIG_ESPIX_BT */

const char *espix_bt_bdastr(const uint8_t bda[ESPIX_BDA_LEN], char *buf, size_t len)
{
    if (buf == NULL || len < 18) {
        return "";
    }
    snprintf(buf, len, "%02x:%02x:%02x:%02x:%02x:%02x",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
    return buf;
}

esp_err_t espix_bt_parse_bda(const char *s, uint8_t out[ESPIX_BDA_LEN])
{
    unsigned v[ESPIX_BDA_LEN];
    if (s == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (sscanf(s, "%x:%x:%x:%x:%x:%x",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < ESPIX_BDA_LEN; i++) {
        if (v[i] > 0xff) {
            return ESP_ERR_INVALID_ARG;
        }
        out[i] = (uint8_t)v[i];
    }
    return ESP_OK;
}
