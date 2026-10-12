/*
 * The supervisor.
 *
 * One task wakes once a second, reaps whatever has exited, applies the restart
 * policy and starts whatever should be running. Every unit is a process -- a
 * program, or a builtin run as a DAEMON (espix_proc_spawn_fn()) -- so one pid,
 * one wait and one stop cover both. It runs under a session no logout can end,
 * whose output is the klog: there is no terminal to write to, and a daemon's
 * first line is usually the reason it is worth having.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "espix_kernel.h"
#include "espix_proc.h"
#include "espix_shell.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_system.h"
#include "esp_timer.h"

#include "espix_fault.h"
#include "espix_fs.h"
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
#define SVC_SUCCESS_MAX   4     /* success= codes a unit may list */
#define SVC_REQUIRES_LEN  64    /* requires=, as written */
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
    bool        stop;            /* asked to stop, and not gone yet */
    bool        ran;             /* has started at least once */
    int         last_code;       /* what it exited with, once it has */
    bool        last_ok;         /* and whether that counts as success */

    int         success[SVC_SUCCESS_MAX];   /* exit statuses besides 0 that do */
    uint8_t     nsuccess;
    char        requires[SVC_REQUIRES_LEN]; /* units that must be up first */
    const char *waiting;         /* why it has not started, last logged */

    /* A scheduled unit: run once every every_s seconds. 0 means it is not
     * scheduled, and the restart policy above governs instead. */
    uint32_t    every_s;
    int64_t     next_due_us;
    uint32_t    fails;
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

/* A builtin unit: the command, run as a DAEMON process under the unit session,
 * which is what lets upgrade --check be a unit without becoming an ELF. The
 * process owns this and frees it. */
typedef struct {
    const espix_cmd_t *cmd;
    int                argc;
    char             **argv;
    char              *block;     /* the argv strings */
} svc_builtin_t;

static int builtin_unit_main(void *arg)
{
    svc_builtin_t *const c = arg;

    espix_shell_set_current(&s_svc_session);
    const int code = c->cmd->fn(&s_svc_session, c->argc, c->argv);
    espix_shell_set_current(NULL);

    free(c->block);
    free(c->argv);
    free(c);
    return code;
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
    if (c != NULL) {
        c->argv  = calloc((size_t)argc + 1, sizeof(*c->argv));
        c->block = malloc(bytes);
    }
    if (c == NULL || c->argv == NULL || c->block == NULL) {
        if (c != NULL) {
            free(c->argv);
            free(c->block);
        }
        free(c);
        u->enabled = false;
        return;
    }

    char *w = c->block;
    for (int i = 0; i < argc; i++) {
        const size_t n = strlen(argv[i]) + 1;
        memcpy(w, argv[i], n);
        c->argv[i] = w;
        w += n;
    }
    c->argv[argc] = NULL;
    c->argc = argc;
    c->cmd  = cmd;

    const espix_proc_fn_opts_t opts = {
        .name           = u->name,
        .fn             = builtin_unit_main,
        .arg            = c,
        .kind           = ESPIX_PROC_DAEMON,
        .stack          = (cmd->stack != 0) ? cmd->stack : SVC_BUILTIN_STACK,
        .priority       = SVC_PRIORITY,
        .core           = 1,
        /* It maps or writes flash, which cannot be done from a PSRAM stack. */
        .internal_stack = cmd->internal_stack,
        .session        = &s_svc_session,
    };
    const esp_err_t err = espix_proc_spawn_fn(&opts, &u->pid);
    if (err != ESP_OK) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "%s: cannot start: %s", u->name,
                   esp_err_to_name(err));
        free(c->argv);
        free(c->block);
        free(c);
        u->pid     = ESPIX_PID_NONE;
        u->enabled = false;
        return;
    }

    u->ran = true;
    espix_klog(ESPIX_KLOG_INFO, TAG, "%s: started pid %d (builtin %s)", u->name,
               (int)u->pid, cmd->name);
}

