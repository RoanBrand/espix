/* Internal to the espix_proc component: the process table representation
 * shared between proc.c (table, wait, kill) and exec.c (loading and running). */
#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "esp_elf.h"

#include "espix_proc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESPIX_PROC_MAX CONFIG_ESPIX_PROC_MAX

typedef struct {
    espix_proc_info_t info;

    esp_elf_t elf;
    bool      elf_valid;
    uint8_t  *image;        /* raw file bytes, freed once the ELF is torn down */

    /* argv storage: one allocation holding the char* array followed by the
     * argument strings, so the whole vector frees in one call. */
    void  *argv_block;
    int    argc;
    char **argv;

    /*
     * Set when someone has asked this process to stop. Volatile because the
     * app polls it from its own task while another task writes it, and no lock
     * is taken on the read path: a single bool needs none, and an app should
     * not block to ask whether it is still wanted.
     *
     * Still a plain bool rather than "SIGTERM is pending": it is the *decision*
     * to leave, which a default action reaches and a handled signal does not.
     * An app that handles SIGINT and returns keeps running, and this stays
     * false.
     */
    volatile bool stop_requested;

    /*
     * The process's working directory, so a relative path an app hands to
     * fopen() means what it would on any Unix.
     *
     * Per process, not per session, and seeded from the spawning session: an
     * app's chdir() must not move the shell that started it, which is what
     * fork/exec gives you everywhere else.
     *
     * This is only reachable because espix owns the root VFS -- ESP-IDF's
     * chdir() is an ENOSYS stub and its getcwd() always answers "/", but
     * espix's VFS receives the caller's path verbatim and resolves it itself,
     * so IDF's stubs never come into it. Here rather than in
     * espix_proc_info_t for the usual reason: that struct is bulk-copied onto
     * callers' stacks.
     */
    char cwd[ESPIX_PATH_MAX];

    /*
     * Signal state.
     *
     * Here rather than in espix_proc_info_t deliberately. That struct is what
     * espix_proc_snapshot() bulk-copies, and callers put it on the stack —
     * cmd_ps as [8], espix_proc_hangup as [12] on an SSH task. A handler table
     * in there would cost every one of those arrays 128 bytes an entry to carry
     * something no caller can use.
     *
     * Bit N is signal N, matching newlib's sigset_t convention exactly, so the
     * two interchange without a shuffle. uint32_t and not uint8_t: SIGTERM is
     * bit 15 and SIGUSR2 is bit 31, and a narrower field would silently drop
     * every signal above SIGBUS -- `|=` on a uint8_t does not warn.
     */
    volatile uint32_t sig_pending;
    volatile uint32_t sig_blocked;

    /*
     * [NSIG] of them, allocated on the first signal()/sigaction() call and
     * freed with the rest of the slot's resources. NULL means every signal is
     * still at its default, which is the common case and costs one pointer.
     * SIG_DFL is 0 and SIG_IGN is 1, so a zeroed table already reads correctly.
     */
    void (**sig_handlers)(int);

    /*
     * SIGSTOP asked for, and the semaphore the process parks on once it takes
     * the hint. Created lazily, on the first SIGSTOP a process actually gets.
     *
     * A semaphore rather than vTaskSuspend()/vTaskResume(): a give that lands
     * before the take is remembered, so SIGCONT arriving in the window between
     * the target deciding to park and actually parking cannot be lost. The same
     * race against vTaskResume() has no fix that does not involve polling
     * eTaskGetState(), and parking is not worth a poll.
     */
    volatile bool     sig_stop_req;
    SemaphoreHandle_t sig_cont;
} espix_proc_slot_t;

/* Bit for `sig`, or 0 if it is not a signal. Not sigaddset(): that macro is
 * `1 << sig` on a signed int, and SIGUSR2 is 31. */
static inline uint32_t espix_sigbit(int sig)
{
    return (sig > 0 && sig < NSIG) ? ((uint32_t)1u << sig) : 0u;
}

/* Signals that cannot be caught, blocked or ignored, as POSIX requires. */
#define ESPIX_SIG_UNCATCHABLE (espix_sigbit(SIGKILL) | espix_sigbit(SIGSTOP))

/*
 * Not a signal: bit 0, which no signal uses because they start at 1. Set in the
 * mask espix_sigcheck_mask() returns to mean "this process has been asked to
 * terminate", so one call answers both questions a blocking call needs to ask.
 */
#define ESPIX_SIG_STOPPING ((uint32_t)1u << 0)

/*
 * The delivery point, reporting what it delivered.
 *
 * espix_sigcheck() is this with the answer reduced to a bool. The mask is what
 * an interrupted blocking call needs: POSIX says sleep() returns its unslept
 * remainder and nanosleep() fails with EINTR when a *caught* signal arrives,
 * and a handler that merely sets a flag would otherwise never get the chance to
 * act -- the sleep would resume and the flag go unread until it expired.
 */
uint32_t espix_sigcheck_mask(void);

/* Table access. The lock covers slot allocation and state transitions; readers
 * that must not block (the fault path) read without it and tolerate a torn
 * view, which is why `pid` is written last on allocation. */
extern espix_proc_slot_t  g_espix_procs[ESPIX_PROC_MAX];
extern SemaphoreHandle_t  g_espix_proc_lock;
extern EventGroupHandle_t g_espix_proc_events;

/* Claim a free slot (preferring one never used, else the oldest finished one).
 * Returns NULL if the table is full of live processes. Caller must hold the
 * lock. */
espix_proc_slot_t *espix_proc_alloc_slot(void);

/* Release everything a finished slot owns: ELF image, argv block. Caller must
 * NOT hold the lock. */
void espix_proc_release_resources(espix_proc_slot_t *slot);

/* Record a terminal state and wake anyone in espix_proc_wait(). */
void espix_proc_finish(espix_proc_slot_t *slot, espix_proc_state_t state,
                       int exit_code);

/* Monotonic pid allocation; pids are never reused. Caller must hold the lock. */
espix_pid_t espix_proc_next_pid(void);

/* Publish the C++ runtime an app needs to resolve at load time. See
 * abi_cxx.cpp, the one C++ translation unit in espix. */
void espix_proc_abi_cxx_register(void);

/* Publish the peripheral surface an app needs. See abi_drivers.c: naming a
 * symbol there is also what keeps its driver linked into the firmware. */
void espix_proc_abi_drivers_register(void);
void espix_proc_abi_time_register(void);

/* Publish the filesystem an app needs: fopen, open, stat, opendir and the rest,
 * plus espix's own chdir/getcwd because IDF's are stubs. See abi_fs.c -- almost
 * all of it is unwrapped libc, because those calls already dispatch into
 * espix's own VFS. */
void espix_proc_abi_fs_register(void);

/* Publish the POSIX signal surface, and interpose the blocking calls that have
 * to become delivery points. See abi_signal.c: this one installs a symbol
 * resolver rather than only adding a table, because the loader's own libc table
 * is searched first and already answers for sleep() and usleep(). */
void espix_proc_abi_signal_register(void);

/* The slot for `pid`, or NULL. Caller must hold the lock. */
espix_proc_slot_t *espix_proc_find(espix_pid_t pid);

/* The calling task's slot, or NULL if it is not a process. Takes no lock: the
 * caller is the process itself, so its slot cannot be recycled underneath it. */
espix_proc_slot_t *espix_proc_self(void);

/* True once this state means the process is over. */
bool espix_proc_state_is_finished(espix_proc_state_t s);

#ifdef __cplusplus
}
#endif
