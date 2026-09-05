/*
 * Process commands: run, kill, crash.
 *
 * `run` is the centerpiece: load an ELF built on a PC off the rootfs and
 * execute it as a process.
 */

#include <signal.h>
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

/* How often the foreground wait comes up for air to check for Ctrl-C. */
#define RUN_POLL_MS 50

/* How long a foreground `run` waits before giving up on the app and leaving it
 * running in the background. */
#define RUN_FOREGROUND_TIMEOUT_MS 60000

/*
 * Ctrl-C presses before the shell stops asking and starts insisting.
 *
 * Ctrl-C sends SIGINT and nothing more, which is what Unix does and what makes
 * a handler worth writing: an app is allowed to catch it, tidy up on its own
 * schedule, or decline. But espix has one console and no second terminal to run
 * `kill -9` from, so refusing to ever escalate would mean an app that ignores
 * SIGINT could hold the only shell you have. The third press is that escape
 * hatch, and it announces itself before it fires.
 */
#define RUN_INTERRUPTS_TO_KILL 3

/*
 * Spawn `abs` with the given argv and, unless backgrounded, wait for it and
 * report its status. Shared by `run` and by the fallback that resolves a bare
 * command name to a program.
 */
static int run_program(espix_session_t *s, const char *abs, int argc,
                       char **argv, bool background, const char *root,
                       const char *who)
{
    espix_pid_t     pid = ESPIX_PID_NONE;
    const esp_err_t err = espix_proc_spawn_elf(abs, argc, argv, s, root, &pid);

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
    int       exit_code  = -1;
    esp_err_t wait_err   = ESP_ERR_TIMEOUT;
    unsigned  interrupts = 0;

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
        if (s->poll_interrupt == NULL || !s->poll_interrupt(s)) {
            continue;
        }

        espix_printf(s, "^C\n");
        interrupts++;

        /*
         * SIGINT, and only SIGINT. The app may have a handler; running it and
         * letting the app decide is the whole point of having signals, and
         * deleting the task from under a handler that was about to put the
         * hardware back would undo the reason any of this exists.
         *
         * A press only counts once per 50ms slice, since poll_interrupt()
         * reports "something arrived" rather than how many -- which suits a
         * person pressing a key and means a held-down Ctrl-C does not race
         * straight to the kill.
         */
        if (interrupts < RUN_INTERRUPTS_TO_KILL) {
            (void)espix_proc_signal(pid, SIGINT);

            if (interrupts + 1 == RUN_INTERRUPTS_TO_KILL) {
                espix_printf(s, "%s: pid %d is ignoring SIGINT; "
                                "press Ctrl-C again to force it\n",
                             who, (int)pid);
            }
        } else if (interrupts == RUN_INTERRUPTS_TO_KILL) {
            espix_printf(s, "%s: killing pid %d\n", who, (int)pid);
            (void)espix_proc_signal(pid, SIGKILL);
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

/*
 * `run [-R <dir>] <path> [args...] [&]`
 *
 * -R gives the program a root: it can name nothing outside that directory, and
 * starts there. The binary itself is read before the process exists, so it is
 * normal for it to live outside -- `run -R /srv/www /bin/httpd` is the shape
 * this is for, alongside `sudo -u www` giving the same program its own identity.
 */
static int cmd_run(espix_session_t *s, int argc, char **argv)
{
    const char *root  = NULL;
    int         first = 1;

    /* Match the flag first and then require its argument, so a bare `run -R`
     * is a usage error rather than an attempt to run a program called -R. */
    if (argc >= 2 && strcmp(argv[1], "-R") == 0) {
        if (argc < 3) {
            espix_printf(s, "usage: run [-R <dir>] <path> [args...]\n");
            return 1;
        }
        root  = argv[2];
        first = 3;
    }

    if (argc <= first) {
        espix_printf(s, "usage: run [-R <dir>] <path> [args...]\n");
        return 1;
    }

    bool background = false;
    if (strcmp(argv[argc - 1], "&") == 0) {
        background = true;
        argc--;
    }
    if (argc <= first) {
        espix_printf(s, "usage: run [-R <dir>] <path> [args...]\n");
        return 1;
    }

    /*
     * The root is resolved and checked here rather than in spawn, so a typo is
     * an error the user sees instead of a program that silently cannot reach
     * anything. It must be a directory that exists: confining a process to a
     * path that is not there gives it nothing at all.
     */
    char abs_root[ESPIX_PATH_MAX];
    if (root != NULL) {
        if (!espix_cmd_path(s, root, abs_root, sizeof(abs_root))) {
            return 1;
        }

        struct stat rootst;
        if (stat(abs_root, &rootst) != 0 || !S_ISDIR(rootst.st_mode)) {
            espix_printf(s, "run: %s: not a directory\n", abs_root);
            return 1;
        }
        root = abs_root;
    }

    char abs[ESPIX_PATH_MAX];
    if (!espix_cmd_path(s, argv[first], abs, sizeof(abs))) {
        return 1;
    }

    /*
     * Same gate as the bare-name path below. Naming the file explicitly is not
     * a way around the execute bit; `run` is a convenience, not a privilege.
     *
     * A missing file falls through to espix_proc_spawn_elf(), which reports it
     * as not found -- checking the mode first would turn "no such file" into
     * "permission denied", which is a worse answer and a false one.
     */
    struct stat rst;
    if (stat(abs, &rst) == 0 && (rst.st_mode & S_IXUSR) == 0) {
        espix_printf(s, "run: %s: Permission denied\n", abs);
        return 126;
    }

    /* The app sees argv[0] as its own path, then its own arguments. */
    char *app_argv[ESPIX_ARGS_MAX];
    int   app_argc = 0;

    app_argv[app_argc++] = abs;
    for (int i = first + 1; i < argc && app_argc < ESPIX_ARGS_MAX; i++) {
        app_argv[app_argc++] = argv[i];
    }

    return run_program(s, abs, app_argc, app_argv, background, root, "run");
}

/*
 * Resolve a command line whose first word is not a builtin, the way a shell
 * falls through to PATH.
 *
 * A name containing a slash is a path, relative to the session's cwd; anything
 * else is looked for in /bin. PATH is that one fixed directory for now, since
 * espix has no environment to put a real one in.
 *
 * Two gates, in the order a shell applies them. The execute bit decides whether
 * you are allowed to run it — `chmod -x` makes a program stop working, which is
 * the whole point of having the bit — and the ELF magic then decides whether it
 * is a program at all. Keeping both is what lets `Permission denied` and
 * `Exec format error` stay different answers.
 *
 * The magic is also what sets the default mode in the first place, so a
 * freshly-copied binary is executable without anyone running chmod; see the
 * rule in espix_fs/mode.c.
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

    if ((st.st_mode & S_IXUSR) == 0) {
        espix_printf(s, "espix: %s: Permission denied\n", argv[0]);
        return 126;                     /* what a shell returns for this */
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
        return 126;
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

    return run_program(s, abs, app_argc, app_argv, background, NULL, "espix");
}

void espix_cmds_register_exec_fallback(void)
{
    espix_shell_set_exec_fallback(exec_fallback);
}

/* `kill -l`: every signal espix names, four to a row. */
static void kill_list(espix_session_t *s)
{
    int shown = 0;

    for (int sig = 1; sig < NSIG; sig++) {
        const char *name = espix_signal_name(sig);

        if (name == NULL) {
            continue;
        }
        espix_printf(s, "%2d) SIG%-9s", sig, name);

        if (++shown % 4 == 0) {
            espix_printf(s, "\n");
        }
    }
    if (shown % 4 != 0) {
        espix_printf(s, "\n");
    }
}

static int cmd_kill(espix_session_t *s, int argc, char **argv)
{
    int sig   = SIGTERM;
    int first = 1;

    if (argc < 2) {
        espix_printf(s, "usage: kill [-s] <pid>...\n");
        return 1;
    }

    /* A leading dash selects the signal: -9, -KILL, -SIGKILL all work. */
    if (argv[1][0] == '-' && argv[1][1] != '\0') {
        if (strcmp(argv[1], "-l") == 0) {
            kill_list(s);
            return 0;
        }

        sig = espix_signal_from_name(argv[1] + 1);
        if (sig < 0) {
            espix_printf(s, "kill: %s: invalid signal (try kill -l)\n",
                         argv[1] + 1);
            return 1;
        }
        first = 2;
    }

    if (first >= argc) {
        espix_printf(s, "usage: kill [-s] <pid>...\n");
        return 1;
    }

    int status = 0;

    for (int i = first; i < argc; i++) {
        char      *end = NULL;
        const long v   = strtol(argv[i], &end, 10);

        /* Previously any unparsable argument became pid 0 and was reported as
         * "no such process", which is a confusing way to say "that is not a
         * number" -- and is what every `kill -9` attempt used to produce. */
        if (end == argv[i] || *end != '\0') {
            espix_printf(s, "kill: %s: arguments must be process ids\n", argv[i]);
            status = 1;
            continue;
        }

        const espix_pid_t pid = (espix_pid_t)v;

        /*
         * SIGTERM goes through espix_proc_kill(), which asks and then insists:
         * `kill <pid>` is expected to end the process, and on a device whose
         * only console may be the one you are typing into, an app that ignores
         * SIGTERM staying alive is a worse default than the escalation.
         * Any other signal is delivered and nothing more, which is what asking
         * for a specific signal means.
         */
        const esp_err_t err = (sig == SIGTERM) ? espix_proc_kill(pid)
                                               : espix_proc_signal(pid, sig);

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
      .usage = "run [-R <dir>] <path> [args...] [&]" },
    { .name = "kill",  .fn = cmd_kill,
      .help = "send a signal to a process",
      .usage = "kill [-SIG|-l] <pid>..." },
    { .name = "crash", .fn = cmd_crash,
      .help = "fault on purpose, to test fault reporting",
      .usage = "crash" },
};

void espix_cmds_register_run(void)
{
    espix_cmds_register_table(s_run_cmds,
                             sizeof(s_run_cmds) / sizeof(s_run_cmds[0]));
}
