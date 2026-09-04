/*
 * espix process table: allocation, introspection, wait, kill.
 */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>    /* strcasecmp, for `kill -TERM` */
#include <sys/stat.h>   /* stat, for chdir's directory check */

#include "esp_log.h"

#include "espix_kernel.h"
#include "espix_proc.h"
#include "espix_proc_priv.h"

#define TAG "proc"

espix_proc_slot_t  g_espix_procs[ESPIX_PROC_MAX];
SemaphoreHandle_t  g_espix_proc_lock;
EventGroupHandle_t g_espix_proc_events;

static espix_pid_t s_next_pid = 1;

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

    memset(g_espix_procs, 0, sizeof(g_espix_procs));

    /* The loader announces its version and entry address on every single load.
     * That is startup chatter, not something a user running an app wants to
     * see, so lift its threshold to warnings. */
    esp_log_level_set("ELF", ESP_LOG_WARN);

    /* Publish the C++ runtime before any app can be loaded. See abi_cxx.cpp:
     * espix itself is C, but a C++ app cannot resolve operator new without it. */
    espix_proc_abi_cxx_register();
    espix_proc_abi_drivers_register();
    espix_proc_abi_time_register();
    espix_proc_abi_signal_register();
    espix_proc_abi_fs_register();

    espix_klog(ESPIX_KLOG_INFO, TAG, "process table ready (%d slots)",
               ESPIX_PROC_MAX);
    return ESP_OK;
}

/*
 * Clear this slot's "finished" bit before anything waits on it.
 *
 * espix_proc_wait() calls xEventGroupWaitBits() with xClearOnExit false, so
 * that several waiters can all see one exit -- which means nothing ever cleared
 * the bit either. A slot reused by a later process therefore started life with
 * its predecessor's bit already set, and the first espix_proc_wait() on it
 * returned "finished" immediately.
 *
 * The pid re-check in espix_proc_wait() cannot catch that: the pid in the slot
 * *is* the one being waited on, because the process is real and just started.
 * So `run` believed a freshly spawned app had already exited, reported its
 * predecessor's exit code, and moved on while the app was still starting.
 *
 * Harmless-looking for a long time, because the shell simply returned to a
 * prompt a fraction early. It surfaced over `ssh host <cmd>`, where returning
 * early means finish_session() closes the channel underneath a still-running
 * app: output truncated at whatever had made it out, and no exit status, so the
 * client reported 255.
 *
 * Cleared at allocation rather than at exit because allocation is the point
 * where the slot changes identity, and it happens under the table lock with
 * the task not yet created -- so no one can be waiting on it yet.
 */
static void clear_finished_bit(const espix_proc_slot_t *slot)
{
    const int index = (int)(slot - g_espix_procs);
    xEventGroupClearBits(g_espix_proc_events, (EventBits_t)1 << index);
}

espix_proc_slot_t *espix_proc_alloc_slot(void)
{
    espix_proc_slot_t *oldest_done = NULL;

    for (int i = 0; i < ESPIX_PROC_MAX; i++) {
        espix_proc_slot_t *s = &g_espix_procs[i];

        if (s->info.state == ESPIX_PROC_FREE) {
            clear_finished_bit(s);
            return s;
        }
        if (state_is_finished(s->info.state)) {
            if (oldest_done == NULL ||
                s->info.started_us < oldest_done->info.started_us) {
                oldest_done = s;
            }
        }
    }

    if (oldest_done != NULL) {
        /* Reclaiming a finished slot: its resources were released when it
         * finished, so only the bookkeeping needs clearing. */
        memset(oldest_done, 0, sizeof(*oldest_done));
        clear_finished_bit(oldest_done);
    }
    return oldest_done;
}

void espix_proc_release_resources(espix_proc_slot_t *slot)
{
    if (slot == NULL) {
        return;
    }

    if (slot->elf_valid) {
        esp_elf_deinit(&slot->elf);
        slot->elf_valid = false;
    }

    free(slot->image);
    slot->image = NULL;

    free(slot->argv_block);
    slot->argv_block = NULL;
    slot->argv = NULL;
    slot->argc = 0;

    free(slot->sig_handlers);
    slot->sig_handlers = NULL;

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
}

