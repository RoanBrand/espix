/*
 * espix shell: sessions, the command registry, and dispatch.
 *
 * Everything here is deliberately transport-agnostic. A session owns a way to
 * read a line and a way to write bytes; the console is one implementation, and
 * an SSH channel will be another, without the command layer changing.
 *
 * espix does NOT use esp_console_run() / esp_console_cmd_register(): the
 * console component copies every command line through a single shared static
 * buffer (s_tmp_line_buf in components/console/commands.c), so two concurrent
 * sessions would corrupt each other. We keep our own registry and reuse only
 * esp_console_split_argv(), which is reentrant.
 *
 * Line editing is espressif/esp_linenoise, one instance per session. IDF's own
 * linenoise keeps its history and callbacks in file-scope statics and reads raw
 * descriptors, so it could serve exactly one fd-backed console and never an SSH
 * session, whose bytes arrive inside encrypted packets.
 */
#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"
#include "esp_linenoise.h"

#include "espix_kernel.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESPIX_LINE_MAX 256
#define ESPIX_ARGS_MAX 16
#define ESPIX_SESSION_USER_MAX 17   /* 16 + NUL; matches espix_auth's limit */

/* FreeRTOS TLS slot holding the current session pointer. Index 0 is taken by
 * ESP-IDF's pthread implementation, hence the default of 1. Requires
 * CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS > ESPIX_TLS_SESSION_IDX. */
#define ESPIX_TLS_SESSION_IDX 1

struct espix_session {
    const char *name;                      /* "console", "ssh0", ... */
    char        cwd[ESPIX_PATH_MAX];

    /*
     * Who this session belongs to. Always set: an SSH session carries the
     * authenticated name, the serial console is "root" because physical access
     * to the board is the privilege, and there is nothing to authenticate it
     * against. `whoami`, the prompt and the greeting all read this, so they
     * cannot disagree the way they used to.
     */
    char        user[ESPIX_SESSION_USER_MAX];

    /*
     * The same identity as a number, which is what the filesystem compares
     * against. Resolved once, when the session is set up, rather than looked up
     * per file operation: /etc/passwd is the authority but it is not something
     * to be re-read on the path of every open().
     *
     * uid 0 is the superuser and skips permission checks entirely. The console
     * is 0 for the reason `user` is "root" -- whoever is holding the board has
     * already won -- and an SSH session gets whatever its account carries.
     *
     * A process inherits these from the session that launched it; see
     * espix_proc_cred_of_task(), which is what espix_fs_access_check() asks.
     */
    uint16_t    uid;
    uint16_t    gid;

    /*
     * Every group this identity is in, `gid` included, resolved once at login
     * from /etc/group. The permission check matches its group triad if any of
     * these matches the file's, which is what lets two accounts share access --
     * the thing a gid that always equalled its uid could never express.
     */
    uint16_t    groups[ESPIX_NGROUPS_MAX];
    uint8_t     ngroups;

    /*
     * Home directory, or empty for a session that has none. The prompt shows it
     * as `~`, and a bare `cd` returns to it. Root's is deliberately empty: the
     * console starts at / and showing an absolute path there is honest.
     */
    char        home[ESPIX_PATH_MAX];

    /*
     * Whether a login happened to get here, which is what `logout` requires.
     *
     * Separate from `user` on purpose. That test used to be "is the user field
     * empty", which happened to work only while the console had no name at all
     * -- it was conflating having a user with being a login shell, and naming
     * the console user broke it silently.
     */
    bool        login;

    /*
     * Opens a fresh stdout/stderr stream for a spawned process, or NULL to
     * leave the task's streams alone (which is what the console wants — its
     * stdout is already the right place).
     *
     * An app calls libc printf, which writes to its task's stdout, not through
     * this struct's write(). espix_proc points that task's streams at what this
     * returns, which under newlib rebinds only that task; without it, `run`
     * over SSH would print on the serial console.
     *
     * A factory rather than one shared FILE, because ESP-IDF closes a task's
     * streams when the task is deleted — esp_cleanup_r() in
     * components/esp_libc/src/newlib_init.c fcloses stdin, stdout and stderr
     * whenever they differ from the global ones. A shared stream would be torn
     * down under the still-live session by the first app to exit, and closed
     * twice over if stdout and stderr pointed at the same object. Each stream
     * therefore belongs to one task, and that task's teardown closes it.
     *
     * Not a descriptor, despite SSH having a socket: channel output must be
     * wrapped in CHANNEL_DATA and encrypted, so writing to the raw fd would
     * bypass the protocol. The transport builds these with funopen() over its
     * own write path.
     */
    FILE *(*open_stream)(espix_session_t *s);

