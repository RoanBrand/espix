/*
 * espix WiFi station.
 *
 * Thin: esp_wifi plus esp_netif_create_default_wifi_sta() already handle
 * association, DHCP, DNS and the default route. What is added here is the
 * /etc/wifi.conf lifecycle, retry policy, and a state view for `wifi status`.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"

#include "espix_kernel.h"
#include "espix_net.h"
#include "espix_net_priv.h"

#define TAG "wifi"

#define WIFI_CONF_PATH  "/etc/wifi.conf"
#define SCAN_MAX        24

/* Retry with a ceiling rather than forever-fast: a wrong PSK should not spin
 * the radio, but a router rebooting should still be recovered from. */
#define RETRY_DELAY_MS  5000

static esp_netif_t          *s_sta;
static espix_wifi_state_t    s_state;
static unsigned              s_retries;
static bool                  s_want_connect;
static EventGroupHandle_t    s_events;
static esp_timer_handle_t    s_retry_timer;

/*
 * Boot barrier hold, taken only for the connect started at boot. Released once
 * this interface has settled — either it has an address, or it has failed once
 * and is now in a retry loop. Idempotent, because either outcome can arrive
 * first and only one release is owed.
 */
static bool s_boot_hold;

static void release_boot_hold(void)
{
    if (s_boot_hold) {
        s_boot_hold = false;
        espix_kernel_boot_release();
    }
}

#define BIT_SCAN_DONE  BIT0

/* Fires off the timer task, not the event loop, so calling back into
 * esp_wifi_connect() here is safe. */
static void retry_connect(void *arg)
{
    (void)arg;
    if (s_want_connect) {
        esp_wifi_connect();
    }
}

/* ------------------------------------------------------------------ */
/* Config file                                                         */
/* ------------------------------------------------------------------ */

esp_err_t espix_net_conf_write_wifi(const char *ssid, const char *psk)
{
    FILE *f = fopen(WIFI_CONF_PATH, "w");
    if (f == NULL) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "cannot write %s: %s",
                   WIFI_CONF_PATH, strerror(errno));
        return ESP_FAIL;
    }

    fprintf(f, "# espix wifi configuration\n");
    fprintf(f, "ssid=%s\n", ssid ? ssid : "");
    fprintf(f, "psk=%s\n", psk ? psk : "");
    fprintf(f, "# dhcp=yes is the default\n");
    fclose(f);

    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Events                                                              */
/* ------------------------------------------------------------------ */

static void on_wifi_event(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    (void)arg;
    (void)base;

    switch (id) {
    case WIFI_EVENT_STA_START:
        s_state = ESPIX_WIFI_IDLE;
        if (s_want_connect) {
            s_state = ESPIX_WIFI_CONNECTING;
            esp_wifi_connect();
        }
        break;

    case WIFI_EVENT_STA_CONNECTED: {
        wifi_event_sta_connected_t *ev = data;
        s_state   = ESPIX_WIFI_CONNECTED;
        s_retries = 0;
        espix_klog(ESPIX_KLOG_INFO, TAG,
                   "wlan0: associated with %.*s on channel %u",
                   ev->ssid_len, (const char *)ev->ssid, ev->channel);
        break;
    }

    case WIFI_EVENT_STA_DISCONNECTED: {
        wifi_event_sta_disconnected_t *ev = data;
        s_state = s_want_connect ? ESPIX_WIFI_CONNECTING : ESPIX_WIFI_IDLE;

        /*
         * Boot is no longer settling once we have failed once: s_want_connect
         * keeps us retrying forever, so there is no later "gave up" event to
         * wait for, and holding the console until the cap would be wrong.
         */
        release_boot_hold();

        if (s_want_connect) {
            s_retries++;
            espix_klog(ESPIX_KLOG_WARN, TAG,
                       "wlan0: disconnected (reason %d), retry %u in %d ms",
                       ev->reason, s_retries, RETRY_DELAY_MS);
            /*
             * Delayed via a timer, NOT vTaskDelay(): this runs on the default
             * event loop task, so sleeping here would stall every other event
             * — including the IP events we depend on — and let the event queue
             * back up. Retrying instantly is also wrong; a bad PSK would spin
             * the radio and flood the log.
             */
            esp_timer_start_once(s_retry_timer, RETRY_DELAY_MS * 1000ULL);
        } else {
            espix_klog(ESPIX_KLOG_INFO, TAG, "wlan0: disconnected");
        }
        break;
    }

    case WIFI_EVENT_SCAN_DONE:
        if (s_events != NULL) {
            xEventGroupSetBits(s_events, BIT_SCAN_DONE);
        }
        break;

    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base,
                        int32_t id, void *data)
{
    (void)arg;
    (void)base;

    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        char ip[ESPIX_IP4STR_MAX], gw[ESPIX_IP4STR_MAX];

        espix_klog(ESPIX_KLOG_INFO, TAG, "wlan0: %s/%d via %s",
                   espix_net_ip4str(ev->ip_info.ip.addr, ip, sizeof(ip)),
                   espix_net_prefix_len(ev->ip_info.netmask.addr),
                   espix_net_ip4str(ev->ip_info.gw.addr, gw, sizeof(gw)));

        uint32_t dns[2];
        const size_t n = espix_net_dns(dns, 2);
        for (size_t i = 0; i < n; i++) {
            char d[ESPIX_IP4STR_MAX];
            espix_klog(ESPIX_KLOG_INFO, TAG, "wlan0: nameserver %s",
                       espix_net_ip4str(dns[i], d, sizeof(d)));
        }

        /* Last: the interface is fully up, and this is the message the console
         * has been holding its first prompt for. */
        release_boot_hold();
    } else if (id == IP_EVENT_STA_LOST_IP) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "wlan0: lost address");
    }
}

