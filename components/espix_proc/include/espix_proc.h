/*
 * espix process table.
 *
 * A "process" is a FreeRTOS task plus the bookkeeping that makes it
 * addressable from a shell: a pid, a name, an exit status, and — for apps
 * loaded off the filesystem — the relocated ELF image it is running.
 *
 * There is no memory isolation between processes on chips without an MMU; see
 * the crash-handling model in the project README. What this table buys is
 * identity: something for `ps` to list, `kill` to target, and the fault path
 * to name when a task dies.
 */
#pragma once

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"

#include "espix_kernel.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESPIX_PROC_NAME_MAX 24

/*
 * Signal numbers come from <signal.h> and espix defines none of its own.
 *
 * That is not just tidiness. The toolchain's numbering is the BSD set, so
 * SIGUSR1 is 30 and SIGUSR2 is 31 -- not the 10 and 12 a Linux habit expects,
 * which here are SIGBUS and SIGSYS. An app compiled against the real header and
 * a kernel carrying its own table would disagree silently, and the process
 * would receive a signal nobody sent.
 *
 * Signal sets are plain uint32_t: NSIG is 32, and newlib's own sigset_t is a
 * 32-bit unsigned with bit N standing for signal N, so the two interchange
 * directly. espix uses its own set operations rather than the <signal.h>
 * macros, because sigaddset() expands to `1 << sig` on a signed int, which for
 * SIGUSR2 is `1 << 31` -- undefined behaviour, under -Werror.
 */

typedef enum {
    ESPIX_PROC_FREE = 0,    /* table slot unused */
    ESPIX_PROC_READY,       /* accepted, task not started yet */
    ESPIX_PROC_RUNNING,
    ESPIX_PROC_STOPPED,     /* parked itself on SIGSTOP; alive, not running */
    ESPIX_PROC_EXITED,      /* returned normally */
    ESPIX_PROC_FAULTED,     /* killed by the fault handler */
    ESPIX_PROC_KILLED,      /* killed by request */
} espix_proc_state_t;

typedef struct {
    espix_pid_t        pid;
    char               name[ESPIX_PROC_NAME_MAX];
    char               path[ESPIX_PATH_MAX];
    TaskHandle_t       task;
    espix_proc_state_t state;
    int                exit_code;
    int64_t            started_us;
    size_t             image_bytes;   /* relocated ELF size, 0 for kernel tasks */
    espix_session_t   *session;       /* stdio/cwd owner, may be NULL */

    /*
     * Who the process runs as, copied from the launching session rather than
     * read through it: the session lives on the caller's stack and a
     * backgrounded process outlives the command that started it, so following
     * the pointer at check time would be a use-after-free on exactly the path
     * that decides whether a file may be opened.
     */
    uint16_t           uid;
    uint16_t           gid;
    uint16_t           groups[ESPIX_NGROUPS_MAX];
    uint8_t            ngroups;
} espix_proc_info_t;

esp_err_t espix_proc_init(void);

/*
 * Load the ELF at `abs_path` and run it as a new process. Returns as soon as
 * the process is admitted; use espix_proc_wait() to block for completion.
 *
 * `argv` is copied, so the caller's buffers need not outlive the call.
 */
esp_err_t espix_proc_spawn_elf(const char *abs_path, int argc, char **argv,
                               espix_session_t *session, espix_pid_t *out_pid);

/*
 * Block until `pid` leaves the running state. Returns ESP_ERR_TIMEOUT if it is
 * still running when `timeout` expires.
 */
esp_err_t espix_proc_wait(espix_pid_t pid, int *out_exit_code, TickType_t timeout);

/*
 * Send `sig` to `pid`. The one entry point; everything else here wraps it.
 *
 * Delivery is not asynchronous. Setting a pending bit is all this does for a
 * catchable signal — the handler runs later, in the target's own task, at a
 * delivery point (see espix_sigcheck). A real Unix kernel interrupts the thread
 * at an arbitrary instruction and manufactures a signal frame on its stack;
 * doing that here would mean rewriting a FreeRTOS task's saved program counter
 * on windowed-register Xtensa, which is not a trade worth making.
 *
 * What it does do is wake a target blocked in sleep(), so the handler runs
 * promptly rather than whenever the sleep happened to end.
 *
 * SIGKILL and SIGSTOP cannot be caught, blocked, or ignored. SIGKILL deletes
 * the task outright, with the consequences described on espix_proc_kill().
 *
 * ESP_ERR_INVALID_ARG for a signal outside 1..NSIG-1, ESP_ERR_NOT_FOUND for an
 * unknown pid, ESP_ERR_INVALID_STATE for one that has already finished.
 */
esp_err_t espix_proc_signal(espix_pid_t pid, int sig);