    /* Read one line, without the terminator. Returns the length, or a negative
     * value on EOF / transport error. */
    int (*read_line)(espix_session_t *s, const char *prompt,
                     char *buf, size_t len);

    /* Write raw bytes. Returns bytes written, or negative on error. */
    int (*write)(espix_session_t *s, const char *data, size_t len);

    /*
     * Non-blocking: has the user pressed Ctrl-C? Polled while a foreground
     * process runs, which is the one time nothing else is reading input.
     *
     * The transport consumes whatever is waiting either way. Anything typed at
     * a program that is not reading has nowhere to go, and leaving it queued
     * means it lands on the shell's next prompt instead — which is exactly what
     * used to happen: a handful of unanswered Ctrl-Cs arriving as blank lines
     * the moment the app exited.
     *
     * NULL for a transport that cannot poll; the foreground wait then simply
     * cannot be interrupted.
     */
    bool (*poll_interrupt)(espix_session_t *s);

    /*
     * The terminal understands escape sequences. Set by the transport: the
     * console learns it from esp_linenoise_probe(), an SSH session always has a
     * pty in this build. Colour is emitted only when this is set.
     */
    bool        ansi;

    void       *transport;                 /* implementation-owned */
    espix_pid_t fg_pid;                    /* foreground process, or ESPIX_PID_NONE */
    int         last_status;               /* $? */
    bool        want_exit;

    /*
     * Set for the duration of one command when its output was redirected with
     * `>` / `>>`. espix_puts()/espix_printf() honour it; a spawned app's own
     * stdout does not, so `run app > file` still writes to the console.
     */
    FILE       *redirect;
};

/*
 * Command registry. `cmd` must have static storage duration — the registry
 * links the structs together rather than copying them.
 */
typedef int (*espix_cmd_fn)(espix_session_t *s, int argc, char **argv);

typedef struct espix_cmd {
    const char      *name;
    const char      *help;                 /* one-line summary, for `help` */
    const char      *usage;                /* e.g. "rm [-r] <path>..." */
    espix_cmd_fn     fn;
    struct espix_cmd *next;                /* registry-owned; do not set */
} espix_cmd_t;

esp_err_t espix_shell_register(espix_cmd_t *cmd);
const espix_cmd_t *espix_shell_find(const char *name);

/*
 * Per-session command history, newest at index 0. Each transport owns one, so
 * a serial user and a remote user cannot read each other's typing.
 */
/*
 * Input pacing.
 *
 * esp_linenoise — and IDF's linenoise before it — treats bytes arriving less
 * than 30ms apart as a clipboard paste, and inserts them with a raw write
 * instead of a refresh. That write never updates the editor's cursor or its
 * high-water row count, so once a line wraps the terminal moves on while the
 * editor's bookkeeping stands still. The next refresh then clears from the
 * wrong row and redraws below the old copy, which is why typing quickly past
 * the right margin duplicates the line on every keystroke. The same heuristic
 * eats escape sequences, turning a held-down arrow key into "[A" in the buffer.
 *
 * So a transport hands bytes over no faster than that threshold. Only bytes
 * that would otherwise arrive too close together are held; anything typed at
 * human speed already clears it and is passed straight through, so ordinary
 * editing gains no latency. A pasted block is the case that gets slowed, and
 * correctness there is worth more than the milliseconds.
 *
 * 50ms rather than 31 because of tick granularity: at the default 100Hz,
 * pdMS_TO_TICKS(35) is three ticks and vTaskDelay only guarantees the last
 * full one, so the shortest real delay is about 20ms — back under the
 * threshold, which is exactly the trap.
 *
 * The heuristic is really asking "was this byte already waiting?": it stamps
 * the clock before the read and measures how long the read took. So anything
 * that lets a read return instantly turns typing into pasting, which is why
 * the console's window for unpaced cursor reports is bounded by wall-clock
 * time rather than by a count of reads — see tty_console.c.
 */