/* ------------------------------------------------------------------ */
/* Bring-up                                                            */
/* ------------------------------------------------------------------ */

/*
 * The driver narrates every association, buffer allocation and power-save
 * transition at INFO, straight over the shell prompt — and the session task is
 * blocked inside linenoise() at the time, so nothing redraws it. espix reports
 * the parts that matter (associated / address / gateway / nameserver) through
 * klog, so lift these to WARN: anything the driver considers a real problem
 * still comes through, and dmesg keeps everything either way.
 *
 * Must run before esp_wifi_init(): the pp and net80211 version banners are
 * emitted during init, and phy_init's during start.
 */
static void quiet_driver_logs(void)
{
    static const char *const noisy[] = {
        "wifi",       /* state machine, block-ack, power save */
        "wifi_init",  /* the buffer-count block */
        "pp",
        "net80211",
        "phy_init",
        "esp_netif_handlers",   /* "sta ip: ..." duplicates our own klog line */
    };

    for (size_t i = 0; i < sizeof(noisy) / sizeof(noisy[0]); i++) {
        esp_log_level_set(noisy[i], ESP_LOG_WARN);
    }
}

static esp_err_t driver_start(void)
{
    if (s_sta != NULL) {
        return ESP_OK;
    }

    quiet_driver_logs();

    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const esp_timer_create_args_t retry_args = {
        .callback = retry_connect,
        .name     = "wifi_retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&retry_args, &s_retry_timer));

    s_sta = esp_netif_create_default_wifi_sta();
    if (s_sta == NULL) {
        return ESP_FAIL;
    }

    /* Named here, not by esp_netif's own key ("WIFI_STA_DEF"). */
    espix_net_register_if("wlan0", ESPIX_IF_WIFI_STA, s_sta);

    const wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, ESP_EVENT_ANY_ID, on_ip_event, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    return ESP_OK;
}

esp_err_t espix_net_wifi_start(void)
{
    char ssid[ESPIX_SSID_MAX] = {0};
    char psk[ESPIX_PSK_MAX]   = {0};

    const bool have_ssid =
        espix_net_conf_get(WIFI_CONF_PATH, "ssid", ssid, sizeof(ssid)) &&
        ssid[0] != '\0';

    /* Start the driver either way, so `wifi scan` works before any network is
     * configured — otherwise you cannot discover what to connect to. */
    const esp_err_t err = driver_start();
    if (err != ESP_OK) {
        return err;
    }

    if (!have_ssid) {
        espix_klog(ESPIX_KLOG_INFO, TAG,
                   "no network configured (%s); use 'wifi connect'",
                   WIFI_CONF_PATH);
        return ESP_ERR_NOT_FOUND;
    }

    espix_net_conf_get(WIFI_CONF_PATH, "psk", psk, sizeof(psk));

    /*
     * Only the boot connect holds the barrier. A later `wifi connect` from the
     * shell must not — the console is already running by then, and there is
     * nothing left to gate.
     */
    s_boot_hold = true;
    espix_kernel_boot_hold();

    const esp_err_t cerr = espix_net_wifi_connect(ssid, psk);
    if (cerr != ESP_OK) {
        release_boot_hold();
    }
    return cerr;
}

esp_err_t espix_net_wifi_connect(const char *ssid, const char *psk)
{
    char file_ssid[ESPIX_SSID_MAX] = {0};
    char file_psk[ESPIX_PSK_MAX]   = {0};

    /* No arguments: re-read the file, so `wifi connect` after editing
     * /etc/wifi.conf by hand does what you would expect. */
    if (ssid == NULL) {
        if (!espix_net_conf_get(WIFI_CONF_PATH, "ssid",
                                file_ssid, sizeof(file_ssid))) {
            return ESP_ERR_NOT_FOUND;
        }
        espix_net_conf_get(WIFI_CONF_PATH, "psk", file_psk, sizeof(file_psk));
        ssid = file_ssid;
        psk  = file_psk;
    } else {
        espix_net_conf_write_wifi(ssid, psk);
    }

    esp_err_t err = driver_start();
    if (err != ESP_OK) {
        return err;
    }

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    if (psk != NULL) {
        strlcpy((char *)wc.sta.password, psk, sizeof(wc.sta.password));
    }

    err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) {
        return err;
    }

    s_want_connect = true;
    s_retries      = 0;
    s_state        = ESPIX_WIFI_CONNECTING;

    espix_klog(ESPIX_KLOG_INFO, TAG, "wlan0: connecting to %s", ssid);

    err = esp_wifi_connect();
    if (err == ESP_ERR_WIFI_NOT_STARTED) {
        /* The STA_START handler will connect for us. */
        return ESP_OK;
    }
    return err;
}

