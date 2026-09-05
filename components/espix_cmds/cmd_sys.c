/*
 * System commands: help, uname, uptime, free, ps, top, dmesg, reboot, echo,
 * clear.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
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

        /*
         * A process parked on SIGSTOP is blocked on a semaphore as far as
         * FreeRTOS is concerned, which would read as a plain 'B' and be
         * indistinguishable from one waiting on a queue. 'T' is what ps(1)
         * shows for a stopped process, and it is the one state here that espix
         * knows about and FreeRTOS does not.
         */
        char st = task_state_char(tasks[i].eCurrentState);
        if (pid != ESPIX_PID_NONE &&
            espix_proc_state_of(pid) == ESPIX_PROC_STOPPED) {
            st = 'T';
        }

        espix_printf(s, "%5s %-16s %2c %4u %4s %6u %4u%%\n",
                     pid_str,
                     tasks[i].pcTaskName,
                     st,
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
        /* STOPPED belongs with the living: it is listed above with a 'T', and
         * reporting it here as finished would claim an exit that has not
         * happened and an exit_code that means nothing yet. */
        if (procs[i].state == ESPIX_PROC_RUNNING ||
            procs[i].state == ESPIX_PROC_READY ||
            procs[i].state == ESPIX_PROC_STOPPED) {
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

/*
 * The ring stores monotonic milliseconds, which is the right thing to store:
 * it cannot be invalidated by the clock being set. Wall-clock rendering for
 * `dmesg -T` is therefore derived here, from the offset between the two clocks
 * *right now* -- exactly how Linux's dmesg -T works, and with the same happy
 * side effect that lines logged before NTP answered come out with correct
 * times afterwards.
 */
typedef struct {
    espix_session_t *s;
    bool             ctime;         /* -T */
    time_t           wall_at_zero;  /* wall clock at monotonic ms == 0 */
} dmesg_ctx_t;

static bool dmesg_visit(void *ctx, const espix_klog_entry_t *e)
{
    dmesg_ctx_t *d = ctx;

    if (d->ctime) {
        const time_t t = d->wall_at_zero + (time_t)(e->ts_ms / 1000);
        struct tm    tm;
        char         when[32];

        localtime_r(&t, &tm);
        strftime(when, sizeof(when), "%a %b %e %H:%M:%S %Y", &tm);

        espix_printf(d->s, "[%s] %s %s\n",
                     when, klog_level_char(e->level), e->text);
    } else {
        espix_printf(d->s, "[%6u.%03u] %s %s\n",
                     (unsigned)(e->ts_ms / 1000),
                     (unsigned)(e->ts_ms % 1000),
                     klog_level_char(e->level),
                     e->text);
    }
    return true;
}

static int cmd_dmesg(espix_session_t *s, int argc, char **argv)
{
    dmesg_ctx_t ctx = { .s = s };

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-T") == 0 || strcmp(argv[i], "--ctime") == 0) {
            ctx.ctime = true;
        } else {
            espix_printf(s, "dmesg: unknown option '%s'\n", argv[i]);
            return 1;
        }
    }

    if (ctx.ctime) {
        /* Once per invocation, not per line: every entry shares the offset,
         * and re-reading the clock 96 times would be both slower and capable
         * of straddling a second boundary mid-listing. */
        ctx.wall_at_zero = time(NULL) - (time_t)(esp_log_timestamp() / 1000);
    }

    espix_klog_foreach(dmesg_visit, &ctx);

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
    /*
     * `passwd -l <user>` takes a password away rather than setting one, which
     * is how root goes back to being unreachable after somebody gave it one.
     * Root's alone: locking an account is administration, not self-service, and
     * locking your own would be a way to shut yourself out with one typo.
     */
    if (argc == 3 && strcmp(argv[1], "-l") == 0) {
        if (s == NULL || s->uid != 0) {
            espix_printf(s, "passwd: only root can lock an account\n");
            return 1;
        }

        const esp_err_t err = espix_auth_lock(argv[2]);
        if (err != ESP_OK) {
            espix_printf(s, "passwd: %s: %s\n", argv[2],
                         (err == ESP_ERR_NOT_FOUND) ? "no such user"
                                                    : esp_err_to_name(err));
            return 1;
        }
        espix_printf(s, "%s can no longer log in\n", argv[2]);
        return 0;
    }

    if (argc < 3) {
        espix_printf(s, "usage: passwd [user] <new-password>\n");
        espix_printf(s, "       passwd -l <user>    take the password away\n");
        espix_printf(s, "note: the password is echoed and enters shell "
                        "history; no-echo input needs the new line editor\n");
        return 1;
    }

    const char *password = argv[argc - 1];
    if (argc == 2) {
        user = "esp";
    }

    /*
     * Your own password, or root changing anybody's.
     *
     * This check did not exist and did not need to: before accounts had uids
     * nothing followed from being one user rather than another. It does now.
     * root is seeded locked precisely so that nobody can log in as uid 0, and
     * uid 0 skips every permission check -- so an unprivileged `passwd root
     * hunter2` would have handed out the superuser, undoing the whole branch in
     * one command.
     */
    if (s != NULL && s->uid != 0 && strcmp(user, s->user) != 0) {
        espix_printf(s, "passwd: only root can change another user's "
                        "password\n");
        return 1;
    }

    const esp_err_t err = espix_auth_set_password(user, password);
    if (err != ESP_OK) {
        espix_printf(s, "passwd: %s: %s\n", user,
                     (err == ESP_ERR_NOT_FOUND) ? "no such user"
                                                : esp_err_to_name(err));
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
 * uid=1000(esp) gid=1000(esp), the way id(1) writes it.
 *
 * The name comes from the session and the number from the same field the
 * filesystem checks, so if these two ever disagree the output says so rather
 * than hiding it behind one of them.
 */
static int cmd_id(espix_session_t *s, int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (s == NULL) {
        return 1;
    }

    const char *uname = espix_auth_name_for_uid(s->uid);
    char        ubuf[ESPIX_USER_MAX];
    strlcpy(ubuf, (uname != NULL) ? uname : s->user, sizeof(ubuf));

    const char *gname = espix_auth_name_for_uid(s->gid);

    espix_printf(s, "uid=%u(%s) gid=%u(%s)\n",
                 (unsigned)s->uid, ubuf,
                 (unsigned)s->gid, (gname != NULL) ? gname : ubuf);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Accounts and groups                                                 */
/* ------------------------------------------------------------------ */

/* Every one of these is root's. Nothing below re-checks it. */
static bool require_root(espix_session_t *s, const char *cmd)
{
    if (s != NULL && s->uid == 0) {
        return true;
    }
    espix_printf(s, "%s: only root can do that\n", cmd);
    return false;
}

static const char *auth_err(esp_err_t rc)
{
    switch (rc) {
    case ESP_ERR_NOT_FOUND:      return "no such user or group";
    case ESP_ERR_INVALID_STATE:  return "already exists";
    case ESP_ERR_INVALID_ARG:    return "invalid name";
    case ESP_ERR_NOT_ALLOWED:    return "not permitted";
    case ESP_ERR_NO_MEM:         return "no room left";
    default:                     return esp_err_to_name(rc);
    }
}

/*
 * useradd [-m] [-r] [-G group,...] <name>
 *
 * useradd(8) semantics, including the two that surprise people: no home
 * directory unless -m, and no password ever. Both are right here -- a session
 * whose home is missing already falls back to /, and leaving the password to
 * `passwd` keeps the one command that has to take a password on the command
 * line down to one.
 */
static int cmd_useradd(espix_session_t *s, int argc, char **argv)
{
    bool        system    = false;
    bool        make_home = false;
    const char *groups    = NULL;
    const char *name      = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-m") == 0) {
            make_home = true;
        } else if (strcmp(argv[i], "-r") == 0) {
            system = true;
        } else if (strcmp(argv[i], "-G") == 0 && i + 1 < argc) {
            groups = argv[++i];
        } else if (argv[i][0] == '-') {
            espix_printf(s, "useradd: %s: unsupported option\n", argv[i]);
            return 1;
        } else if (name == NULL) {
            name = argv[i];
        } else {
            espix_printf(s, "useradd: one name at a time\n");
            return 1;
        }
    }

    if (name == NULL) {
        espix_printf(s, "usage: useradd [-m] [-r] [-G group,...] <name>\n");
        return 1;
    }
    if (!require_root(s, "useradd")) {
        return 1;
    }

    esp_err_t rc = espix_auth_user_add(name, system, make_home);
    if (rc != ESP_OK) {
        espix_printf(s, "useradd: %s: %s\n", name, auth_err(rc));
        return 1;
    }

    if (groups != NULL) {
        rc = espix_auth_set_groups(name, groups, true);
        if (rc != ESP_OK) {
            espix_printf(s, "useradd: %s: added, but groups: %s\n", name,
                         auth_err(rc));
            return 1;
        }
    }

    espix_printf(s, "%s created; it has no password until you set one\n", name);
    return 0;
}

static int cmd_userdel(espix_session_t *s, int argc, char **argv)
{
    bool        remove_home = false;
    const char *name        = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0) {
            remove_home = true;
        } else if (argv[i][0] == '-') {
            espix_printf(s, "userdel: %s: unsupported option\n", argv[i]);
            return 1;
        } else {
            name = argv[i];
        }
    }

    if (name == NULL) {
        espix_printf(s, "usage: userdel [-r] <name>\n");
        return 1;
    }
    if (!require_root(s, "userdel")) {
        return 1;
    }

    const esp_err_t rc = espix_auth_user_del(name, remove_home);
    if (rc != ESP_OK) {
        espix_printf(s, "userdel: %s: %s\n", name, auth_err(rc));
        return 1;
    }
    return 0;
}

