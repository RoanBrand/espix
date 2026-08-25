/*
 * System commands: help, uname, uptime, free, ps, top, dmesg, reboot, echo,
 * clear.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_system.h"

#include "espix_auth.h"
#include "espix_cmds_priv.h"
#include "espix_fault.h"
#include "espix_kernel.h"
#include "espix_proc.h"
#include "espix_shell.h"

/* ------------------------------------------------------------------ */

typedef struct {
    espix_session_t *s;
    const char      *want;      /* NULL = list everything */
    bool             found;
} help_ctx_t;

static bool help_visit(void *ctx, const espix_cmd_t *cmd)
{
    help_ctx_t *h = ctx;

    if (h->want != NULL) {
        if (strcmp(cmd->name, h->want) != 0) {
            return true;
        }
        h->found = true;
        espix_printf(h->s, "%s\n  %s\n", cmd->usage ? cmd->usage : cmd->name,
                     cmd->help ? cmd->help : "");
        return false;
    }

    espix_printf(h->s, "  %-8s %s\n", cmd->name, cmd->help ? cmd->help : "");
    return true;
}

static int cmd_help(espix_session_t *s, int argc, char **argv)
{
    help_ctx_t ctx = {
        .s    = s,
        .want = (argc > 1) ? argv[1] : NULL,
    };

    espix_shell_foreach(help_visit, &ctx);

    if (ctx.want != NULL && !ctx.found) {
        espix_printf(s, "help: %s: no such command\n", ctx.want);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */

static int cmd_uname(espix_session_t *s, int argc, char **argv)
{
    const bool all = (argc > 1 && strcmp(argv[1], "-a") == 0);

    char buf[128];
    espix_uname(buf, sizeof(buf), all);
    espix_printf(s, "%s\n", buf);
    return 0;
}

static int cmd_uptime(espix_session_t *s, int argc, char **argv)
{
    (void)argc;
    (void)argv;

    char buf[48];
    espix_uptime_str(buf, sizeof(buf));
    espix_printf(s, "%s, last reset: %s\n", buf,
                 espix_fault_reset_reason_str());
    return 0;
}

static int cmd_echo(espix_session_t *s, int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        espix_printf(s, "%s%s", argv[i], (i + 1 < argc) ? " " : "");
    }
    espix_puts(s, "\n");
    return 0;
}

static int cmd_clear(espix_session_t *s, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    espix_puts(s, "\033[H\033[2J");
    return 0;
}

static int cmd_reboot(espix_session_t *s, int argc, char **argv)
{
    (void)argc;
    (void)argv;

    espix_puts(s, "rebooting\n");
    espix_klog(ESPIX_KLOG_WARN, "sys", "reboot requested from %s",
               (s != NULL && s->name != NULL) ? s->name : "?");
    fflush(stdout);
    esp_restart();
    return 0;   /* not reached */
}

/* ------------------------------------------------------------------ */

static void print_heap_line(espix_session_t *s, const char *label, uint32_t caps)
{
    multi_heap_info_t info;
    heap_caps_get_info(&info, caps);

    if (info.total_free_bytes + info.total_allocated_bytes == 0) {
        return;     /* capability not present on this chip/build */
    }

    const size_t total = info.total_free_bytes + info.total_allocated_bytes;

    espix_printf(s, "%-9s %9u %9u %9u %11u\n",
                 label,
                 (unsigned)(total / 1024),
                 (unsigned)(info.total_allocated_bytes / 1024),
                 (unsigned)(info.total_free_bytes / 1024),
                 (unsigned)(info.largest_free_block / 1024));
}

static int cmd_free(espix_session_t *s, int argc, char **argv)
{
    (void)argc;
    (void)argv;

    espix_printf(s, "%-9s %9s %9s %9s %11s\n",
                 "", "total(K)", "used(K)", "free(K)", "largest(K)");
    print_heap_line(s, "internal", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    print_heap_line(s, "psram",    MALLOC_CAP_SPIRAM);
    espix_printf(s, "min free internal since boot: %u K\n",
                 (unsigned)(heap_caps_get_minimum_free_size(
                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024));
    return 0;
}

/* ------------------------------------------------------------------ */

#if configUSE_TRACE_FACILITY

static char task_state_char(eTaskState state)
{
    switch (state) {
    case eRunning:   return 'R';
    case eReady:     return 'r';
    case eBlocked:   return 'B';
    case eSuspended: return 'S';
    case eDeleted:   return 'D';
    default:         return '?';
    }
}

static int cmd_ps(espix_session_t *s, int argc, char **argv)
{
    (void)argc;
    (void)argv;

    const UBaseType_t capacity = uxTaskGetNumberOfTasks() + 4;

    TaskStatus_t *tasks = calloc(capacity, sizeof(TaskStatus_t));
    if (tasks == NULL) {
        espix_printf(s, "ps: out of memory\n");
        return 1;
    }

    configRUN_TIME_COUNTER_TYPE total_runtime = 0;
    const UBaseType_t count = uxTaskGetSystemState(tasks, capacity,
                                                   &total_runtime);

    espix_printf(s, "%5s %-16s %2s %4s %4s %6s %5s\n",
                 "PID", "NAME", "ST", "PRI", "CORE", "STACK", "CPU%");

    for (UBaseType_t i = 0; i < count; i++) {
        const espix_pid_t pid = espix_proc_pid_of_task(tasks[i].xHandle);

        char pid_str[12];
        if (pid != ESPIX_PID_NONE) {
            snprintf(pid_str, sizeof(pid_str), "%d", (int)pid);
        } else {
            snprintf(pid_str, sizeof(pid_str), "-");
        }

        char core_str[12];
#if configTASKLIST_INCLUDE_COREID
        if (tasks[i].xCoreID == tskNO_AFFINITY) {
            snprintf(core_str, sizeof(core_str), "any");
        } else {
            snprintf(core_str, sizeof(core_str), "%d", (int)tasks[i].xCoreID);
        }
#else
        snprintf(core_str, sizeof(core_str), "-");
#endif

        /* Cumulative share of run time since boot, not an instantaneous
         * reading — a real `top` needs two samples and comes later. */
        unsigned pct = 0;
        if (total_runtime > 0) {
            pct = (unsigned)((uint64_t)tasks[i].ulRunTimeCounter * 100 /
                             total_runtime);
        }

        espix_printf(s, "%5s %-16s %2c %4u %4s %6u %4u%%\n",
                     pid_str,
                     tasks[i].pcTaskName,
                     task_state_char(tasks[i].eCurrentState),
                     (unsigned)tasks[i].uxCurrentPriority,
                     core_str,
                     (unsigned)tasks[i].usStackHighWaterMark,
                     pct);
    }

    free(tasks);

    /* Processes that have finished keep their table slot, so report them too:
     * that is where `run` gets an exit status from. */
    espix_proc_info_t procs[8];
    const size_t n = espix_proc_snapshot(procs, sizeof(procs) / sizeof(procs[0]));

    bool header = false;
    for (size_t i = 0; i < n; i++) {
        if (procs[i].state == ESPIX_PROC_RUNNING ||
            procs[i].state == ESPIX_PROC_READY) {
            continue;
        }
        if (!header) {
            espix_printf(s, "\nfinished:\n%5s %-16s %-6s %s\n",
                         "PID", "NAME", "STATE", "EXIT");
            header = true;
        }
        espix_printf(s, "%5d %-16s %-6s %d\n",
                     (int)procs[i].pid, procs[i].name,
                     espix_proc_state_str(procs[i].state),
                     procs[i].exit_code);
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* top                                                                 */
/* ------------------------------------------------------------------ */

/*
 * `ps` reports each task's share of run time *since boot*, which stops meaning
 * anything within a minute of uptime: a task that pegged a core during WiFi
 * association still reads high an hour later. An instantaneous reading needs
 * two samples, and this is them.
 */

#define TOP_INTERVAL_MS 1000
#define TOP_SLICE_MS      50    /* how often Ctrl-C is checked between frames */
#define TOP_ROWS_MAX      24    /* tasks listed; a terminal has only so many */

typedef struct {
    TaskHandle_t                 handle;
    configRUN_TIME_COUNTER_TYPE  runtime;
} top_prev_t;

typedef struct {
    const TaskStatus_t *task;
    unsigned            pct;
    bool                known;  /* false for a task first seen this frame */
    bool                idle;   /* an idle task: counted, but not listed */
} top_row_t;

/* Descending by CPU. Sorting is the one thing top does that ps does not, and
 * it is what makes the display worth watching rather than re-reading. */
static int top_row_cmp(const void *a, const void *b)
{
    const top_row_t *x = a;
    const top_row_t *y = b;

    if (x->pct != y->pct) {
        return (int)y->pct - (int)x->pct;
    }
    return strcmp(x->task->pcTaskName, y->task->pcTaskName);
}

/*
 * The idle tasks are not listed, and are used only to work out how busy the
 * machine is.
 *
 * There is one per core and each soaks up whatever nothing else wants, so they
 * sit at 99% and 98% forever and, sorted by CPU, permanently occupy the top two
 * rows -- burying the work you opened top to look at. Linux does not show an
 * idle process either.
 *
 * Leaving them out also removes an oddity: with them listed the column sums to
 * ~200% on a dual-core part, because FreeRTOS derives total run time from a
 * single timer (wall time) while each core contributes its own occupancy
 * against it. Every remaining task runs on one core at a time, so every
 * percentage shown is out of 100 and needs no explaining.
 */
#define TOP_CORES_MAX 4

static void top_header(espix_session_t *s, UBaseType_t count, unsigned running,
                       const unsigned *idle_per_core, unsigned idle_pct)
{
    char uptime[64];
    espix_uptime_str(uptime, sizeof(uptime));

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    const unsigned cores    = chip.cores > 0 ? chip.cores : 1;
    const unsigned capacity = 100u * cores;
    const unsigned busy     = idle_pct >= capacity ? 0 : capacity - idle_pct;

    multi_heap_info_t internal;
    multi_heap_info_t psram;
    heap_caps_get_info(&internal, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    heap_caps_get_info(&psram, MALLOC_CAP_SPIRAM);

    const size_t int_total = internal.total_free_bytes + internal.total_allocated_bytes;
    const size_t psr_total = psram.total_free_bytes + psram.total_allocated_bytes;

    espix_printf(s, "top - %s\n", uptime);
    espix_printf(s, "Mem:  internal %uK/%uK",
                 (unsigned)(internal.total_allocated_bytes / 1024),
                 (unsigned)(int_total / 1024));
    if (psr_total > 0) {
        espix_printf(s, "    psram %uK/%uK",
                     (unsigned)(psram.total_allocated_bytes / 1024),
                     (unsigned)(psr_total / 1024));
    }
    espix_printf(s, "\nCpu:  %u%% busy across %u core%s",
                 busy / cores, cores, cores == 1 ? "" : "s");

    /*
     * Per core, which costs nothing to work out: each idle task is pinned to
     * one core, so that core's occupancy is simply whatever its idle task did
     * not take. Only shown when there is more than one, since on a single-core
     * part it would just repeat the figure above.
     */
    if (cores > 1) {
        for (unsigned c = 0; c < cores && c < TOP_CORES_MAX; c++) {
            const unsigned idle = idle_per_core[c] > 100 ? 100 : idle_per_core[c];
            espix_printf(s, "   core%u %u%%", c, 100u - idle);
        }
    }
    espix_printf(s, "\n");
    espix_printf(s, "Tasks: %u total, %u running\n\n",
                 (unsigned)count, running);
}

static int cmd_top(espix_session_t *s, int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /*
     * Sized once and kept for the whole run rather than per frame: this is a
     * loop, and uxTaskGetSystemState() wants a worst-case array anyway. The
     * slack covers tasks created while top is running.
     */
    const UBaseType_t capacity = uxTaskGetNumberOfTasks() + 8;

    TaskStatus_t *tasks = calloc(capacity, sizeof(TaskStatus_t));
    top_prev_t   *prev  = calloc(capacity, sizeof(top_prev_t));
    top_row_t    *rows  = calloc(capacity, sizeof(top_row_t));

    if (tasks == NULL || prev == NULL || rows == NULL) {
        espix_printf(s, "top: out of memory\n");
        free(tasks); free(prev); free(rows);
        return 1;
    }

    size_t                      prev_count   = 0;
    configRUN_TIME_COUNTER_TYPE prev_total   = 0;
    bool                        have_prev    = false;
    bool                        interrupted  = false;

    while (!interrupted) {
        configRUN_TIME_COUNTER_TYPE total = 0;
        const UBaseType_t count = uxTaskGetSystemState(tasks, capacity, &total);

        const configRUN_TIME_COUNTER_TYPE window = total - prev_total;

        unsigned running = 0;
        for (UBaseType_t i = 0; i < count; i++) {
            if (tasks[i].eCurrentState == eRunning) {
                running++;
            }

            rows[i].task  = &tasks[i];
            rows[i].pct   = 0;
            rows[i].known = false;

            if (!have_prev || window == 0) {
                continue;
            }

            /*
             * Match by handle. A handle can be recycled after a task is
             * deleted, so in principle a stale entry could be matched against a
             * different task -- the cost is one wrong reading in one frame, and
             * tracking task creation to avoid it is not worth the machinery.
             */
            for (size_t j = 0; j < prev_count; j++) {
                if (prev[j].handle != tasks[i].xHandle) {
                    continue;
                }
                const configRUN_TIME_COUNTER_TYPE used =
                    tasks[i].ulRunTimeCounter - prev[j].runtime;

                rows[i].pct   = (unsigned)((uint64_t)used * 100 / window);
                rows[i].known = true;
                break;
            }
        }

        /* Idle share drives the "busy" figure and nothing else. Matched by
         * name: TaskStatus_t does not carry the per-core idle handles. */
        unsigned idle_pct = 0;
        unsigned idle_per_core[TOP_CORES_MAX] = { 0 };

        for (UBaseType_t i = 0; i < count; i++) {
            rows[i].idle = strncmp(rows[i].task->pcTaskName, "IDLE", 4) == 0;
            if (!rows[i].idle) {
                continue;
            }
            idle_pct += rows[i].pct;

            /*
             * Attribute it to the core it is pinned to. xCoreID is authoritative
             * where the build provides it; the trailing digit of "IDLE0" is the
             * same answer from the only other place FreeRTOS states it.
             */
#if configTASKLIST_INCLUDE_COREID
            const int core = (int)rows[i].task->xCoreID;
#else
            const char  d    = rows[i].task->pcTaskName[4];
            const int   core = (d >= '0' && d <= '9') ? d - '0' : -1;
#endif
            if (core >= 0 && core < TOP_CORES_MAX) {
                idle_per_core[core] += rows[i].pct;
            }
        }

        qsort(rows, count, sizeof(rows[0]), top_row_cmp);

        /*
         * Home the cursor and erase downwards rather than clearing the screen,
         * which would flicker once a second. Only when the terminal can take
         * escape sequences: a dumb one is better served by a scrolling block
         * than by literal ESC[2J.
         */
        if (s->ansi) {
            espix_printf(s, "\033[H\033[J");
        } else {
            espix_printf(s, "\n");
        }

        top_header(s, count, running, idle_per_core, idle_pct);
        espix_printf(s, "%5s %-16s %2s %4s %4s %6s %5s\n",
                     "PID", "NAME", "ST", "PRI", "CORE", "STACK", "CPU%");

        unsigned shown = 0;
        for (UBaseType_t i = 0; i < count && shown < TOP_ROWS_MAX; i++) {
            if (rows[i].idle) {
                continue;
            }
            shown++;

            const TaskStatus_t *t   = rows[i].task;
            const espix_pid_t   pid = espix_proc_pid_of_task(t->xHandle);

            char pid_str[12];
            if (pid != ESPIX_PID_NONE) {
                snprintf(pid_str, sizeof(pid_str), "%d", (int)pid);
            } else {
                snprintf(pid_str, sizeof(pid_str), "-");
            }

            char core_str[12];
#if configTASKLIST_INCLUDE_COREID
            if (t->xCoreID == tskNO_AFFINITY) {
                snprintf(core_str, sizeof(core_str), "any");
            } else {
                snprintf(core_str, sizeof(core_str), "%d", (int)t->xCoreID);
            }
#else
            snprintf(core_str, sizeof(core_str), "-");
#endif

            /* A task first seen this frame has no delta to report. Printing its
             * since-boot share here would be exactly the confusion this command
             * exists to remove, so it shows nothing instead. */
            char cpu[8];
            if (rows[i].known) {
                snprintf(cpu, sizeof(cpu), "%u%%", rows[i].pct);
            } else {
                snprintf(cpu, sizeof(cpu), "-");
            }

            espix_printf(s, "%5s %-16s %2c %4u %4s %6u %5s\n",
                         pid_str, t->pcTaskName,
                         task_state_char(t->eCurrentState),
                         (unsigned)t->uxCurrentPriority,
                         core_str,
                         (unsigned)t->usStackHighWaterMark,
                         cpu);
        }

        if (shown >= TOP_ROWS_MAX) {
            espix_printf(s, "...\n");
        }
        espix_printf(s, "\nCtrl-C to quit\n");

        /* Remember this sample for the next frame's delta. */
        for (UBaseType_t i = 0; i < count; i++) {
            prev[i].handle  = tasks[i].xHandle;
            prev[i].runtime = tasks[i].ulRunTimeCounter;
        }
        prev_count = count;
        prev_total = total;
        have_prev  = true;

        /*
         * Sleep in slices so Ctrl-C is felt straight away rather than a whole
         * frame later. poll_interrupt() consumes what is waiting either way,
         * which is what stops keystrokes typed at top arriving at the next
         * prompt.
         */
        for (unsigned waited = 0; waited < TOP_INTERVAL_MS; waited += TOP_SLICE_MS) {
            if (s->poll_interrupt != NULL && s->poll_interrupt(s)) {
                interrupted = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(TOP_SLICE_MS));
        }
    }

    free(tasks);
    free(prev);
    free(rows);

    /* Leave the prompt somewhere sensible rather than on top of the table. */
    espix_printf(s, "\n");
    return 0;
}

#else  /* !configUSE_TRACE_FACILITY */

static int cmd_ps(espix_session_t *s, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    espix_printf(s, "ps: rebuild with CONFIG_FREERTOS_USE_TRACE_FACILITY=y\n");
    return 1;
}

static int cmd_top(espix_session_t *s, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    espix_printf(s, "top: rebuild with CONFIG_FREERTOS_USE_TRACE_FACILITY=y\n");
    return 1;
}

#endif

/* ------------------------------------------------------------------ */

static const char *klog_level_char(uint8_t level)
{
    switch (level) {
    case ESPIX_KLOG_ERROR: return "E";
    case ESPIX_KLOG_WARN:  return "W";
    case ESPIX_KLOG_DEBUG: return "D";
    default:               return "I";
    }
}

static bool dmesg_visit(void *ctx, const espix_klog_entry_t *e)
{
    espix_session_t *s = ctx;

    espix_printf(s, "[%6u.%03u] %s %s\n",
                 (unsigned)(e->ts_ms / 1000),
                 (unsigned)(e->ts_ms % 1000),
                 klog_level_char(e->level),
                 e->text);
    return true;
}

static int cmd_dmesg(espix_session_t *s, int argc, char **argv)
{
    (void)argc;
    (void)argv;

    espix_klog_foreach(dmesg_visit, s);

    const uint32_t dropped = espix_klog_dropped();
    if (dropped > 0) {
        espix_printf(s, "[%u earlier line%s dropped]\n",
                     (unsigned)dropped, dropped == 1 ? "" : "s");
    }
    return 0;
}

/* ------------------------------------------------------------------ */

static int cmd_coredump(espix_session_t *s, int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "erase") == 0) {
        const esp_err_t err = espix_fault_coredump_erase();
        if (err != ESP_OK) {
            espix_printf(s, "coredump: erase failed: %s\n",
                         esp_err_to_name(err));
            return 1;
        }
        espix_printf(s, "core dump erased\n");
        return 0;
    }

    espix_coredump_info_t info;
    if (espix_fault_coredump_status(&info) != ESP_OK) {
        espix_printf(s, "coredump: cannot read the coredump partition\n");
        return 1;
    }

    if (!info.present) {
        espix_printf(s, "no core dump stored\n");
        return 0;
    }

    espix_printf(s, "core dump: %u bytes at flash 0x%06x\n",
                 (unsigned)info.size, (unsigned)info.flash_addr);
    if (info.task[0] != '\0') {
        espix_printf(s, "  faulting task: %s\n", info.task);
        espix_printf(s, "  pc:            0x%08x%s\n", (unsigned)info.pc,
                     info.same_build ? "" : "  (stale, see below)");
    }
    if (!info.same_build) {
        espix_printf(s, "  WARNING: this dump came from a different firmware "
                        "build.\n"
                        "           Its addresses do not correspond to the "
                        "running ELF —\n"
                        "           decoding them yields the wrong functions.\n");
    }

    /* The registers and stacks are all in there, but decoding them needs the
     * matching ELF, which lives on the host. */
    espix_printf(s, "  full backtrace: idf.py -p <port> coredump-info\n");
    espix_printf(s, "  free it with:   coredump erase\n");
    return 0;
}

/* ------------------------------------------------------------------ */

static int cmd_passwd(espix_session_t *s, int argc, char **argv)
{
    const char *user = (argc > 1) ? argv[1] : "esp";

    /*
     * Taken as an argument rather than prompted for. Reading a password without
     * echo needs terminal control the session abstraction does not expose yet,
     * and prompting *with* echo would be worse than being honest about it.
     * Revisit when the reentrant line editor lands.
     */
    if (argc < 3) {
        espix_printf(s, "usage: passwd [user] <new-password>\n");
        espix_printf(s, "note: the password is echoed and enters shell "
                        "history; no-echo input needs the new line editor\n");
        return 1;
    }

    const char *password = argv[argc - 1];
    if (argc == 2) {
        user = "esp";
    }

    const esp_err_t err = espix_auth_set_password(user, password);
    if (err != ESP_OK) {
        espix_printf(s, "passwd: %s: %s\n", user, esp_err_to_name(err));
        return 1;
    }

    espix_printf(s, "password updated for %s\n", user);
    return 0;
}

static int cmd_whoami(espix_session_t *s, int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* Every session carries a name now -- "root" on the console, the
     * authenticated one over SSH -- so there is no fallback to invent here. */
    espix_printf(s, "%s\n", (s != NULL && s->user[0] != '\0') ? s->user : "?");
    return 0;
}

/*
 * End the session, with an exit status.
 *
 * `exit` is the general form; `logout` is the same thing restricted to a login
 * shell, which is how bash draws the line and why both work over SSH while
 * only `exit` makes sense on a console nobody logged in to.
 */
static int session_end(espix_session_t *s, int argc, char **argv, bool login_only)
{
    /* Asking whether a login happened, not whether a user is named: the
     * console is called root and still never logged in. */
    if (login_only && !s->login) {
        espix_printf(s, "%s: not login shell: use `exit'\n", argv[0]);
        return 1;
    }

    /* Bare `exit` carries the last command's status, as $? does. */
    int status = s->last_status;

    if (argc > 1) {
        char *end = NULL;
        const long n = strtol(argv[1], &end, 10);

        if (end == argv[1] || *end != '\0' || n < 0 || n > 255) {
            espix_printf(s, "usage: %s [status]\n", argv[0]);
            return 1;               /* refuse, and stay */
        }
        status = (int)n;
    }

    s->last_status = status;
    s->want_exit   = true;
    return status;
}

static int cmd_exit(espix_session_t *s, int argc, char **argv)
{
    return session_end(s, argc, argv, false);
}

static int cmd_logout(espix_session_t *s, int argc, char **argv)
{
    return session_end(s, argc, argv, true);
}

/* ------------------------------------------------------------------ */

static espix_cmd_t s_sys_cmds[] = {
    { .name = "help",   .fn = cmd_help,
      .help = "list commands, or describe one", .usage = "help [command]" },
    { .name = "uname",  .fn = cmd_uname,
      .help = "print system information",       .usage = "uname [-a]" },
    { .name = "uptime", .fn = cmd_uptime,
      .help = "how long the system has been up", .usage = "uptime" },
    { .name = "free",   .fn = cmd_free,
      .help = "report memory usage",            .usage = "free" },
    { .name = "ps",     .fn = cmd_ps,
      .help = "list tasks and processes",       .usage = "ps" },
    { .name = "top",    .fn = cmd_top,
      .help = "live view of tasks and memory",  .usage = "top" },
    { .name = "dmesg",  .fn = cmd_dmesg,
      .help = "print the kernel log",           .usage = "dmesg" },
    { .name = "coredump", .fn = cmd_coredump,
      .help = "show or erase the stored core dump",
      .usage = "coredump [erase]" },
    { .name = "passwd", .fn = cmd_passwd,
      .help = "set a user's password",
      .usage = "passwd [user] <new-password>" },
    { .name = "whoami", .fn = cmd_whoami,
      .help = "print the current user",
      .usage = "whoami" },
    { .name = "echo",   .fn = cmd_echo,
      .help = "print arguments",                .usage = "echo [text]..." },
    { .name = "clear",  .fn = cmd_clear,
      .help = "clear the screen",               .usage = "clear" },
    { .name = "exit",   .fn = cmd_exit,
      .help = "end this session",
      .usage = "exit [status]" },
    { .name = "logout", .fn = cmd_logout,
      .help = "end this session (login shells only)",
      .usage = "logout" },
    { .name = "reboot", .fn = cmd_reboot,
      .help = "restart the system",             .usage = "reboot" },
};

void espix_cmds_register_sys(void)
{
    espix_cmds_register_table(s_sys_cmds,
                             sizeof(s_sys_cmds) / sizeof(s_sys_cmds[0]));
}
