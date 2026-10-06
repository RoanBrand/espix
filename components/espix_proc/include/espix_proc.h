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
 * How many completed processes the process table remembers. See
 * espix_proc_history() and docs/WORKLIST.md R-P1.5: this is the zombie store,
 * sized to what a person reads rather than to concurrency, and separated from
 * the live table so a finished process stops competing for a slot the moment
 * it finishes.
 */
#define ESPIX_PROC_DONE_MAX 8

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

/*
 * One completed process, as the process table remembers it after the slot has
 * been released. This is not espix_proc_info_t on purpose: the task handle and
 * the session pointer are meaningless once the process is gone -- the session
 * may already have been freed -- so the log keeps only what it can still
 * answer honestly. See espix_proc_history().
 */
typedef struct {
    espix_pid_t        pid;
    espix_pid_t        ppid;
    char               name[ESPIX_PROC_NAME_MAX];
    espix_proc_state_t state;       /* EXITED, FAULTED or KILLED */
    int                exit_code;
    int64_t            started_us;
    int64_t            ended_us;
    bool               reaped;      /* wait() has already returned this status */
} espix_proc_record_t;

typedef struct {
    espix_pid_t        pid;

    /*
     * The process that spawned this one, or ESPIX_PID_NONE when espix started
     * it itself -- which today is every process, because the shell is a session
     * task and not a process, and nothing an app can call spawns. Recorded
     * anyway because it is what a child's exit is reported to (SIGCHLD), and
     * because a listing should be able to say who asked for a process.
     */
    espix_pid_t        ppid;
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
 *
 * `root` confines the process to that directory, or NULL leaves it able to
 * name the whole filesystem -- see espix_proc_root(). It must be absolute, and
 * if the *calling* process is itself confined it must lie within that root: a
 * root narrows and never widens. The new process starts in `root` rather than
 * inheriting a cwd that lies outside it.
 *
 * The binary itself is read before the process exists, so it may live anywhere;
 * `confine /srv/www /bin/httpd` is the ordinary shape rather than a loophole.
 *
 * `foreground` says the caller will block in espix_proc_wait() for this
 * process. It decides one thing: whether the process's stdout and stderr may
 * be pointed at the session's `>` / `2>` redirection, whose FILE the shell
 * closes as soon as the command returns. A backgrounded process outlives that,
 * so it writes to the terminal instead -- see the stream note in exec.c.
 */
esp_err_t espix_proc_spawn_elf(const char *abs_path, int argc, char **argv,
                               espix_session_t *session, const char *root,
                               bool foreground, espix_pid_t *out_pid);

/*
 * Block until `pid` leaves the running state, and reap its status.
 *
 * This consumes the completed-log entry, as waitpid(2) does: the first call
 * returns the exit code, and a second call for the same pid fails with
 * ESP_ERR_NOT_FOUND -- which is ECHILD. ESP_ERR_TIMEOUT if it is still running
 * when `timeout` expires, and ESP_ERR_NOT_FOUND for a pid that never existed
 * or has already been reaped.
 *
 * Callers that only need to know the process is gone -- kill's own escalation,
 * a session hangup -- must use the internal observe-only path, or they would
 * take the status away from whoever is the process's parent.
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
 * The shutdown path: SIGTERM every live process except the caller, then wait for
 * the table to empty or deadline_us to pass. Returns how many were still alive
 * when it gave up.
 *
 * No escalation, deliberately: espix_proc_kill()'s forced half is unsafe on a
 * shared address space, and a reset takes the task anyway, so a force would buy
 * nothing here and could leave a volume lock held. The wait is the process
 * table's own finish event, so it ends when the last process does rather than
 * after a fixed grace. Called from the kernel's shutdown sequence.
 */
size_t espix_proc_stop_all(int64_t deadline_us);

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
 * Has this process been asked to stop? Reads the flag and nothing else.
 *
 * The difference from espix_sigcheck() is what it does NOT do: no handler
 * dispatch, no parking on SIGSTOP. That makes it the only form safe to call
 * from inside a transport write, where running an app's signal handler would
 * re-enter the very send that is in progress, and parking would hold the
 * channel's transmit lock for as long as the process stayed stopped.
 *
 * It exists so a writing process is interruptible at all. Everything else
 * espix can be stopped inside -- sleep, pause, a read -- has a delivery point;
 * the send path had none, so `kill` could not touch an app that was writing and
 * force-deleting it was the only outcome. Deleting a task blocked in lwIP or
 * holding a channel lock is what produced three separate panics; see
 * docs/KNOWN-ISSUES.md.
 *
 * False for a task that is not a process, so the connection task's own writes
 * and the key exchange are unaffected.
 */
bool espix_proc_stopping(void);

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

/*
 * The completed log, oldest first, up to ESPIX_PROC_DONE_MAX records. Returns
 * how many were written. This is what `ps` lists under "finished:" once the
 * live table stops retaining finished slots.
 *
 * A reaped record is still reported: the log is a bounded history and not only
 * the zombie store, because seeing what an app exited with is a debugging
 * affordance worth its fixed 8 slots. `reaped` says whether wait() has already
 * consumed it.
 */
size_t espix_proc_history(espix_proc_record_t *out, size_t n);

/*
 * R-P1.6: the reaper as the single teardown point.
 *
 * A process that finishes on its own stops deleting itself. It parks, and the
 * reaper deletes it, which moves the newlib teardown and the PSRAM stack free
 * off the idle task and onto a task with a real stack. espix_fault's reaper
 * registers here at start; espix_proc must not depend on it, so the direction
 * is a callback rather than a call.
 */
void espix_proc_set_reap_task(void (*fn)(TaskHandle_t task));

/* On the reaper task: run the teardown of a process that parked itself (arena,
 * fds, slot) and say whether `task` was one. False means it was not, and the
 * caller should use the fault path. */
bool espix_proc_reaped(TaskHandle_t task);

/* Look up the process owning `task`, or ESPIX_PID_NONE. Safe to call from a
 * restricted context: it only reads the table. */
espix_pid_t espix_proc_pid_of_task(TaskHandle_t task);

/*
 * Bytes the process has *live* in its own memory arena (R-P1.2), summed over
 * its regions, or 0 if it has none. This is used, not reserved: a region is
 * sized to the request that failed, so reserved is often several times this,
 * and a column that showed reserved would read as a leak. Best-effort -- it
 * reads the table without the process lock -- which is what a listing wants.
 */
size_t espix_proc_heap_used(espix_pid_t pid);

/*
 * The pid that spawned `pid`, or ESPIX_PID_NONE if it was espix itself or the
 * pid is unknown. Takes the table lock, so it is a listing's call and not a
 * delivery point's.
 */
espix_pid_t espix_proc_parent_of(espix_pid_t pid);

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
 * Root of the calling process: a directory outside of which it cannot resolve
 * a path at all. "" when the process is unconfined, and for any task that is
 * not a process.
 *
 * espix's VFS consults this in resolve(), which every path operation passes
 * through -- so one test covers open, opendir, unlink, rename, mkdir, truncate
 * and stat alike. A path outside is reported ENOENT rather than EACCES: the
 * point is that it is not there, and "denied" would confirm that it is.
 *
 * A filesystem boundary and nothing more. Device VFSes register longer prefixes
 * and are routed by ESP-IDF before espix sees them, which is what keeps a
 * confined process's stdio working; and with no MMU an app shares the address
 * space with the kernel either way. Like setuid, this is a guardrail against
 * mistakes today and a real boundary on a part with an MMU.
 */
const char *espix_proc_root(void);

/*
 * Both of the above in one walk of the process table, for the VFS, which wants
 * both on every path it resolves. Either pointer may be NULL. On return they
 * point at storage owned by the process table, valid for as long as the calling
 * process is -- which, since the caller is that process, is long enough.
 */
void espix_proc_paths(const char **cwd, const char **root);

/*
 * The calling process's pid, or ESPIX_PID_NONE if the caller is not one. The
 * same lookup espix_proc_paths() does, for callers that want only the identity
 * -- the VFS stamps it on every file it opens, so the reaper can find what a
 * killed process left open.
 */
espix_pid_t espix_proc_self_pid(void);

/*
 * A process about to block in select(): arms the wake path and returns the
 * wake fd to fold into the read set, or -1 for a caller with none. The pair
 * exists so a signal can reach a process that is waiting on a socket, which is
 * otherwise unreachable (R-P6.6). Pending signals are delivered by begin(),
 * before the block, so nothing is left unhandled. select_end() clears it.
 */
int  espix_proc_select_begin(void);
void espix_proc_select_end(void);

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

/*
 * The process name behind a pid -- the basename it was spawned as -- copied
 * into out. False when no live process has that pid.
 *
 * This is what ps prints for a process, and not the FreeRTOS task name: an
 * app's task is "app:testapp" while its process is "testapp", so the live
 * listing and the finished one would otherwise show one process under two
 * names (R-P7.6). A kernel task has no pid and no process name.
 */
bool espix_proc_name_of(espix_pid_t pid, char *out, size_t len);

const char *espix_proc_state_str(espix_proc_state_t state);

#if CONFIG_ESPIX_PROC_ABI_WATCHPOINT
/*
 * Store to the watched ABI table state, to prove the watchpoint fires. Panics
 * if it is working. Writes back the value already there, so nothing is damaged
 * by asking. `crash abi` is the caller.
 */
void espix_proc_abi_watch_selftest(void);
#endif

#ifdef __cplusplus
}
#endif
