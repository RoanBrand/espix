/*
 * espix process table: allocation, introspection, wait, kill.
 */

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>    /* strcasecmp, for `kill -TERM` */
#include <sys/stat.h>   /* stat, for chdir's directory check */

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "espix_kernel.h"
#include "espix_proc.h"
#include "espix_proc_priv.h"
#include "espix_tty.h"
#include "espix_umode.h"
#include "espix_fs.h"
#include "espix_shell.h"   /* session->task_gone, told when a kill orphans a lock */
#include "espix_pm.h"       /* ...and espix_pm_task_gone(), for the same reason */
#include "espix_audio.h"    /* ...and espix_audio_streams_recover(), likewise */

#define TAG "proc"

espix_proc_table_t  g_espix_proc_table;
SemaphoreHandle_t  g_espix_proc_lock;
EventGroupHandle_t g_espix_proc_events;

static espix_pid_t s_next_pid = 2;     /* 1 is init's; see spawn_fn */

/*
 * The completed log: where a status lives once the slot is gone.
 *
 * Before R-P1.5 the slot *was* the zombie, so a finished process kept its
 * table entry until a later spawn recycled it, and history competed with
 * concurrency inside a fixed table. Now espix_proc_finish() copies what a
 * listing or a wait needs in here and hands the slot straight back.
 *
 * Fixed and statically allocated because it is bookkeeping and must never be
 * the reason a spawn fails, and eight entries because a bounded history is
 * the resource-efficient version of what Linux keeps until a parent reaps.
 * A pid is never reused, so 'pid > 0' is the test for an entry being live;
 * before any finish every field is zero, and pid 1 is the first ever handed
 * out.
 *
 * A reaped entry stays listed: this is a history as well as the zombie store,
 * and 'reaped' only decides whether espix_proc_wait() will return it again.
 * The whole design is recorded in docs/WORKLIST.md under R-P1.5.
 */
static espix_proc_record_t s_done[ESPIX_PROC_DONE_MAX];
static unsigned            s_done_head;   /* next write; oldest once wrapped */
static uint32_t            s_done_seq;    /* total finishes, for ordering */

/* Write one completion into the ring. Called with the table lock held. */
static void done_record(const espix_proc_slot_t *slot, espix_proc_state_t state,
                        int exit_code)
{
    espix_proc_record_t *const r = &s_done[s_done_head];

    r->pid        = slot->info.pid;
    r->ppid       = slot->info.ppid;
    r->state      = state;
    r->exit_code  = exit_code;
    r->started_us = slot->info.started_us;
    r->ended_us   = esp_timer_get_time();
    r->reaped     = false;
    strlcpy(r->name, slot->info.name, sizeof(r->name));

    s_done_head = (s_done_head + 1u) % ESPIX_PROC_DONE_MAX;
    s_done_seq++;
}
/* The log entry for this pid, reaped or not, or NULL. Called with the lock
 * held. */
static espix_proc_record_t *done_find(espix_pid_t pid)
{
    for (unsigned i = 0; i < ESPIX_PROC_DONE_MAX; i++) {
        if (s_done[i].pid > 0 && s_done[i].pid == pid) {
            return &s_done[i];
        }
    }
    return NULL;
}

const char *espix_proc_state_str(espix_proc_state_t state)
{
    switch (state) {
    case ESPIX_PROC_FREE:    return "free";
    case ESPIX_PROC_READY:   return "ready";
    case ESPIX_PROC_RUNNING: return "run";
    case ESPIX_PROC_STOPPED: return "stop";
    case ESPIX_PROC_EXITED:  return "exit";
    case ESPIX_PROC_FAULTED: return "fault";
    case ESPIX_PROC_KILLED:  return "kill";
    default:                 return "?";
    }
}

/*
 * Note what is absent: ESPIX_PROC_STOPPED. A stopped process is alive and will
 * run again, so it must not be reaped by espix_proc_alloc_slot() looking for
 * something to recycle, and espix_proc_wait() must not report it as an exit.
 */
bool espix_proc_state_is_finished(espix_proc_state_t s)
{
    return s == ESPIX_PROC_EXITED || s == ESPIX_PROC_FAULTED ||
           s == ESPIX_PROC_KILLED;
}

static bool state_is_finished(espix_proc_state_t s)
{
    return espix_proc_state_is_finished(s);
}

/*
 * A session's shell. Over SSH the connection's own task runs it, so it keeps
 * the connection's name until sshd and its shell are separate processes;
 * everywhere else it is "sh".
 */
static void shell_begin(espix_session_t *s)
{
    const bool  ssh  = strncmp(s->name, "ssh", 3) == 0 ||
                       strcmp(s->name, "sftp") == 0;
    const char *name = ssh ? "sshd:conn" : "sh";
    if (espix_proc_adopt(s, name, &s->pid) == ESP_ERR_NO_MEM) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "%s: no process slot for the session",
                   s->user);
    }
    espix_proc_set_fg(s, ESPIX_PID_NONE);
}

static size_t shell_end(espix_session_t *s)
{
    const size_t ended = espix_proc_hangup(s);
    espix_proc_leave(s->last_status);
    s->pid = ESPIX_PID_NONE;
    return ended;
}

/* A command the shell runs as a process: its priority, any core. */
static esp_err_t shell_spawn(espix_session_t *s, espix_shell_spawn_t *sp,
                             espix_pid_t *pid)
{
    const espix_proc_fn_opts_t o = {
        .name           = sp->name,
        .fn             = sp->fn,
        .arg            = sp->arg,
        .kind           = ESPIX_PROC_BUILTIN,
        .stack          = sp->stack,
        .priority       = uxTaskPriorityGet(NULL),
        .core           = tskNO_AFFINITY,
        .internal_stack = sp->internal_stack,
        .session        = s,
        .pgid           = sp->pgid,
        .foreground     = sp->foreground,
    };
    const esp_err_t err = espix_proc_spawn_fn(&o, pid);
    if (err == ESP_OK) {
        /* Worked out again rather than looked up: it may have finished. */
        sp->pgid = espix_proc_group_for(espix_proc_self(), sp->pgid, *pid);
    }
    return err;
}

static int shell_wait(espix_session_t *s, espix_pid_t pid)
{
    return espix_proc_wait_fg(s, pid, NULL);
}

static bool shell_alive(espix_pid_t pid)
{
    return espix_proc_state_of(pid) != ESPIX_PROC_FREE;
}

static const espix_shell_proc_ops_t s_shell_ops = {
    .begin    = shell_begin,
    .end      = shell_end,
    .sigcheck = espix_sigcheck_io,
    .stopping = espix_proc_stopping,
    .spawn    = shell_spawn,
    .wait     = shell_wait,
    .alive    = shell_alive,
};

static int tty_signal_group(pid_t pgid, int sig)
{
    return (espix_proc_signal_group((espix_pid_t)pgid, sig) == ESP_OK) ? 0 : -ESRCH;
}

static pid_t tty_self_pgid(void)
{
    const espix_proc_slot_t *slot = espix_proc_self();
    return (slot != NULL) ? (pid_t)slot->info.pgid : 0;
}

static int tty_interrupted(void)
{
    return espix_sigcheck_io() ? -EINTR : 0;
}

static bool action_is_default(const espix_proc_slot_t *slot, int sig);
static bool sig_refused(const espix_proc_slot_t *slot, int sig);

/* A read of its terminal from a background group, as Linux answers it: EIO for a reader that cannot stop. */
static int tty_background(void)
{
    espix_proc_slot_t *slot = espix_proc_self();
    if (slot == NULL) {
        return 0;
    }
    if (slot->info.kind != ESPIX_PROC_APP || sig_refused(slot, SIGTTIN)) {
        return -EIO;
    }
    const bool caught = !action_is_default(slot, SIGTTIN);
    if (espix_proc_signal_group(slot->info.pgid, SIGTTIN) != ESP_OK) {
        return -EIO;
    }
    /* The stop parks here until SIGCONT; a handler instead ends the read. */
    return (espix_sigcheck_io() || caught) ? -EINTR : 0;
}

