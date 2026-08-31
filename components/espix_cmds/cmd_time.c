/*
 * `date` and `timedatectl`.
 *
 * The split is the one Linux has, and it is worth keeping for the same reason:
 * `date` answers "what time is it", `timedatectl` answers "and can I believe
 * it". On a device with no battery-backed clock the second question is the one
 * that actually matters, because the answer is "no" for the first stretch of
 * every cold boot.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_timer.h"

#include "espix_cmds_priv.h"
#include "espix_kernel.h"
#include "espix_time.h"

/*
 * Anything earlier than this means the clock was never set: the epoch, or a few
 * seconds past it. Used only to choose which explanation to print, so the exact
 * boundary does not matter -- no real device clock lands in the 20th century.
 */
#define YEAR_2000   946684800L

/* Long enough for a full RFC-ish stamp with a zone name. */
#define STAMP_MAX   80

/*
 * POSIX defines date's default output as "%a %b %e %H:%M:%S %Z %Y", which is
 * what `LC_ALL=C date` prints on Linux:
 *
 *     Mon Aug 31 11:49:16 SAST 2026
 *
 * Matched exactly rather than approximately, so espix's output can be diffed
 * against a real system's instead of merely resembling it. A distribution's own
 * `date` may look different -- Raspberry Pi OS renders 12-hour with %r and an
 * AM/PM marker -- but that is its locale talking, and espix has no locales, so
 * the C locale is the thing to match.
 *
 * %e, not %d: POSIX space-pads a single-digit day ("Sep  1", two spaces).
 */
#define DATE_FMT     "%a %b %e %H:%M:%S %Z %Y"

/*
 * The -u form spells UTC out rather than using %Z, because newlib's %Z reads
 * the global tzname that tzset() left behind and not the struct tm it was
 * handed -- so gmtime_r() output formatted with %Z claims the local zone.
 * Measured: `date -u` under TZ=SAST-2 printed the right hour labelled "SAST".
 */
#define DATE_FMT_UTC "%a %b %e %H:%M:%S UTC %Y"

/* ------------------------------------------------------------------ */

static void render(char *out, size_t len, time_t t, bool utc, const char *fmt)
{
    struct tm tm;

    if (utc) {
        gmtime_r(&t, &tm);
    } else {
        localtime_r(&t, &tm);
    }

    if (strftime(out, len, fmt, &tm) == 0) {
        /* strftime returns 0 both for "did not fit" and for a format that
         * legitimately produced nothing, so say which is more likely rather
         * than leaving a blank line. */
        strlcpy(out, "(format too long)", len);
    }
}

/*
 * Parse "YYYY-MM-DD HH:MM:SS", and that shape only.
 *
 * GNU date accepts most of English here; espix accepts one unambiguous format
 * because the alternative is a parser larger than the rest of this file, for a
 * command whose whole purpose is the rare case of setting a clock by hand on a
 * device with no network.
 */
static bool parse_stamp(const char *s, time_t *out)
{
    struct tm tm = { 0 };
    int y, mo, d, h, mi, sec;

    if (sscanf(s, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &sec) != 6) {
        return false;
    }

    tm.tm_year  = y - 1900;
    tm.tm_mon   = mo - 1;
    tm.tm_mday  = d;
    tm.tm_hour  = h;
    tm.tm_min   = mi;
    tm.tm_sec   = sec;
    tm.tm_isdst = -1;           /* let mktime work out DST from the zone */

    const time_t t = mktime(&tm);
    if (t == (time_t)-1) {
        return false;
    }

    *out = t;
    return true;
}

static int cmd_date(espix_session_t *s, int argc, char **argv)
{
    bool        utc = false;
    const char *fmt = NULL;     /* NULL = the default for whichever zone */

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-u") == 0) {
            utc = true;
        } else if (strcmp(argv[i], "-s") == 0) {
            if (i + 1 >= argc) {
                espix_printf(s, "date: -s needs a time, "
                                "as \"YYYY-MM-DD HH:MM:SS\"\n");
                return 1;
            }

            time_t t;
            if (!parse_stamp(argv[i + 1], &t)) {
                espix_printf(s, "date: cannot parse '%s'; "
                                "expected \"YYYY-MM-DD HH:MM:SS\"\n",
                             argv[i + 1]);
                return 1;
            }
            if (espix_time_set(t) != ESP_OK) {
                espix_printf(s, "date: could not set the clock\n");
                return 1;
            }
            i++;
        } else if (argv[i][0] == '+') {
            fmt = argv[i] + 1;
        } else {
            espix_printf(s, "date: unknown option '%s'\n", argv[i]);
            return 1;
        }
    }

    if (fmt == NULL) {
        fmt = utc ? DATE_FMT_UTC : DATE_FMT;
    }

    char   out[STAMP_MAX];
    time_t now = time(NULL);

    render(out, sizeof(out), now, utc, fmt);
    espix_printf(s, "%s\n", out);
    return 0;
}

