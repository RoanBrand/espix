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

/*
 * The version, generated from version.txt at the repository root by this
 * component's CMakeLists. version.txt is also what ESP-IDF reads for
 * PROJECT_VER, so the string in the SSH banner and the string in the app
 * descriptor (what OTA tooling sees) come from one source and cannot drift.
 *
 * ESPIX_VERSION_STR, ESPIX_VERSION_MAJOR/MINOR/PATCH, ESPIX_BUILD_IS_RELEASE
 * and ESPIX_GIT_DESCRIBE all arrive from there.
 */
#include "espix_version.h"

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

/*
 * Block until every hold has been released, or timeout_ms passes. Zero waits
 * without a limit. True when the count reached zero; the timeout is the
 * backstop the console keeps, not the mechanism.
 */
bool espix_kernel_boot_settled_wait(uint32_t timeout_ms);

const char *espix_version(void);        /* "0.3.0", from version.txt */
const char *espix_target(void);         /* "esp32s3" */
const char *espix_board(void);          /* "s3-n16r8": target + flash + PSRAM */
const char *espix_chip_model(void);     /* "ESP32-S3" */
int64_t espix_uptime_us(void);

/*
 * Whether this build is the tagged, unmodified release. The greeting mentions
 * the build id only when it is not, which is why it has to ask.
 */
bool espix_build_is_release(void);

/*
 * The node name, which is what `uname -n` reports. Linux keeps this in the
 * kernel and so does espix: espix_net owns the source of truth (/etc/hostname)
 * and pushes it here, rather than the kernel reaching into networking.
 */
#define ESPIX_NODENAME_MAX 33   /* 32 + NUL, matching ESPIX_HOSTNAME_MAX */
const char *espix_nodename(void);
void        espix_kernel_set_nodename(const char *name);

/*
 * Which build is running: the first nine hex digits of the image's ELF SHA-256.
 * A content identity rather than a version, and the only thing that can tell you
 * the board is not running the tree in front of you -- `make test` builds and
 * runs without flashing, so hours can go into an image from a tree that no
 * longer exists. tests/run.sh compares this against build/espix.bin and refuses
 * to start when they differ.
 */
const char *espix_build_id(void);

/*
 * Fill `buf` with a uname-style string, like uname(1). `flags` is the set of
 * option letters in effect without the dash: "a" is the long form, an empty
 * string is the default (`-s`), and letters combine ("snrvm") the way the real
 * command accepts them.
 */
size_t espix_uname(char *buf, size_t len, const char *flags);

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
/* Allocate the ring (PSRAM). Must run before the first espix_klog(); called by
 * espix_kernel_early_init(). */
void espix_klog_init(void);

