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
 * the reentrant parts of that component — esp_console_split_argv(), linenoise
 * and argtable3.
 */
#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "esp_err.h"

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

    /* Authenticated user, empty for the console, which has no login step. */
    char        user[ESPIX_SESSION_USER_MAX];

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

/* Walk the registry in name order. Return false from `cb` to stop. */
typedef bool (*espix_cmd_iter_fn)(void *ctx, const espix_cmd_t *cmd);
void espix_shell_foreach(espix_cmd_iter_fn cb, void *ctx);

/*
 * Run one command line in the context of `s`. Returns the command's status,
 * or a negative espix status for "not found" / "empty line".
 */
#define ESPIX_SHELL_ENOENT (-127)
#define ESPIX_SHELL_EMPTY  (-1)
int espix_shell_exec(espix_session_t *s, const char *line);

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
 * Sets up the driver, line endings and linenoise, then runs the session loop
 * on the calling task. Normally never returns.
 */
esp_err_t espix_console_session_start(void);

#ifdef __cplusplus
}
#endif