esp_err_t espix_net_wifi_disconnect(void)
{
    s_want_connect = false;
    s_state        = ESPIX_WIFI_IDLE;
    return esp_wifi_disconnect();
}

esp_err_t espix_net_wifi_scan(espix_ap_t *out, size_t n, size_t *found)
{
    if (out == NULL || found == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *found = 0;

    esp_err_t err = driver_start();
    if (err != ESP_OK) {
        return err;
    }

    /* Blocking scan: the caller is a shell command, and a command that returns
     * before its results exist would be useless. */
    err = esp_wifi_scan_start(NULL, true);
    if (err != ESP_OK) {
        return err;
    }

    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num == 0) {
        return ESP_OK;
    }
    if (num > SCAN_MAX) {
        num = SCAN_MAX;
    }

    wifi_ap_record_t *recs = calloc(num, sizeof(wifi_ap_record_t));
    if (recs == NULL) {
        esp_wifi_clear_ap_list();
        return ESP_ERR_NO_MEM;
    }

    err = esp_wifi_scan_get_ap_records(&num, recs);
    if (err == ESP_OK) {
        for (uint16_t i = 0; i < num && *found < n; i++) {
            espix_ap_t *ap = &out[(*found)++];
            strlcpy(ap->ssid, (const char *)recs[i].ssid, sizeof(ap->ssid));
            memcpy(ap->bssid, recs[i].bssid, sizeof(ap->bssid));
            ap->rssi    = recs[i].rssi;
            ap->channel = recs[i].primary;
            ap->secure  = (recs[i].authmode != WIFI_AUTH_OPEN);
        }
    }

    free(recs);
    return err;
}

esp_err_t espix_net_wifi_status(espix_wifi_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->state   = s_sta ? s_state : ESPIX_WIFI_OFF;
    out->retries = s_retries;

    wifi_ap_record_t ap;
    if (s_state == ESPIX_WIFI_CONNECTED &&
        esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        strlcpy(out->ssid, (const char *)ap.ssid, sizeof(out->ssid));
        memcpy(out->bssid, ap.bssid, sizeof(out->bssid));
        out->rssi    = ap.rssi;
        out->channel = ap.primary;
    } else {
        /* Not associated: report what we are trying to reach. */
        char ssid[ESPIX_SSID_MAX] = {0};
        if (espix_net_conf_get(WIFI_CONF_PATH, "ssid", ssid, sizeof(ssid))) {
            strlcpy(out->ssid, ssid, sizeof(out->ssid));
        }
    }

    return ESP_OK;
}
