/*
 * espix kernel core: version info, uptime, and the kernel log ring (dmesg).
 *
 * This is the bottom of the espix component graph — it depends on no other
 * espix component, so everything else is free to log.
 */
#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESPIX_VERSION_MAJOR 0
#define ESPIX_VERSION_MINOR 1
#define ESPIX_VERSION_PATCH 0

/* Longest absolute path espix will handle. Kept small deliberately: paths get
 * embedded in per-session and per-process structs. */
#define ESPIX_PATH_MAX 128

/*
 * Types shared across the espix component graph live here so that espix_proc
 * and espix_shell can reference each other's objects without a dependency
 * cycle: proc hands processes a session, sessions track a foreground process.
 */
typedef int32_t espix_pid_t;
#define ESPIX_PID_NONE ((espix_pid_t) - 1)

struct espix_session;
typedef struct espix_session espix_session_t;

/* Max characters retained per kernel log line (excluding the NUL). */
#define ESPIX_KLOG_LINE_MAX 120

typedef enum {
    ESPIX_KLOG_ERROR = 0,
    ESPIX_KLOG_WARN,
    ESPIX_KLOG_INFO,
    ESPIX_KLOG_DEBUG,
} espix_klog_level_t;

typedef struct {
    /* esp_log_timestamp() at capture: milliseconds on the same clock ESP-IDF
     * stamps its own log lines with, so dmesg and the console agree. Wraps
     * after ~49 days, which a 96-line ring will never notice. */
    uint32_t ts_ms;
    uint32_t seq;                          /* monotonic, never reused */
    uint8_t  level;                        /* espix_klog_level_t */
    char     text[ESPIX_KLOG_LINE_MAX + 1];
} espix_klog_entry_t;

/*
 * Must be the first espix call in app_main: sets up the kernel log ring and
 * installs the esp_log hook, so ESP-IDF's own boot chatter lands in dmesg too.
 */
void espix_kernel_early_init(void);

/* Boot banner, written to stdout. Called once, before any session exists. */
void espix_kernel_print_banner(void);

const char *espix_version(void);        /* "0.1.0" */
const char *espix_target(void);         /* "esp32s3" */
int64_t espix_uptime_us(void);

/*
 * Fill `buf` with a uname-style string. `all` selects the long form
 * (kernel + version + chip + revision + cores + IDF version).
 */
size_t espix_uname(char *buf, size_t len, bool all);

/* Human-readable uptime, e.g. "up 2 days, 3:14" or "up 41 min". */
size_t espix_uptime_str(char *buf, size_t len);

/*
 * Kernel log. espix_klog() formats; espix_klog_put() takes an already-formatted
 * line. Both store to the ring and (unless CONFIG_ESPIX_KLOG_ECHO_CONSOLE is
 * off) print to the console, so `dmesg` replays what you saw at boot.
 *
 * Neither is safe from panic context — they use stdio. The fault handler uses
 * panic_print_str() and a noinit record instead; see espix_fault.h.
 */
void espix_klog(espix_klog_level_t level, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
void espix_klog_put(espix_klog_level_t level, const char *line);

/* Iterate the ring oldest-first. Return false from `cb` to stop early. */
typedef bool (*espix_klog_iter_fn)(void *ctx, const espix_klog_entry_t *e);
void espix_klog_foreach(espix_klog_iter_fn cb, void *ctx);

/* Number of lines currently retained, and how many were dropped by wraparound. */
size_t   espix_klog_count(void);
uint32_t espix_klog_dropped(void);

#ifdef __cplusplus
}
#endif