/* usermod -aG a,b <user> to append, -G a,b to replace. */
static int cmd_usermod(espix_session_t *s, int argc, char **argv)
{
    const char *groups = NULL;
    const char *name   = NULL;
    bool        append = false;
    bool        seen   = false;

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-aG") == 0 || strcmp(argv[i], "-Ga") == 0) &&
            i + 1 < argc) {
            append = true;
            seen   = true;
            groups = argv[++i];
        } else if (strcmp(argv[i], "-G") == 0 && i + 1 < argc) {
            seen   = true;
            groups = argv[++i];
        } else if (argv[i][0] == '-') {
            espix_printf(s, "usermod: %s: unsupported option\n", argv[i]);
            return 1;
        } else {
            name = argv[i];
        }
    }

    if (name == NULL || !seen) {
        espix_printf(s, "usage: usermod -aG <group,...> <user>   append\n");
        espix_printf(s, "       usermod -G <group,...> <user>    replace\n");
        return 1;
    }
    if (!require_root(s, "usermod")) {
        return 1;
    }

    const esp_err_t rc = espix_auth_set_groups(name, groups, append);
    if (rc != ESP_OK) {
        espix_printf(s, "usermod: %s: %s\n", name, auth_err(rc));
        return 1;
    }
    return 0;
}

