/*
 * System time: the wall clock, and the SNTP client that sets it.
 *
 * espix draws the same line Linux does between two clocks that are not
 * interchangeable:
 *
 *   - CLOCK_REALTIME, reached through time() and gettimeofday(). Calendar time.
 *     Wrong until NTP says otherwise, and it can step -- forward by 56 years on
 *     the first sync of a cold boot.
 *   - CLOCK_MONOTONIC, reached through esp_timer_get_time() and
 *     esp_log_timestamp(). Counts from boot, never steps, means nothing as a
 *     date.
 *
 * Anything measuring a duration wants the second. Anything naming a moment
 * wants the first. espix's own code was entirely monotonic before this
 * component existed and stays that way: uptime, the klog ring, the process
 * table. Only file timestamps and things a human reads use the wall clock.
 *
 * There is no battery-backed RTC on an ESP32, exactly as on a Raspberry Pi, so
 * calendar time has to come off the network. See the note on cold boot below.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Where /etc/timezone and /etc/ntp.conf live, shared with the commands that
 * edit them. */
#define ESPIX_TZ_PATH   "/etc/timezone"
#define ESPIX_NTP_PATH  "/etc/ntp.conf"

/*
 * Load the timezone and arm the SNTP client.
 *
 * Call after the rootfs is mounted (it reads /etc/timezone) and before
 * networking starts (it registers an IP_EVENT handler, and must not miss the
 * first GOT_IP). Takes no boot hold: nothing should wait on the network for a
 * clock.
 */
esp_err_t espix_time_init(void);

/*
 * Whether the clock has been set by NTP or by hand since boot.
 *
 * Before the first sync of a cold boot the clock reads 1970, deliberately: an
 * obviously wrong date is a better signal than a plausible wrong one. Anything
 * that cannot work with a wrong clock -- certificate validity above all --
 * must ask this rather than assume.
 *
 * Note that a soft `reboot` keeps the clock: ESP-IDF holds the boot-time offset
 * in an RTC retention register that survives a restart, and only a power cycle
 * clears it. This flag still reads false after that reboot, because espix has
 * not itself verified the time this boot -- the clock is probably right, but
 * nothing has confirmed it.
 */
bool espix_time_is_synced(void);

/* esp_timer_get_time() at the last successful sync, or 0 if never. Monotonic
 * on purpose: "synced 4 minutes ago" must stay true across a clock step. */
int64_t espix_time_synced_at_us(void);

/* The server actually in use, or NULL before one is chosen. */
const char *espix_time_server(void);

/* Where that server came from: "conf", "dhcp" or "default". */
const char *espix_time_source(void);

/*
 * Set the clock by hand, for `date -s` on a device with no network. Marks the
 * clock synced -- the operator is the authority here.
 */
esp_err_t espix_time_set(time_t t);

/* The active POSIX TZ string, e.g. "UTC" or "SAST-2". Never empty. */
size_t espix_time_zone(char *buf, size_t len);

/*
 * Change the timezone, optionally writing it to /etc/timezone so it survives a
 * reboot -- the file does not exist until then, and the compiled-in default is
 * UTC. `tz` is a POSIX TZ string, not a zoneinfo name; `timedatectl
 * set-timezone` with no argument documents the format.
 */
esp_err_t espix_time_set_zone(const char *tz, bool persist);

#ifdef __cplusplus
}
#endif