/* The file when there is none, and what is parsed when it cannot be written.
 * Everything but the update check is commented out. */
static const char *const SVC_DEFAULT_UNITS =
    "# What espix runs at boot: one unit per line.\n"
    "#\n"
    "#   <name>  <restart>  [options]  <command> [args...]\n"
    "#\n"
    "#   name      what service and the klog call it; unique\n"
    "#   restart   always    start it, and again whenever it exits\n"
    "#             once      run it once\n"
    "#             every N   run it on that schedule (s, m, h, d)\n"
    "#   options   success=N[,N]   exit statuses besides 0 that mean it worked\n"
    "#             requires=A[,B]  start once those are up: a once unit has\n"
    "#                             succeeded, an always unit is running\n"
    "#   command   a program if it starts with /, else a shell builtin; it runs\n"
    "#             as root with no terminal, and its output goes to the klog\n"
    "#\n"
    "# service lists the units; service reload applies an edit.\n"
    "\n"
    "# Ask every six hours whether a newer build exists; 100 means one does.\n"
    "autoupdate  every 6h  success=100  upgrade --check\n"
    "\n"
    "# Serve what /etc/exports lists.\n"
    "# nfsd  always  nfsd\n"
    "\n"
    "# Bring wg0 up with the peers in /etc/vpn.\n"
    "# vpn  once  vpn up\n"
    "\n"
    "# The canvas and its VNC server, then the desktop on it.\n"
    "# vnc      once  vnc start\n"
    "# desktop  once  requires=vnc  desktop start\n"
    "\n"
    "# Dynamic DNS through dynu, every 15 minutes. Use the IP-update password:\n"
    "# fetch masks it in the log, but this file holds it.\n"
    "# dynu  every 15m  fetch https://api.dynu.com/nic/update?hostname=HOST&password=PASSWORD /tmp/ddns.out\n";

/*
 * Write the file the first time, so the format and the options are where
 * somebody will read them. 0600, like /etc/sudoers: a unit line is a root
 * command line, and this is where it is written down.
 */
static void units_seed(void)
{
    const int fd = open(SVC_UNITS_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "%s: cannot create it", SVC_UNITS_FILE);
        return;
    }
    FILE *f = fdopen(fd, "w");
    if (f == NULL) {
        close(fd);
        return;
    }
    fputs(SVC_DEFAULT_UNITS, f);
    fclose(f);

    /*
     * Asked for explicitly, because a file's mode comes from the rule for its
     * path and not from what open() was given: a fresh /etc/units arrived 0644.
     * That is the wrong answer here -- this file can hold a ddns URL with a
     * password in it, and a unit line is a root command line, which is why
     * /etc/sudoers is 0600 for the same reason.
     */
    (void)espix_fs_chmod(SVC_UNITS_FILE, 0600);

    struct stat st;
    if (stat(SVC_UNITS_FILE, &st) == 0) {
        espix_klog(ESPIX_KLOG_INFO, TAG, "%s: written, mode 0%o",
                   SVC_UNITS_FILE, (unsigned)(st.st_mode & 0777));
    }
}

/* The next blank-separated word of *p, terminated in place; NULL at the end. */
static char *next_word(char **p)
{
    char *w = *p;
    while (*w == ' ' || *w == '\t') {
        w++;
    }
    if (*w == '\0' || *w == '\n') {
        *p = w;
        return NULL;
    }
    char *e = w;
    while (*e != '\0' && *e != ' ' && *e != '\t' && *e != '\n') {
        e++;
    }
    if (*e != '\0') {
        *e++ = '\0';
    }
    *p = e;
    return w;
}

/* success=N[,N]: false when it is not a list of statuses. */
static bool parse_success(svc_unit_t *u, const char *v)
{
    while (*v != '\0') {
        char      *end = NULL;
        const long c   = strtol(v, &end, 10);
        if (end == v || c < 0 || c > 255 || u->nsuccess >= SVC_SUCCESS_MAX ||
            (*end != ',' && *end != '\0')) {
            return false;
        }
        u->success[u->nsuccess++] = (int)c;
        v = (*end == ',') ? end + 1 : end;
    }
    return u->nsuccess > 0;
}