static int cmd_groupadd(espix_session_t *s, int argc, char **argv)
{
    bool        system = false;
    const char *name   = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0) {
            system = true;
        } else if (argv[i][0] == '-') {
            espix_printf(s, "groupadd: %s: unsupported option\n", argv[i]);
            return 1;
        } else {
            name = argv[i];
        }
    }

    if (name == NULL) {
        espix_printf(s, "usage: groupadd [-r] <name>\n");
        return 1;
    }
    if (!require_root(s, "groupadd")) {
        return 1;
    }

    const esp_err_t rc = espix_auth_group_add(name, system);
    if (rc != ESP_OK) {
        espix_printf(s, "groupadd: %s: %s\n", name, auth_err(rc));
        return 1;
    }
    return 0;
}

static int cmd_groupdel(espix_session_t *s, int argc, char **argv)
{
    if (argc != 2) {
        espix_printf(s, "usage: groupdel <name>\n");
        return 1;
    }
    if (!require_root(s, "groupdel")) {
        return 1;
    }

    const esp_err_t rc = espix_auth_group_del(argv[1]);
    if (rc != ESP_OK) {
        espix_printf(s, "groupdel: %s: %s\n", argv[1], auth_err(rc));
        return 1;
    }
    return 0;
}

/* groups [user] -- the current session's if none is named. */
static int cmd_groups(espix_session_t *s, int argc, char **argv)
{
    const char *user = (argc > 1) ? argv[1] : (s != NULL ? s->user : NULL);

    if (user == NULL || user[0] == '\0') {
        return 1;
    }

    espix_gid_t  gids[ESPIX_NGROUPS_MAX];
    const size_t n = espix_auth_groups(user, gids, ESPIX_NGROUPS_MAX);

    if (n == 0) {
        espix_printf(s, "groups: %s: no such user\n", user);
        return 1;
    }

    espix_printf(s, "%s :", user);
    for (size_t i = 0; i < n; i++) {
        const char *name = espix_auth_group_name(gids[i]);
        if (name != NULL) {
            espix_printf(s, " %s", name);
        } else {
            espix_printf(s, " %u", (unsigned)gids[i]);
        }
    }
    espix_printf(s, "\n");
    return 0;
}

