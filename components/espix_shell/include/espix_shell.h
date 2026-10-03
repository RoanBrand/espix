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
 * sessions would corrupt each other. We keep our own registry and our own word
 * splitter, which is reentrant and honours both quotes (R-P2.12);
 * esp_console_split_argv() knew only a double quote, and only when one started
 * a word.
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

/*
 * Background jobs one session remembers, for `jobs`, `fg` and `bg`.
 *
 * Two kinds of job share one record. A *process* job is a loaded app: it has a
 * pid, it can be signalled, and `fg`/`bg` resume it with SIGCONT. A *builtin*
 * job is a task of the shell's own. espix can delete a task but not stop and
 * resume one, so it has no pid and nothing for `fg`/`bg` to act on -- what it
 * has instead is a stop flag its blocking loops poll, and one owner for its
 * teardown. See espix_shell_stopping() and espix_shell_jobs_drain().
 */
#define ESPIX_SESSION_JOBS  4
#define ESPIX_JOB_NAME_MAX 24

typedef struct espix_job {
    espix_pid_t   pid;          /* >0 for a process job, 0 for a builtin task */

    /*
     * The builtin task and its context. `task` is a TaskHandle_t, typed void *
     * because this header stays FreeRTOS-free; `ctx` is heap memory holding
     * the task's session copy and its argv. `caps` remembers which allocator
     * the stack came from, so it is deleted the matching way.
     */
    void         *task;
    void         *ctx;
    bool          caps;

    /* Set to ask the task to return. espix_shell_stopping() reads it. */
    volatile bool stop;

    /*
     * The teardown handshake. `finished` says the task has reached its end;
     * `owner` says who frees the context and deletes the task -- 0 while
     * nobody has claimed it, 1 when the task claimed it, 2 when the drain did.
     * One critical section decides, so exactly one of them ever acts.
     */
    volatile bool finished;
    volatile int  owner;

    char          name[ESPIX_JOB_NAME_MAX];
} espix_job_t;

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Session variables. A cap rather than an unbounded table, in the style of
 * ESPIX_NGROUPS_MAX: a shell on a device with 145K of internal RAM should
 * refuse the twenty-fifth variable and say so, not discover the limit as an
 * allocation failure somewhere else.
 */
#define ESPIX_ENV_MAX        24
#define ESPIX_ENV_NAME_MAX   32
#define ESPIX_ENV_VALUE_MAX  192

/* Assignments in front of one command: `A=1 B=2 cmd`. */
#define ESPIX_ENV_SCOPE_MAX  8

typedef struct espix_env espix_env_t;

#define ESPIX_LINE_MAX 256
#define ESPIX_ARGS_MAX 16
#define ESPIX_SESSION_USER_MAX 17   /* 16 + NUL; matches espix_auth's limit */

/* FreeRTOS TLS slot holding the current session pointer. Index 0 is taken by
 * ESP-IDF's pthread implementation, hence the default of 1. Requires
 * CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS > ESPIX_TLS_SESSION_IDX. */
#define ESPIX_TLS_SESSION_IDX 1

/*
 * Which of a process's three streams open_stream() is being asked for.
 *
 * Was a `bool err`, which could not name a third case. Two implementations and
 * one caller, so widening it was cheaper than carrying a second hook.
 */
typedef enum {
    ESPIX_STREAM_IN,
    ESPIX_STREAM_OUT,
    ESPIX_STREAM_ERR,
} espix_stream_t;

struct espix_session {
    const char *name;                      /* "console", "ssh0", ... */
    char        cwd[ESPIX_PATH_MAX];

    /*
     * This login's variables, allocated on first use and owned outright -- no
     * two sessions ever share one. NULL until something is set, so a session
     * that never touches a variable costs nothing. See env.c.
     */
    espix_env_t *env;

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
     * returns, which under newlib rebinds only that task; without it, an app run
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
     *
     * ESPIX_STREAM_IN is the same arrangement in reverse, and comes with one
     * extra rule: the transport, not this stream, owns the queue behind it.
     * Over SSH the connection task is the only reader of the wire, so the
     * stream cannot pull from the socket itself -- see chan_stream_read().
     */
    FILE *(*open_stream)(espix_session_t *s, espix_stream_t which);

    /* Read one line, without the terminator. Returns the length, or a negative
     * value on EOF / transport error. */
    int (*read_line)(espix_session_t *s, const char *prompt,
                     char *buf, size_t len);

    /* Write raw bytes. Returns bytes written, or negative on error. */
    int (*write)(espix_session_t *s, const char *data, size_t len);