esp_err_t espix_proc_init(void)
{
    if (g_espix_proc_lock != NULL) {
        return ESP_OK;
    }

    g_espix_proc_lock = xSemaphoreCreateMutex();
    g_espix_proc_events = xEventGroupCreate();

    if (g_espix_proc_lock == NULL || g_espix_proc_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    memset(g_espix_proc_table.slots, 0, sizeof(g_espix_proc_table.slots));

    /* The canary, set once the table is clean. See espix_proc_table_t. */
    g_espix_proc_table.guard = ESPIX_PROC_GUARD_MAGIC;

    /* Two uint64_t per core, sampled from the tick; not a per-read scan. */
    if (espix_cpu_acct_init() != ESP_OK) {
        ESP_LOGW(TAG, "CPU time accounting unavailable");
    }

    /* The loader announces its version and entry address on every single load.
     * That is startup chatter, not something a user running an app wants to
     * see, so lift its threshold to warnings. */
    esp_log_level_set("ELF", ESP_LOG_WARN);

    /* Publish the C++ runtime before any app can be loaded. See abi_cxx.cpp:
     * espix itself is C, but a C++ app cannot resolve operator new without it. */
#if CONFIG_ESPIX_UMODE
    espix_um_init();
#endif

    /* The resolver first: every table below registers into it. */
    espix_proc_abi_resolver_register();

    espix_proc_abi_cxx_register();
    espix_proc_abi_drivers_register();
    espix_proc_abi_time_register();
    espix_proc_abi_signal_register();
    espix_shell_set_proc_ops(&s_shell_ops);
    espix_kthread_set_ops(&g_espix_kthread_ops);   /* adopts the early ones */
    espix_proc_abi_env_register();
    espix_proc_abi_exit_register();
    espix_proc_abi_alloc_register();
    espix_proc_abi_pthread_register();
    espix_proc_abi_stdio_register();
    espix_proc_abi_fs_register();
    espix_proc_abi_libc_register();
    espix_proc_abi_libm_register();
    espix_proc_abi_posix_register();

    /* A session's terminal signals its foreground group, and a read on it waits where a signal can end it. */
    static const espix_tty_hooks_t tty_hooks = {
        .signal_group = tty_signal_group,
        .self_pgid    = tty_self_pgid,
        .interrupted  = tty_interrupted,
        .background   = tty_background,
    };
    espix_tty_set_hooks(&tty_hooks);
    (void)espix_tty_init();
    espix_proc_abi_ident_register();
    espix_proc_abi_gfx_register();
    espix_proc_abi_audio_register();

#if CONFIG_ESPIX_PROC_ABI_WATCHPOINT
    /* After the last registration, so setup's own writes do not trip it. */
    espix_proc_abi_watch_arm();
#endif

    espix_klog(ESPIX_KLOG_INFO, TAG, "process table ready (%d slots)",
               ESPIX_PROC_MAX);
    return ESP_OK;
}

/* Set when the guard has been reported, so a corrupt table says so once rather
 * than on every spawn. Written without a lock: the worst a race does is print
 * the same line twice. */
static bool s_guard_reported;

/*
 * Is the word just past the table still the one put there? See
 * espix_proc_table_t.
 *
 * This does not replace the watchpoint and cannot be reached while it is armed:
 * a store to the guard traps in the debug unit first, which is the better of the
 * two answers because it names the writer. What it covers is the build where the
 * watchpoint is off -- CONFIG_ESPIX_PROC_ABI_WATCHPOINT is a Kconfig, and there
 * are only two watchpoint registers to go round -- and a board nobody is
 * attached to, where a log line is the whole of the evidence.
 */
bool espix_proc_table_intact(void)
{
    if (g_espix_proc_table.guard == ESPIX_PROC_GUARD_MAGIC) {
        return true;
    }

    if (!s_guard_reported) {
        s_guard_reported = true;
        espix_klog(ESPIX_KLOG_ERROR, TAG,
                   "process table guard reads %08x, not %08x -- something wrote "
                   "past the table",
                   (unsigned)g_espix_proc_table.guard,
                   (unsigned)ESPIX_PROC_GUARD_MAGIC);
    }
    return false;
}

espix_proc_slot_t *espix_proc_alloc_slot(void)
{
    /* Cheap, and this is the moment the table is about to be written. */
    (void)espix_proc_table_intact();

    /*
     * A finished process releases its slot in espix_proc_finish(), so the only
     * thing that can stand in a spawn's way is ESPIX_PROC_MAX processes
     * genuinely alive at once -- which is exactly the number this table is
     * sized for. There is no 'oldest finished' fallback any more: a terminal
     * state is never observable outside espix_proc_finish()'s critical
     * section, and a recycled slot arrives already zeroed.
     */
    for (int i = 0; i < ESPIX_PROC_MAX; i++) {
        espix_proc_slot_t *s = &g_espix_proc_table.slots[i];

        if (s->info.state == ESPIX_PROC_FREE) {
            return s;
        }
    }
    return NULL;
}

/*
 * Wake every task of `slot` that is waiting for a signal: its main task and its
 * threads. Caller holds the lock, which is what keeps them alive to be woken --
 * a live slot's tasks cannot finish without taking it. Only waits on
 * ESPIX_NOTIFY_SIG_IDX end; a lock or a join the target is waiting on does not.
 */
static void sig_wake_locked(espix_proc_slot_t *slot)
{
    const TaskHandle_t task = slot->info.task;

    if (task != NULL && task != xTaskGetCurrentTaskHandle()) {
        (void)xTaskNotifyGiveIndexed(task, ESPIX_NOTIFY_SIG_IDX);
    }
    espix_pthread_wake_locked(slot);
    /*
     * And any of them in esp_vfs_select(), waiting on a semaphore no notify
     * reaches: the wake eventfd is the only way in. Written only while one is
     * there, so a signal to a running process does not arm it and make its next
     * select() wake for nothing; and under the lock, which keeps it open. The
     * fence pairs with select_begin()'s: the pending bit, then in_select.
     */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (slot->in_select > 0 && slot->wake_fd >= 0) {
        (void)espix_fs_wake_notify(slot->wake_fd);
    }
#if CONFIG_ESPIX_UMODE
    /* And a thread computing in U on the other core, now rather than at its next tick. */
    if (slot->app != NULL && slot->app->um != NULL) {
        espix_um_sig_nudge(((const espix_um_thread_t *)slot->app->um)->satp);
    }
#endif
}

void espix_proc_release_resources(espix_proc_slot_t *slot)
{
    if (slot == NULL) {
        return;
    }

    /*
     * First of all, end any thread the process still has running.
     *
     * First, not last, because a thread is *running*: it can be inside the
     * process's memory, its descriptors or its stream at the moment the rest of
     * this takes them away.
     *
     * And here, not in the task-gone hook where the first version put it. A
     * graceful exit does not go through that hook -- proven on the board, where
     * Doom quitting to the desktop returned its arena (this function ran) and
     * still left a pthread at 98% of a core. This is the function both exits
     * run, so this is where the threads have to be ended. See abi_pthread.c.
     */
    espix_pthread_reap(slot);

    /*
     * First, whatever it left open. A process killed with files open never
     * runs its own close(), and the descriptors IDF allocated for them are a
     * fixed MAX_FDS pool shared with sockets -- so enough kills stop the whole
     * system opening a file. By pid rather than by slot because this runs on
     * the reaper's task, which is not the process. Before the memory below,
     * because the layer underneath may still need it in order to close.
     */
#if CONFIG_ESPIX_UMODE
    if (slot->app != NULL) {
        espix_um_close_fds(slot);
    }
#endif
    espix_fs_fds_close_owned(slot->info.pid);

    /* The wake eventfd is espix's, not one the app opened, so the owned-fd
     * sweep above does not know about it. Taken out under the lock, which its
     * writers hold. */
    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);
    const int wake_fd = slot->wake_fd;
    slot->wake_fd   = -1;
    slot->in_select = 0;
    xSemaphoreGive(g_espix_proc_lock);
    espix_fs_wake_close(wake_fd);

    /* An app's image, argv, environment and paths. The block itself goes
     * last: the process's tasks are gone, so nothing reads its paths now. */
    if (slot->app != NULL) {
#if CONFIG_ESPIX_UMODE
        espix_um_release(slot);
#endif
        if (slot->app->elf_valid) {
            esp_elf_deinit(&slot->app->elf);
            slot->app->elf_valid = false;
        }

        free(slot->app->image);
        slot->app->image = NULL;

        free(slot->app->argv_block);
        slot->app->argv_block = NULL;
        slot->app->argv = NULL;
        slot->app->argc = 0;

        free(slot->app->env_block);
        slot->app->env_block = NULL;
        slot->app->envp = NULL;

        for (int i = 0; i < slot->app->env_added_count; i++) {
            free(slot->app->env_added[i]);
            slot->app->env_added[i] = NULL;
        }
        slot->app->env_added_count = 0;

        free(slot->app);
        slot->app = NULL;
    }

    free(slot->sig_actions);
    slot->sig_actions = NULL;

    /* A finished slot must not hand its streams to the next process to use it. */
    slot->std[0] = slot->std[1] = slot->std[2] = NULL;

    /*
     * Safe to delete only because the process is already gone by the time this
     * runs -- either it returned from main(), or vTaskDelete() took it. A task
     * still blocked in xSemaphoreTake() on this handle would be left waiting on
     * freed memory.
     */
    if (slot->sig_cont != NULL) {
        vSemaphoreDelete(slot->sig_cont);
        slot->sig_cont = NULL;
    }
    slot->sig_stop_req = false;
    slot->sig_pending  = 0;
    slot->sig_blocked  = 0;

    /*
     * The alarm timer. Deleted here rather than left to the process, which may
     * never have run again -- a force-kill does not. esp_timer_delete() waits
     * for a callback already running, so the slot cannot be reused under one.
     */
    if (slot->alarm_timer != NULL) {
        (void)esp_timer_stop((esp_timer_handle_t)slot->alarm_timer);
        (void)esp_timer_delete((esp_timer_handle_t)slot->alarm_timer);
        slot->alarm_timer = NULL;
    }
    slot->alarm_deadline_us = 0;
    slot->alarm_interval_us = 0;

    /*
     * And the app's own memory. This is the point of the arena: one release per
     * region rather than a walk of every block, so a hard kill returns exactly
     * what a clean exit does -- and both paths already arrive here.
     */
    espix_proc_regions_release(slot);

    /*
     * And the screen, if this process was the one holding it. It is not memory
     * the slot owns, but it is a resource the process took and cannot give back
     * once it is gone -- and the one such resource whose leak wedges the whole
     * board rather than the process, because the desktop and the VNC encoder
     * take the same lock. R-P1.7: reclaimed here with everything else, rather
     * than by a hand call after the slot is already done.
     */
    espix_gfx_recover(slot->info.pid);
}

