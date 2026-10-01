/* Internal to the espix_proc component: the process table representation
 * shared between proc.c (table, wait, kill) and exec.c (loading and running). */
#pragma once

#include <setjmp.h>     /* jmp_buf: how an app's exit() gets home */
#include <stdio.h>      /* struct _reent, for the stdio a force-kill puts back */

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "esp_elf.h"

#include "espix_proc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESPIX_PROC_MAX CONFIG_ESPIX_PROC_MAX

/* Room for an app to add variables of its own beyond what it inherited. */
#define ESPIX_PROC_ENV_ADDED_MAX 8

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

    /* The environment this process inherited, same packing as argv_block: the
     * char* array then the strings, one free() for the lot. Built at spawn
     * from the system environment and the session's exports (copy_env), so
     * nothing is shared with the session it came from. NULL when empty. */
    void  *env_block;
    char **envp;

    /*
     * Variables the app added with setenv() after it started, kept apart from
     * the inherited block rather than merged into it: that block is one
     * allocation with interior pointers, so growing it would invalidate every
     * char* the app is already holding onto from a previous getenv().
     */
    char  *env_added[ESPIX_PROC_ENV_ADDED_MAX];
    int    env_added_count;

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
     * How exit() gets home.
     *
     * An app's exit()/_Exit()/abort() must run the same teardown a return from
     * app_main() does -- streams closed, ELF released, the slot marked EXITED
     * with the right status. Deleting the task right there instead is what used
     * to leak the image and lose the exit status, so proc_task() arms a longjmp
     * target across the call into the app and espix_proc_exit() jumps to it.
     *
     * The jmp_buf lives in proc_task()'s frame, not here: it is valid only
     * while that frame is alive, and a jmp_buf per slot would be paid by every
     * slot to hold a pointer that is NULL except while the app runs. exit_jmp
     * being non-NULL is therefore also the test for "this slot's own task is
     * running the app", which is what keeps exit() called from a thread the app
     * created from longjmp'ing into a stack it does not own.
     */
    jmp_buf  *exit_jmp;
    int       exit_status;

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
     * The process's root: a directory it may not resolve a path outside of.
     * Empty for the overwhelming majority of processes, which are unconfined.
     *
     * This answers the question permissions cannot. A uid decides whether a
     * path may be *opened*; it never stops the path being *named*, and espix's
     * mode rule hands out 0755 directories and 0644 files, so a service account
     * can walk the whole tree and read nearly all of it. A root is the other
     * default: nothing is reachable except what was handed over.
     *
     * Restriction rather than chroot -- paths stay globally absolute, so the
     * process sees /srv/www/db and not /db. Real chroot needs getcwd()
     * translation and, more to the point, needs mounts: a jail with no /bin, no
     * /etc and no /tmp is not somewhere a program can run, and bind mounts are
     * what would fill it. See docs/ROADMAP.md; the mount table is the
     * precondition, and this is what is useful without it.
     *
     * Beside cwd rather than in espix_proc_info_t for the same reason cwd is.
     */
    char root[ESPIX_PATH_MAX];

    /*
     * Whether `root` is being enforced yet. Set once, by the process's own task,
     * after its ELF is loaded and immediately before its entry point is called.
     *
     * The delay is not an implementation detail, it is the semantics: espix
     * opens the binary, and the *app* is what gets confined. execve(2) draws
     * the line in the same place -- the image is read through the caller's view
     * of the filesystem, and only the new program runs in the new one. Arming
     * at spawn instead means the loader is confined too, and a rooted process
     * can never be given a program from outside its root, which makes -R
     * useless without a copy of every binary in every jail.
     *
     * Raising privilege around the load would have been the other way to get
     * there, and it is wrong: privilege bypasses the permission check as well,
     * so `confine ... /home/someone/private` would load a file the caller may
     * not read. The load must stay unprivileged and merely unrooted.
     *
     * volatile and unlocked for the same reason as stop_requested: one writer,
     * and the only reader that matters is the same task asking about itself.
     */
    volatile bool root_active;

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

    /*
     * The shell is waiting for this process, so its `>` redirection is usable.
     *
     * Only a foreground process may write to the session's redirect FILE:
     * redirects_release() closes it when the command returns, and a
     * backgrounded process outlives that. run_program() blocks in
     * espix_proc_wait() for a foreground one, which is exactly the guarantee
     * the FILE needs -- see the note in exec.c.
     */
    bool              foreground;

    /*
     * The task's own struct _reent, published by the process itself before it
     * replaced any of its stdio.
     *
     * Kept so a force-kill can put the global streams back before deleting the
     * task. Deleting a task runs prvDeleteTCB() -> _reclaim_reent(), which
     * fcloses every stream in that reent which is not the global one -- and a
     * process's stdout and stderr are funopen() objects over its session, so
     * closing one writes into the SSH channel. See espix_proc_detach_streams().
     */
    struct _reent *reent;
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

