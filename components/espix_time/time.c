/*
 * The wall clock: timezone, and the SNTP client that sets the time.
 *
 * Structured around one fact about the hardware: an ESP32 has no
 * battery-backed RTC, so on a cold boot the calendar starts at the epoch and
 * stays there until the network provides a real answer. That is the same
 * position a Raspberry Pi is in, and espix takes the same shape of solution --
 * ask the network as soon as an interface has an address -- without the
 * fake-hwclock trick of persisting a guess to disk. An obviously wrong 1970 is
 * a better signal to a reader than a plausible wrong date.
 *
 * What espix does *not* have to do is keep the clock across a reboot. ESP-IDF
 * stores the boot-time offset in an RTC retention register
 * (RTC_BOOT_TIME_LOW_REG), which survives esp_restart() and deep sleep and is
 * cleared only by a power cycle. So `reboot` on a synced device comes back with
 * the right time already.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_event.h"
#include "esp_netif_sntp.h"
#include "esp_netif_types.h"
#include "esp_sntp.h"
#include "esp_timer.h"

#include "espix_fs.h"
#include "espix_kernel.h"
#include "espix_time.h"

#define TAG "time"

/* Used when /etc/ntp.conf names nothing and DHCP offers nothing. The pool is
 * what every Linux distribution falls back to for the same reason: it is the
 * only server that is correct to hardcode. */
#define NTP_FALLBACK    "pool.ntp.org"

/*
 * "UTC0", not "UTC": POSIX makes the offset mandatory in a TZ string, and
 * newlib rejects the bare name -- silently, leaving strftime's %Z empty rather
 * than complaining. Measured: TZ=UTC prints no zone, TZ=UTC0 prints "UTC".
 */
#define TZ_DEFAULT      "UTC0"
#define TZ_MAX          64
#define SERVER_MAX      64

static bool     s_inited;
static bool     s_started;              /* SNTP running */
static bool     s_synced;
static int64_t  s_synced_at_us;
static char     s_tz[TZ_MAX]     = TZ_DEFAULT;
static char     s_server[SERVER_MAX];   /* configured explicitly, else empty */
static bool     s_from_conf;

/* ------------------------------------------------------------------ */
/* Timezone                                                            */
/* ------------------------------------------------------------------ */

/*
 * newlib reads the zone out of the TZ environment variable, so "setting the
 * timezone" is setenv() plus tzset(); everything downstream -- localtime(),
 * mktime(), strftime()'s %Z -- follows from there.
 *
 * A POSIX TZ string, not a zoneinfo name: Linux would put "Africa/Johannesburg"
 * in /etc/timezone and keep the rules in /etc/localtime, but espix ships no
 * tzdata -- the database is megabytes -- so the rules live in the string
 * itself. `timedatectl set-timezone` documents the format; see cmd_time.c.
 *
 * The obvious question is why this is static configuration when the clock
 * beside it comes off the network: surely the sync could bring the zone too?
 * It cannot. NTP carries no timezone -- struct sntp_msg is a stratum, a poll
 * interval, a precision, root delay and dispersion, a reference identifier and
 * four timestamps, and every one of those timestamps is UTC. There has never
 * been a field for it.
 *
 * Linux is in the same position and answers it the same way: the zone is
 * written once at install time (the installer asks; on a Raspberry Pi the
 * first-boot wizard or raspi-config does), and systemd-timesyncd only ever sets
 * the clock. Where a machine *does* pick a zone up automatically it is from
 * geolocation or a cellular network, not from time service. The only wire
 * protocol that carries one is DHCP, options 100 and 101 of RFC 4833 -- which
 * lwIP does not parse (its dhcp.h knows 42 and 58 and no more) and which
 * essentially no router serves.
 *
 * So espix behaves as Linux does. What it does not have is an installer to ask
 * the question, which is why the shipped default is UTC and `timedatectl
 * set-timezone` is how you answer it.
 */
static void apply_tz(const char *tz)
{
    setenv("TZ", tz, 1);
    tzset();
}

size_t espix_time_zone(char *buf, size_t len)
{
    return strlcpy(buf, s_tz, len);
}