/* ------------------------------------------------------------------ */

/* "4 min ago" / "2 h 11 min ago", from a monotonic timestamp so that a clock
 * step cannot make it negative. */
static void ago_str(char *out, size_t len, int64_t then_us)
{
    const int64_t secs = (esp_timer_get_time() - then_us) / 1000000;

    if (secs < 60) {
        snprintf(out, len, "%lds ago", (long)secs);
    } else if (secs < 3600) {
        snprintf(out, len, "%ldmin ago", (long)(secs / 60));
    } else {
        snprintf(out, len, "%ldh %ldmin ago",
                 (long)(secs / 3600), (long)((secs % 3600) / 60));
    }
}

static int cmd_timedatectl(espix_session_t *s, int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "set-timezone") == 0) {
        if (argc < 3) {
            espix_printf(s, "timedatectl: set-timezone needs a POSIX TZ "
                            "string, e.g. SAST-2 or EST5EDT,M3.2.0,M11.1.0\n");
            return 1;
        }
        if (espix_time_set_zone(argv[2], true) != ESP_OK) {
            espix_printf(s, "timedatectl: could not set the timezone\n");
            return 1;
        }
        return 0;
    }
    if (argc > 1) {
        espix_printf(s, "timedatectl: unknown argument '%s'\n", argv[1]);
        return 1;
    }

    const time_t now = time(NULL);
    char         local[STAMP_MAX], utc[STAMP_MAX], tz[64];

    render(local, sizeof(local), now, false, "%a %Y-%m-%d %H:%M:%S %Z");
    render(utc,   sizeof(utc),   now, true,  "%a %Y-%m-%d %H:%M:%S UTC");
    espix_time_zone(tz, sizeof(tz));

    espix_printf(s, "               Local time: %s\n", local);
    espix_printf(s, "           Universal time: %s\n", utc);
    espix_printf(s, "                Time zone: %s\n", tz);

    if (espix_time_is_synced()) {
        char ago[32];
        ago_str(ago, sizeof(ago), espix_time_synced_at_us());
        espix_printf(s, "System clock synchronized: yes (%s)\n", ago);
    } else if (now < YEAR_2000) {
        /*
         * Say what it means rather than only that it is false. A reader seeing
         * a 1970 date above needs to know it is expected and self-correcting,
         * not that something is broken.
         */
        espix_printf(s, "System clock synchronized: no "
                        "(at the epoch until NTP answers)\n");
    } else {
        /*
         * Unsynced but plausible, which is the case after a reboot: ESP-IDF
         * keeps the clock in an RTC register across a restart, so the time is
         * very likely right -- espix has simply not confirmed it this boot.
         * Saying "at the epoch" here would contradict the date printed three
         * lines above.
         */
        espix_printf(s, "System clock synchronized: no "
                        "(carried across a reboot, unverified)\n");
    }

    const char *server = espix_time_server();
    if (server != NULL) {
        espix_printf(s, "               NTP server: %s (%s)\n",
                     server, espix_time_source());
    } else {
        espix_printf(s, "               NTP server: none yet\n");
    }

    return 0;
}

/* ------------------------------------------------------------------ */

static espix_cmd_t s_time_cmds[] = {
    { .name = "date", .fn = cmd_date,
      .help  = "print or set the system time",
      .usage = "date [-u] [+FORMAT] [-s \"YYYY-MM-DD HH:MM:SS\"]" },
    { .name = "timedatectl", .fn = cmd_timedatectl,
      .help  = "show time, timezone and NTP status",
      .usage = "timedatectl [set-timezone <TZ>]" },
};

void espix_cmds_register_time(void)
{
    espix_cmds_register_table(s_time_cmds,
                              sizeof(s_time_cmds) / sizeof(s_time_cmds[0]));
}