void espix_klog(espix_klog_level_t level, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
void espix_klog_put(espix_klog_level_t level, const char *line);

/*
 * Start the console flusher: after this, log calls only format and queue, and
 * this task writes to the console. Called once the scheduler is running; before
 * it, klog echoes inline. See the note in klog.c.
 */
void espix_klog_start_flusher(void);

/* Iterate the ring oldest-first. Return false from `cb` to stop early. */
typedef bool (*espix_klog_iter_fn)(void *ctx, const espix_klog_entry_t *e);
void espix_klog_foreach(espix_klog_iter_fn cb, void *ctx);

/* Number of lines currently retained, and how many were dropped by wraparound. */
size_t   espix_klog_count(void);

/*
 * The log as a stream: everything newer than *seq, at most n entries, with *seq
 * left past what was handed over. *lost counts lines the ring dropped before
 * the reader got to them, so falling behind is reported rather than silent.
 * espix_klog_next_seq() is where a reader that wants only new lines starts.
 */
size_t   espix_klog_since(uint32_t *seq, espix_klog_entry_t *out, size_t n,
                          uint32_t *lost);
uint32_t espix_klog_next_seq(void);
uint32_t espix_klog_dropped(void);

/*
 * DEBUG lines dropped because the ring was full of higher-level lines. The ring
 * is one buffer for every level, so a chatty DEBUG source would otherwise evict
 * the INFO/WARN/ERROR lines that explain a boot; this is how many were given up
 * to avoid that.
 */
uint32_t espix_klog_dropped_debug(void);

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
 * Block until the klog flusher has caught up with the ring, or timeout_ms
 * passes. True when there is nothing left to echo -- and when there is no ring
 * or no flusher, which is not a wait at all.
 */
bool espix_klog_drained_wait(uint32_t timeout_ms);

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

/* ------------------------------------------------------------------ */
/* Shutdown                                                            */
/* ------------------------------------------------------------------ */

/*
 * What espix does before it stops being espix.
 *
 * A reset here is total -- tasks, sockets, drivers and RAM all go with it -- so
 * a Linux-sized shutdown would be mostly ceremony. The three things that do
 * survive are what this sequence is for:
 *
 *   - storage that stays powered across the reset (a USB volume), which has to
 *     be flushed and unmounted or its FAT is left dirty;
 *   - the peers on the network, which should be refused rather than half-served;
 *   - the hardware an app switched on, which a reset does not switch off.
 *
 * Hence the phases, which are the order and not a ranking: each is what the one
 * after it depends on. A subsystem registers what it has to do rather than being
 * called by name, because this is the bottom of the component graph and must not
 * learn what ssh or nfsd is. espix_main.c does the wiring.
 */
typedef enum {
    /* Stop supervised units, and anything serving that a unit started. */
    ESPIX_SHUTDOWN_UNITS = 0,
    /* SIGTERM every process and wait for the table to empty. */
    ESPIX_SHUTDOWN_APPS,
    /* Flush and unmount every volume. */
    ESPIX_SHUTDOWN_STORAGE,
    /* Park what a reset would otherwise leave running: radios, audio, display. */
    ESPIX_SHUTDOWN_HARDWARE,
    ESPIX_SHUTDOWN_PHASE_COUNT,
} espix_shutdown_phase_t;

/*
 * How long the whole sequence may spend waiting for things to stop. Long enough
 * for an app to put its own hardware back, short enough that nobody concludes
 * the board has hung. A phase that finishes early does not use it: every wait in
 * here ends when the last thing it is waiting for does.
 */
#define ESPIX_SHUTDOWN_GRACE_US (5LL * 1000000)

/* How long a power-off stays asleep by default, before the timer wakes it. */
#define ESPIX_POWEROFF_MINUTES 10

/*
 * What a handler is given: an absolute esp_timer value, shared by the whole
 * sequence, so a handler that waits gives up when the system's budget is gone
 * rather than when its own is.
 */
typedef void (*espix_shutdown_fn)(int64_t deadline_us);

/*
 * What is left of the sequence's budget, in milliseconds, for a handler that has
 * a blocking call to make with a timeout.
 *
 * Never zero: zero means "no limit" to the like of espix_task_exit_wait(), which
 * is the opposite of what a handler past its deadline wants. Once the budget is
 * gone this returns 1, so the call still comes back.
 */
uint32_t espix_shutdown_remaining_ms(int64_t deadline_us);

/*
 * Register one, in the phase it belongs to. Handlers run in the order they were
 * added, and the name is for the log.
 *
 * ESP_ERR_INVALID_STATE once a shutdown has begun -- a table filled after the
 * fact would be a phase that silently did not run -- and ESP_ERR_NO_MEM when
 * that phase's table is full.
 */
esp_err_t espix_shutdown_add(espix_shutdown_phase_t phase, espix_shutdown_fn fn,
                             const char *name);

/*
 * Whether a shutdown is under way.
 *
 * This is what makes "no new connections" real rather than a race: an accept
 * loop asks it and refuses, the service supervisor asks it and starts nothing,
 * espix_proc_spawn_elf() asks it and loads nothing. It is set before the first
 * handler runs, so the moment between the request and the sequence is not a hole
 * for something new to arrive through.
 */
bool espix_shutdown_started(void);

/*
 * Reboot, or power off, after the sequence. Each returns once the sequence is
 * under way -- the work is on its own task -- and whether to wait is the
 * caller's:
 *
 * A command parks itself afterwards, because a prompt that comes back while the
 * system is leaving is a lie. The desktop does not, and must not: a pointer
 * event is dispatched in the task serving the screen, and that is one of the
 * tasks the sequence stops, so parking it here would leave the sequence waiting
 * for a task that is waiting for the sequence.
 *
 * Power off is deep sleep, because the ESP32-S3 cannot cut its own power: the
 * chip drops to microamps and everything but the RTC domain goes. The minutes
 * argument is how long to stay there before the RTC timer wakes it into an
 * ordinary boot. Zero is the test value -- a second, which exercises the sleep
 * path without a ten-minute wait. There is deliberately no "stay off": a device
 * nobody remembers to power-cycle is a device that is gone.
 */
void espix_shutdown_reboot(void);
void espix_shutdown_poweroff(unsigned minutes);

#ifdef __cplusplus
}
#endif