/*
 * Kill a process: SIGTERM, a grace period, then SIGKILL if it is still there.
 *
 * The forced half is unsafe in the general case on a shared-address-space
 * system — the task may hold a lock or own heap blocks it will now never free —
 * so it reclaims the ELF image but makes no attempt to undo anything else the
 * task did. See the reaper notes in espix_fault. Asking first is what gives an
 * app the chance to put its hardware back: an LED off, a motor stopped.
 */
esp_err_t espix_proc_kill(espix_pid_t pid);

/* SIGTERM without the escalation: ask, and return immediately. */
esp_err_t espix_proc_request_stop(espix_pid_t pid);

/*
 * Run any pending handlers for the calling process, park it if it has been sent
 * SIGSTOP, and report whether it has been asked to terminate.
 *
 * This is the delivery point. espix calls it from inside the blocking calls it
 * publishes to apps — sleep, usleep, nanosleep, pause — so an ordinary app
 * never calls it directly: it registers a handler with signal(), and that
 * handler runs, returns, and execution carries on where it was.
 *
 * It is exported to apps for the one case that has no delivery point of its
 * own: a long compute loop that blocks on nothing. Calling it once an iteration
 * is what makes such a loop interruptible, and in the common case where nothing
 * is pending it costs a table scan and two volatile loads.
 *
 * Returns false for a task that is not a process, so kernel tasks calling it
 * see "carry on".
 *
 * This replaces espix_app_stopping(), which was the same question before there
 * were signals to answer it with. Apps built against that name must be rebuilt:
 * an unresolved symbol is a load-time failure, not a runtime one.
 */
bool espix_sigcheck(void);

/*
 * Hang up on `session`: SIGHUP to everything it owns, then force what is left,
 * as happens when a terminal goes away. The session's stdio dies with it, so
 * anything still holding it must not outlive it. Returns how many were ended.
 */
size_t espix_proc_hangup(const espix_session_t *session);

/* Signal name without the "SIG" ("TERM", "KILL"), or NULL if `sig` is not one
 * espix names. Backs `kill -l` and the name form of `kill -TERM`. */
const char *espix_signal_name(int sig);

/* Inverse: "TERM", "SIGTERM" and "15" all give 15. Returns -1 if unrecognised. */
int espix_signal_from_name(const char *name);

/* Copy up to `n` live entries into `out`. Returns how many were written. */
size_t espix_proc_snapshot(espix_proc_info_t *out, size_t n);

/* Look up the process owning `task`, or ESPIX_PID_NONE. Safe to call from a
 * restricted context: it only reads the table. */
espix_pid_t espix_proc_pid_of_task(TaskHandle_t task);

/*
 * The credentials of the process running on `task`, or false if that task is
 * not a process -- the console, an SSH connection task, SNTP, the WiFi driver.
 *
 * Takes no lock. It runs on the path of every file operation in the system, and
 * the fields it reads are written once, before the process is admitted.
 */
bool espix_proc_cred_of_task(TaskHandle_t task, uint16_t *uid, uint16_t *gid,
                             uint16_t *groups, uint8_t *ngroups);

/*
 * The calling process's working directory, or "/" for a task that is not a
 * process -- the console, an SSH connection task, SNTP, the WiFi driver.
 *
 * espix's VFS calls this to resolve a relative path, which is what gives a
 * loaded app a working directory at all. ESP-IDF has none to offer: its
 * chdir() is an ENOSYS stub and its getcwd() always answers "/". espix's VFS
 * receives the caller's path verbatim and resolves it here instead, so those
 * stubs never come into it.
 *
 * Never NULL, and never blocks: it is on the path of every file operation in
 * the system.
 */
const char *espix_proc_cwd(void);

/*
 * Move the calling process's working directory. `abs_path` must be absolute
 * and must be a directory.
 *
 * Per process, so this does not move the session that spawned it -- an app
 * calling chdir() leaves the shell where it was, as fork/exec does everywhere
 * else.
 *
 * ESP_ERR_NOT_FOUND if the path is not a directory, ESP_ERR_INVALID_STATE for
 * a caller that is not a process (there is nowhere to record it).
 */
esp_err_t espix_proc_chdir(const char *abs_path);

/*
 * State of one process, or ESPIX_PROC_FREE if there is no such pid.
 *
 * For a caller that wants one process's state and not a whole snapshot —
 * `ps` walks the FreeRTOS task list and needs to know which of those tasks
 * espix considers stopped, and copying the table onto its stack to find out
 * would cost far more than it answers.
 */
espix_proc_state_t espix_proc_state_of(espix_pid_t pid);

const char *espix_proc_state_str(espix_proc_state_t state);

#ifdef __cplusplus
}
#endif
