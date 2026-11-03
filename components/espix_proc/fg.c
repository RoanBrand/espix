/*
 * The foreground wait: a shell's job, run to its end or until it stops.
 *
 * ^C and ^Z are the session's tty's: its line discipline signals the job's
 * group (and a third ^C insists with SIGKILL), so this only waits -- on the
 * notification a child's exit or stop sends its parent, or, for a job another
 * process started, by checking every 50 ms.
 */

#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_timer.h"

#include "espix_proc_priv.h"
#include "espix_shell.h"
#include "espix_tty.h"

#define FG_POLL_MS 50

/* After a time limit's SIGTERM, how long before SIGKILL. */
#define FG_KILL_GRACE_MS 2000

/* Not espix_printf(): the job may be mid-way through the session's buffer. */
static void say(espix_session_t *s, bool err, const char *fmt, ...)
{
    char    buf[96];
    va_list ap;

    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n > 0) {
        const size_t len = ((size_t)n < sizeof(buf)) ? (size_t)n : sizeof(buf) - 1;
        (void)espix_session_write(s, buf, len, err);
    }
}

/* A stopped job into the session's jobs, unless `fg` brought it from there, and said as bash says it. */
static void stopped_job(espix_session_t *s, espix_pid_t pid)
{
    char name[ESPIX_JOB_NAME_MAX] = "?";
    (void)espix_proc_name_of(pid, name, sizeof(name));
    espix_shell_jobs_prune(s);
    bool listed = false;
    for (int i = 0; i < ESPIX_SESSION_JOBS; i++) {
        listed = listed || s->jobs[i].pid == pid;
    }
    if (!listed && !espix_shell_job_add(s, pid, name)) {
        say(s, true, "espix: %d: no job slot; it stays stopped, untracked\n", (int)pid);
        return;
    }
    int n = 0;
    for (int i = 0; i < ESPIX_SESSION_JOBS; i++) {
        n += s->jobs[i].pid > 0;
        if (s->jobs[i].pid == pid) {
            break;
        }
    }
    say(s, false, "[%d]+  Stopped  %s\n", n, name);
}

void espix_proc_set_fg(espix_session_t *s, espix_pid_t pgid)
{
    s->fg_pgid = pgid;
    if (s->tty != NULL) {
        espix_tty_set_fg(s->tty, (pid_t)((pgid != ESPIX_PID_NONE) ? pgid : s->pid));
    }
}

void espix_proc_fg_spawned_locked(espix_session_t *s, const espix_proc_slot_t *parent, espix_pid_t pgid)
{
    if (s != NULL && s->tty != NULL && parent != NULL && parent->info.kind == ESPIX_PROC_SESSION) {
        espix_proc_set_fg(s, pgid);
    }
}

int espix_proc_wait_fg(espix_session_t *s, espix_pid_t pid, espix_proc_fg_t *fg)
{
    espix_proc_fg_t none = { 0 };
    if (fg == NULL) {
        fg = &none;
    }
    const char *const who = (fg->who != NULL) ? fg->who : "espix";
    fg->timed_out = false;
    fg->stopped   = false;

    const espix_proc_slot_t *const self  = espix_proc_self();
    const bool                     shell = self != NULL &&
                                           self->info.kind == ESPIX_PROC_SESSION;

    /* A child's exit and its stop notify its parent; a job some other process started is checked instead. */
    const bool notified = self != NULL && espix_proc_parent_of(pid) == self->info.pid;

    espix_proc_kind_t kind = ESPIX_PROC_APP;
    (void)espix_proc_kind_of(pid, &kind);

    espix_pid_t pgid = espix_proc_pgid_of(pid);
    if (pgid == ESPIX_PID_NONE) {
        pgid = pid;
    }
    if (shell) {
        espix_proc_set_fg(s, pgid);
    }
    struct termios before;
    const bool keep_tty = shell && s->tty != NULL;
    if (keep_tty) {
        espix_tty_getattr(s->tty, &before);
    }

    const int64_t deadline = (fg->limit_us > 0) ? esp_timer_get_time() + fg->limit_us
                                                : 0;
    int       exit_code  = -1;
    esp_err_t err        = ESP_ERR_TIMEOUT;
    bool      killed     = false;
    int64_t   term_at    = 0;

    for (;;) {
        const int64_t now = esp_timer_get_time();

        if (deadline > 0 && !fg->timed_out && now >= deadline) {
            say(s, true, "%s: pid %d: time limit reached\n", who, (int)pid);
            (void)espix_proc_signal_group(pgid, SIGTERM);
            fg->timed_out = true;
            term_at       = now;
        } else if (fg->timed_out && !killed &&
                   now >= term_at + (int64_t)FG_KILL_GRACE_MS * 1000) {
            say(s, true, "%s: pid %d: did not stop; killing it\n", who, (int)pid);
            (void)espix_proc_signal_group(pgid, SIGKILL);
            killed = true;
        }

        /* Until the next thing the limit has to do, or for ever. */
        int64_t until = 0;
        if (deadline > 0 && !fg->timed_out) {
            until = deadline;
        } else if (fg->timed_out && !killed) {
            until = term_at + (int64_t)FG_KILL_GRACE_MS * 1000;
        }
        const uint32_t ms = (until == 0) ? UINT32_MAX
                          : (uint32_t)((until > now ? until - now : 0) / 1000 + 1);

        err = espix_proc_wait(pid, &exit_code, 0);
        if (err != ESP_ERR_TIMEOUT) {
            break;
        }
        /* Stopped, by ^Z or a signal from elsewhere: a background job now, as in any shell. */
        if (shell && kind == ESPIX_PROC_APP && espix_proc_state_of(pid) == ESPIX_PROC_STOPPED) {
            fg->stopped = true;
            break;
        }
        if (notified) {
            (void)ulTaskNotifyTakeIndexed(ESPIX_NOTIFY_SIG_IDX, pdTRUE,
                                          (ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(ms));
        } else {
            const TickType_t ticks = shell ? pdMS_TO_TICKS(FG_POLL_MS)
                                   : (ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(ms);
            err = espix_proc_wait(pid, &exit_code, ticks);
            if (err != ESP_ERR_TIMEOUT) {
                break;
            }
        }
    }

    if (shell) {
        espix_proc_set_fg(s, ESPIX_PID_NONE);
    }
    /* A job a signal ended can leave the terminal as it set it, raw or silent: the shell's back, as bash does. */
    if (keep_tty && !fg->stopped && (killed || exit_code > 128)) {
        espix_tty_setattr(s->tty, TCSANOW, &before);
    }
    /* After a stop or a signal's end -- ^C, ^Z, a third ^C's SIGKILL, all echoed -- the next line is the shell's. */
    if (shell && s->tty != NULL && (fg->stopped || exit_code > 128)) {
        say(s, false, "\n");
    }
    if (fg->stopped) {
        stopped_job(s, pid);
        return 128 + SIGTSTP;
    }

    if (err != ESP_OK) {
        say(s, true, "%s: pid %d: %s\n", who, (int)pid, esp_err_to_name(err));
        return -1;
    }
    return exit_code;
}

bool espix_proc_sleep(uint32_t ms)
{
    const TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(ms);

    for (;;) {
        if (espix_sigcheck_io()) {
            return true;
        }
        const int32_t left = (int32_t)(end - xTaskGetTickCount());
        if (left <= 0) {
            return false;
        }
        (void)ulTaskNotifyTakeIndexed(ESPIX_NOTIFY_SIG_IDX, pdTRUE,
                                      (TickType_t)left);
    }
}
