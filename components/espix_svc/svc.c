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
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "espix_kernel.h"
#include "espix_proc.h"
#include "espix_shell.h"
#include "espix_svc.h"

static const char *TAG = "espix:svc";

#define SVC_UNITS_FILE "/etc/units"
#define SVC_MAX        8
#define SVC_LINE_MAX   256
#define SVC_TICK_MS    1000
#define SVC_PRIORITY   2
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
} svc_unit_t;

static svc_unit_t        s_units[SVC_MAX];
static int               s_count;
static SemaphoreHandle_t s_lock;
static bool              s_started;

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

    if (argc == 0 || argv[0][0] != '/') {
        espix_klog(ESPIX_KLOG_WARN, TAG, "%s: needs an absolute program path",
                   u->name);
        u->enabled = false;
        return;
    }

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
    espix_klog(ESPIX_KLOG_INFO, TAG, "%s: started pid %d", u->name, (int)u->pid);
}

/*
 * Read the units file into a fresh table, then reconcile it with what is
 * running: a unit that is still declared keeps its pid, its policy and its
 * count; one that is gone is asked to stop. Called under the lock.
 */
static void units_load(void)
{
    svc_unit_t fresh[SVC_MAX];
    int        n = 0;

    FILE *f = fopen(SVC_UNITS_FILE, "r");
    if (f == NULL) {
        espix_klog(ESPIX_KLOG_INFO, TAG, "no %s; nothing to supervise",
                   SVC_UNITS_FILE);
    } else {
        char line[SVC_LINE_MAX];

        while (n < SVC_MAX && fgets(line, sizeof(line), f) != NULL) {
            char *p = line;
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            if (*p == '#' || *p == '\n' || *p == '\0') {
                continue;
            }

            char *save = NULL;
            char *name    = strtok_r(p, " \t\n", &save);
            char *restart = strtok_r(NULL, " \t\n", &save);
            char *cmd     = strtok_r(NULL, "\n", &save);

            if (name == NULL || restart == NULL || cmd == NULL) {
                continue;
            }
            while (*cmd == ' ' || *cmd == '\t') {
                cmd++;
            }
            if (*cmd == '\0' || strlen(name) >= ESPIX_SVC_NAME_MAX) {
                continue;
            }

            svc_unit_t *u = &fresh[n++];
            memset(u, 0, sizeof(*u));
            strlcpy(u->name, name, sizeof(u->name));
            strlcpy(u->cmdline, cmd, sizeof(u->cmdline));
            u->always  = (strcmp(restart, "always") == 0);
            u->enabled = true;
            u->pid     = ESPIX_PID_NONE;
        }
        fclose(f);
    }

    for (int i = 0; i < n; i++) {
        svc_unit_t *old = unit_find(fresh[i].name);
        if (old != NULL) {
            fresh[i].pid      = old->pid;
            fresh[i].enabled  = old->enabled;
            fresh[i].restarts = old->restarts;
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

/* ------------------------------------------------------------- the task --- */

static void supervisor_task(void *arg)
{
    (void)arg;

    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);

        for (int i = 0; i < s_count; i++) {
            svc_unit_t *u = &s_units[i];

            if (u->pid != ESPIX_PID_NONE) {
                int code = 0;
                if (espix_proc_wait(u->pid, &code, 0) == ESP_OK) {
                    const espix_pid_t gone = u->pid;
                    u->pid = ESPIX_PID_NONE;
                    espix_klog(ESPIX_KLOG_INFO, TAG, "%s: pid %d exited (%d)",
                               u->name, (int)gone, code);
                    if (u->always && u->enabled) {
                        u->restarts++;      /* the start below is the restart */
                    } else {
                        u->enabled = false;
                    }
                }
            }

            if (u->enabled && u->pid == ESPIX_PID_NONE) {
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
        out->restarts = s_units[index].restarts;
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
        u->enabled = true;              /* the supervisor does the starting */
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

    xSemaphoreTake(s_lock, portMAX_DELAY);
    units_load();
    xSemaphoreGive(s_lock);
    return ESP_OK;
}