void espix_proc_finish(espix_proc_slot_t *slot, espix_proc_state_t state,
                       int exit_code)
{
    if (slot == NULL) {
        return;
    }

    const int index = (int)(slot - g_espix_procs);

    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);
    slot->info.state     = state;
    slot->info.exit_code = exit_code;
    slot->info.task      = NULL;
    xSemaphoreGive(g_espix_proc_lock);

    xEventGroupSetBits(g_espix_proc_events, (EventBits_t)1 << index);
}

espix_proc_slot_t *espix_proc_find(espix_pid_t pid)
{
    for (int i = 0; i < ESPIX_PROC_MAX; i++) {
        if (g_espix_procs[i].info.state != ESPIX_PROC_FREE &&
            g_espix_procs[i].info.pid == pid) {
            return &g_espix_procs[i];
        }
    }
    return NULL;
}

static espix_proc_slot_t *find_by_pid(espix_pid_t pid)
{
    return espix_proc_find(pid);
}

espix_proc_slot_t *espix_proc_self(void)
{
    const TaskHandle_t self = xTaskGetCurrentTaskHandle();

    /*
     * Unlocked, on the same reasoning espix_app_stopping() has always used: the
     * caller is the process itself, so the one slot that matters cannot be
     * recycled while it is asking, and taking the table lock here would let any
     * app stall every other process by blocking inside a delivery point.
     */
    for (int i = 0; i < ESPIX_PROC_MAX; i++) {
        if (g_espix_procs[i].info.task == self &&
            g_espix_procs[i].info.state != ESPIX_PROC_FREE) {
            return &g_espix_procs[i];
        }
    }
    return NULL;
}

espix_pid_t espix_proc_next_pid(void)
{
    return s_next_pid++;
}

esp_err_t espix_proc_wait(espix_pid_t pid, int *out_exit_code, TickType_t timeout)
{
    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);
    espix_proc_slot_t *slot = find_by_pid(pid);
    const int index = (slot != NULL) ? (int)(slot - g_espix_procs) : -1;
    const espix_proc_state_t state = (slot != NULL) ? slot->info.state
                                                    : ESPIX_PROC_FREE;
    xSemaphoreGive(g_espix_proc_lock);

    if (slot == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    if (!state_is_finished(state)) {
        const EventBits_t bit = (EventBits_t)1 << index;
        const EventBits_t got = xEventGroupWaitBits(g_espix_proc_events, bit,
                                                    pdFALSE, pdTRUE, timeout);
        if ((got & bit) == 0) {
            return ESP_ERR_TIMEOUT;
        }
    }

    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);
    /* Re-check identity: the slot could have been recycled while we waited. */
    const bool same = (slot->info.pid == pid);
    if (same && out_exit_code != NULL) {
        *out_exit_code = slot->info.exit_code;
    }
    xSemaphoreGive(g_espix_proc_lock);

    return same ? ESP_OK : ESP_ERR_NOT_FOUND;
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
static bool sig_default_ignores(int sig)
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

static void set_state(espix_proc_slot_t *slot, espix_proc_state_t st)
{
    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);
    /* Never walk a finished process back to life: it may be mid-teardown. */
    if (!state_is_finished(slot->info.state)) {
        slot->info.state = st;
    }
    xSemaphoreGive(g_espix_proc_lock);
}

/*
 * Run the handlers for everything pending and unblocked. Returns what was
 * delivered.
 *
 * One pass, not a loop until empty: a handler that re-raises its own signal
 * would spin here for ever. A signal that arrives while a handler is running is
 * picked up at the next delivery point instead, which is a delay of microseconds
 * in an app that blocks and no worse than the alternative in one that does not.
 */