/* SIGCHLD to slot's parent, and its wake: a shell's foreground wait sees an exit or a stop. Under the lock. */
static void parent_wake_locked(espix_proc_slot_t *slot)
{
    if (slot->info.ppid == ESPIX_PID_NONE) {
        return;
    }
    for (int i = 0; i < ESPIX_PROC_MAX; i++) {
        espix_proc_slot_t *const p = &g_espix_proc_table.slots[i];
        if (p != slot && p->info.state != ESPIX_PROC_FREE &&
            p->info.pid == slot->info.ppid) {
            p->sig_pending |= espix_sigbit(SIGCHLD);
            sig_wake_locked(p);
            return;
        }
    }
}

void espix_proc_finish(espix_proc_slot_t *slot, espix_proc_state_t state,
                       int exit_code)
{
    if (slot == NULL) {
        return;
    }

    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);

    /*
     * Tell the parent, if it is still there. Found by walking the table rather
     * than by espix_proc_find(), which is defined below this and would need a
     * forward declaration to say the same thing.
     *
     * SIGCHLD is one of the signals espix's default action already ignores, so
     * this is inert for a parent that does not handle it -- which is what stops
     * a child's exit from ending the shell. The write is a single aligned word
     * and the reader clears it under its own delivery point, so no lock beyond
     * the one already held is needed.
     */
    parent_wake_locked(slot);

    /*
     * Into the log, then the slot straight back. Under one lock, so no one can
     * ever observe a slot in a terminal state: espix_proc_find() is live-only
     * and the log is the only record from here on. The memset is last, and the
     * slot pointer must not be used after this function returns -- the next
     * spawn may claim it immediately.
     */
    /* A shell or a kernel task leaves no record: nobody waits for one, and
     * logins and connections would push every app's exit out of the short
     * list ps shows. */
    if (slot->info.kind != ESPIX_PROC_SESSION &&
        slot->info.kind != ESPIX_PROC_KTHREAD) {
        done_record(slot, state, exit_code);
    }
    memset(slot, 0, sizeof(*slot));

    /*
     * Wake every waiter. One global bit rather than the old per-slot one: the
     * slot above may already belong to the next process by the time a waiter
     * looks, so a bit keyed to the slot index would be read by the wrong
     * waiter. The bit stays set until a waiter clears it, and every waiter
     * rescans the table and then the log, so a finish cannot be missed
     * whatever the order of the set and the scan.
     */
    xEventGroupSetBits(g_espix_proc_events, ESPIX_PROC_EVENT_FINISH);
    xSemaphoreGive(g_espix_proc_lock);
}

void espix_proc_reap_self(espix_proc_slot_t *slot, espix_proc_state_t state,
                          int exit_code)
{
    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);
    const bool ours = (slot->info.task == xTaskGetCurrentTaskHandle());
    if (ours) {
        slot->exiting = true;
    }
    xSemaphoreGive(g_espix_proc_lock);

    if (ours) {
        /*
         * The teardown runs here, as Linux's do_exit() does. The slot cannot
         * move underneath: `exiting` refuses a killer, and a spawn only takes a
         * FREE slot. Then the reaper frees this task's stack.
         */
        espix_proc_release_resources(slot);
        espix_proc_finish(slot, state, exit_code);
        espix_task_end();
    }

    /*
     * A killer already took the slot and the task: it nulls info.task under
     * this lock before deleting, so seeing that here means the teardown is
     * someone else's. Touch nothing; it will delete us.
     */
    vTaskSuspend(NULL);
    for (;;) {
    }
}

/*
 * The live slot for this pid, or NULL. Live-only since R-P1.5: a finished
 * process has no slot at all, so kill and every signal reach only running
 * processes -- Linux gives ESRCH for a process that is gone, and this is the
 * same answer for the same reason. The completed log is consulted by ps and
 * by espix_proc_wait() and by nothing else.
 */
espix_proc_slot_t *espix_proc_find(espix_pid_t pid)
{
    for (int i = 0; i < ESPIX_PROC_MAX; i++) {
        espix_proc_slot_t *const s = &g_espix_proc_table.slots[i];

        if (s->info.state != ESPIX_PROC_FREE &&
            !state_is_finished(s->info.state) && s->info.pid == pid) {
            return s;
        }
    }
    return NULL;
}

espix_pid_t espix_proc_group_for(const espix_proc_slot_t *parent,
                                 espix_pid_t asked, espix_pid_t pid)
{
    if (asked > 0) {
        return asked;
    }
    if (parent == NULL || parent->info.kind == ESPIX_PROC_SESSION ||
        parent->info.kind == ESPIX_PROC_KTHREAD) {
        return pid;
    }
    return parent->info.pgid;
}

espix_proc_slot_t *espix_proc_self(void)
{
    /*
     * An app's own task and its threads keep their slot in TLS: a pointer read
     * from the current task cannot belong to another task the way a recycled
     * handle could. The slot is freed only after its tasks are gone.
     */
    espix_proc_slot_t *const mine =
        (espix_proc_slot_t *)pvTaskGetThreadLocalStoragePointer(NULL,
                                                               ESPIX_TLS_PROC_IDX);
    if (mine != NULL && mine->info.state != ESPIX_PROC_FREE) {
        return mine;
    }

    /*
     * Any other process's task -- a session, a kernel process -- by its handle.
     * Unlocked, on the same reasoning espix_app_stopping() has always used: the
     * caller is the process itself, so the one slot that matters cannot be
     * recycled while it is asking, and taking the table lock here would let any
     * app stall every other process by blocking inside a delivery point.
     */
    const TaskHandle_t self = xTaskGetCurrentTaskHandle();
    for (int i = 0; i < ESPIX_PROC_MAX; i++) {
        if (g_espix_proc_table.slots[i].info.task == self &&
            g_espix_proc_table.slots[i].info.state != ESPIX_PROC_FREE) {
            return &g_espix_proc_table.slots[i];
        }
    }
    return NULL;
}

espix_pid_t espix_proc_next_pid(void)
{
    return s_next_pid++;
}

/*
 * The wait itself, shared by the reaping public call and the observe-only
 * internal one.
 *
 * `reap` decides whether a completed entry is consumed. POSIX's parent reaps;
 * everything that merely needs the process gone -- kill's own escalation, a
 * session hangup -- must not, or it would take the status away from whoever
 * is the parent.
 *
 * Table first, then log. The scan happens before every sleep, so a finish is
 * found by the scan or by the rescan after the wakeup, whatever the order.
 */
static esp_err_t proc_wait_common(espix_pid_t pid, int *out_exit_code,
                                  TickType_t timeout, bool reap)
{
    const TickType_t start = xTaskGetTickCount();

    for (;;) {
        xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);

        if (espix_proc_find(pid) != NULL) {
            /*
             * Still running. Wait for any finish and rescan; the bit is
             * global, so another process finishing first is a spurious
             * wakeup and not an error.
             */
            xSemaphoreGive(g_espix_proc_lock);

            TickType_t wait_ticks = portMAX_DELAY;

            if (timeout != portMAX_DELAY) {
                const TickType_t elapsed = xTaskGetTickCount() - start;

                if (elapsed >= timeout) {
                    return ESP_ERR_TIMEOUT;
                }
                wait_ticks = timeout - elapsed;
            }

            const EventBits_t got = xEventGroupWaitBits(
                g_espix_proc_events, ESPIX_PROC_EVENT_FINISH, pdTRUE, pdTRUE,
                wait_ticks);

            if ((got & ESPIX_PROC_EVENT_FINISH) == 0) {
                return ESP_ERR_TIMEOUT;
            }
            continue;
        }

        espix_proc_record_t *const rec = done_find(pid);

        if (rec == NULL) {
            xSemaphoreGive(g_espix_proc_lock);
            return ESP_ERR_NOT_FOUND;   /* never existed, or already forgotten */
        }
        if (reap && rec->reaped) {
            xSemaphoreGive(g_espix_proc_lock);
            return ESP_ERR_NOT_FOUND;   /* ECHILD: this status was already taken */
        }

        const int code = rec->exit_code;

        if (reap) {
            rec->reaped = true;
        }
        xSemaphoreGive(g_espix_proc_lock);

        if (out_exit_code != NULL) {
            *out_exit_code = code;
        }
        return ESP_OK;
    }
}

esp_err_t espix_proc_wait(espix_pid_t pid, int *out_exit_code, TickType_t timeout)
{
    return proc_wait_common(pid, out_exit_code, timeout, true);
}

/* Wait for a process to be gone without taking its exit status. Used where
 * espix is not the parent; see the note on proc_wait_common(). */
