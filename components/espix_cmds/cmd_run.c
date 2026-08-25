/*
 * Process commands: run, kill, crash.
 *
 * `run` is the centerpiece: load an ELF built on a PC off the rootfs and
 * execute it as a process.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"

#include "espix_cmds_priv.h"
#include "espix_fs.h"
#include "espix_kernel.h"
#include "espix_proc.h"
#include "espix_shell.h"

/* How long a foreground `run` waits before giving up on the app and leaving it
 * running in the background. */
/* How often the foreground wait comes up for air to check for Ctrl-C. */
#define RUN_POLL_MS 50

#define RUN_FOREGROUND_TIMEOUT_MS 60000

/*
 * Spawn `abs` with the given argv and, unless backgrounded, wait for it and
 * report its status. Shared by `run` and by the fallback that resolves a bare
 * command name to a program.
 */
static int run_program(espix_session_t *s, const char *abs, int argc,
                       char **argv, bool background, const char *who)
{
    espix_pid_t     pid = ESPIX_PID_NONE;
    const esp_err_t err = espix_proc_spawn_elf(abs, argc, argv, s, &pid);

    if (err != ESP_OK) {
        espix_printf(s, "%s: %s: %s\n", who, abs, esp_err_to_name(err));
        return 1;
    }

    if (background) {
        espix_printf(s, "[%d] %s\n", (int)pid, abs);
        return 0;
    }

    s->fg_pid = pid;

    /*
     * Wait in slices rather than one long block, so Ctrl-C can be noticed.
     * Nothing else reads input while a foreground process runs -- the editor is
     * not running and this task is the one that would be reading -- so without
     * this a program that ignores its own exit conditions cannot be stopped
     * from the session that started it, and the Ctrl-Cs surface as blank lines
     * once it finally dies.
     *
     * 50ms is short enough to feel immediate and long enough that polling costs
     * nothing measurable.
     */
    int       exit_code = -1;
    esp_err_t wait_err  = ESP_ERR_TIMEOUT;
    bool      asked     = false;

    for (unsigned waited = 0; waited < RUN_FOREGROUND_TIMEOUT_MS;
         waited += RUN_POLL_MS) {

        wait_err = espix_proc_wait(pid, &exit_code, pdMS_TO_TICKS(RUN_POLL_MS));
        if (wait_err != ESP_ERR_TIMEOUT) {
            break;                      /* finished, one way or another */
        }

        /*
         * Poll on every slice, even once the process has been asked to stop:
         * the point is to keep *consuming* input, not just to notice the first
         * Ctrl-C. Someone who presses it five times should not get five blank
         * lines on the next prompt.
         */
        if (s->poll_interrupt != NULL && s->poll_interrupt(s) && !asked) {
            /*
             * Ask once. espix_proc_kill() escalates to deleting the task if the
             * app does not take the hint, so a second Ctrl-C would only race
             * that -- and an app that cleans up deserves the chance to finish.
             */
            asked = true;
            espix_printf(s, "^C\n");
            espix_proc_kill(pid);
        }
    }

    /* Whatever was typed between the last poll and the process exiting is still
     * queued, and would otherwise arrive at the next prompt. */
    if (s->poll_interrupt != NULL) {
        (void)s->poll_interrupt(s);
    }

    s->fg_pid = ESPIX_PID_NONE;

    if (wait_err == ESP_ERR_TIMEOUT) {
        espix_printf(s, "%s: pid %d still running, detaching\n", who, (int)pid);
        return 1;
    }
    if (wait_err != ESP_OK) {
        espix_printf(s, "%s: pid %d: %s\n", who, (int)pid, esp_err_to_name(wait_err));
        return 1;
    }

    if (exit_code != 0) {
        espix_printf(s, "[exit %d]\n", exit_code);
    }
    return exit_code;
}

static int cmd_run(espix_session_t *s, int argc, char **argv)
{
    if (argc < 2) {
        espix_printf(s, "usage: run <path> [args...]\n");
        return 1;
    }

    bool background = false;
    if (strcmp(argv[argc - 1], "&") == 0) {
        background = true;
        argc--;
    }

    char abs[ESPIX_PATH_MAX];
    if (!espix_cmd_path(s, argv[1], abs, sizeof(abs))) {
        return 1;
    }

    /* The app sees argv[0] as its own path, then its own arguments. */
    char *app_argv[ESPIX_ARGS_MAX];
    int   app_argc = 0;

    app_argv[app_argc++] = abs;
    for (int i = 2; i < argc && app_argc < ESPIX_ARGS_MAX; i++) {
        app_argv[app_argc++] = argv[i];
    }

    return run_program(s, abs, app_argc, app_argv, background, "run");
}

