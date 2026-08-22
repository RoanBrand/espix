/*
 * Process commands: run, kill, crash.
 *
 * `run` is the centerpiece: load an ELF built on a PC off the rootfs and
 * execute it as a process.
 */

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

#include "espix_cmds_priv.h"
#include "espix_fs.h"
#include "espix_kernel.h"
#include "espix_proc.h"
#include "espix_shell.h"

/* How long a foreground `run` waits before giving up on the app and leaving it
 * running in the background. */
#define RUN_FOREGROUND_TIMEOUT_MS 60000

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

    espix_pid_t pid = ESPIX_PID_NONE;
    const esp_err_t err = espix_proc_spawn_elf(abs, app_argc, app_argv, s, &pid);
    if (err != ESP_OK) {
        espix_printf(s, "run: %s: %s\n", abs, esp_err_to_name(err));
        return 1;
    }

    if (background) {
        espix_printf(s, "[%d] %s\n", (int)pid, abs);
        return 0;
    }

    s->fg_pid = pid;

    int exit_code = -1;
    const esp_err_t wait_err =
        espix_proc_wait(pid, &exit_code, pdMS_TO_TICKS(RUN_FOREGROUND_TIMEOUT_MS));

    s->fg_pid = ESPIX_PID_NONE;

    if (wait_err == ESP_ERR_TIMEOUT) {
        espix_printf(s, "run: pid %d still running, detaching\n", (int)pid);
        return 1;
    }
    if (wait_err != ESP_OK) {
        espix_printf(s, "run: pid %d: %s\n", (int)pid, esp_err_to_name(wait_err));
        return 1;
    }

    if (exit_code != 0) {
        espix_printf(s, "[exit %d]\n", exit_code);
    }
    return exit_code;
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