static esp_err_t proc_wait_gone(espix_pid_t pid, TickType_t timeout)
{
    return proc_wait_common(pid, NULL, timeout, false);
}

/*
 * The shutdown path: SIGTERM every live process but the caller, then wait for
 * the table to empty or the deadline to pass. Returns how many were still alive
 * when it gave up.
 *
 * No escalation, deliberately. espix_proc_kill()'s forced half is unsafe on a
 * shared address space -- a task deleted while holding the VFS lock or a
 * volume's is how a clean stop becomes a corrupt volume -- and a reset takes the
 * task anyway, so the force would buy nothing here.
 *
 * The wait is the process table's own finish event, so it ends when the last
 * process does rather than after a fixed grace.
 *
 * The caller is skipped. In the ordinary case that skips nothing -- the sequence
 * runs on a task that is not a process -- but the fallback when no stack can be
 * had for that task runs it on the caller's, and then signalling itself would
 * only make it wait for itself.
 */
size_t espix_proc_stop_all(int64_t deadline_us)
{
    /*
     * Heap, not stack: this is a whole process table, and the caller is whichever
     * task asked for the shutdown -- a session, the desktop, a unit. None of
     * those stacks is guaranteed to have room for it on top of what it is doing.
     */
    espix_proc_info_t *live = calloc(ESPIX_PROC_MAX, sizeof(*live));
    if (live == NULL) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "cannot list processes to stop them");
        return 0;
    }

    const espix_pid_t self = espix_proc_self_pid();

    size_t n     = espix_proc_snapshot(live, ESPIX_PROC_MAX);
    size_t asked = 0;

    for (size_t i = 0; i < n; i++) {
        if (live[i].pid == self || live[i].kind == ESPIX_PROC_SESSION) {
            continue;
        }
        if (espix_proc_signal(live[i].pid, SIGTERM) == ESP_OK) {
            asked++;
        }
    }

    if (asked > 0) {
        espix_klog(ESPIX_KLOG_INFO, TAG, "SIGTERM to %u process%s; waiting",
                   (unsigned)asked, (asked == 1) ? "" : "es");
    }

    size_t left = 0;

    for (;;) {
        n = espix_proc_snapshot(live, ESPIX_PROC_MAX);

        /* A kernel thread or a shell is not asked, so it is not waited for. */
        left = 0;
        for (size_t i = 0; i < n; i++) {
            if (live[i].pid != self && live[i].kind != ESPIX_PROC_KTHREAD &&
                live[i].kind != ESPIX_PROC_SESSION) {
                left++;
            }
        }
        if (left == 0) {
            break;
        }

        const int64_t now_us = esp_timer_get_time();
        if (now_us >= deadline_us) {
            break;
        }

        TickType_t wait = pdMS_TO_TICKS((uint32_t)((deadline_us - now_us) / 1000));
        if (wait == 0) {
            wait = 1;
        }
        (void)xEventGroupWaitBits(g_espix_proc_events, ESPIX_PROC_EVENT_FINISH,
                                  pdTRUE, pdTRUE, wait);
    }

    if (left > 0) {
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "%u process%s did not stop in time; the reset will take them",
                   (unsigned)left, (left == 1) ? "" : "es");
    }

    free(live);
    return left;
}

/*
 * How long a process gets to leave on its own after SIGTERM.
 *
 * Longer than the 400ms this used to be, and the reason is that a signal now
 * wakes a sleeping process instead of waiting for it to look up. An app that
 * cooperates answers in about the time it takes to run its handler, so the
 * grace is no longer the common path -- it is only what an app that ignores
 * SIGTERM costs you. Spending that on letting a cleanup routine flush a file
 * is a better trade than cutting one short.
 */
#define TERM_GRACE_MS 2000

/*
 * Names without the "SIG", indexed by signal number, for `kill -l` and the
 * name form of `kill -TERM`. Indexed by the <signal.h> macro rather than a
 * literal, so this stays correct if the numbering ever moves under us -- which
 * is not hypothetical, since these are the BSD numbers and not Linux's.
 */
static const char *const s_signames[NSIG] = {
    [SIGHUP] = "HUP",   [SIGINT] = "INT",    [SIGQUIT] = "QUIT",
    [SIGILL] = "ILL",   [SIGTRAP] = "TRAP",  [SIGABRT] = "ABRT",
    [SIGEMT] = "EMT",   [SIGFPE] = "FPE",    [SIGKILL] = "KILL",
    [SIGBUS] = "BUS",   [SIGSEGV] = "SEGV",  [SIGSYS] = "SYS",
    [SIGPIPE] = "PIPE", [SIGALRM] = "ALRM",  [SIGTERM] = "TERM",
    [SIGURG] = "URG",   [SIGSTOP] = "STOP",  [SIGTSTP] = "TSTP",
    [SIGCONT] = "CONT", [SIGCHLD] = "CHLD",  [SIGTTIN] = "TTIN",
    [SIGTTOU] = "TTOU", [SIGIO] = "IO",      [SIGXCPU] = "XCPU",
    [SIGXFSZ] = "XFSZ", [SIGVTALRM] = "VTALRM", [SIGPROF] = "PROF",
    [SIGWINCH] = "WINCH", [SIGLOST] = "LOST", [SIGUSR1] = "USR1",
    [SIGUSR2] = "USR2",
};

const char *espix_signal_name(int sig)
{
    return (sig > 0 && sig < NSIG) ? s_signames[sig] : NULL;
}

int espix_signal_from_name(const char *name)
{
    if (name == NULL || *name == '\0') {
        return -1;
    }

    if (isdigit((unsigned char)*name)) {
        char      *end = NULL;
        const long v   = strtol(name, &end, 10);

        return (end != NULL && *end == '\0' && v > 0 && v < NSIG) ? (int)v : -1;
    }

    if (strncasecmp(name, "SIG", 3) == 0) {
        name += 3;
    }
    for (int sig = 1; sig < NSIG; sig++) {
        if (s_signames[sig] != NULL && strcasecmp(name, s_signames[sig]) == 0) {
            return sig;
        }
    }
    return -1;
}

/*
 * Signals whose default action is to do nothing. Everything else espix names
 * defaults to terminating, which here means setting stop_requested and letting
 * the process leave on its own.
 */
bool espix_sig_default_ignores(int sig)
{
    switch (sig) {
    case SIGCHLD:       /* there are no children to report on */
    case SIGURG:
    case SIGWINCH:
    case SIGCONT:       /* continuing already happened; nothing left to do */
        return true;
    default:
        return false;
    }
}

bool espix_sig_default_stops(int sig)
{
    return sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU;
}

static void set_state(espix_proc_slot_t *slot, espix_proc_state_t st)
{
    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);
    /* Never walk a finished process back to life: it may be mid-teardown. */
    if (!state_is_finished(slot->info.state)) {
        slot->info.state = st;
        if (st == ESPIX_PROC_STOPPED) {
            parent_wake_locked(slot);
        }
    }
    xSemaphoreGive(g_espix_proc_lock);
}

/*
 * Run the handlers for everything pending and unblocked. Returns the signals
 * whose handler ran: only a caught signal interrupts a sleep, so an ignored one
 * -- SIGCONT from fg, SIGCHLD -- must not end it.
 *
 * One pass, not a loop until empty: a handler that re-raises its own signal
 * would spin here for ever. A signal that arrives while a handler is running is
 * picked up at the next delivery point instead, which is a delay of microseconds
 * in an app that blocks and no worse than the alternative in one that does not.
 */
/* The signals whose action now would be to terminate the process. */
static uint32_t sig_terminating(const espix_proc_slot_t *slot)
{
    uint32_t mask = 0;

    for (int sig = 1; sig < NSIG; sig++) {
        const bool dfl = slot->sig_actions == NULL ||
                         slot->sig_actions[sig].handler == SIG_DFL;
        if (dfl && !espix_sig_default_ignores(sig) && !espix_sig_default_stops(sig)) {
            mask |= espix_sigbit(sig);
        }
    }
    return mask;
}

/*
 * A U-mode process is ended on its way back to U (espix_um_return()), never by a
 * longjmp out of a kernel call: that would skip the call's own cleanup.
 */
static bool ends_in_user_mode(const espix_proc_slot_t *slot)
{
#if CONFIG_ESPIX_UMODE
    return slot->app != NULL && slot->app->um != NULL;
#else
    (void)slot;
    return false;
#endif
}

/*
 * A handler, in place, with what sigaction() promises around it: its mask and
 * the signal itself (unless SA_NODEFER) blocked until it returns, SIG_DFL back
 * first under SA_RESETHAND, and a siginfo_t under SA_SIGINFO.
 */
static void run_handler(espix_proc_slot_t *slot, int sig, const espix_sigact_t *act)
{
    if (act->flags & SA_RESETHAND) {
        slot->sig_actions[sig].handler = SIG_DFL;
    }
    const uint32_t saved = slot->sig_blocked;
    slot->sig_blocked = (saved | act->mask | ((act->flags & SA_NODEFER) ? 0 : espix_sigbit(sig))) &
                        ~ESPIX_SIG_UNCATCHABLE;
    if (act->flags & SA_SIGINFO) {
        siginfo_t info = { .si_signo = sig, .si_code = SI_USER };
        ((void (*)(int, siginfo_t *, void *))(void *)act->handler)(sig, &info, NULL);
    } else {
        act->handler(sig);      /* app code; no lock held, by design */
    }
    slot->sig_blocked = saved;
}

