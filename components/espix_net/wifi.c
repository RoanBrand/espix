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

#include "espix_fs.h"
#include "espix_kernel.h"
#include "espix_net.h"
#include "espix_net_priv.h"

#define TAG "wifi"

#define WIFI_CONF_PATH  "/etc/wifi.conf"
#define SCAN_MAX        24

/*
 * Retry policy.
 *
 * A flat interval retried forever is wrong in both directions: it splats the
 * shell prompt every few seconds indefinitely and churns the klog ring so boot
 * history is lost, while still never succeeding against a bad password.
 *
 * So back off — 5s, 10s, 20s, 40s, then 60s forever — and treat two classes of
 * failure differently, which the reason code tells us apart:
 *
 *  - Credentials rejected. More attempts cannot succeed, so give up after a few
 *    and say so. Not on the first: APs do emit spurious handshake timeouts.
 *  - Anything else (AP out of range, powered off, rebooting). Keep trying at the
 *    ceiling forever. A headless board must come back on its own when the router
 *    returns; silently staying offline after a router reboot would be worse than
 *    the noise this replaces.
 */
#define RETRY_DELAY_MIN_MS   5000
#define RETRY_DELAY_MAX_MS  60000
#define RETRY_AUTH_GIVE_UP      4   /* consecutive credential rejections */

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

/* Retry state, all touched only from the event loop and the shell command. */
static unsigned s_retry_delay_ms = RETRY_DELAY_MIN_MS;
static unsigned s_auth_failures;
static bool     s_gave_up;
static int      s_last_reason;
static char     s_ssid[ESPIX_SSID_MAX];

/*
 * Did the AP reject who we are, as opposed to simply not being there?
 *
 * Only the first class is hopeless. NO_AP_FOUND and the various threshold
 * variants mean "not visible right now", which a router reboot or moving the
 * board can fix, so those must keep retrying.
 */
static bool reason_is_credential(int reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
    case WIFI_REASON_IE_IN_4WAY_DIFFERS:
    case WIFI_REASON_INVALID_PMKID:
        return true;
    default:
        return false;
    }
}

/* Fresh attempt: forget the backoff and the give-up decision. */
static void reset_retry_state(void)
{
    s_retry_delay_ms = RETRY_DELAY_MIN_MS;
    s_auth_failures  = 0;
    s_gave_up        = false;
    s_retries        = 0;
}

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

/*
 * The file holds the PSK in plaintext, so it is root's alone to read.
 *
 * Not a nicety: the rule in espix_fs gives an unstamped file 0644, and that
 * default outlived the arrival of permissions -- until this, every account on
 * the device could read the network's password. Discretionary permissions
 * start out readable and are locked by someone remembering to; this is the
 * remembering.
 *
 * Called after every write and again on the boot read, because a device
 * flashed before this existed carries no stored mode and would otherwise stay
 * 0644 forever. espix_fs_ensure_mode() is what makes the boot call free once
 * it has been done once.
 */
static void secure_conf(void)
{
    (void)espix_fs_ensure_mode(WIFI_CONF_PATH, 0600);
}

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

    secure_conf();
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
        s_state = ESPIX_WIFI_CONNECTED;
        /* Success clears the backoff, so a link that flaps once does not carry
         * a minute-long delay into its next outage. */
        reset_retry_state();
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

        if (!s_want_connect) {
            espix_klog(ESPIX_KLOG_INFO, TAG, "wlan0: disconnected");
            break;
        }

        s_retries++;
        s_last_reason = ev->reason;

        if (reason_is_credential(ev->reason)) {
            s_auth_failures++;
            if (s_auth_failures >= RETRY_AUTH_GIVE_UP) {
                /* Retrying cannot help. Stop, and say what to do about it. */
                s_want_connect  = false;
                s_gave_up       = true;
                s_state         = ESPIX_WIFI_IDLE;
                s_retry_delay_ms = 0;
                espix_klog(ESPIX_KLOG_ERROR, TAG,
                           "wlan0: %s rejected our credentials %u times "
                           "(reason %d); giving up",
                           s_ssid, s_auth_failures, ev->reason);
                espix_klog(ESPIX_KLOG_ERROR, TAG,
                           "wlan0: fix /etc/wifi.conf and run 'wifi connect'");
                break;
            }
        } else {
            /* Transient: an AP that is absent now may return. */
            s_auth_failures = 0;
        }

        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "wlan0: disconnected (reason %d), retry %u in %u ms",
                   ev->reason, s_retries, s_retry_delay_ms);

        /*
         * Delayed via a timer, NOT vTaskDelay(): this runs on the default event
         * loop task, so sleeping here would stall every other event — including
         * the IP events we depend on — and let the event queue back up.
         */
        esp_timer_start_once(s_retry_timer, s_retry_delay_ms * 1000ULL);

        /* Double for next time, to the ceiling. */
        if (s_retry_delay_ms < RETRY_DELAY_MAX_MS) {
            s_retry_delay_ms *= 2;
            if (s_retry_delay_ms > RETRY_DELAY_MAX_MS) {
                s_retry_delay_ms = RETRY_DELAY_MAX_MS;
            }
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

    /* Before the first read, so a device that predates this gets its mode
     * corrected whether or not it ever runs `wifi connect` again. */
    secure_conf();

    const bool have_ssid =
        espix_fs_conf_get(WIFI_CONF_PATH, "ssid", ssid, sizeof(ssid)) &&
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

    espix_fs_conf_get(WIFI_CONF_PATH, "psk", psk, sizeof(psk));

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
        if (!espix_fs_conf_get(WIFI_CONF_PATH, "ssid",
                                file_ssid, sizeof(file_ssid))) {
            return ESP_ERR_NOT_FOUND;
        }
        espix_fs_conf_get(WIFI_CONF_PATH, "psk", file_psk, sizeof(file_psk));
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
    s_state        = ESPIX_WIFI_CONNECTING;
    strlcpy(s_ssid, ssid, sizeof(s_ssid));

    /* An explicit connect is a fresh start, including after giving up. */
    reset_retry_state();

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
    out->state       = s_sta ? s_state : ESPIX_WIFI_OFF;
    out->retries     = s_retries;
    out->gave_up     = s_gave_up;
    out->last_reason = s_last_reason;
    out->retry_delay_ms =
        (s_want_connect && s_state != ESPIX_WIFI_CONNECTED) ? s_retry_delay_ms : 0;

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
        if (espix_fs_conf_get(WIFI_CONF_PATH, "ssid", ssid, sizeof(ssid))) {
            strlcpy(out->ssid, ssid, sizeof(out->ssid));
        }
    }

    return ESP_OK;
}