    /*
     * The same, for diagnostics. NULL means "no separate error path", and the
     * error path then falls back to write() -- which is correct for the serial
     * console, where there is one descriptor and both streams belong on the
     * terminal, exactly as they do on a real one.
     *
     * SSH sets it, because SSH genuinely has somewhere else to put them:
     * CHANNEL_EXTENDED_DATA, which the client routes to *its* stderr. That is
     * what makes `ssh host cmd 2>/dev/null` behave on the far end.
     */
    int (*write_err)(espix_session_t *s, const char *data, size_t len);

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
     * The terminal's size, for an app that has just been sent SIGWINCH. NULL
     * when the transport has no answer -- the serial console is the case -- and
     * then espix_shell_term_size() fails rather than inventing a size, which is
     * the same choice lsblk makes about a feature it cannot report.
     */
    void (*term_size)(espix_session_t *s, int *cols, int *rows);

    /*
     * A process using this session was force-deleted, and here is its task.
     *
     * The transport is told so it can write off anything the dead task can
     * never hand back. For SSH that is the channel's transmit lock: FreeRTOS
     * lets only the owner release a mutex, so one held at deletion is orphaned
     * for good -- and every later take() dereferences the freed TCB through
     * priority inheritance, which is a use-after-free whether the wait is
     * bounded or not. Knowing *which* task died is what lets the transport
     * check whether it actually held the lock, rather than tearing down a
     * perfectly good session every time something is killed.
     *
     * The handle is opaque here on purpose: this header is transport-agnostic
     * and has no FreeRTOS in it. Called after vTaskDelete(), with no espix_proc
     * lock held. NULL for a transport with nothing a process can hold -- the
     * console has no such lock.
     */
    void (*task_gone)(espix_session_t *s, void *task);

    /*
     * The terminal understands escape sequences. Set by the transport: the
     * console learns it from esp_linenoise_probe(), an SSH session always has a
     * pty in this build. Colour is emitted only when this is set.
     */
    bool        ansi;

    void       *transport;                 /* implementation-owned */
    espix_pid_t fg_pid;                    /* foreground process, or ESPIX_PID_NONE */

    /*
     * Background jobs, in the order `&` started them, with the name the shell
     * showed. A slot is free when both pid and task are clear, so a session
     * that never backgrounds anything needs no initialisation. See
     * espix_shell_job_add().
     */
    espix_job_t jobs[ESPIX_SESSION_JOBS];

    /*
     * Non-NULL only in a background builtin's session copy, where it points at
     * that job's stop flag. The loops that can block poll it; NULL everywhere
     * else, which is what makes espix_shell_stopping() false in an ordinary
     * session and leaves poll_interrupt the only way to cut a command short.
     */
    const volatile bool *stop;

    int         last_status;               /* $? */
    bool        want_exit;

    /*
     * Set for the duration of one command when the line ended in `&`, and read
     * by a command that spawns (R-P2.2). The shell strips the operator before
     * dispatch, so no command ever receives it as an argument -- `cat f &`
     * used to open a file called `&`.
     */
    bool        background;

    /*
     * Set for the duration of one command when its output was redirected with
     * `>` / `>>`. espix_puts()/espix_printf() honour it; a spawned app's own
     * stdout does not, so `app > file` still writes to the console.
     */
    FILE       *redirect;

    /*
     * The same for `2>` / `2>>`, honoured by espix_eprintf() alone.
     *
     * Diagnostics deliberately ignore `redirect`. `cmd > file` used to capture
     * espix's own error messages into the file, which is not merely untidy: it
     * cost a wrong conclusion once, when a failed ELF load's message vanished
     * into a redirect and the loader was blamed for not naming a symbol it had
     * named perfectly.
     */
    FILE       *redirect_err;

    /*
     * `2>&1`: diagnostics follow output wherever it went.
     *
     * A flag rather than pointing redirect_err at the same FILE, because two
     * handles on one stream get closed twice -- the same trap the app stream
     * pair documents.
     */
    bool        err_to_out;

    /*
     * Set for the duration of one command when its input was redirected with
     * `<`, and NULL otherwise. A builtin that reads input takes it from here:
     * it runs on the session task, not as a process, so there is no other
     * standard input for it. See R-P2.4.
     */
    FILE       *redirect_in;

    /*
     * Where espix_printf() formats, rather than on the stack of whoever called
     * it. 256 bytes is not much, but it is on *every* command's stack, and a
     * command task is sized for its own frames -- a line buffer it does not own
     * should not be part of that sizing. One per session, because two sessions
     * can print at once and each needs its own.
     *
     * Within a session it is not reentrant, which is fine: a command runs to
     * completion before the next begins, and one that waits for another -- `sudo`
     * -- is on its own stack by then.
     */
    char        printf_buf[ESPIX_LINE_MAX];
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

