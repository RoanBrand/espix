/*
 * The foreground wait: a shell's job, run to its end, with ^C.
 *
 * Nothing else reads the terminal while a job runs, so the shell does. It
 * blocks in the transport's wait_input() until a key arrives or the job ends;
 * a transport without one is polled every 50 ms. A waiter that is not a
 * shell, or a job that reads the terminal itself, waits for the exit alone.
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

#define FG_POLL_MS 50

/*
 * Ctrl-C presses before the shell stops asking and starts insisting. SIGINT is
 * all a press sends, so an app may catch it and tidy up; but espix may have one
 * console and no second terminal to run `kill -9` from, so the third press
 * forces it, and says so first.
 */
#define FG_INTERRUPTS_TO_KILL 3

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

int espix_proc_wait_fg(espix_session_t *s, espix_pid_t pid, espix_proc_fg_t *fg)
{
    espix_proc_fg_t none = { 0 };
    if (fg == NULL) {
        fg = &none;
    }
    const char *const who = (fg->who != NULL) ? fg->who : "espix";
    fg->timed_out = false;

    const espix_proc_slot_t *const self  = espix_proc_self();
    const bool                     shell = self != NULL &&
                                           self->info.kind == ESPIX_PROC_SESSION;
    const bool poll = shell && !fg->tty && s->poll_interrupt != NULL;

    /*
     * Blocking rather than polling needs both wakes: the transport's for a key,
     * and the exit notification, which goes to the job's parent -- so not for a
     * job some other process started, such as `fg` of one sudo backgrounded.
     */
    const bool push = poll && s->wait_input != NULL &&
                      espix_proc_parent_of(pid) == self->info.pid;

    espix_proc_kind_t kind = ESPIX_PROC_APP;
    (void)espix_proc_kind_of(pid, &kind);

    espix_pid_t pgid = espix_proc_pgid_of(pid);
    if (pgid == ESPIX_PID_NONE) {
        pgid = pid;
    }
    if (shell) {
        s->fg_pgid = pgid;
    }

    const int64_t deadline = (fg->limit_us > 0) ? esp_timer_get_time() + fg->limit_us
                                                : 0;
    int       exit_code  = -1;
    esp_err_t err        = ESP_ERR_TIMEOUT;
    unsigned  interrupts = 0;
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

        TickType_t ticks;
        if (push) {
            ticks = 0;
        } else if (poll) {
            ticks = pdMS_TO_TICKS(FG_POLL_MS);
        } else {
            ticks = (ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(ms);
        }

        err = espix_proc_wait(pid, &exit_code, ticks);
        if (err != ESP_ERR_TIMEOUT) {
            break;
        }
        if (poll && s->poll_interrupt(s)) {
            say(s, false, "^C\n");
            interrupts++;

            if (interrupts < FG_INTERRUPTS_TO_KILL) {
                (void)espix_proc_signal_group(pgid, SIGINT);
                if (interrupts + 1 == FG_INTERRUPTS_TO_KILL) {
                    say(s, true, "%s: pid %d has not stopped; press Ctrl-C "
                                 "again to force it\n", who, (int)pid);
                }
            } else if (interrupts == FG_INTERRUPTS_TO_KILL) {
                say(s, true, "%s: killing pid %d\n", who, (int)pid);
                (void)espix_proc_signal_group(pgid, SIGKILL);
            }
        }
        if (push) {
            s->wait_input(s, ms);
        }
    }

    /* Whatever was typed at a job that did not read it goes with it. */
    if (poll) {
        (void)s->poll_interrupt(s);
    }
    if (shell) {
        s->fg_pgid = ESPIX_PID_NONE;
    }

    if (err != ESP_OK) {
        say(s, true, "%s: pid %d: %s\n", who, (int)pid, esp_err_to_name(err));
        return -1;
    }
    /* A command has no handler to choose another status: ^C ended it. */
    if (interrupts > 0 && kind != ESPIX_PROC_APP) {
        return 128 + SIGINT;
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
