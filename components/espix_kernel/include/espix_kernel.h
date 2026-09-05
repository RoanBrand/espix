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
#define ESPIX_VERSION_MINOR 3
#define ESPIX_VERSION_PATCH 0

/*
 * The same three numbers as a string literal.
 *
 * espix_version() below is the way to ask at runtime, and what most callers
 * want. This exists for the places that have to paste the version *inside* a
 * wider constant at compile time -- the SSH identification banner is one, and
 * it previously carried its own hand-written copy that a version bump would
 * have left stale.
 */
#define ESPIX_STR_(x) #x
#define ESPIX_STR(x)  ESPIX_STR_(x)

#define ESPIX_VERSION_STR   ESPIX_STR(ESPIX_VERSION_MAJOR) "." \
                            ESPIX_STR(ESPIX_VERSION_MINOR) "." \
                            ESPIX_STR(ESPIX_VERSION_PATCH)

/* Longest absolute path espix will handle. Kept small deliberately: paths get
 * embedded in per-session and per-process structs. */
#define ESPIX_PATH_MAX 128

/*
 * How many groups one identity can hold at once: a primary plus seven.
 *
 * Here rather than in espix_auth, which owns the concept, because a session and
 * a process both have to carry the set and neither may depend on that component
 * -- espix_auth depends on espix_fs, which depends on espix_shell. A number
 * three components need is a kernel-wide number.
 *
 * It is a size as much as a limit: credentials are copied rather than looked up
 * per file operation, so this costs 16 bytes a session and 192 across the whole
 * process table.
 */
#define ESPIX_NGROUPS_MAX 8

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

/*
 * Boot barrier.
 *
 * A subsystem whose bring-up continues asynchronously past its init call takes
 * a hold, and releases it once it has settled one way or the other. Its only
 * consumer is the console transport, which uses it to avoid drawing its first
 * prompt into the middle of boot chatter — asynchronous output would overwrite
 * it, and the session task is then blocked inside a line editor that offers no
 * way to redraw.
 *
 * Deliberately a plain count rather than a set of named events: the shell must
 * not need to know what is booting, and eth0/usb0/an SSH listener should slot in
 * without it learning. Holders must guarantee a release on every path, including
 * failure — the console's cap is a backstop, not the mechanism.
 */
void     espix_kernel_boot_hold(void);
void     espix_kernel_boot_release(void);
unsigned espix_kernel_boot_pending(void);

const char *espix_version(void);        /* e.g. "0.2.0" */
const char *espix_target(void);         /* "esp32s3" */
const char *espix_chip_model(void);     /* "ESP32-S3" */
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

/*
 * Timestamp of the most recent line echoed to the console, or 0 if none.
 *
 * Exists so a console session can hold its first prompt until boot chatter has
 * stopped — otherwise the prompt is drawn while asynchronous bring-up is still
 * narrating, and is immediately overwritten. Waiting on log silence rather than
 * on any particular subsystem keeps this self-limiting and subsystem-agnostic.
 */
uint32_t espix_klog_last_echo_ms(void);

/*
 * Told after kernel output reaches the console, so a shell can put its prompt
 * back underneath instead of leaving it buried.
 *
 * Notification only -- the kernel does not ask the terminal owner to draw
 * anything, and deliberately does not hand it the text. Repairing the line from
 * out here would mean guessing where the editor thinks its prompt is, and being
 * wrong about that is how the first attempt at this erased the wrong rows. The
 * shell reacts by restarting its own input line, which is the one operation that
 * leaves the editor's idea of the screen correct.
 *
 * Called *after* the write, not before: a shell acting on it mid-message would
 * redraw its prompt into the middle of the line being printed.
 *
 * The transport supplies this, not the other way round -- espix_kernel is the
 * bottom of the component graph and must not learn what a shell is.
 */
typedef struct {
    void (*output_begin)(void);   /* about to write; clear a prompt if one is up */
    void (*output_done)(void);    /* written; put a prompt back */
} espix_klog_console_hooks_t;

/* `hooks` must outlive the call -- it is held by pointer, not copied. NULL
 * restores plain printing with no notification. */
void espix_klog_set_console_hooks(const espix_klog_console_hooks_t *hooks);

/*
 * How much of the kernel log reaches the console, as `dmesg -n` sets it.
 *
 * Everything is always kept in the ring for `dmesg`; this only decides what is
 * *echoed* while it happens. A message is echoed when its level is at or below
 * this one, so ESPIX_KLOG_ERROR is the quietest useful setting and
 * ESPIX_KLOG_DEBUG puts the lot on the terminal.
 *
 * The default is ESPIX_KLOG_INFO, which is the split Linux draws too: routine
 * per-event chatter should not be on the terminal you are trying to work in.
 * Not persisted -- a boot starts quiet again, deliberately, so a device left in
 * a debugging setting does not stay there.
 */
void               espix_klog_set_console_level(espix_klog_level_t level);
espix_klog_level_t espix_klog_console_level(void);

#ifdef __cplusplus
}
#endif