    /*
     * The task stack this command needs, in bytes, or 0 to run on the session's
     * own task.
     *
     * A command's stack cost is part of its contract. The session task is sized
     * for the protocol plus the commands that declare 0 here, and anything that
     * declares a size runs on a task of its own -- sized for it, freed when it
     * exits, and never charged to every open connection. `sudo` declares one
     * because it re-enters the dispatcher, so the command it runs gets a stack
     * of its own instead of paying for two frames on one.
     *
     * A command that declares too little is caught by the stack canary, which is
     * how `mount` was caught using 9556 bytes of a 10240-byte session stack.
     * `ps` reports a running task's high-water mark, and the dispatcher logs the
     * same figure when a spawned command finishes, so the numbers here are
     * measured rather than believed.
     */
    uint32_t         stack;

    /*
     * Run this command on an internal-RAM stack, never on a PSRAM one.
     *
     * Rewriting the MMU -- esp_partition_mmap(), and therefore
     * esp_ota_set_boot_partition() -- freezes the external-memory cache for the
     * duration. On this part flash and PSRAM are both behind that cache, so
     * while it is frozen neither is addressable and the stack the code is
     * running on has to be in internal RAM. esp_mm asserts exactly that
     * (s_task_stack_is_sane_when_cache_frozen, esp_cache_utils.c), and the
     * spi_flash write path asserts the same thing. A command that maps flash
     * sets this; upgrade panicked once because it did not.
     */
    bool             internal_stack;

