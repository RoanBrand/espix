/*
 * The supervisor.
 *
 * One task wakes once a second, reaps whatever has exited, applies the restart
 * policy and starts whatever should be running. A unit runs as an espix process
 * with no session (so no logout can take it), under a session whose output is
 * the klog -- there is no terminal to write to, and a daemon's first line is
 * usually the reason it is worth having.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "espix_kernel.h"
#include "espix_proc.h"
#include "espix_shell.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "espix_fault.h"
#include "espix_svc.h"

static const char *TAG = "espix:svc";

#define SVC_UNITS_FILE "/etc/units"
#define SVC_MAX        8
#define SVC_LINE_MAX   256
#define SVC_TICK_MS    1000
#define SVC_PRIORITY   2
/* Past this many restarts a unit is left stopped, and says so. A service
 * that dies this often is not being restarted into health. */
#define SVC_MAX_RESTARTS 5

/* A scheduled unit is not run at boot but a moment after it -- long enough for
 * the network and the clock to be up. A failed run retries sooner, a few
 * times, so "the route is not there yet" is not a six-hour wait. */
#define SVC_FIRST_DUE_US  (30LL * 1000000)
#define SVC_RETRY_US      (60LL * 1000000)
#define SVC_MAX_RETRIES   3
#define SVC_BUILTIN_STACK 8192
/* espix_proc_spawn_elf() runs on this task, and it copies the image, the
 * argv and the environment before handing relocation to its own task --
 * the same work the session task does for a foreground command, so it gets
 * the same kind of stack. */
#define SVC_STACK      8192

typedef struct {
    char        name[ESPIX_SVC_NAME_MAX];
    char        cmdline[ESPIX_PATH_MAX];
    bool        always;
    bool        enabled;
    espix_pid_t pid;
    uint32_t    restarts;
    bool        ran;             /* has started at least once */
    int         last_code;       /* what it exited with, once it has */

    /* A scheduled unit: run once every every_s seconds. 0 means it is not
     * scheduled, and the restart policy above governs instead. */
    uint32_t    every_s;
    int64_t     next_due_us;
    uint32_t    fails;

    /* A builtin unit runs on a task, not as a process, so there is no pid to
     * watch. The task owns its context and only the supervisor frees it, so
     * the unit itself is never written from another task. */
    void       *task;         /* the builtin task, or NULL */
    void       *builtin;      /* svc_builtin_t while it runs */
} svc_unit_t;

static svc_unit_t        s_units[SVC_MAX];
static int               s_count;
static SemaphoreHandle_t s_lock;
static bool              s_started;
static bool              s_safe_mode;

/* ------------------------------------------------------------ the stdio --- */

static int svc_stream_write(void *cookie, const char *buf, int len)
{
    (void)cookie;

    /* klog is line-oriented: split, so a unit that prints without a trailing
     * newline does not glue its next line onto this one. */
    int start = 0;
    for (int i = 0; i < len; i++) {
        if (buf[i] == '\n') {
            if (i > start) {
                espix_klog(ESPIX_KLOG_INFO, TAG, "%.*s", i - start, buf + start);
            }
            start = i + 1;
        }
    }
    if (len > start) {
        espix_klog(ESPIX_KLOG_INFO, TAG, "%.*s", len - start, buf + start);
    }
    return len;
}

static FILE *svc_open_stream(espix_session_t *s, espix_stream_t which)
{
    if (which == ESPIX_STREAM_IN) {
        return NULL;                 /* a unit has no stdin */
    }

    FILE *f = funopen(s, NULL, svc_stream_write, NULL, NULL);
    if (f != NULL) {
        setvbuf(f, NULL, _IOLBF, 128);
    }
    return f;
}

/*
 * The session a unit runs under: not a login, no terminal, output to the klog.
 * root, because a unit is started by the system rather than by anyone who had
 * to authenticate -- the same reason the serial console is root.
 */
static espix_session_t s_svc_session = {
    .name        = "svc",
    .cwd         = "/",
    .user        = "root",
    .open_stream = svc_open_stream,
};

/* -------------------------------------------------------------- the table --- */

static svc_unit_t *unit_find(const char *name)
{
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_units[i].name, name) == 0) {
            return &s_units[i];
        }
    }
    return NULL;
}