/* sig is at SIG_DFL in slot's process. */
static bool action_is_default(const espix_proc_slot_t *slot, int sig)
{
#if CONFIG_ESPIX_UMODE
    if (ends_in_user_mode(slot)) {
        return ((const espix_um_thread_t *)slot->app->um)->proc->act[sig].handler == 0;
    }
#endif
    return slot->sig_actions == NULL || slot->sig_actions[sig].handler == SIG_DFL;
}

/* Blocked by the caller, or ignored by its process. */
static bool sig_refused(const espix_proc_slot_t *slot, int sig)
{
#if CONFIG_ESPIX_UMODE
    if (ends_in_user_mode(slot)) {
        return espix_um_sig_refused(slot, sig);
    }
#endif
    return (slot->sig_blocked & espix_sigbit(sig)) ||
           (slot->sig_actions != NULL && slot->sig_actions[sig].handler == SIG_IGN);
}

static uint32_t sig_dispatch(espix_proc_slot_t *slot, bool may_terminate)
{
    /*
     * Terminating is the process's own task's to do, and only where it holds
     * nothing: the default action is a longjmp out of whatever called in. So a
     * thread runs handlers and leaves such a signal pending for the main task,
     * which the sender has already woken; and so does a stdio read callback
     * (espix_sigcheck_io()), which runs inside fgets() with the FILE's lock
     * held -- jumping out of it left that lock taken, and the exit path's
     * fclose() asserted.
     *
     * Decided before the lock, so a thread that has nothing it may deliver
     * returns without taking it. One that retries an EINTR in a loop otherwise
     * takes the table lock on every pass and starves everyone else of it.
     */
#if CONFIG_ESPIX_UMODE
    if (ends_in_user_mode(slot)) {
        return espix_um_sig_claim(slot);    /* its handlers run in U, on the way back */
    }
#endif
    uint32_t mine = ~slot->sig_blocked;
    if (!may_terminate || slot->info.task != xTaskGetCurrentTaskHandle()) {
        mine &= ~sig_terminating(slot);
    }

    /*
     * Fast path, unlocked. espix_app_stopping() sits in apps' inner loops and
     * used to cost two linear scans of the table; it must not now cost a mutex
     * on every iteration. Two volatile loads, and a reader one iteration behind
     * is harmless.
     */
    if ((slot->sig_pending & mine) == 0) {
        return 0;
    }

    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);
    const uint32_t deliver = slot->sig_pending & mine;
    slot->sig_pending &= ~deliver;
    xSemaphoreGive(g_espix_proc_lock);

    uint32_t caught = 0;

    for (int sig = 1; sig < NSIG; sig++) {
        if ((deliver & espix_sigbit(sig)) == 0) {
            continue;
        }

        /*
         * sig_actions is only ever written by this process, from this task, so
         * reading it here needs no lock. Another task can free it -- but only
         * after the task is gone, which is to say never while we are here.
         */
        const espix_sigact_t act = (slot->sig_actions != NULL)
                                       ? slot->sig_actions[sig]
                                       : (espix_sigact_t){ .handler = SIG_DFL };

        if (act.handler == SIG_IGN) {
            continue;
        }
        if (act.handler != SIG_DFL) {
            run_handler(slot, sig, &act);
            caught |= espix_sigbit(sig);
            continue;
        }
        if (espix_sig_default_stops(sig)) {
            (void)espix_proc_signal(slot->info.pid, SIGSTOP);
            continue;
        }
        if (!espix_sig_default_ignores(sig)) {
            /*
             * POSIX's default action for this signal is "terminate", and a
             * default action is defined by needing nothing from the process --
             * which is why it may not be cooperative. It used to set
             * stop_requested and wait for the app to notice at its next
             * espix_sigcheck(), so a process that never asked to be stopped was
             * left running until something escalated to SIGKILL.
             *
             * An app that wants to put its hardware back installs a handler and
             * never arrives here. For an Arduino sketch that is the shim's job
             * and not espix's: neopixel catches SIGTERM/SIGINT/SIGHUP precisely
             * so that teardown() runs, and espix does not bend its semantics to
             * give a sketch what the shim can give it.
             *
             * This runs on the process's own task (a thread left it pending,
             * above), at a delivery point it reached itself, with no lock held
             * (the table lock is released above the loop), so ending the
             * process is the ordinary exit:
             * streams closed, the ELF released, the slot marked EXITED with
             * 128+sig, and that status reaches whoever ran it. The same door an
             * app's own exit() uses.
             */
            espix_proc_exit(128 + sig);
        }
    }

    return caught;
}

/*
 * Park while SIGSTOP stands.
 *
 * The process suspends *itself*, here, at a point where it demonstrably holds
 * no libc, VFS or heap lock. Suspending it from outside would be the Unix
 * behaviour and is not available: vTaskSuspend() on a task parked inside
 * malloc() or a VFS call holds that mutex for as long as the stop lasts and
 * wedges every other task that touches it -- the same hazard the fault reaper
 * refuses to accept, and worse here because both cores are live.
 */
static void sig_park(espix_proc_slot_t *slot, bool may_terminate)
{
    while (slot->sig_stop_req) {
        if (slot->sig_cont == NULL) {
            slot->sig_stop_req = false;     /* nothing to park on */
            break;
        }

        /*
         * Discard a continue token left over from a stop that was lifted before
         * this process got round to parking, or this stop would end the instant
         * it began. Anything arriving after this point is a real SIGCONT, and
         * the semaphore remembers it even if it lands before the take below --
         * which is the whole reason this is a semaphore and not vTaskResume().
         */
        (void)xSemaphoreTake(slot->sig_cont, 0);

        if (!slot->sig_stop_req) {
            break;                          /* lifted while we tidied up */
        }

        set_state(slot, ESPIX_PROC_STOPPED);
        (void)xSemaphoreTake(slot->sig_cont, portMAX_DELAY);
        set_state(slot, ESPIX_PROC_RUNNING);

        /* Pass the continue on: the process's threads may be parked here too,
         * and one give wakes one taker. A token nobody takes is discarded by
         * the next stop, above. */
        if (!slot->sig_stop_req) {
            (void)xSemaphoreGive(slot->sig_cont);
        }

        /* Whatever woke us may have been a signal rather than SIGCONT. */
        (void)sig_dispatch(slot, may_terminate);
    }
}

uint32_t espix_sigcheck_mask(void)
{
    espix_proc_slot_t *slot = espix_proc_self();

    if (slot == NULL) {
        return 0;           /* not a process; nobody is signalling it */
    }

    uint32_t got = sig_dispatch(slot, true);

    sig_park(slot, true);

    if (slot->stop_requested ||
        (ends_in_user_mode(slot) && espix_proc_termsig(slot) != 0)) {
        got |= ESPIX_SIG_STOPPING;
    }
    return got;
}

bool espix_sigcheck(void)
{
    return (espix_sigcheck_mask() & ESPIX_SIG_STOPPING) != 0;
}

bool espix_sigcheck_io(void)
{
    espix_proc_slot_t *slot = espix_proc_self();

    if (slot == NULL) {
        return false;
    }

    const uint32_t got = sig_dispatch(slot, false);
    sig_park(slot, false);

    /* A U-mode thread's caught signal ends the read too: its handler runs in U, after it. */
    return slot->stop_requested || espix_proc_termsig(slot) != 0 ||
           (ends_in_user_mode(slot) && got != 0);
}

int espix_proc_termsig(const espix_proc_slot_t *slot)
{
#if CONFIG_ESPIX_UMODE
    if (ends_in_user_mode(slot)) {
        return espix_um_termsig(slot);
    }
#endif
    const uint32_t due = slot->sig_pending & ~slot->sig_blocked &
                         sig_terminating(slot);

    for (int sig = 1; sig < NSIG; sig++) {
        if (due & espix_sigbit(sig)) {
            return sig;
        }
    }
    return 0;
}

bool espix_proc_stopping(void)
{
    const espix_proc_slot_t *slot = espix_proc_self();

    return slot != NULL && slot->stop_requested;
}

/*
 * SIGKILL. Deleting another task on a system with no memory protection is a
 * blunt instrument: anything it held at the time -- a VFS mutex, a heap block,
 * an open file -- stays held or leaked. We reclaim what the process table owns
 * (its ELF image and argv) and nothing more. This is the same accepted tradeoff
 * described in the project's crash-handling model, and the reason espix_fault's
 * reaper exists as a separate, deferred path.
 */
/*
 * How long proc_force_kill() lets a process leave a libc call before deleting
 * it. Long enough for one tick plus the unwinding; short enough that `kill -9`
 * still feels immediate.
 */
#define KILL_UNWIND_MS 250