/*
 * Resolve a command line whose first word is not a builtin, the way a shell
 * falls through to PATH.
 *
 * A name containing a slash is a path, relative to the session's cwd; anything
 * else is looked for in /bin. PATH is that one fixed directory for now, since
 * espix has no environment to put a real one in.
 *
 * There is no execute bit to check — LittleFS has no mode bits — so the ELF
 * magic is the gate, which is what espix_proc would test anyway. That makes
 * "present but not a program" a distinguishable answer rather than a silent
 * "not found".
 */
static int exec_fallback(espix_session_t *s, int argc, char **argv)
{
    char abs[ESPIX_PATH_MAX];

    if (strchr(argv[0], '/') != NULL) {
        if (!espix_cmd_path(s, argv[0], abs, sizeof(abs))) {
            return 1;
        }
    } else if ((size_t)snprintf(abs, sizeof(abs), "/bin/%s", argv[0]) >= sizeof(abs)) {
        return ESPIX_SHELL_ENOENT;
    }

    struct stat st;
    if (stat(abs, &st) != 0 || !S_ISREG(st.st_mode)) {
        return ESPIX_SHELL_ENOENT;      /* reported as "command not found" */
    }

    FILE *f = fopen(abs, "rb");
    if (f == NULL) {
        return ESPIX_SHELL_ENOENT;
    }
    char magic[4] = { 0 };
    const size_t got = fread(magic, 1, sizeof(magic), f);
    fclose(f);

    if (got != sizeof(magic) || memcmp(magic, "\177ELF", sizeof(magic)) != 0) {
        espix_printf(s, "espix: %s: Exec format error\n", argv[0]);
        return 126;                     /* what a shell returns for this */
    }

    bool background = false;
    if (argc > 1 && strcmp(argv[argc - 1], "&") == 0) {
        background = true;
        argc--;
    }

    /* argv[0] becomes the resolved path, as execve() would leave it. */
    char *app_argv[ESPIX_ARGS_MAX];
    int   app_argc = 0;

    app_argv[app_argc++] = abs;
    for (int i = 1; i < argc && app_argc < ESPIX_ARGS_MAX; i++) {
        app_argv[app_argc++] = argv[i];
    }

    return run_program(s, abs, app_argc, app_argv, background, "espix");
}

void espix_cmds_register_exec_fallback(void)
{
    espix_shell_set_exec_fallback(exec_fallback);
}

static int cmd_kill(espix_session_t *s, int argc, char **argv)
{
    if (argc < 2) {
        espix_printf(s, "usage: kill <pid>...\n");
        return 1;
    }

    int status = 0;

    for (int i = 1; i < argc; i++) {
        const espix_pid_t pid = (espix_pid_t)strtol(argv[i], NULL, 10);
        const esp_err_t   err = espix_proc_kill(pid);

        if (err == ESP_ERR_NOT_FOUND) {
            espix_printf(s, "kill: %d: no such process\n", (int)pid);
            status = 1;
        } else if (err == ESP_ERR_INVALID_STATE) {
            espix_printf(s, "kill: %d: already finished\n", (int)pid);
            status = 1;
        } else if (err != ESP_OK) {
            espix_printf(s, "kill: %d: %s\n", (int)pid, esp_err_to_name(err));
            status = 1;
        }
    }

    return status;
}

static int cmd_crash(espix_session_t *s, int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /*
     * Deliberate null-pointer store, to exercise the fault hook end to end.
     * Kept as a built-in rather than a test app because it must be triggerable
     * before the ELF loader path works.
     */
    espix_printf(s, "storing to address 0 — expect a fault report\n");
    espix_klog(ESPIX_KLOG_WARN, "crash", "deliberate fault requested");

    volatile int *nowhere = NULL;
    *nowhere = 1;

    espix_printf(s, "unreachable\n");
    return 1;
}

static espix_cmd_t s_run_cmds[] = {
    { .name = "run",   .fn = cmd_run,
      .help = "load and run an app from the filesystem",
      .usage = "run <path> [args...] [&]" },
    { .name = "kill",  .fn = cmd_kill,
      .help = "terminate a process",
      .usage = "kill <pid>..." },
    { .name = "crash", .fn = cmd_crash,
      .help = "fault on purpose, to test fault reporting",
      .usage = "crash" },
};

void espix_cmds_register_run(void)
{
    espix_cmds_register_table(s_run_cmds,
                             sizeof(s_run_cmds) / sizeof(s_run_cmds[0]));
}