/* 30s, 5m, 6h, 1d; 0 on anything else. */
static uint32_t parse_duration(const char *s)
{
    char      *end = NULL;
    const long v   = strtol(s, &end, 10);
    if (end == s || v <= 0) {
        return 0;
    }

    uint32_t mult = 1;
    switch (*end) {
    case 0: case 's': case 'S': mult = 1;     break;
    case 'm': case 'M':          mult = 60;    break;
    case 'h': case 'H':          mult = 3600;  break;
    case 'd': case 'D':          mult = 86400; break;
    default: return 0;
    }

    const uint64_t total = (uint64_t)v * mult;
    return (total > 0xFFFFFFFFull) ? 0 : (uint32_t)total;
}

/* A builtin unit: the command runs on a task of its own, under the unit
 * session, because that is all a process would have given it -- and it means
 * upgrade --check is a unit without anything becoming an ELF. */
typedef struct {
    svc_unit_t        *u;
    const espix_cmd_t *cmd;
    espix_session_t   *s;
    int                argc;
    char             **argv;
    bool               caps;      /* which allocator the stack came from */
    volatile bool      done;      /* read by the supervisor, set by the task */
    int                code;
} svc_builtin_t;

static void builtin_unit_task(void *arg)
{
    svc_builtin_t *c = arg;

    espix_shell_set_current(c->s);
    c->code = c->cmd->fn(c->s, c->argc, c->argv);
    espix_shell_set_current(NULL);

    /* Hand the result to the supervisor, which frees the context: a task
     * that freed it could do so while the supervisor is reading it. */
    c->done = true;

    if (c->caps) {
        vTaskDeleteWithCaps(NULL);
    } else {
        vTaskDelete(NULL);
    }
}

static void unit_start(svc_unit_t *u)
{
    char  buf[ESPIX_PATH_MAX];
    char *argv[ESPIX_ARGS_MAX];
    int   argc = 0;

    strlcpy(buf, u->cmdline, sizeof(buf));

    char *save = NULL;
    for (char *t = strtok_r(buf, " ", &save); t != NULL && argc < ESPIX_ARGS_MAX;
         t = strtok_r(NULL, " ", &save)) {
        argv[argc++] = t;
    }

    if (argc == 0) {
        u->enabled = false;
        return;
    }

    if (argv[0][0] == '/') {
        const esp_err_t err = espix_proc_spawn_elf(argv[0], argc, argv,
                                                   &s_svc_session, NULL, false,
                                                   &u->pid);
        if (err != ESP_OK) {
            espix_klog(ESPIX_KLOG_WARN, TAG, "%s: cannot start: %s", u->name,
                       esp_err_to_name(err));
            u->pid = ESPIX_PID_NONE;
            u->enabled = false;
            return;
        }
        espix_klog(ESPIX_KLOG_INFO, TAG, "%s: started pid %d", u->name,
                   (int)u->pid);
        u->ran = true;
        return;
    }

    /* Not a program: a builtin of the shell, run under this unit's session. */
    const espix_cmd_t *cmd = espix_shell_find(argv[0]);
    if (cmd == NULL) {
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "%s: %s is neither a program nor a command", u->name,
                   argv[0]);
        u->enabled = false;
        return;
    }

    size_t bytes = 0;
    for (int i = 0; i < argc; i++) {
        bytes += strlen(argv[i]) + 1;
    }

    svc_builtin_t *c = calloc(1, sizeof(*c));
    char          *block = malloc(bytes);
    if (c != NULL) {
        c->argv = calloc((size_t)argc + 1, sizeof(*c->argv));
    }
    if (c == NULL || c->argv == NULL || block == NULL) {
        if (c != NULL) {
            free(c->argv);
        }
        free(block);
        free(c);
        u->enabled = false;
        return;
    }

    char *w = block;
    for (int i = 0; i < argc; i++) {
        const size_t n = strlen(argv[i]) + 1;
        memcpy(w, argv[i], n);
        c->argv[i] = w;
        w += n;
    }
    c->argv[argc] = NULL;
    c->argc = argc;
    c->cmd  = cmd;
    c->u    = u;
    c->s    = &s_svc_session;

    /* strlcpy, not snprintf: -Wformat-truncation is right that a 32-byte unit
     * name does not fit a 16-byte task name, and FreeRTOS truncates anyway. */
    char           name[configMAX_TASK_NAME_LEN];
    const uint32_t stack = (cmd->stack != 0) ? cmd->stack : SVC_BUILTIN_STACK;
    strlcpy(name, "svc:", sizeof(name));
    strlcat(name, u->name, sizeof(name));

    TaskHandle_t task = NULL;
    bool         caps = false;
    BaseType_t   ok;
    if (cmd->internal_stack) {
        /* It maps or writes flash, which cannot be done from a PSRAM stack. */
        ok = xTaskCreatePinnedToCore(builtin_unit_task, name, stack, c,
                                     SVC_PRIORITY, &task, 1);
    } else {
        ok = xTaskCreatePinnedToCoreWithCaps(builtin_unit_task, name, stack, c,
                                             SVC_PRIORITY, &task, 1,
                                             MALLOC_CAP_SPIRAM);
        if (ok == pdPASS) {
            caps = true;
        } else {
            ok = xTaskCreatePinnedToCore(builtin_unit_task, name, stack, c,
                                         SVC_PRIORITY, &task, 1);
        }
    }
    if (ok != pdPASS) {
        free(c->argv);
        free(block);
        free(c);
        u->enabled = false;
        return;
    }

    c->caps    = caps;
    u->builtin = c;
    u->task    = task;
    u->ran     = true;
    espix_klog(ESPIX_KLOG_INFO, TAG, "%s: started (builtin %s)", u->name,
               cmd->name);
}