/*
 * Let the target out of any libc call it is inside, then delete it.
 *
 * Deleting a task that is blocked *inside stdio* leaves that FILE's lock held
 * by a task that no longer exists, and the next user of the stream waits on it
 * forever. Under newlib that next user was the deletion itself -- its teardown
 * fclosed the dead task's streams -- and the board reset: measured, with a
 * control, `kill -9` on a process blocked in fgets(stdin) reset it every time,
 * while the same kill on one blocked in sleep() was harmless, and SIGTERM on
 * the blocked one was harmless too because it unwinds first. The teardown no
 * longer touches stdio, but an orphaned lock is still an orphaned lock: on the
 * console it is the kernel's own stdout.
 *
 * So do what SIGTERM demonstrably does, without making SIGKILL catchable.
 * Setting stop_requested is not a signal -- no handler runs -- but it is what
 * espix_sigcheck() reports, and espix's blocking calls poll that and return.
 * A process reading stdin therefore leaves fgets() on its own, releases the
 * lock and usually exits, so there is nothing left to delete.
 *
 * An app that ignores it is deleted anyway when the grace runs out, exactly as
 * before: this weakens `kill -9`'s promptness by a quarter of a second and
 * weakens nothing else.
 */
static void kill_unwind(espix_pid_t pid, TaskHandle_t task)
{
    if (task == NULL || task == xTaskGetCurrentTaskHandle()) {
        return;
    }

    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);
    espix_proc_slot_t *slot = espix_proc_find(pid);
    SemaphoreHandle_t cont = NULL;
    if (slot != NULL) {
        slot->stop_requested = true;
        sig_wake_locked(slot);
        /* A stopped process wakes to leave, as SIGKILL wakes one on Linux. */
        if (slot->sig_stop_req) {
            slot->sig_stop_req = false;
            cont = slot->sig_cont;
            if (slot->info.state == ESPIX_PROC_STOPPED) {
                slot->info.state = ESPIX_PROC_RUNNING;
            }
        }
    }
    xSemaphoreGive(g_espix_proc_lock);
    if (cont != NULL) {
        (void)xSemaphoreGive(cont);
    }
    /*
     * Finished on its own is the good outcome, and the common one. Observing
     * only: the kill did not ask for the status and must not consume it, or
     * the process's parent would get nothing from its own wait.
     */
    (void)proc_wait_gone(pid, pdMS_TO_TICKS(KILL_UNWIND_MS));
}

/*
 * A signal to a process that is not an app. It has no handlers and may not be
 * deleted, so anything that would end it is a stop request, which it answers
 * at its own pace; SIGKILL too. A kernel thread is not asked at all. Under the
 * lock.
 */
static esp_err_t sig_kernel_locked(espix_proc_slot_t *slot, int sig)
{
    /* A shell is not asked either: a stop request would mute it for good, and
     * logging out is how it ends. */
    if (slot->info.kind == ESPIX_PROC_KTHREAD ||
        slot->info.kind == ESPIX_PROC_SESSION || sig == SIGSTOP ||
        espix_sig_default_stops(sig)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (espix_sig_default_ignores(sig)) {
        return ESP_OK;
    }
    if (slot->stop_sig == 0) {
        slot->stop_sig = sig;
    }
    slot->stop_requested = true;
    sig_wake_locked(slot);
    return ESP_OK;
}

void espix_proc_stop_tasks(espix_proc_slot_t *slot)
{
    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);
    slot->stop_requested = true;
    sig_wake_locked(slot);
    xSemaphoreGive(g_espix_proc_lock);
}

static esp_err_t proc_force_kill(espix_pid_t pid)
{
    /*
     * Outside the table lock: espix_proc_wait() blocks, and holding the lock
     * across it would stop the very task we are waiting for from finishing.
     */
    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);
    espix_proc_slot_t *pre = espix_proc_find(pid);
    if (pre != NULL && pre->info.kind != ESPIX_PROC_APP &&
        !state_is_finished(pre->info.state)) {
        const esp_err_t r = sig_kernel_locked(pre, SIGKILL);
        xSemaphoreGive(g_espix_proc_lock);
        return r;
    }
    /*
     * An app killing itself, never torn down under its own call: its main task leaves as exit() does, a
     * U-mode app on its way back to U, and an M-mode thread's process at its next delivery point.
     */
    if (pre != NULL && pre == espix_proc_self()) {
        pre->stop_requested = true;
        sig_wake_locked(pre);
        const bool main_task = !ends_in_user_mode(pre) && pre->info.task == xTaskGetCurrentTaskHandle();
        xSemaphoreGive(g_espix_proc_lock);
        if (main_task) {
            espix_proc_exit(128 + SIGKILL);
        }
        return ESP_OK;
    }
    TaskHandle_t       pre_task = (pre != NULL) ? pre->info.task : NULL;
    const bool         pre_live = pre != NULL && !state_is_finished(pre->info.state);
    xSemaphoreGive(g_espix_proc_lock);

    kill_unwind(pid, pre_task);

    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);

    /* Gone during the unwind is this kill done, as it is for any app that leaves when asked. */
    espix_proc_slot_t *slot = espix_proc_find(pid);
    if (slot == NULL) {
        xSemaphoreGive(g_espix_proc_lock);
        return pre_live ? ESP_OK : ESP_ERR_NOT_FOUND;
    }
    if (state_is_finished(slot->info.state)) {
        xSemaphoreGive(g_espix_proc_lock);
        return pre_live ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

    /*
     * Already tearing itself down: that teardown owns the slot and the task,
     * and a killer that proceeded would free the image under it, or leave the
     * locks its flushes hold for good. It is ending, which is what was asked.
     */
    if (slot->exiting) {
        xSemaphoreGive(g_espix_proc_lock);
        return ESP_OK;
    }

    /* Still loading: deleted now, it would keep the filesystem's locks. It leaves when the load is done. */
    if (slot->loading) {
        slot->stop_requested = true;
        xSemaphoreGive(g_espix_proc_lock);
        return ESP_OK;
    }

    /*
     * A second kill of the same process -- or a kill racing the session hangup
     * that lands on the same slot -- must not run the teardown below twice. The
     * first killer clears info.task under this lock and then releases it before
     * vTaskDelete(), so a live slot with no task is one already being killed.
     * Two of them would free the ELF image while the first is still inside
     * vTaskDelete(), with the victim possibly still executing it.
     */
    if (slot->info.task == NULL) {
        xSemaphoreGive(g_espix_proc_lock);
        return ESP_ERR_INVALID_STATE;
    }

    TaskHandle_t task = slot->info.task;

    /* Never this task: suspending ourselves here wedges the caller instead of
     * killing anything, and deleting ourselves abandons everything below.
     * kill_unwind() already refuses the same case. */
    if (task == xTaskGetCurrentTaskHandle()) {
        task = NULL;
    }

    /*
     * Suspended under the lock, deleted outside it, and the order is the whole
     * correctness argument -- do not tidy the suspend out of here.
     *
     * A process has two possible deleters: itself, via vTaskDelete(NULL) at the
     * end of proc_task(), and this function. Both used to be able to fire. The
     * handle was copied here, the lock released, and only then deleted -- and in
     * that window the process could finish and delete itself, leaving this to
     * free a TCB that was already gone. Two deletes of one task corrupts the
     * scheduler's lists, and what that looks like is IDLE taking an
     * IllegalInstruction inside 0xa5a5a5a5 -- filled stack, no longer a stack --
     * followed by an assert that it cannot select a task. `kill -9` on an app
     * that was writing hard reproduced it on the first try, every try.
     *
     * The lock closes it because a process cannot reach its own vTaskDelete()
     * without passing through espix_proc_finish(), which takes this same lock.
     * So while it is held the process is in one of two decidable states: already
     * finished, in which case its handle may be stale and the early return above
     * has already left it alone; or not yet in finish(), in which case it has
     * not self-deleted and cannot, once suspended, ever get there.
     */
    bool detached = false;
    if (task != NULL) {
        vTaskSuspend(task);

        /*
         * And before deleting it, forget its streams. They are not closed --
         * that needs the FILE lock a process killed mid-write still holds -- but
         * the slot stops pointing at them. See espix_proc_detach_streams().
         */
        detached = espix_proc_detach_streams(slot);
    }
    slot->info.task = NULL;

    /* Copy the name while the lock still protects it: the slot can be recycled
     * the moment espix_proc_finish() below wakes whoever was waiting. The
     * session goes with it, for the same reason and for task_gone() below. */
    char name[ESPIX_PROC_NAME_MAX];
    strlcpy(name, slot->info.name, sizeof(name));
    espix_session_t *const session = slot->info.session;

    xSemaphoreGive(g_espix_proc_lock);

    if (task != NULL) {
        vTaskDeleteWithCaps(task);      /* frees the PSRAM stack it was given */

        /*
         * And tell the transport which task just died.
         *
         * A process deleted inside a write still holds its channel's transmit
         * lock, and only a mutex's owner may give it back -- so that lock is
         * orphaned for the life of the channel. It is not merely unavailable:
         * FreeRTOS walks the recorded holder's TCB for priority inheritance on
         * every subsequent take, and that TCB has just been freed. The
         * transport checks whether this task was the holder and, if so, stops
         * using the lock at all. Nothing else in the system knows both facts at
         * the same moment.
         */
        if (session != NULL && session->task_gone != NULL) {
            session->task_gone(session, (void *)task);
        }
        /*
         * And the same for any power-management request it was holding. They
         * are counted rather than flagged so that they *can* be released by
         * name like this: a radio kept awake by a session that no longer exists
         * is the WifiLock bug, and it is silent and permanent.
         */
        espix_pm_task_gone((void *)task);

        /*
         * And any audio stream it was holding. A killed player that keeps its
         * stream goes on being mixed -- as a producer that will never write
         * again -- and the sink never suspends while it is "open".
         */
        espix_audio_streams_recover((void *)task);
    }

    /* Said out loud, and only once the lock is down. "stdio detached" means this
     * kill went through the path that used to strand a session or panic the
     * board -- worth seeing happen, rather than inferring it from an absence of
     * panics. */
    espix_klog(ESPIX_KLOG_WARN, TAG, "killed pid %d (%s)%s", (int)pid, name,
               detached ? " [stdio detached]" : "");

    espix_proc_release_resources(slot);
    espix_proc_finish(slot, ESPIX_PROC_KILLED, -1);

    return ESP_OK;
}