static uint32_t sig_dispatch(espix_proc_slot_t *slot)
{
    /*
     * Fast path, unlocked. espix_app_stopping() sits in apps' inner loops and
     * used to cost two linear scans of the table; it must not now cost a mutex
     * on every iteration. Two volatile loads, and a reader one iteration behind
     * is harmless.
     */
    if ((slot->sig_pending & ~slot->sig_blocked) == 0) {
        return 0;
    }

    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);
    const uint32_t deliver = slot->sig_pending & ~slot->sig_blocked;
    slot->sig_pending &= ~deliver;
    xSemaphoreGive(g_espix_proc_lock);

    for (int sig = 1; sig < NSIG; sig++) {
        if ((deliver & espix_sigbit(sig)) == 0) {
            continue;
        }

        /*
         * sig_handlers is only ever written by this process, from this task, so
         * reading it here needs no lock. Another task can free it -- but only
         * after the task is gone, which is to say never while we are here.
         */
        void (*handler)(int) = (slot->sig_handlers != NULL)
                                   ? slot->sig_handlers[sig]
                                   : SIG_DFL;

        if (handler == SIG_IGN) {
            continue;
        }
        if (handler != SIG_DFL) {
            handler(sig);       /* app code; no lock held, by design */
            continue;
        }
        if (!sig_default_ignores(sig)) {
            slot->stop_requested = true;
        }
    }

    return deliver;
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
static void sig_park(espix_proc_slot_t *slot)
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

        /* Whatever woke us may have been a signal rather than SIGCONT. */
        (void)sig_dispatch(slot);
    }
}

uint32_t espix_sigcheck_mask(void)
{
    espix_proc_slot_t *slot = espix_proc_self();

    if (slot == NULL) {
        return 0;           /* not a process; nobody is signalling it */
    }

    uint32_t got = sig_dispatch(slot);

    sig_park(slot);

    if (slot->stop_requested) {
        got |= ESPIX_SIG_STOPPING;
    }
    return got;
}

bool espix_sigcheck(void)
{
    return (espix_sigcheck_mask() & ESPIX_SIG_STOPPING) != 0;
}

/*
 * SIGKILL. Deleting another task on a system with no memory protection is a
 * blunt instrument: anything it held at the time -- a VFS mutex, a heap block,
 * an open file -- stays held or leaked. We reclaim what the process table owns
 * (its ELF image and argv) and nothing more. This is the same accepted tradeoff
 * described in the project's crash-handling model, and the reason espix_fault's
 * reaper exists as a separate, deferred path.
 */