/*
 * Begin enforcing the calling process's root. Called once by proc_task(), after
 * the ELF is loaded and before the entry point runs; see root_active above.
 */
void espix_proc_root_arm(void);

/* Table access. The lock covers slot allocation and state transitions; readers
 * that must not block (the fault path) read without it and tolerate a torn
 * view, which is why `pid` is written last on allocation. */
/*
 * The process table, and the guard word that follows it.
 *
 * The guard is not decoration. Something wrote one word past this table once
 * and permanently disabled half the ABI resolver, and the only reason anyone
 * could reason about it at all is that the damaged word happened to be
 * s_table_count, which the map showed sitting immediately after the array.
 *
 * "Immediately after" was a linker accident, and it has since stopped being
 * true on one target: on RISC-V s_table_count lands in .sbss, so it is nowhere
 * near the table, and a watchpoint aimed at it would never fire. Putting the
 * guard *inside a struct* makes the adjacency the language's promise instead of
 * the linker's whim, on every target, and it gives the corruption a name to be
 * checked against rather than a neighbour to be inferred from.
 *
 * The slots are still reached as g_espix_procs[...] because that is what the
 * code says everywhere; only the declaration moved.
 */
#define ESPIX_PROC_GUARD_MAGIC 0x50524f43u   /* "PROC", and not a plausible slot */

typedef struct {
    espix_proc_slot_t slots[ESPIX_PROC_MAX];

    /*
     * The first word past the last slot. A run past the end of the table lands
     * here, which is the point: it is watched by the debug unit (see
     * abi_resolver.c) and checked in software, so the next occurrence is named
     * rather than inferred an hour later from a missing symbol.
     */
    volatile uint32_t guard;
} espix_proc_table_t;

extern espix_proc_table_t g_espix_proc_table;

/*
 * Whether the guard still holds its magic. Logs once when it does not, and is
 * cheap enough to ask on every spawn -- which is what makes it worth having on
 * a board nobody is attached to.
 */
bool espix_proc_table_intact(void);
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

/* Put the global stdio back into the process's reent, so that deleting its task
 * does not close espix's streams from the killer's -- or from IDLE's, which
 * takes the board down. Returns whether espix's streams were installed. Caller
 * must hold the lock and must have suspended the task: see the note in
 * espix_proc_detach_streams(). */
bool espix_proc_detach_streams(espix_proc_slot_t *slot);

/* Monotonic pid allocation; pids are never reused. Caller must hold the lock. */
espix_pid_t espix_proc_next_pid(void);

/* Publish the C++ runtime an app needs to resolve at load time. See
 * abi_cxx.cpp, the one C++ translation unit in espix. */
void espix_proc_abi_cxx_register(void);

/* Publish the peripheral surface an app needs. See abi_drivers.c: naming a
 * symbol there is also what keeps its driver linked into the firmware. */
void espix_proc_abi_drivers_register(void);
void espix_proc_abi_time_register(void);