esp_err_t espix_proc_signal(espix_pid_t pid, int sig)
{
    if (espix_sigbit(sig) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (sig == SIGKILL) {
        return proc_force_kill(pid);
    }

    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);

    espix_proc_slot_t *slot = espix_proc_find(pid);
    if (slot == NULL) {
        xSemaphoreGive(g_espix_proc_lock);
        return ESP_ERR_NOT_FOUND;
    }
    if (state_is_finished(slot->info.state)) {
        xSemaphoreGive(g_espix_proc_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (slot->info.kind != ESPIX_PROC_APP) {
        const esp_err_t r = sig_kernel_locked(slot, sig);
        xSemaphoreGive(g_espix_proc_lock);
        return r;
    }

    /* Ctrl-Z's stop, and a terminal's: SIGSTOP's action, unless the app catches them. */
    if (espix_sig_default_stops(sig) && action_is_default(slot, sig)) {
        sig = SIGSTOP;
    }

    const bool was_stopped = slot->sig_stop_req;

    if (sig == SIGSTOP) {
        if (slot->sig_cont == NULL) {
            slot->sig_cont = xSemaphoreCreateBinary();
        }
        if (slot->sig_cont == NULL) {
            xSemaphoreGive(g_espix_proc_lock);
            return ESP_ERR_NO_MEM;
        }
        slot->sig_stop_req = true;
    } else {
        slot->sig_pending |= espix_sigbit(sig);

        /*
         * Any other signal lifts a stop, as bash's `kill` follows one to a
         * stopped job with SIGCONT: a stopped process left holding a SIGTERM
         * would sit on it until the grace ran out and then be deleted, cleanup
         * and all. Running from now, not from when it wakes: `fg` waits next
         * and must not see it stopped.
         */
        slot->sig_stop_req = false;
        if (was_stopped && slot->info.state == ESPIX_PROC_STOPPED) {
            slot->info.state = ESPIX_PROC_RUNNING;
        }
    }

    SemaphoreHandle_t cont = slot->sig_cont;

    /*
     * Wake it, and its threads. The pending bits are already set and a notify
     * is remembered, so a target racing us into a wait returns from it at once.
     * Only a wait for a signal ends: never a lock or a join, which is what
     * xTaskAbortDelay() used to break. Signalling yourself skips your own task
     * -- you are not waiting -- and raise() calls espix_sigcheck() directly.
     */
    sig_wake_locked(slot);

    xSemaphoreGive(g_espix_proc_lock);

    if (sig != SIGSTOP && was_stopped && cont != NULL) {
        (void)xSemaphoreGive(cont);
    }

    return ESP_OK;
}

int espix_proc_select_begin(uint32_t *got)
{
    espix_proc_slot_t *slot = espix_proc_self();
    if (got != NULL) {
        *got = 0;
    }
    if (slot == NULL) {
        return -1;
    }
    /* An app has one from spawn; espix's own code gets one when it first
     * waits. The teardown closes it either way. */
    if (slot->wake_fd < 0) {
        slot->wake_fd = espix_fs_wake_create();
    }

    __atomic_add_fetch(&slot->in_select, 1, __ATOMIC_SEQ_CST);

    /*
     * Deliver whatever is pending before blocking. A signal that arrives after
     * this line finds in_select set and writes the fd, so there is no window in
     * which a signal is set but nobody is coming to write it.
     */
    const uint32_t r = espix_sigcheck_mask();
    if (got != NULL) {
        *got = r;
    }

    return slot->wake_fd;
}

void espix_proc_select_end(void)
{
    espix_proc_slot_t *slot = espix_proc_self();
    if (slot != NULL) {
        __atomic_sub_fetch(&slot->in_select, 1, __ATOMIC_SEQ_CST);
    }
}

esp_err_t espix_proc_signal_group(espix_pid_t pgid, int sig)
{
    if (pgid <= 0 || (sig != 0 && espix_sigbit(sig) == 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    espix_pid_t pids[ESPIX_PROC_MAX];
    size_t      n = 0;

    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);
    for (int i = 0; i < ESPIX_PROC_MAX; i++) {
        const espix_proc_info_t *const info = &g_espix_proc_table.slots[i].info;
        if (info->state != ESPIX_PROC_FREE && info->pgid == pgid &&
            !state_is_finished(info->state)) {
            pids[n++] = info->pid;
        }
    }
    xSemaphoreGive(g_espix_proc_lock);

    if (n == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (sig == 0) {
        return ESP_OK;              /* kill(-pgid, 0): the group exists */
    }
    /* The caller last: a signal that ends it may not return. */
    const espix_pid_t self = espix_proc_self_pid();
    bool              mine = false;
    esp_err_t         err  = ESP_ERR_INVALID_STATE;
    for (size_t i = 0; i < n; i++) {
        if (pids[i] == self) {
            mine = true;
        } else if (espix_proc_signal(pids[i], sig) == ESP_OK) {
            err = ESP_OK;
        }
    }
    if (mine && espix_proc_signal(self, sig) == ESP_OK) {
        err = ESP_OK;
    }
    return err;
}

espix_pid_t espix_proc_pgid_of(espix_pid_t pid)
{
    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);
    const espix_proc_slot_t *const slot = espix_proc_find(pid);
    const espix_pid_t pgid = (slot != NULL) ? slot->info.pgid : ESPIX_PID_NONE;
    xSemaphoreGive(g_espix_proc_lock);
    return pgid;
}

espix_pid_t espix_proc_sid_of(espix_pid_t pid)
{
    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);
    const espix_proc_slot_t *const slot = espix_proc_find(pid);
    const espix_pid_t sid = (slot != NULL) ? slot->info.sid : ESPIX_PID_NONE;
    xSemaphoreGive(g_espix_proc_lock);
    return sid;
}

esp_err_t espix_proc_request_stop(espix_pid_t pid)
{
    return espix_proc_signal(pid, SIGTERM);
}

esp_err_t espix_proc_kill(espix_pid_t pid)
{
    /*
     * Ask before deleting. An app that takes the hint gets to put its hardware
     * back — an LED off, a motor stopped — which deleting the task outright
     * never allows. An app that ignores it is no worse off than before, just
     * TERM_GRACE_MS later.
     */
    const esp_err_t asked = espix_proc_signal(pid, SIGTERM);
    if (asked != ESP_OK) {
        return asked;           /* no such pid, or already finished */
    }

    if (proc_wait_gone(pid, pdMS_TO_TICKS(TERM_GRACE_MS)) == ESP_OK) {
        espix_klog(ESPIX_KLOG_INFO, TAG, "pid %d stopped on request", (int)pid);
        return ESP_OK;
    }

    espix_proc_kind_t kind = ESPIX_PROC_APP;
    if (espix_proc_kind_of(pid, &kind) && kind != ESPIX_PROC_APP) {
        /* Only an app is deleted; espix's own code is left to finish. */
        espix_klog(ESPIX_KLOG_WARN, TAG, "pid %d did not stop when asked, "
                   "and is not an app to delete", (int)pid);
        return ESP_ERR_TIMEOUT;
    }

    espix_klog(ESPIX_KLOG_WARN, TAG, "pid %d did not stop when asked", (int)pid);
    return proc_force_kill(pid);
}

size_t espix_proc_hangup(const espix_session_t *session)
{
    if (session == NULL) {
        return 0;
    }

    /*
     * The session's pids first, then signal: espix_proc_signal() takes the
     * table lock itself, and a process may well finish on its own in between --
     * which it reports and we ignore, because that is the outcome we wanted.
     * Pids rather than a snapshot of each slot: this runs on the ending
     * session's own stack.
     */
    espix_pid_t pids[ESPIX_PROC_MAX];
    size_t      n = 0;

    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);
    for (int i = 0; i < ESPIX_PROC_MAX; i++) {
        const espix_proc_info_t *const info = &g_espix_proc_table.slots[i].info;
        if (info->state != ESPIX_PROC_FREE && info->session == session &&
            info->kind != ESPIX_PROC_SESSION && !state_is_finished(info->state)) {
            pids[n++] = info->pid;
        }
    }
    xSemaphoreGive(g_espix_proc_lock);

    bool   targeted[ESPIX_PROC_MAX] = { false };
    size_t ended = 0;

    for (size_t i = 0; i < n; i++) {
        targeted[i] = (espix_proc_signal(pids[i], SIGHUP) == ESP_OK);
    }

    /*
     * One grace shared across all of them rather than one each: a session with
     * three apps on it should not hold the connection open for three times as
     * long, and a hangup is usually the last thing a dropped SSH channel does
     * before its stdio goes away.
     */
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(TERM_GRACE_MS);

    for (size_t i = 0; i < n; i++) {
        if (!targeted[i]) {
            continue;
        }

        const int32_t left = (int32_t)(deadline - xTaskGetTickCount());

        if (proc_wait_gone(pids[i], (left > 0) ? (TickType_t)left : 0)
            != ESP_OK) {
            (void)proc_force_kill(pids[i]);
        }
        ended++;
    }

    return ended;
}

size_t espix_proc_snapshot(espix_proc_info_t *out, size_t n)
{
    if (out == NULL || n == 0) {
        return 0;
    }

    size_t count = 0;

    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);
    for (int i = 0; i < ESPIX_PROC_MAX && count < n; i++) {
        if (g_espix_proc_table.slots[i].info.state != ESPIX_PROC_FREE) {
            out[count++] = g_espix_proc_table.slots[i].info;
        }
    }
    xSemaphoreGive(g_espix_proc_lock);

    return count;
}