static esp_err_t proc_force_kill(espix_pid_t pid)
{
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

    TaskHandle_t task = slot->info.task;
    slot->info.task = NULL;

    /* Copy the name while the lock still protects it: the slot can be recycled
     * the moment espix_proc_finish() below wakes whoever was waiting. */
    char name[ESPIX_PROC_NAME_MAX];
    strlcpy(name, slot->info.name, sizeof(name));

    xSemaphoreGive(g_espix_proc_lock);

    if (task != NULL) {
        vTaskDelete(task);
    }

    espix_klog(ESPIX_KLOG_WARN, TAG, "killed pid %d (%s)", (int)pid, name);

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
         * Any other signal lifts a stop. POSIX would leave the process stopped
         * with the signal pending until SIGCONT, but espix has no `fg` to
         * deliver that: a stopped process left holding a SIGTERM would sit on
         * it until the grace ran out and then be deleted, cleanup and all. So a
         * stop here yields to anything else, and the process gets to act.
         */
        slot->sig_stop_req = false;
    }

    TaskHandle_t      task = slot->info.task;
    SemaphoreHandle_t cont = slot->sig_cont;

    xSemaphoreGive(g_espix_proc_lock);

    /*
     * Wake it. The pending bits are already set, so a target racing us into a
     * blocking call sees them rather than sleeping through them -- which is why
     * this is after the unlock and not before the write.
     *
     * xTaskAbortDelay returns pdFAIL for a task that was not blocked, and that
     * is fine: it means the target is running and will reach a delivery point
     * on its own. Signalling yourself skips it -- you are, definitionally, not
     * blocked -- and raise() calls espix_sigcheck() directly instead.
     */
    if (sig != SIGSTOP && was_stopped && cont != NULL) {
        (void)xSemaphoreGive(cont);
    }
    if (task != NULL && task != xTaskGetCurrentTaskHandle()) {
        (void)xTaskAbortDelay(task);
    }

    return ESP_OK;
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

    int exit_code = -1;
    if (espix_proc_wait(pid, &exit_code, pdMS_TO_TICKS(TERM_GRACE_MS)) == ESP_OK) {
        espix_klog(ESPIX_KLOG_INFO, TAG, "pid %d stopped on request", (int)pid);
        return ESP_OK;
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
     * Snapshot first, then signal: espix_proc_signal() takes the table lock
     * itself, and a process may well finish on its own in between — which it
     * reports and we ignore, because that is the outcome we wanted anyway.
     */
    espix_proc_info_t procs[ESPIX_PROC_MAX];
    const size_t      n = espix_proc_snapshot(procs, ESPIX_PROC_MAX);

    bool   targeted[ESPIX_PROC_MAX] = { false };
    size_t ended = 0;

    for (size_t i = 0; i < n; i++) {
        if (procs[i].session != session || state_is_finished(procs[i].state)) {
            continue;
        }
        targeted[i] = (espix_proc_signal(procs[i].pid, SIGHUP) == ESP_OK);
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

        if (espix_proc_wait(procs[i].pid, NULL, (left > 0) ? (TickType_t)left : 0)
            != ESP_OK) {
            (void)proc_force_kill(procs[i].pid);
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
        if (g_espix_procs[i].info.state != ESPIX_PROC_FREE) {
            out[count++] = g_espix_procs[i].info;
        }
    }
    xSemaphoreGive(g_espix_proc_lock);

    return count;
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
        if (g_espix_procs[i].info.task == task) {
            return g_espix_procs[i].info.pid;
        }
    }
    return ESPIX_PID_NONE;
}

bool espix_proc_cred_of_task(TaskHandle_t task, uint16_t *uid, uint16_t *gid)
{
    if (task == NULL) {
        return false;
    }

    for (int i = 0; i < ESPIX_PROC_MAX; i++) {
        if (g_espix_procs[i].info.task == task) {
            if (uid != NULL) {
                *uid = g_espix_procs[i].info.uid;
            }
            if (gid != NULL) {
                *gid = g_espix_procs[i].info.gid;
            }
            return true;
        }
    }
    return false;
}

/*
 * Working directory of the calling process.
 *
 * Takes no lock, deliberately, because espix's VFS calls this on every file
 * operation in the system -- a mutex here would serialise all I/O behind the
 * process table. espix_proc_self() is safe without one for the reason given on
 * its declaration: the caller *is* the process, so its slot cannot be recycled
 * underneath it. The only writer of `cwd` is that same task, in
 * espix_proc_chdir().
 *
 * "/" for a task espix does not know as a process -- the console, an SSH
 * connection task, SNTP, the WiFi driver -- which is what they resolved
 * against before this existed.
 */
const char *espix_proc_cwd(void)
{
    const espix_proc_slot_t *slot = espix_proc_self();

    if (slot == NULL || slot->cwd[0] == '\0') {
        return "/";
    }
    return slot->cwd;
}

esp_err_t espix_proc_chdir(const char *abs_path)
{
    espix_proc_slot_t *slot = espix_proc_self();

    if (abs_path == NULL || abs_path[0] != '/') {
        return ESP_ERR_INVALID_ARG;
    }
    if (slot == NULL) {
        /* A kernel task has nowhere to record one, and the sessions that do
         * have a cwd manage it themselves. */
        return ESP_ERR_INVALID_STATE;
    }

    struct stat st;
    if (stat(abs_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return ESP_ERR_NOT_FOUND;
    }

    strlcpy(slot->cwd, abs_path, sizeof(slot->cwd));
    return ESP_OK;
}