/*
 * Read the units file into a fresh table, then reconcile it with what is
 * running: a unit that is still declared keeps its pid, its policy and its
 * count; one that is gone is asked to stop. Called under the lock.
 */
/*
 * The default when /etc/units is absent, in the same syntax the file uses.
 *
 * The system provides it the way it provides /etc itself and every other
 * default: a device whose filesystem is bare still checks for updates. A units
 * file replaces this outright -- there is no merge, so there is one place to
 * read the answer.
 */
static const char *const SVC_DEFAULT_UNITS =
    "# built-in default; create /etc/units to replace it\n"
    "poll every 6h upgrade --check\n";

/* One line, file or built-in. Mutates the line, as strtok_r does. */
static void units_parse(svc_unit_t *fresh, int *n, char *line)
{
    char *p = line;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == '#' || *p == '\n' || *p == '\0') {
        return;
    }

    char *save    = NULL;
    char *name    = strtok_r(p, " \t\n", &save);
    char *restart = strtok_r(NULL, " \t\n", &save);
    char *cmd     = NULL;
    uint32_t every_s = 0;

    if (name == NULL || restart == NULL) {
        return;
    }

    if (strcmp(restart, "every") == 0) {
        /* name every <interval> command: a scheduled, oneshot unit. */
        char *interval = strtok_r(NULL, " \t\n", &save);
        every_s = (interval != NULL) ? parse_duration(interval) : 0;
        if (every_s == 0) {
            return;
        }
    }

    cmd = strtok_r(NULL, "\n", &save);
    if (cmd == NULL) {
        return;
    }
    while (*cmd == ' ' || *cmd == '\t') {
        cmd++;
    }
    if (*cmd == '\0' || strlen(name) >= ESPIX_SVC_NAME_MAX) {
        return;
    }

    svc_unit_t *u = &fresh[(*n)++];
    memset(u, 0, sizeof(*u));
    strlcpy(u->name, name, sizeof(u->name));
    strlcpy(u->cmdline, cmd, sizeof(u->cmdline));
    u->always  = (strcmp(restart, "always") == 0);
    u->enabled = !s_safe_mode;
    u->pid     = ESPIX_PID_NONE;
    u->every_s = every_s;
    u->next_due_us = esp_timer_get_time() + SVC_FIRST_DUE_US;
}