/*
 * The completed log, oldest first. s_done_head is the next write, which is the
 * oldest entry once the ring has wrapped; before that the entries run from
 * zero and the head is simply one past the newest.
 */
size_t espix_proc_history(espix_proc_record_t *out, size_t n)
{
    if (out == NULL || n == 0) {
        return 0;
    }

    size_t count = 0;

    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);

    const uint32_t wrapped = (s_done_seq >= (uint32_t)ESPIX_PROC_DONE_MAX);
    const unsigned total   = wrapped ? (unsigned)ESPIX_PROC_DONE_MAX
                                     : (unsigned)s_done_seq;
    unsigned       idx     = wrapped ? s_done_head : 0u;

    for (unsigned i = 0; i < total && count < n; i++) {
        out[count++] = s_done[idx];
        idx = (idx + 1u) % ESPIX_PROC_DONE_MAX;
    }

    xSemaphoreGive(g_espix_proc_lock);

    return count;
}

bool espix_proc_name_of(espix_pid_t pid, char *out, size_t len)
{
    if (out == NULL || len == 0) {
        return false;
    }

    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);
    const espix_proc_slot_t *slot = espix_proc_find(pid);
    const bool found = (slot != NULL);
    if (found) {
        strlcpy(out, slot->info.name, len);
    }
    xSemaphoreGive(g_espix_proc_lock);

    return found;
}

espix_proc_state_t espix_proc_state_of(espix_pid_t pid)
{
    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);
    const espix_proc_slot_t *slot  = espix_proc_find(pid);
    const espix_proc_state_t state = (slot != NULL) ? slot->info.state
                                                    : ESPIX_PROC_FREE;
    xSemaphoreGive(g_espix_proc_lock);

    return state;
}

espix_pid_t espix_proc_pid_of_task(TaskHandle_t task)
{
    if (task == NULL) {
        return ESPIX_PID_NONE;
    }

    /* Lock-free on purpose: the fault handler calls this from panic context,
     * where taking a mutex is not an option. */
    for (int i = 0; i < ESPIX_PROC_MAX; i++) {
        if (g_espix_proc_table.slots[i].info.task == task) {
            return g_espix_proc_table.slots[i].info.pid;
        }
    }
    return ESPIX_PID_NONE;
}

bool espix_proc_cred_of_task(TaskHandle_t task, uint16_t *uid, uint16_t *gid,
                             uint16_t *groups, uint8_t *ngroups)
{
    if (task == NULL) {
        return false;
    }

    for (int i = 0; i < ESPIX_PROC_MAX; i++) {
        const espix_proc_info_t *info = &g_espix_proc_table.slots[i].info;

        if (info->task != task) {
            continue;
        }
        /* A shell's credentials, and a command's, are its session's, live --
         * sudo changes them -- so the caller asks the session instead. */
        if (info->kind == ESPIX_PROC_SESSION ||
            info->kind == ESPIX_PROC_BUILTIN) {
            return false;
        }
        if (uid != NULL) {
            *uid = info->uid;
        }
        if (gid != NULL) {
            *gid = info->gid;
        }
        if (groups != NULL && ngroups != NULL) {
            *ngroups = info->ngroups;
            for (uint8_t j = 0; j < info->ngroups; j++) {
                groups[j] = info->groups[j];
            }
        }
        return true;
    }
    return false;
}

/*
 * Working directory and root of the calling process, in one lookup.
 *
 * Both together because the VFS wants both on every path it resolves, and
 * espix_proc_self() is a walk of the process table: asking twice doubles the
 * cost of the single hottest operation in the system, and for the callers that
 * are not processes at all -- the console, an SSH connection task, SNTP, the
 * WiFi driver -- the walk runs to the end and finds nothing either time.
 *
 * Takes no lock, deliberately; a mutex here would serialise all I/O behind the
 * process table. espix_proc_self() is safe without one for the reason given on
 * its declaration: the caller *is* the process, so its slot cannot be recycled
 * underneath it. The only writer of `cwd` is that same task, in
 * espix_proc_chdir(), and `root` is written once at spawn.
 *
 * The defaults are what a task espix does not know as a process gets: "/" to
 * resolve against, which is what those callers used before a cwd existed, and
 * "" for the root, meaning unconfined.
 */
void espix_proc_paths(const char **cwd, const char **root)
{
    const espix_proc_slot_t *const slot = espix_proc_self();
    const espix_proc_app_t  *const app  = (slot != NULL) ? slot->app : NULL;

    /* An app has its own cwd; a shell or a command has its session's -- a
     * job's or a pipe stage's own copy; anything else gets "/" and no root, as
     * a kernel task does. */
    if (cwd != NULL) {
        const espix_session_t *const s =
            (app == NULL && slot != NULL &&
             (slot->info.kind == ESPIX_PROC_SESSION ||
              slot->info.kind == ESPIX_PROC_BUILTIN)) ? espix_shell_current()
                                                       : NULL;
        if (app != NULL) {
            *cwd = (app->cwd[0] == '\0') ? "/" : app->cwd;
        } else if (s != NULL && s->cwd[0] != '\0') {
            *cwd = s->cwd;
        } else {
            *cwd = "/";
        }
    }
    if (root != NULL) {
        *root = (app == NULL || !app->root_active) ? "" : app->root;
    }
}

espix_pid_t espix_proc_self_pid(void)
{
    const espix_proc_slot_t *const slot = espix_proc_self();
    return (slot != NULL) ? slot->info.pid : ESPIX_PID_NONE;
}

espix_pid_t espix_proc_parent_of(espix_pid_t pid)
{
    if (g_espix_proc_lock == NULL) {
        return ESPIX_PID_NONE;
    }

    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);
    const espix_proc_slot_t *const slot = espix_proc_find(pid);
    const espix_pid_t ppid = (slot != NULL) ? slot->info.ppid : ESPIX_PID_NONE;
    xSemaphoreGive(g_espix_proc_lock);

    return ppid;
}

const char *espix_proc_cwd(void)
{
    const char *cwd = "/";

    espix_proc_paths(&cwd, NULL);
    return cwd;
}

/*
 * Root of the calling process, or "" for one that has none. The VFS uses
 * espix_proc_paths() instead; this is for callers that want only the root, of
 * which there is one -- the never-widen check in espix_proc_spawn_elf().
 */
const char *espix_proc_root(void)
{
    const char *root = "";

    espix_proc_paths(NULL, &root);
    return root;
}

struct espix_tty *espix_proc_std_tty(void)
{
    const espix_proc_slot_t *slot = espix_proc_self();
    return (slot != NULL) ? slot->std_tty : NULL;
}

bool espix_proc_loaded(void)
{
    espix_proc_slot_t *slot = espix_proc_self();
    if (slot == NULL) {
        return true;
    }
    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);
    slot->loading = false;
    const bool stop = slot->stop_requested;
    xSemaphoreGive(g_espix_proc_lock);
    if (slot->app != NULL) {
        slot->app->root_active = true;
    }
    return !stop;
}

esp_err_t espix_proc_chdir(const char *abs_path)
{
    espix_proc_slot_t *slot = espix_proc_self();

    if (abs_path == NULL || abs_path[0] != '/') {
        return ESP_ERR_INVALID_ARG;
    }
    if (slot == NULL || slot->app == NULL) {
        /* A kernel task has nowhere to record one, and the sessions that do
         * have a cwd manage it themselves. */
        return ESP_ERR_INVALID_STATE;
    }

    struct stat st;
    if (stat(abs_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return ESP_ERR_NOT_FOUND;
    }

    strlcpy(slot->app->cwd, abs_path, sizeof(slot->app->cwd));
    return ESP_OK;
}