esp_err_t espix_time_set_zone(const char *tz, bool persist)
{
    if (tz == NULL || tz[0] == '\0' || strlen(tz) >= sizeof(s_tz)) {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(s_tz, tz, sizeof(s_tz));
    apply_tz(s_tz);

    if (!persist) {
        return ESP_OK;
    }

    FILE *f = fopen(ESPIX_TZ_PATH, "w");
    if (f == NULL) {
        return ESP_FAIL;
    }
    fprintf(f, "%s\n", s_tz);
    fclose(f);

    return ESP_OK;
}

/*
 * /etc/timezone is one bare line, unlike the key=value files espix uses
 * elsewhere, because that is the shape the Linux file of the same name has and
 * a one-value file gains nothing from a key.
 */
static void load_tz(void)
{
    FILE *f = fopen(ESPIX_TZ_PATH, "r");
    if (f == NULL) {
        apply_tz(s_tz);         /* the TZ_DEFAULT the static holds */
        return;
    }

    /* First line that is neither blank nor a comment. The shipped file leads
     * with a dozen lines explaining the POSIX format, so reading only line one
     * would find a '#' and silently keep the default. */
    char line[TZ_MAX];
    while (fgets(line, sizeof(line), f) != NULL) {
        /*
         * A line longer than the buffer comes back in pieces, and the pieces
         * after the first do not start with '#' -- so a long comment would
         * otherwise be accepted as a timezone. Observed, not theorised: the
         * shipped file's own explanatory comment is 76 characters and set the
         * zone to `hannesburg" in`.
         *
         * Discard the remainder and skip the line. Nothing this long is a
         * valid TZ string anyway, so there is no case where dropping it loses
         * a real setting.
         */
        if (strchr(line, '\n') == NULL && !feof(f)) {
            int c;
            while ((c = fgetc(f)) != EOF && c != '\n') {
            }
            continue;
        }

        line[strcspn(line, "\r\n")] = '\0';

        char *p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0' || *p == '#') {
            continue;
        }

        strlcpy(s_tz, p, sizeof(s_tz));
        break;
    }
    fclose(f);

    apply_tz(s_tz);
}

/* ------------------------------------------------------------------ */
/* Sync state                                                          */
/* ------------------------------------------------------------------ */

bool espix_time_is_synced(void)
{
    return s_synced;
}

int64_t espix_time_synced_at_us(void)
{
    return s_synced_at_us;
}

const char *espix_time_server(void)
{
    if (s_from_conf) {
        return s_server;
    }

    /* Index 0 is where DHCP's answer lands, overwriting the fallback that was
     * put there at init; the fallback is re-registered at index 1 when the
     * lease arrives. So index 0 is always the server actually preferred. */
    const char *name = esp_sntp_getservername(0);
    return (name != NULL && name[0] != '\0') ? name : NULL;
}

const char *espix_time_source(void)
{
    if (s_from_conf) {
        return "conf";
    }
    if (!s_started) {
        return "none";
    }

    /* DHCP having supplied one shows up as index 0 no longer holding the
     * fallback we put there. */
    const char *name = esp_sntp_getservername(0);
    if (name != NULL && strcmp(name, NTP_FALLBACK) != 0) {
        return "dhcp";
    }
    return "default";
}