    /*
     * Whether this command handles a trailing `&` itself.
     *
     * True only for a command that *spawns*: `confine` turns a program into a
     * process and gives it a pid, so it does its own backgrounding. Every
     * other builtin is now run on a job task by the shell when `&` is present
     * (R-P2.8), so this flag is no longer a refusal -- without it a spawner
     * would be quietly forked a second time.
     */
    bool             backgrounds;

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

/* ------------------------------------------------------------------ */
/* Session variables and the environment. See env.c for the three tiers. */

/*
 * The session's value for `name`, or the system environment's if the session
 * has none, or NULL. Never the caller's to free.
 */
const char *espix_env_get(const espix_session_t *s, const char *name);

/*
 * Set or replace. `exported` is sticky: exporting an existing variable exports
 * it, and assigning to an already-exported one leaves it exported, as sh does.
 *
 * ESP_ERR_INVALID_ARG for a name that is not a POSIX identifier,
 * ESP_ERR_INVALID_SIZE for an over-long value, ESP_ERR_NO_MEM when the table is
 * full -- all of which the caller is expected to report rather than swallow.
 */
esp_err_t espix_env_set(espix_session_t *s, const char *name,
                        const char *value, bool exported);

/* True if it was there. Removes it from the session only; the system
 * environment is not the session's to change. */
bool espix_env_unset(espix_session_t *s, const char *name);

/* Walk the session's own variables -- `slot` runs 0..ESPIX_ENV_MAX-1 and false
 * means "nothing here", not "end". Used by `env` and `export` to list, and by
 * the spawn path to collect what is exported. */
bool espix_env_at(const espix_session_t *s, size_t slot, const char **name,
                  const char **value, bool *exported);

size_t espix_env_count(const espix_session_t *s);

/* Release the table. Called when a session ends. */
void espix_env_free(espix_session_t *s);

/* USER, HOME, PATH and TERM for a fresh login. Call once the session's user,
 * cwd and `ansi` are settled -- they are what these are derived from. */
void espix_env_set_login_defaults(espix_session_t *s);

/* Is this a name the shell will accept? POSIX's rule, and what makes
 * `FOO=bar cmd` decidable from `cmd` at all. */
bool espix_env_name_ok(const char *name);

/*
 * Assignments that last for one command. The scope holds whatever was shadowed
 * and puts it back, so `FOO=bar cmd` leaves the session exactly as it found it.
 * Stack-allocated by the caller; init, set, run the command, end.
 */
typedef struct {
    struct {
        char *name;
        char *value;
        bool  exported;
        bool  existed;
    } saved[ESPIX_ENV_SCOPE_MAX];
    int n;
} espix_env_scope_t;

void      espix_env_scope_init(espix_env_scope_t *scope);
/* Keep one assignment rather than undoing it -- `FOO=bar` with no command. */
esp_err_t espix_env_scope_keep(espix_session_t *s, espix_env_scope_t *scope,
                               const char *name);
esp_err_t espix_env_scope_set(espix_session_t *s, espix_env_scope_t *scope,
                              const char *name, const char *value);
void      espix_env_scope_end(espix_session_t *s, espix_env_scope_t *scope);

/* ------------------------------------------------------------------ */

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
 * Record a background *process* against the session, for `jobs`/`fg`/`bg`.
 * Returns false when the table is full, which the caller reports rather than
 * losing the job silently. Lives here and not with the `jobs` command because
 * the table is the session's; the process lookups that prune it are in
 * espix_cmds, which may depend on espix_proc and this header may not.
 */
bool espix_shell_job_add(espix_session_t *s, espix_pid_t pid, const char *name);

/*
 * Reclaim a finished job slot. Called by the `jobs` command when it finds a
 * process gone or a builtin task finished; the task owns its own teardown, so
 * this only forgets what the session remembered. Lives here because the
 * teardown handshake is under a lock this component owns.
 */
void espix_shell_job_clear(espix_session_t *s, int slot);

/*
 * Has this session -- a background builtin's copy -- been asked to stop?
 *
 * The cooperative equivalent of a signal, and the only one a builtin can have:
 * espix can delete a task but not signal it. A blocking loop polls this and
 * returns early; false in every ordinary session, so it is free there.
 */
bool espix_shell_stopping(const espix_session_t *s);

/*
 * Stop every background builtin this session still has, before the session and
 * its transport go away. Sets each job's stop flag, gives its loops a bounded
 * moment to notice, then deletes only what ignored it. Called from
 * espix_shell_session_run() before the session-end hook, and by SSH's
 * finish_session() for the exec path, which never reaches the REPL.
 */
void espix_shell_jobs_drain(espix_session_t *s);

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

/*
 * The current session's terminal size, for an app that got SIGWINCH. Returns 0
 * with cols and rows filled, or -1 when there is no session or its transport
 * cannot answer.
 */
int espix_shell_term_size(int *cols, int *rows);
void espix_shell_set_current(espix_session_t *s);

/* Output helpers — commands must use these rather than printf(), or their
 * output goes to the console instead of the session that asked for it. */
int espix_puts(espix_session_t *s, const char *str);
int espix_printf(espix_session_t *s, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/*
 * Diagnostics: anything the user did not ask for. Errors, warnings, usage after
 * a mistake.
 *
 * The distinction is what `>` and `2>` are for, and it is a judgement per call
 * rather than a rule about wording. `usage:` printed because somebody asked for
 * help is output; the same line printed because they got the arguments wrong is
 * a diagnostic. For a command whose whole job is reporting a problem, the
 * report is its output.
 */
int espix_eprintf(espix_session_t *s, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/*
 * Raw write to one of the session's two output paths, resolving `>`, `2>` and
 * `2>&1` exactly as espix_printf()/espix_eprintf() do.
 *
 * Exists for a loaded process's stdio: espix_proc builds funopen() streams over
 * this so an app's printf() and fprintf(stderr, ...) land wherever the shell
 * says, without the process ever *holding* the shell's redirect FILE. It held
 * one once, and a force-killed process then had stdout and stderr pointing at
 * the same object -- which esp_cleanup_r() fclosed twice on task deletion, the
 * second close asserting inside newlib on a lock the first had already
 * destroyed.
 */
int espix_session_write(espix_session_t *s, const char *data, size_t len,
                        bool err);

/*
 * Console transport (UART or USB-Serial-JTAG, whichever the build selects).
 * Sets up the driver, line endings and the line editor, then runs the session
 * loop on the calling task. Normally never returns.
 */
/*
 * Called when a session ends and whatever it spawned must go with it.
 *
 * Registered rather than called directly because the component that owns the
 * process table -- espix_proc -- sits *above* this one: it depends on
 * espix_shell for espix_session_t, so the call cannot go the other way. Same
 * shape, and the same reason, as task_gone and poll_interrupt.
 *
 * It is here, rather than in each transport, because that is where Linux puts
 * it: losing a session's controlling terminal hangs its processes up in the
 * *kernel*, so sshd only has to close the pty and no transport can forget.
 * espix's equivalent of losing the terminal is read_line() reporting EOF, which
 * happens inside espix_shell_session_run() -- so that is where it lands, and
 * every transport gets it without knowing it exists.
 */
void espix_shell_set_session_end_hook(size_t (*fn)(const espix_session_t *s));

esp_err_t espix_console_session_start(void);

/*
 * The on-screen console: the same shell, rendered into the display canvas and
 * fed from the display's input queue. Started and stopped by the display
 * service when a viewer attaches and leaves, so a headless board pays for
 * none of it.
 */
esp_err_t espix_console_canvas_start(void);
void      espix_console_canvas_stop(void);

#ifdef __cplusplus
}
#endif