/* One line, file or built-in. Mutates the line. */
static void units_parse(svc_unit_t *fresh, int *n, char *line)
{
    char *p       = line;
    char *name    = next_word(&p);
    char *restart = next_word(&p);

    if (name == NULL || name[0] == '#' || restart == NULL ||
        strlen(name) >= ESPIX_SVC_NAME_MAX) {
        return;
    }

    svc_unit_t u;
    memset(&u, 0, sizeof(u));

    if (strcmp(restart, "every") == 0) {
        char *interval = next_word(&p);
        u.every_s = (interval != NULL) ? parse_duration(interval) : 0;
        if (u.every_s == 0) {
            espix_klog(ESPIX_KLOG_WARN, TAG, "%s: bad interval; ignored", name);
            return;
        }
    }

    /* Options, until the first word that is not one: the command. */
    for (;;) {
        char *q = p;
        char *w = next_word(&q);
        if (w == NULL) {
            break;
        }
        bool ok;
        if (strncmp(w, "success=", 8) == 0) {
            ok = parse_success(&u, w + 8);
        } else if (strncmp(w, "requires=", 9) == 0) {
            ok = w[9] != '\0' && strlen(w + 9) < sizeof(u.requires);
            if (ok) {
                strlcpy(u.requires, w + 9, sizeof(u.requires));
            }
        } else {
            /* Not an option; next_word() cut it, so put the blank back. */
            if (q > w && q[-1] == '\0') {
                q[-1] = ' ';
            }
            break;
        }
        if (!ok) {
            espix_klog(ESPIX_KLOG_WARN, TAG, "%s: bad option %s; ignored", name, w);
            return;
        }
        p = q;
    }

    while (*p == ' ' || *p == '\t') {
        p++;
    }
    p[strcspn(p, "\n")] = '\0';
    if (*p == '\0') {
        return;
    }

    strlcpy(u.name, name, sizeof(u.name));
    strlcpy(u.cmdline, p, sizeof(u.cmdline));
    u.always      = (strcmp(restart, "always") == 0);
    u.enabled     = !s_safe_mode;
    u.pid         = ESPIX_PID_NONE;
    u.next_due_us = esp_timer_get_time() + SVC_FIRST_DUE_US;
    fresh[(*n)++] = u;
}

/*
 * Read the units file into a fresh table, then reconcile it with what is
 * running: a unit still declared keeps its pid and its counts; one that is
 * gone is asked to stop. Called under the lock.
 */