esp_err_t espix_time_set(time_t t)
{
    struct timeval tv = { .tv_sec = t, .tv_usec = 0 };

    if (settimeofday(&tv, NULL) != 0) {
        return ESP_FAIL;
    }

    s_synced       = true;
    s_synced_at_us = esp_timer_get_time();
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* SNTP                                                                */
/* ------------------------------------------------------------------ */

static void on_sync(struct timeval *tv)
{
    (void)tv;

    const bool first = !s_synced;

    s_synced       = true;
    s_synced_at_us = esp_timer_get_time();

    /*
     * The first sync of a boot is news -- it is the moment every timestamp on
     * the device becomes meaningful. The hourly ones after it are not, so they
     * go to the ring at DEBUG rather than onto the console.
     */
    char       when[40];
    time_t     now = time(NULL);
    struct tm  tm;
    localtime_r(&now, &tm);
    strftime(when, sizeof(when), "%a %Y-%m-%d %H:%M:%S %Z", &tm);

    const char *server = espix_time_server();

    espix_klog(first ? ESPIX_KLOG_INFO : ESPIX_KLOG_DEBUG, TAG,
               "clock set from %s (%s): %s",
               server != NULL ? server : "?", espix_time_source(), when);
}

/*
 * Start on the first address on any interface.
 *
 * IP_EVENT rather than a call from espix_net: IP_EVENT_ETH_GOT_IP and
 * IP_EVENT_STA_GOT_IP both arrive here, so Ethernet works the day it is added
 * and this component needs no dependency on espix_net. GOT_IP is also the right
 * moment rather than merely the convenient one -- it means an address, a
 * gateway and a DHCP lease, which is what the DHCP-supplied server needs.
 */
static void on_ip_event(void *arg, esp_event_base_t base,
                        int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;

    if (id != IP_EVENT_STA_GOT_IP && id != IP_EVENT_ETH_GOT_IP) {
        return;
    }
    if (s_started) {
        return;             /* a reconnect; the client keeps polling */
    }

    if (esp_netif_sntp_start() != ESP_OK) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "could not start the SNTP client");
        return;
    }

    s_started = true;
    espix_klog(ESPIX_KLOG_DEBUG, TAG, "SNTP started");
}

esp_err_t espix_time_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    load_tz();

    /*
     * The default event loop, which esp_netif_sntp_init() registers an IP
     * handler on and which espix_net creates for its own use. Whichever of the
     * two initialises first creates it, so both treat "already exists" as
     * success -- otherwise this component's position in the boot order would
     * silently decide whether it works, which is how the first version of this
     * failed: SNTP init returned ESP_ERR_INVALID_STATE and the clock never
     * left 1970.
     */
    const esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "no event loop: %s",
                   esp_err_to_name(loop_err));
        return loop_err;
    }

    s_from_conf = espix_fs_conf_get(ESPIX_NTP_PATH, "server",
                                    s_server, sizeof(s_server));

    esp_sntp_config_t cfg = {
        /*
         * Step, never slew.
         *
         * The smooth mode looks right on paper -- adjtime() for small errors,
         * a step for anything over 35 minutes -- and it does not work from the
         * epoch. ESP-IDF's adjtime() computes `delta->tv_sec * 1000000L` in a
         * 32-bit long, so a 56-year correction (1.798e15 us) overflows to
         * about 2.14e9 us, which is ~35 minutes: exactly the threshold that
         * was supposed to reject it. adjtime() accepts the wrapped value and
         * returns success, sntp_sync_time() therefore never reaches its
         * settimeofday() fallback, and the clock slews a fictional 35-minute
         * error forever while reporting that it synced.
         *
         * Measured, not deduced: the sync callback fired, dmesg said "clock
         * set", and `date` still read 1970.
         *
         * Stepping costs espix nothing anyway. Everything of its own that
         * measures a duration -- uptime, the klog ring, the process table --
         * reads the monotonic clock, so nothing here can observe time going
         * backwards.
         */
        .smooth_sync = false,

        /* Started by hand from the IP event below, not at init: there is no
         * network yet, and DHCP has not offered a server. */
        .start         = false,
        .wait_for_sync = false,     /* nothing waits on a clock */
        .sync_cb       = on_sync,

        /*
         * With no explicit config, take whatever DHCP offers and keep the pool
         * as a second server. index_of_first_server is applied on renew rather
         * than at init, which is exactly the behaviour wanted: the fallback
         * sits at index 0 until a lease arrives, DHCP's answer overwrites it,
         * and the renew puts the fallback back at index 1.
         */
        .server_from_dhcp           = !s_from_conf,
        .renew_servers_after_new_IP = !s_from_conf,
        .index_of_first_server      = s_from_conf ? 0 : 1,
        .num_of_servers             = 1,
        .servers                    = { s_from_conf ? s_server : NTP_FALLBACK },
    };

    const esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "SNTP init failed: %s",
                   esp_err_to_name(err));
        return err;
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, ESP_EVENT_ANY_ID, on_ip_event, NULL, NULL));

    s_inited = true;
    espix_klog(ESPIX_KLOG_DEBUG, TAG, "zone %s, ntp %s (%s)", s_tz,
               s_from_conf ? s_server : NTP_FALLBACK,
               s_from_conf ? "conf" : "dhcp or default");

    return ESP_OK;
}