static void units_load(void)
{
    /*
     * Static, and therefore off the caller's stack. Every caller holds
     * s_lock, so one copy is safe -- and it has to be one copy the caller can
     * afford: service reload runs on the SSH connection task, whose 8 KB
     * stack could not hold this 2.6 KB array plus the session, and the task
     * died of a stack protection fault (SP below its own bounds) before the
     * array ever came off it.
     */
    static svc_unit_t fresh[SVC_MAX];
    int n = 0;

    memset(fresh, 0, sizeof(fresh));

    FILE *f = fopen(SVC_UNITS_FILE, "r");
    if (f != NULL) {
        char line[SVC_LINE_MAX];
        while (n < SVC_MAX && fgets(line, sizeof(line), f) != NULL) {
            units_parse(fresh, &n, line);
        }
        fclose(f);
    } else {
        /* Said out loud: "no units file" and "no units" are different states,
         * and this one still runs the update check. */
        espix_klog(ESPIX_KLOG_INFO, TAG,
                   "no %s; using the built-in unit (poll every 6h "
                   "upgrade --check)", SVC_UNITS_FILE);

        char *copy = strdup(SVC_DEFAULT_UNITS);
        if (copy != NULL) {
            for (char *l = strtok(copy, "\n"); l != NULL && n < SVC_MAX;
                 l = strtok(NULL, "\n")) {
                units_parse(fresh, &n, l);
            }
            free(copy);
        }
    }

    for (int i = 0; i < n; i++) {
        svc_unit_t *old = unit_find(fresh[i].name);
        if (old != NULL) {
            fresh[i].pid         = old->pid;
            fresh[i].restarts    = old->restarts;
            fresh[i].next_due_us = old->next_due_us;
            fresh[i].fails       = old->fails;
            fresh[i].task        = old->task;
            fresh[i].builtin     = old->builtin;
            if (!s_safe_mode) {
                fresh[i].enabled = old->enabled;
            }
        }
    }

    for (int i = 0; i < s_count; i++) {
        bool keep = false;
        for (int j = 0; j < n && !keep; j++) {
            keep = (strcmp(s_units[i].name, fresh[j].name) == 0);
        }
        if (!keep && s_units[i].pid != ESPIX_PID_NONE) {
            espix_klog(ESPIX_KLOG_INFO, TAG, "%s: removed; stopping pid %d",
                       s_units[i].name, (int)s_units[i].pid);
            (void)espix_proc_signal(s_units[i].pid, SIGTERM);
        }
    }

    memcpy(s_units, fresh, sizeof(fresh));
    s_count = n;
}

/*
 * What happens after a unit exits, whoever it was. A scheduled unit is
 * oneshot and re-armed; a failure retries sooner a few times, so a network
 * that is not up yet costs a minute and not a whole interval.
 */
static void unit_exited(svc_unit_t *u, int code)
{
    u->last_code = code;
    if (u->every_s > 0) {
        const int64_t now = esp_timer_get_time();
        if (code != 0 && u->fails < SVC_MAX_RETRIES) {
            u->fails++;
            u->next_due_us = now + SVC_RETRY_US;
            espix_klog(ESPIX_KLOG_INFO, TAG, "%s: exit %d; retrying in %d s",
                       u->name, code, (int)(SVC_RETRY_US / 1000000));
        } else {
            u->fails       = 0;
            u->next_due_us = now + (int64_t)u->every_s * 1000000;
        }
        return;
    }

    if (u->always && u->enabled) {
        u->restarts++;
        if (u->restarts > SVC_MAX_RESTARTS) {
            espix_klog(ESPIX_KLOG_WARN, TAG,
                       "%s: %u restarts; leaving it stopped "
                       "(service start %s to retry)",
                       u->name, (unsigned)u->restarts, u->name);
            u->enabled = false;
        }
    } else {
        u->enabled = false;
    }
}

/* ------------------------------------------------------------- the task --- */

static void supervisor_task(void *arg)
{
    (void)arg;

    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);

        const int64_t now = esp_timer_get_time();

        for (int i = 0; i < s_count; i++) {
            svc_unit_t *u = &s_units[i];

            if (u->pid != ESPIX_PID_NONE) {
                int code = 0;
                if (espix_proc_wait(u->pid, &code, 0) == ESP_OK) {
                    const espix_pid_t gone = u->pid;
                    u->pid = ESPIX_PID_NONE;
                    espix_klog(ESPIX_KLOG_INFO, TAG, "%s: pid %d exited (%d)",
                               u->name, (int)gone, code);
                    unit_exited(u, code);
                }
            }

            svc_builtin_t *b = u->builtin;
            if (b != NULL && b->done) {
                const int code = b->code;
                free(b->argv);
                u->builtin = NULL;
                u->task    = NULL;
                espix_klog(ESPIX_KLOG_INFO, TAG, "%s: finished (%d)", u->name,
                           code);
                unit_exited(u, code);
            }

            const bool busy = (u->pid != ESPIX_PID_NONE) || (u->task != NULL);
            if (u->enabled && !busy &&
                (u->every_s == 0 || now >= u->next_due_us)) {
                unit_start(u);
            }
        }

        xSemaphoreGive(s_lock);
        vTaskDelay(pdMS_TO_TICKS(SVC_TICK_MS));
    }
}