static bool units_load(void)
{
    /* Static: callers hold s_lock, and service reload runs on an SSH
     * connection's stack, which has no room for the table. */
    static svc_unit_t fresh[SVC_MAX];
    int n = 0;

    memset(fresh, 0, sizeof(fresh));

    errno = 0;                      /* so the branch below is about this open */
    FILE *f = fopen(SVC_UNITS_FILE, "r");
    if (f != NULL) {
        char line[SVC_LINE_MAX];
        while (n < SVC_MAX && fgets(line, sizeof(line), f) != NULL) {
            units_parse(fresh, &n, line);
        }
        fclose(f);
    } else if (errno != ENOENT) {
        /* There and unreadable: leave the table as it is and say so, rather
         * than fall back to the default and drop the operator's units. */
        espix_klog(ESPIX_KLOG_ERROR, TAG, "%s: cannot be read: %s",
                   SVC_UNITS_FILE, strerror(errno));
        return false;
    } else {
        /* Written, so the format is in front of whoever edits it; parsed
         * from memory, so an unwritable filesystem still gets the default. */
        units_seed();

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
            fresh[i].stop        = old->stop;
            fresh[i].ran         = old->ran;
            fresh[i].last_code   = old->last_code;
            fresh[i].last_ok     = old->last_ok;
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
    return true;
}

/*
 * What happens after a unit exits, whoever it was. A scheduled unit is
 * oneshot and re-armed; a failure retries sooner a few times, so a network
 * that is not up yet costs a minute and not a whole interval.
 */
static bool unit_succeeded(const svc_unit_t *u, int code)
{
    for (int i = 0; i < u->nsuccess; i++) {
        if (u->success[i] == code) {
            return true;
        }
    }
    return code == 0;
}

static void unit_exited(svc_unit_t *u, int code)
{
    u->last_code = code;
    u->last_ok   = unit_succeeded(u, code);
    if (u->every_s > 0) {
        const int64_t now = esp_timer_get_time();
        if (!u->last_ok && u->fails < SVC_MAX_RETRIES) {
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

/*
 * Whether everything u requires is up: a once or every unit whose last run
 * succeeded, an always unit that is running. NULL when it is; otherwise why
 * not, with the unit it is waiting for in dep.
 */
static const char *unit_blocked(const svc_unit_t *u, char *dep, size_t len)
{
    char  list[SVC_REQUIRES_LEN];
    char *save = NULL;

    strlcpy(list, u->requires, sizeof(list));
    for (char *name = strtok_r(list, ",", &save); name != NULL;
         name = strtok_r(NULL, ",", &save)) {
        const svc_unit_t *d = unit_find(name);
        const char       *why = NULL;

        if (d == NULL) {
            why = "which is not a unit";
        } else if (d->always) {
            why = (d->pid != ESPIX_PID_NONE) ? NULL : "which is not running";
        } else if (d->pid != ESPIX_PID_NONE || !d->ran) {
            why = "which has not finished";
        } else if (!d->last_ok) {
            why = "which failed";
        }
        if (why != NULL) {
            strlcpy(dep, name, len);
            return why;
        }
    }
    return NULL;
}

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
                    u->pid  = ESPIX_PID_NONE;
                    u->stop = false;
                    espix_klog(ESPIX_KLOG_INFO, TAG, "%s: pid %d exited (%d)",
                               u->name, (int)gone, code);
                    unit_exited(u, code);
                }
            }

            if (u->enabled && u->pid == ESPIX_PID_NONE &&
                !espix_shutdown_started() &&
                (u->every_s == 0 || now >= u->next_due_us)) {
                char        dep[ESPIX_SVC_NAME_MAX];
                const char *why = unit_blocked(u, dep, sizeof(dep));
                if (why == NULL) {
                    u->waiting = NULL;
                    unit_start(u);
                } else if (why != u->waiting) {
                    u->waiting = why;
                    espix_klog(ESPIX_KLOG_INFO, TAG, "%s: waiting for %s, %s",
                               u->name, dep, why);
                }
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
    /* False means the file was there and could not be read; it has logged why,
     * and an empty table is the honest state rather than the built-in default. */
    (void)units_load();
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
        out->every_s  = s_units[index].every_s;
        out->restarts = s_units[index].restarts;
        out->stopping = s_units[index].stop;
        out->ran      = s_units[index].ran;
        out->last_code = s_units[index].last_code;
        out->last_ok  = s_units[index].last_ok;
        out->waiting  = s_units[index].waiting != NULL &&
                        s_units[index].pid == ESPIX_PID_NONE;
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
            u->stop = true;
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

    /* Cleared before the load, because units_load() decides whether to carry a
     * unit's enabled state over by asking it; put back on failure, so a reload
     * that could not read the file changes nothing at all. */
    s_safe_mode = false;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool loaded = units_load();
    if (loaded && was_safe) {
        for (int i = 0; i < s_count; i++) {
            s_units[i].enabled = true;
        }
        espix_klog(ESPIX_KLOG_INFO, TAG, "safe mode cleared; units resume");
    }
    if (!loaded) {
        s_safe_mode = was_safe;
    }
    xSemaphoreGive(s_lock);

    return loaded ? ESP_OK : ESP_FAIL;
}