/*
 * Run one command as root.
 *
 * espix has no setuid path for builtins and no `su`, so this is the only way up
 * to uid 0 that does not need the serial console. Gated by /etc/sudoers, which
 * espix_auth reads -- the file is 0600 root and that component already holds
 * the privilege to reach it.
 *
 * It does NOT ask for a password, and that is a real weakness rather than an
 * oversight. espix cannot read input without echoing it, which is the same
 * limitation that makes `passwd` take the password as an argument; prompting
 * here would print it. The session is already authenticated as this user, and
 * sudo(8)'s own timestamp caching means a real one frequently does not re-ask
 * either -- but an unattended terminal is a way in that Linux would have shut.
 * Revisit with the reentrant line editor cmd_passwd() is also waiting on.
 *
 * The credential is raised around the dispatch and restored on every path out,
 * including the one where the command fails. A process spawned in between
 * copies uid 0 at admission and keeps it, which is correct: it was started
 * under sudo and may outlive the command that started it.
 */
static int cmd_sudo(espix_session_t *s, int argc, char **argv)
{
    if (s == NULL) {
        return 1;
    }
    /*
     * -u runs as somebody other than root, which is what a service account is
     * for: `sudo -u www /bin/httpd &` and the app has its own identity without
     * espix needing a service manager. No new authority -- anyone who may become
     * root may already become anybody, by becoming root first.
     */
    int         first = 1;
    const char *as    = NULL;

    if (argc > 2 && strcmp(argv[1], "-u") == 0) {
        as    = argv[2];
        first = 3;
    }

    if (first >= argc) {
        espix_printf(s, "usage: sudo [-u <user>] <command> [args...]\n");
        return 1;
    }

    if (s->uid != 0 && !espix_auth_may_sudo(s->user)) {
        espix_printf(s, "sudo: %s is not in %s\n", s->user, "/etc/sudoers");
        espix_klog(ESPIX_KLOG_WARN, "sudo", "%s: refused", s->user);
        return 1;
    }

    /* Resolve the target before touching the session, so a typo leaves the
     * caller as they were rather than mid-way through becoming somebody. */
    espix_user_t target;
    espix_gid_t  target_groups[ESPIX_NGROUPS_MAX];
    size_t       target_ngroups = 0;

    if (as != NULL) {
        if (espix_auth_lookup(as, &target) != ESP_OK) {
            espix_printf(s, "sudo: %s: no such user\n", as);
            return 1;
        }
        target_ngroups = espix_auth_groups(as, target_groups,
                                           ESPIX_NGROUPS_MAX);
    }

    /* Rejoin, because the dispatcher takes a line rather than an argv. Args
     * that were quoted on the way in lose their quoting here, which is the
     * same thing the shell's own re-parsing does elsewhere. */
    char line[ESPIX_LINE_MAX];
    size_t n = 0;

    for (int i = first; i < argc; i++) {
        const int wrote = snprintf(line + n, sizeof(line) - n, "%s%s",
                                   (i > first) ? " " : "", argv[i]);
        if (wrote < 0 || (size_t)wrote >= sizeof(line) - n) {
            espix_printf(s, "sudo: command line too long\n");
            return 1;
        }
        n += (size_t)wrote;
    }

    /*
     * The name goes with the numbers.
     *
     * Leaving `user` alone made the session run as one identity and answer to
     * another: `id` said bob while `whoami` said esp, and a nested sudo checked
     * esp's membership rather than bob's -- so `sudo -u bob sudo ...` was
     * allowed on the strength of a name the session no longer had. Not an
     * escalation, since the outer sudo had already granted the lot, but exactly
     * the disagreement `id` exists to make visible.
     */
    const uint16_t saved_uid     = s->uid;
    const uint16_t saved_gid     = s->gid;
    const uint8_t  saved_ngroups = s->ngroups;
    uint16_t       saved_groups[ESPIX_NGROUPS_MAX];
    char           saved_user[ESPIX_SESSION_USER_MAX];

    memcpy(saved_groups, s->groups, sizeof(saved_groups));
    strlcpy(saved_user, s->user, sizeof(saved_user));

    espix_klog(ESPIX_KLOG_INFO, "sudo", "%s runs as %s: %s", s->user,
               (as != NULL) ? as : "root", line);

    if (as != NULL) {
        s->uid     = target.uid;
        s->gid     = target.gid;
        s->ngroups = (uint8_t)target_ngroups;
        for (size_t i = 0; i < target_ngroups; i++) {
            s->groups[i] = target_groups[i];
        }
        strlcpy(s->user, target.name, sizeof(s->user));
    } else {
        s->uid       = 0;
        s->gid       = 0;
        s->groups[0] = 0;
        s->ngroups   = 1;
        strlcpy(s->user, ESPIX_AUTH_ROOT_USER, sizeof(s->user));
    }

    const int status = espix_shell_run_line(s, line);

    s->uid     = saved_uid;
    s->gid     = saved_gid;
    s->ngroups = saved_ngroups;
    memcpy(s->groups, saved_groups, sizeof(saved_groups));
    strlcpy(s->user, saved_user, sizeof(s->user));

    return status;
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
      .help = "print the kernel log",           .usage = "dmesg [-T]" },
    { .name = "coredump", .fn = cmd_coredump,
      .help = "show or erase the stored core dump",
      .usage = "coredump [erase]" },
    { .name = "passwd", .fn = cmd_passwd,
      .help = "set a user's password",
      .usage = "passwd [user] <new-password> | passwd -l <user>" },
    { .name = "whoami", .fn = cmd_whoami,
      .help = "print the current user",
      .usage = "whoami" },
    { .name = "id",     .fn = cmd_id,
      .help = "print the current user and group ids",
      .usage = "id" },
    { .name = "useradd", .fn = cmd_useradd,
      .help = "create a user account",
      .usage = "useradd [-m] [-r] [-G group,...] <name>" },
    { .name = "userdel", .fn = cmd_userdel,
      .help = "remove a user account",
      .usage = "userdel [-r] <name>" },
    { .name = "usermod", .fn = cmd_usermod,
      .help = "change a user's group membership",
      .usage = "usermod -aG <group,...> <user>" },
    { .name = "groupadd", .fn = cmd_groupadd,
      .help = "create a group",
      .usage = "groupadd [-r] <name>" },
    { .name = "groupdel", .fn = cmd_groupdel,
      .help = "remove a group",
      .usage = "groupdel <name>" },
    { .name = "groups", .fn = cmd_groups,
      .help = "list a user's groups",
      .usage = "groups [user]" },
    { .name = "sudo",   .fn = cmd_sudo,
      .help = "run a command as root, or as another user",
      .usage = "sudo [-u <user>] <command> [args...]" },
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