#define ESPIX_PACE_MS      50
#define ESPIX_PACE_MIN_US  30000

/* Delay if `last_us` is too recent, then stamp it. Shared so both transports
 * pace identically. */
void espix_pace(int64_t *last_us);

#define ESPIX_HISTORY_MAX 16

typedef struct {
    char  *entries[ESPIX_HISTORY_MAX];
    size_t count;
} espix_history_t;

/*
 * The list belonging to `user`, created on first use and kept for the life of
 * the firmware — history follows the user between logins, as it does on Unix,
 * rather than dying with the session. The console passes "": it has no login
 * step, so it is its own principal. Never freed by the caller.
 */
espix_history_t *espix_history_for(const char *user);

/* Record an accepted line, then push the list into the editor. Both are needed
 * after every command; see history.c for why the editor's copy is rebuilt.
 * push() declines to remember a `passwd` command or a line starting with a
 * space. */
void espix_history_push(espix_history_t *h, const char *line);
void espix_history_apply(const espix_history_t *h, esp_linenoise_handle_t ed);
void espix_history_free(espix_history_t *h);

/*
 * Line-editor callbacks, shared by every transport so TAB completion and the
 * usage hint behave the same over serial and SSH. Each session passes these to
 * its own esp_linenoise instance; they read only the registry, so they need no
 * per-session context.
 */
void  espix_shell_completion(const char *buf, void *cb_ctx,
                             esp_linenoise_completion_cb_t cb);
char *espix_shell_hint(const char *buf, int *color, int *bold);

/* Walk the registry in name order. Return false from `cb` to stop. */
typedef bool (*espix_cmd_iter_fn)(void *ctx, const espix_cmd_t *cmd);
void espix_shell_foreach(espix_cmd_iter_fn cb, void *ctx);

/*
 * Called when a command line's first word is not a registered command, so it
 * can be resolved as a program instead. Returns the program's exit status, or
 * ESPIX_SHELL_ENOENT if there is no such program either.
 *
 * A hook rather than a direct call because espix_proc depends on this header
 * for espix_session_t; the shell calling espix_proc_spawn_elf() would close
 * the loop. espix_cmds registers one at boot, since it already sees both.
 */
typedef int (*espix_exec_fallback_fn)(espix_session_t *s, int argc, char **argv);
void espix_shell_set_exec_fallback(espix_exec_fallback_fn fn);

/*
 * Run one command line in the context of `s`. Returns the command's status,
 * or a negative espix status for "not found" / "empty line".
 */
#define ESPIX_SHELL_ENOENT (-127)
#define ESPIX_SHELL_EMPTY  (-1)
int espix_shell_exec(espix_session_t *s, const char *line);

/*
 * espix_shell_exec() plus the reporting a shell does around it: prints
 * "command not found" for a first word that resolves to nothing, and updates
 * `last_status`. Returns the status a caller should report.
 *
 * Use this rather than espix_shell_exec() unless you mean to handle
 * ESPIX_SHELL_ENOENT yourself. The REPL and SSH's exec channel both go through
 * here so the two cannot disagree about what an unknown command looks like.
 */
int espix_shell_run_line(espix_session_t *s, const char *line);

/* Read-eval-print loop for one session. Returns when the session ends. */
void espix_shell_session_run(espix_session_t *s);

/* Per-task current session, so commands running in their own task can still
 * find their stdio and cwd. */
espix_session_t *espix_shell_current(void);
void espix_shell_set_current(espix_session_t *s);

/* Output helpers — commands must use these rather than printf(), or their
 * output goes to the console instead of the session that asked for it. */
int espix_puts(espix_session_t *s, const char *str);
int espix_printf(espix_session_t *s, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/*
 * Console transport (UART or USB-Serial-JTAG, whichever the build selects).
 * Sets up the driver, line endings and the line editor, then runs the session
 * loop on the calling task. Normally never returns.
 */
esp_err_t espix_console_session_start(void);

#ifdef __cplusplus
}
#endif