/* --------------------------------------------------------------- public --- */

esp_err_t espix_svc_init(void)
{
    if (s_started) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /*
     * Safe mode. A core dump means the previous boot faulted, and starting
     * unattended work again is exactly how one bad unit turns into a boot
     * loop. The units are read but left stopped; a manual "service start"
     * still starts one, and "coredump erase" clears the state.
     */
    espix_coredump_info_t dump;
    const bool have_dump =
        (espix_fault_coredump_status(&dump) == ESP_OK && dump.present);

    /*
     * Two signals, because the interesting fault defeats the first one. A core
     * dump says the last run faulted. A panic reset reason says the same even
     * when no dump could be written -- which is what a cache fault during a
     * flash operation does (exccause 0x47, Cache_WriteBack_Addr), and that is
     * precisely the fault that boot-loops a unit.
     */
    const esp_reset_reason_t why = esp_reset_reason();
    const bool crashed = (why == ESP_RST_PANIC || why == ESP_RST_TASK_WDT ||
                          why == ESP_RST_INT_WDT || why == ESP_RST_WDT);

    if (have_dump || crashed) {
        s_safe_mode = true;
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "safe mode: %s; units are not started",
                   have_dump ? "a core dump is stored"
                             : "the last boot panicked");
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    units_load();
    xSemaphoreGive(s_lock);

    s_started = true;

    /*
     * Pinned to core 1 with a PSRAM stack, exactly as espix_proc creates an
     * app's task, and for the same reason: the toolchain emits vectorised
     * memcpy for the S31, the vector unit exists only on core 1, and a
     * vectorised copy on core 0 is an illegal-instruction trap. spawn_elf()
     * below copies the image and the environment, so it is full of them.
     */
    if (xTaskCreatePinnedToCoreWithCaps(supervisor_task, "espix:svc", SVC_STACK,
                                        NULL, SVC_PRIORITY, NULL, 1,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool espix_svc_safe_mode(void)
{
    return s_safe_mode;
}

int espix_svc_count(void)
{
    if (!s_started) {
        return 0;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int n = s_count;
    xSemaphoreGive(s_lock);
    return n;
}

bool espix_svc_info(int index, espix_svc_info_t *out)
{
    if (!s_started || out == NULL) {
        return false;
    }

    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (index >= 0 && index < s_count) {
        memset(out, 0, sizeof(*out));
        strlcpy(out->name, s_units[index].name, sizeof(out->name));
        out->pid      = s_units[index].pid;
        out->always   = s_units[index].always;
        out->enabled  = s_units[index].enabled;
        out->running  = (s_units[index].task != NULL);
        out->every_s  = s_units[index].every_s;
        out->restarts = s_units[index].restarts;
        out->ran      = s_units[index].ran;
        out->last_code = s_units[index].last_code;
        ok = true;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

esp_err_t espix_svc_start(const char *name)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ESP_ERR_NOT_FOUND;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    svc_unit_t *u = unit_find(name);
    if (u != NULL) {
        u->enabled  = true;             /* the supervisor does the starting */
        u->restarts = 0;                /* and a manual start gets a fresh run */
        err = ESP_OK;
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t espix_svc_stop(const char *name)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ESP_ERR_NOT_FOUND;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    svc_unit_t *u = unit_find(name);
    if (u != NULL) {
        u->enabled = false;
        if (u->pid != ESPIX_PID_NONE) {
            (void)espix_proc_signal(u->pid, SIGTERM);
        }
        err = ESP_OK;
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t espix_svc_reload(void)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * An explicit reload is the operator saying they have dealt with whatever
     * faulted, so it is also what leaves safe mode -- otherwise the message
     * that says "coredump erase, then service reload" would be a lie.
     */
    const bool was_safe = s_safe_mode;
    s_safe_mode = false;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    units_load();
    if (was_safe) {
        for (int i = 0; i < s_count; i++) {
            s_units[i].enabled = true;
        }
        espix_klog(ESPIX_KLOG_INFO, TAG, "safe mode cleared; units resume");
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}