/* Publish the screen and the input queue to an app. See abi_gfx.c. */
void espix_proc_abi_gfx_register(void);

/* Orphan the canvas if `pid` died holding it, so a killed app cannot wedge
 * every later repaint. Called from espix_proc_finish(). See abi_gfx.c. */
void espix_gfx_recover(espix_pid_t pid);

/* Publish the filesystem an app needs: fopen, open, stat, opendir and the rest,
 * plus espix's own chdir/getcwd because IDF's are stubs. See abi_fs.c -- almost
 * all of it is unwrapped libc, because those calls already dispatch into
 * espix's own VFS. */
void espix_proc_abi_fs_register(void);

/* The app identity calls: getuid/geteuid, getgid/getegid. */
void espix_proc_abi_ident_register(void);
void espix_proc_abi_libc_register(void);
void espix_proc_abi_libm_register(void);

/* Publish the POSIX signal surface, and interpose the blocking calls that have
 * to become delivery points. See abi_signal.c: this one installs a symbol
 * resolver rather than only adding a table, because the loader's own libc table
 * is searched first and already answers for sleep() and usleep(). */
/*
 * A name an app sees, and the espix function behind it. Used by the one symbol
 * resolver (abi_resolver.c) to shadow libc where espix has to: an app's sleep()
 * must be one a signal can cut short, and its getenv() must read the process's
 * own environment rather than the machine's.
 */
typedef struct {
    const char *name;
    uintptr_t   addr;
} abi_sym_t;

#define ABI_SYM(posix_name, fn) { (posix_name), (uintptr_t)(void *)(fn) }

/* Publish a table of overrides. Registration order decides a clash, and a
 * clash is reported. */
void espix_abi_resolver_add(const abi_sym_t *syms, size_t count);
void espix_proc_abi_resolver_register(void);

#if CONFIG_ESPIX_PROC_ABI_WATCHPOINT
/* Arm the watchpoints on the resolver's table state, on both cores. Call once,
 * after every espix_abi_resolver_add(). See the note in abi_resolver.c. */
void espix_proc_abi_watch_arm(void);
#endif
void espix_proc_abi_env_register(void);

void espix_proc_abi_signal_register(void);

/* The slot for `pid`, or NULL. Caller must hold the lock. */
espix_proc_slot_t *espix_proc_find(espix_pid_t pid);

/* The calling task's slot, or NULL if it is not a process. Takes no lock: the
 * caller is the process itself, so its slot cannot be recycled underneath it. */
espix_proc_slot_t *espix_proc_self(void);

/* End the calling process with `status`, running the same teardown a return
 * from the app's entry point does. Never returns. Called by the override table
 * in abi_exit.c; see the note there for why it is a longjmp rather than a
 * vTaskDelete(). */
void espix_proc_exit(int status) __attribute__((noreturn));

/* Publish exit()/_Exit()/_exit()/abort()/__assert_func to apps. See abi_exit.c:
 * this is a resolver table, not a symbol table, because the loader's own libc
 * table answers for `exit` and is searched first. */
void espix_proc_abi_exit_register(void);

/* Publish the allocator an app gets, under libc's names. See abi_alloc.c: it is a
 * resolver table because the loader's own answers for malloc and is searched
 * first, and it is where the per-process regions (R-P1.2) will attach. */
void espix_proc_abi_alloc_register(void);

/*
 * The allocator itself, so abi_cxx.cpp's operator new/delete reach the same one
 * the C names do. An app's C++ and C allocations must come from the same place
 * or only half of them are findable when the app ends.
 */
void  *espix_abi_alloc(size_t n);
void  *espix_abi_calloc(size_t n, size_t size);
void  *espix_abi_realloc(void *p, size_t n);
void   espix_abi_free(void *p);

/* True once this state means the process is over. */
bool espix_proc_state_is_finished(espix_proc_state_t s);

#ifdef __cplusplus
}
#endif
