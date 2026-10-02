/*
 * Process commands: confine, kill, crash -- and the exec fallback, which is
 * where a bare command name becomes a running program.
 *
 * There was a `run` command here once, for loading an ELF off the rootfs and
 * executing it. It existed because there was no executable bit: something had
 * to say "this file is a program". There is one now, and exec_fallback() below
 * does what a shell does with a word that is not a builtin, so `hello` and
 * `/bin/hello` work without ceremony. `run` was deleted rather than kept as a
 * synonym: a second way to do the ordinary thing is a thing to explain.
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"

#include "esp_timer.h"

#include "espix_appdata.h"
#include "espix_cmds_priv.h"
#include "espix_fs.h"
#include "espix_kernel.h"
#include "espix_proc.h"
#include "espix_shell.h"

/* How often the foreground wait comes up for air to check for Ctrl-C. */
#define RUN_POLL_MS 50

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
 * How long timeout waits after SIGTERM before insisting with SIGKILL. The same
 * shape as the Ctrl-C escalation above, and for the same reason: an app
 * deserves the chance to put its hardware back, but not forever.
 */
#define RUN_KILL_GRACE_MS 2000

/*
 * Whatever the program declared it needs, before it starts.
 *
 * The data a program does not ship is fetched once, and doing it here rather
 * than inside the program is what keeps a network handle out of the app ABI:
 * what a program needs off the network it declares in /etc/apps/<name>.conf and
 * the launcher resolves it, so a program still cannot open a connection.
 *
 * A failure is reported and *not* fatal. The manifest names one way to obtain
 * the data, not the only place the program may find it -- Doom reads a WAD from
 * a USB stick just as happily -- and refusing to run would make a declaration
 * into a straitjacket.
 */
static void appdata_resolve(espix_session_t *s, const char *abs)
{
    const char *app = strrchr(abs, '/');
    app = (app != NULL) ? app + 1 : abs;

    espix_fetch_progress_t progress = { .s = s };
    espix_appdata_info_t   info     = { 0 };

    const espix_appdata_status_t st =
        espix_appdata_ensure(app, espix_cmds_fetch_progress, &progress, &info);

    switch (st) {
    case ESPIX_APPDATA_OK:
        return;

    case ESPIX_APPDATA_NO_ROOM:
        espix_eprintf(s, "%s: %s needs %u bytes and / has %u free\n", app,
                      info.path, (unsigned)info.need, (unsigned)info.free_now);
        espix_eprintf(s, "%s: free some space first; an old kernel in /boot is "
                         "the usual candidate\n", app);
        break;

    case ESPIX_APPDATA_NO_NET:
        espix_eprintf(s, "%s: cannot fetch %s\n", app, info.path);
        break;

    case ESPIX_APPDATA_BAD_HASH:
        espix_eprintf(s, "%s: %s did not match its checksum, discarded\n",
                      app, info.path);
        break;

    default:
        espix_eprintf(s, "%s: cannot write %s: %s\n", app, info.path,
                      strerror(info.err));
        if (info.err == EACCES || info.err == EPERM) {
            /* The one failure whose answer is a different command rather than
             * a different disk. A system path needs the privilege to write it,
             * exactly as installing anything does. */
            espix_eprintf(s, "%s: installing it needs the privilege to write "
                             "there -- try 'sudo %s', or the desktop icon\n",
                          app, app);
        }
        break;
    }

    espix_eprintf(s, "%s: running anyway; it may find what it needs elsewhere\n",
                  app);
}

/*
 * Spawn `abs` with the given argv and, unless backgrounded, wait for it and
 * report its status. Shared by `confine` and by the fallback that resolves a
 * bare command name to a program.
 */
/*
 * Wait for pid, the shell's foreground process, and reap it. Returns its exit
 * status, or -1 when the wait itself failed (already reported). limit_us is how
 * long to allow, 0 for no limit, which is what a plain foreground command
 * wants; timeout passes one, and *timed_out says whether it was reached. The
 * deadline is built from the clock here, not taken from the caller: comparing
 * an absolute uptime against a duration is how timeout first gave everything
 * that started after its limit an instant 124.
 *
 * In 50ms slices rather than one long block, so Ctrl-C can be noticed. Nothing
 * else reads input while a foreground process runs -- the editor is not running
 * and this task is the one that would be reading -- so without this a program
 * that ignores its own exit conditions cannot be stopped from the session that
 * started it, and the presses surface as blank lines once it finally dies.
 */
static int wait_foreground(espix_session_t *s, espix_pid_t pid, const char *who,
                           int64_t limit_us, bool *timed_out)
{
    int       exit_code  = -1;
    esp_err_t wait_err   = ESP_ERR_TIMEOUT;
    unsigned  interrupts = 0;
    bool      overran    = false;
    bool      killed     = false;
    int64_t   term_at    = 0;

    if (timed_out != NULL) {
        *timed_out = false;
    }
    const int64_t deadline = (limit_us > 0) ? esp_timer_get_time() + limit_us : 0;

    s->fg_pid = pid;

    for (;;) {
        const int64_t now = esp_timer_get_time();

        if (deadline > 0 && !overran && now >= deadline) {
            espix_eprintf(s, "%s: pid %d: time limit reached\n", who, (int)pid);
            (void)espix_proc_signal(pid, SIGTERM);
            overran = true;
            term_at = now;
        } else if (overran && !killed &&
                   now >= term_at + (int64_t)RUN_KILL_GRACE_MS * 1000) {
            espix_eprintf(s, "%s: pid %d: did not stop; killing it\n",
                          who, (int)pid);
            (void)espix_proc_signal(pid, SIGKILL);
            killed = true;
        }

        wait_err = espix_proc_wait(pid, &exit_code, pdMS_TO_TICKS(RUN_POLL_MS));
        if (wait_err != ESP_ERR_TIMEOUT) {
            break;
        }

        if (s->poll_interrupt == NULL || !s->poll_interrupt(s)) {
            continue;
        }

        espix_printf(s, "^C\n");
        interrupts++;

        if (interrupts < RUN_INTERRUPTS_TO_KILL) {
            (void)espix_proc_signal(pid, SIGINT);

            if (interrupts + 1 == RUN_INTERRUPTS_TO_KILL) {
                espix_eprintf(s, "%s: pid %d is ignoring SIGINT; "
                                 "press Ctrl-C again to force it\n",
                              who, (int)pid);
            }
        } else if (interrupts == RUN_INTERRUPTS_TO_KILL) {
            espix_eprintf(s, "%s: killing pid %d\n", who, (int)pid);
            (void)espix_proc_signal(pid, SIGKILL);
        }
    }

    if (s->poll_interrupt != NULL) {
        (void)s->poll_interrupt(s);
    }

    s->fg_pid = ESPIX_PID_NONE;

    if (timed_out != NULL) {
        *timed_out = overran;
    }
    if (wait_err != ESP_OK) {
        espix_eprintf(s, "%s: pid %d: %s\n", who, (int)pid,
                      esp_err_to_name(wait_err));
        return -1;
    }
    return exit_code;
}

static int run_program(espix_session_t *s, const char *abs, int argc,
                       char **argv, bool background, const char *root,
                       const char *who)
{
    espix_pid_t     pid = ESPIX_PID_NONE;

    appdata_resolve(s, abs);

    const esp_err_t err = espix_proc_spawn_elf(abs, argc, argv, s, root,
                                               !background, &pid);

    if (err != ESP_OK) {
        espix_eprintf(s, "%s: %s: %s\n", who, abs, esp_err_to_name(err));
        return 1;
    }

    if (background) {
        espix_printf(s, "[%d] %s\n", (int)pid, abs);

        const char *base = strrchr(abs, '/');
        base = (base != NULL) ? base + 1 : abs;
        if (!espix_shell_job_add(s, pid, base)) {
            /* It is running either way; only the session's memory of it is
             * lost, which is worth a line rather than a `jobs` that is quietly
             * short. */
            espix_eprintf(s, "espix: %d: no job slot; it runs untracked\n",
                          (int)pid);
        }
        return 0;
    }

    const int status = wait_foreground(s, pid, who, 0, NULL);
    if (status < 0) {
        return 1;
    }
    if (status != 0) {
        espix_printf(s, "[exit %d]\n", status);
    }
    return status;
}

/*
 * The two gates a shell applies before executing, in the order Linux applies
 * them. Returns 0 to proceed, -1 if there is no such file (the caller decides
 * what that means), or the status the caller should return.
 *
 * The execute bit decides whether you may run it -- `chmod -x` makes a program
 * stop working, which is the whole point of having the bit -- and the format
 * check then decides whether it is a program at all. Keeping both is what lets
 * `Permission denied` and `Exec format error` stay different answers.
 *
 * **Do not drop the magic check on the grounds that the executable bit now
 * exists.** Linux does exactly this too: execve() checks the bit, then hands
 * the file to its binfmt handlers -- binfmt_elf matching \177ELF, binfmt_script
 * matching #! -- and when none matches the call fails with ENOEXEC, which a
 * shell reports as "Exec format error". EACCES before ENOEXEC, both 126.
 *
 * Two reasons of espix's own. Without it a +x file that is not an ELF reaches
 * the loader, which has already claimed a process slot and started a task
 * before it can fail, and says `ELF init failed` instead. And this is the seam
 * where `#!` support hooks in: a file that is executable but not an ELF is
 * exactly where the interpreter line would be read.
 *
 * The magic is also what sets the default mode in the first place, so a
 * freshly-copied binary is executable without anyone running chmod; see the
 * rule in espix_fs/mode.c.
 */
static int program_gate(espix_session_t *s, const char *abs, const char *shown)
{
    struct stat st;
    if (stat(abs, &st) != 0 || !S_ISREG(st.st_mode)) {
        return -1;
    }

    if ((st.st_mode & S_IXUSR) == 0) {
        espix_eprintf(s, "espix: %s: Permission denied\n", shown);
        return 126;                     /* what a shell returns for this */
    }

    FILE *f = fopen(abs, "rb");
    if (f == NULL) {
        return -1;
    }
    char         magic[4] = { 0 };
    const size_t got = fread(magic, 1, sizeof(magic), f);
    fclose(f);

    if (got != sizeof(magic) || memcmp(magic, "\177ELF", sizeof(magic)) != 0) {
        espix_eprintf(s, "espix: %s: Exec format error\n", shown);
        return 126;
    }
    return 0;
}

/*
 * `confine <dir> <path> [args...] [&]`
 *
 * Give the program a root: it can name nothing outside <dir>. The binary itself
 * is read before the process exists, so it is normal for it to live outside --
 * `confine /srv/www /bin/httpd` is the shape this is for, alongside
 * `sudo -u www` giving the same program its own identity.
 *
 * A wrapper that adjusts one aspect of the execution context and then execs,
 * in the family of env, nice, nohup, setsid, setpriv and timeout. Those compose
 * by nesting and so does this: `sudo -u www confine /srv/www /bin/httpd` reads
 * as the two restrictions it is, and needs no special case, because sudo
 * re-dispatches a command line rather than spawning.
 *
 * **Not called chroot**, and the difference is not pedantry. chroot changes what
 * `/` means, so a chrooted program opens /index.html and the kernel gives it
 * /srv/www/index.html. espix resolves the path normally and *then* refuses
 * anything outside the root, so paths stay globally absolute and the program
 * must still say /srv/www/index.html. That is OpenBSD's unveil(2) -- a
 * visibility filter over ordinary resolution -- and a chroot invocation ported
 * here under that name would silently open the wrong paths.
 */
static int cmd_confine(espix_session_t *s, int argc, char **argv)
{
    static const char *const USAGE =
        "usage: confine <dir> <path> [args...] [&]\n";

    /* The shell stripped the trailing `&` and recorded the intent; see
     * espix_cmd_t.backgrounds. */
    const bool background = s->background;

    if (argc < 3) {
        espix_printf(s, "%s", USAGE);
        return 1;
    }

    /*
     * The root is resolved and checked here rather than in spawn, so a typo is
     * an error the user sees instead of a program that silently cannot reach
     * anything. It must be a directory that exists: confining a process to a
     * path that is not there gives it nothing at all.
     */
    char abs_root[ESPIX_PATH_MAX];
    if (!espix_cmd_path(s, argv[1], abs_root, sizeof(abs_root))) {
        return 1;
    }

    struct stat rootst;
    if (stat(abs_root, &rootst) != 0 || !S_ISDIR(rootst.st_mode)) {
        espix_eprintf(s, "confine: %s: not a directory\n", abs_root);
        return 1;
    }

    char abs[ESPIX_PATH_MAX];
    if (!espix_cmd_path(s, argv[2], abs, sizeof(abs))) {
        return 1;
    }

    /*
     * The same gates a bare command name gets: naming the file explicitly is
     * not a way around the execute bit, and confinement is not a privilege.
     *
     * A missing file is left to espix_proc_spawn_elf(), which reports it as not
     * found -- answering "permission denied" for a file that is not there would
     * be a worse answer and a false one.
     */
    const int gate = program_gate(s, abs, abs);
    if (gate > 0) {
        return gate;
    }

    /* The app sees argv[0] as its own path, then its own arguments. */
    char *app_argv[ESPIX_ARGS_MAX];
    int   app_argc = 0;

    app_argv[app_argc++] = abs;
    for (int i = 3; i < argc && app_argc < ESPIX_ARGS_MAX; i++) {
        app_argv[app_argc++] = argv[i];
    }

    return run_program(s, abs, app_argc, app_argv, background, abs_root,
                       "confine");
}

/*
 * Resolve a command line whose first word is not a builtin, the way a shell
 * falls through to PATH.
 *
 * A name containing a slash is a path, relative to the session's cwd; anything
 * else is looked for in each directory of PATH in turn, defaulting to /bin when
 * the session has not set one -- which is where this looked unconditionally
 * before there was an environment to hold the alternative.
 *
 * An empty PATH means "nowhere", as sh does, rather than silently meaning /bin:
 * `PATH= hello` should fail, and a user who has emptied it has said something.
 *
 * The two gates it then applies are program_gate() above, shared with
 * `confine` so that naming a file explicitly cannot get past what a bare name
 * has to satisfy.
 */
/*
 * Resolve a command word to an absolute path, the way a shell falls through to
 * PATH, and apply the two gates. Returns 0 when the program may run, -1 when
 * there is no such file (the caller decides what that means), or the status to
 * return for a file that is there but cannot run (already reported).
 *
 * Shared by the exec fallback and timeout, so both answer the same way about
 * what a command word names.
 */
static int resolve_program(espix_session_t *s, const char *word,
                           char *abs, size_t len)
{
    if (strchr(word, '/') != NULL) {
        if (!espix_cmd_path(s, word, abs, len)) {
            return -1;
        }
    } else {
        const char *path = espix_env_get(s, "PATH");
        if (path == NULL) {
            path = "/bin";
        }

        /*
         * First hit wins, and "hit" means the file exists -- not that it is
         * runnable. A directory earlier in PATH holding an unreadable program
         * shadows a later one, which is what every shell does and what makes
         * the resulting "Permission denied" the truth rather than a puzzle.
         */
        bool found = false;
        for (const char *p = path; *p != '\0' && !found; ) {
            const char  *sep = strchr(p, ':');
            const size_t seg = (sep != NULL) ? (size_t)(sep - p) : strlen(p);

            if (seg > 0) {
                char dir[ESPIX_PATH_MAX];
                if (seg < sizeof(dir)) {
                    memcpy(dir, p, seg);
                    dir[seg] = '\0';

                    if ((size_t)snprintf(abs, len, "%s/%s", dir, word) < len) {
                        struct stat st;
                        if (stat(abs, &st) == 0) {
                            found = true;
                        }
                    }
                }
            }
            p = (sep != NULL) ? sep + 1 : p + seg;
        }

        if (!found) {
            return -1;
        }
    }

    /* Shown as the user typed it, not as resolved: "hello: Permission denied"
     * is the answer they can act on. */
    const int gate = program_gate(s, abs, word);
    if (gate != 0) {
        return (gate < 0) ? -1 : gate;
    }
    return 0;
}

static int exec_fallback(espix_session_t *s, int argc, char **argv)
{
    char abs[ESPIX_PATH_MAX];

    const int resolved = resolve_program(s, argv[0], abs, sizeof(abs));
    if (resolved < 0) {
        return ESPIX_SHELL_ENOENT;
    }
    if (resolved > 0) {
        return resolved;
    }

    /* The shell stripped the trailing & and recorded the intent. */
    const bool background = s->background;

    /* argv[0] becomes the resolved path, as execve() would leave it. */
    char *app_argv[ESPIX_ARGS_MAX];
    int   app_argc = 0;

    app_argv[app_argc++] = abs;
    for (int i = 1; i < argc && app_argc < ESPIX_ARGS_MAX; i++) {
        app_argv[app_argc++] = argv[i];
    }

    return run_program(s, abs, app_argc, app_argv, background, NULL, "espix");
}


/*
 * timeout <seconds> <program> [args...]
 *
 * Run a program with a time limit: SIGTERM at the limit, SIGKILL a grace later,
 * and 124 as the status, which is what coreutils uses. It is a shell command
 * rather than something an app asks for -- there is no fork, so nothing else
 * could supervise a child -- and it reuses the wait a foreground command
 * already had. Zero seconds means no limit, as coreutils defines it, so a
 * caller can pass a computed timeout without special-casing it.
 */
static int cmd_timeout(espix_session_t *s, int argc, char **argv)
{
    if (argc < 3) {
        espix_eprintf(s, "usage: timeout <seconds> <program> [args...]\n");
        return 125;
    }

    char *end  = NULL;
    long  secs = strtol(argv[1], &end, 10);
    if (end == argv[1] || *end != '\0' || secs < 0) {
        espix_eprintf(s, "timeout: bad seconds '%s'\n", argv[1]);
        return 125;
    }
    if (secs > 86400) {
        secs = 86400;
    }

    char abs[ESPIX_PATH_MAX];
    const int resolved = resolve_program(s, argv[2], abs, sizeof(abs));
    if (resolved < 0) {
        espix_eprintf(s, "timeout: %s: command not found\n", argv[2]);
        return 127;
    }
    if (resolved > 0) {
        return resolved;
    }

    char *app_argv[ESPIX_ARGS_MAX];
    int   app_argc = 0;

    app_argv[app_argc++] = abs;
    for (int i = 3; i < argc && app_argc < ESPIX_ARGS_MAX; i++) {
        app_argv[app_argc++] = argv[i];
    }

    appdata_resolve(s, abs);

    espix_pid_t     pid = ESPIX_PID_NONE;
    const esp_err_t err = espix_proc_spawn_elf(abs, app_argc, app_argv, s, NULL,
                                               true, &pid);
    if (err != ESP_OK) {
        espix_eprintf(s, "timeout: %s: %s\n", abs, esp_err_to_name(err));
        return 126;
    }

    bool      timed_out = false;
    const int status = wait_foreground(s, pid, "timeout",
                                       (int64_t)secs * 1000000, &timed_out);
    if (timed_out) {
        return 124;
    }
    if (status < 0) {
        return 125;
    }
    if (status != 0) {
        espix_printf(s, "[exit %d]\n", status);
    }
    return status;
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
        espix_eprintf(s, "usage: kill [-s] <pid>...\n");
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
            espix_eprintf(s, "kill: %s: invalid signal (try kill -l)\n",
                         argv[1] + 1);
            return 1;
        }
        first = 2;
    }

    if (first >= argc) {
        espix_eprintf(s, "usage: kill [-s] <pid>...\n");
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
            espix_eprintf(s, "kill: %s: arguments must be process ids\n", argv[i]);
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
            espix_eprintf(s, "kill: %d: no such process\n", (int)pid);
            status = 1;
        } else if (err == ESP_ERR_INVALID_STATE) {
            espix_eprintf(s, "kill: %d: already finished\n", (int)pid);
            status = 1;
        } else if (err != ESP_OK) {
            espix_eprintf(s, "kill: %d: %s\n", (int)pid, esp_err_to_name(err));
            status = 1;
        }
    }

    return status;
}

static int cmd_crash(espix_session_t *s, int argc, char **argv)
{
#if CONFIG_ESPIX_PROC_ABI_WATCHPOINT
    /* `crash abi` stores to the watched ABI table state instead, so the
     * watchpoint can be seen firing rather than assumed to work. */
    if (argc == 2 && strcmp(argv[1], "abi") == 0) {
        espix_printf(s, "storing to the watched ABI state — expect a watchpoint\n");
        espix_klog(ESPIX_KLOG_WARN, "crash", "deliberate watchpoint test");
        espix_proc_abi_watch_selftest();
        espix_printf(s, "no watchpoint fired — it is not armed\n");
        return 1;
    }
#endif
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

/* ------------------------------------------------------------------ */
/* jobs, fg, bg                                                        */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* jobs, fg, bg                                                        */
/* ------------------------------------------------------------------ */

/*
 * The session's job table holds two kinds of job (see espix_job_t): a
 * *process*, which is a loaded app and can be signalled, and a *builtin task*,
 * which cannot -- espix can delete a task but not stop and resume one. jobs
 * lists both; fg and bg only mean anything for a process.
 *
 * Ctrl-Z is still missing, because the transports report only that a key
 * arrived and not which one -- see R-P7.7.
 */

static bool job_live(const espix_job_t *j)
{
    return j->pid > 0 || (j->task != NULL && !j->finished);
}

/* Forget jobs that have ended. A process is live until espix_proc_state_of()
 * says FREE -- nobody reaps a backgrounded app's status, so without this a
 * finished job would be listed for ever. A builtin task sets finished as it
 * exits; the shell only has to forget it, since the task owns its own
 * teardown. */
static void job_prune(espix_session_t *s)
{
    for (int i = 0; i < ESPIX_SESSION_JOBS; i++) {
        espix_job_t *j = &s->jobs[i];

        if (j->pid > 0 && espix_proc_state_of(j->pid) == ESPIX_PROC_FREE) {
            espix_shell_job_clear(s, i);
        } else if (j->task != NULL && j->finished) {
            espix_shell_job_clear(s, i);
        }
    }
}

/* The job arg names -- "%2" by number, or a pid -- or the most recent one for
 * NULL. NULL when there is no such job. */
static espix_job_t *job_pick(espix_session_t *s, const char *arg)
{
    job_prune(s);

    if (arg == NULL || arg[0] == '\0') {
        for (int i = ESPIX_SESSION_JOBS - 1; i >= 0; i--) {
            if (job_live(&s->jobs[i])) {
                return &s->jobs[i];
            }
        }
        return NULL;
    }

    char *end = NULL;
    if (arg[0] == '%') {
        const long n = strtol(arg + 1, &end, 10);
        if (end == arg + 1 || *end != '\0' || n <= 0) {
            return NULL;
        }
        long seen = 0;
        for (int i = 0; i < ESPIX_SESSION_JOBS; i++) {
            if (job_live(&s->jobs[i]) && ++seen == n) {
                return &s->jobs[i];
            }
        }
        return NULL;
    }

    const long pid = strtol(arg, &end, 10);
    if (end == arg || *end != '\0' || pid <= 0) {
        return NULL;
    }
    for (int i = 0; i < ESPIX_SESSION_JOBS; i++) {
        if (s->jobs[i].pid == (espix_pid_t)pid) {
            return &s->jobs[i];
        }
    }
    return NULL;
}

/* fg and bg need a process to signal. Returns the job, or NULL with the message
 * already printed. */
static espix_job_t *job_pick_process(espix_session_t *s, const char *who,
                                     const char *arg)
{
    espix_job_t *j = job_pick(s, arg);
    if (j == NULL) {
        espix_eprintf(s, "%s: no such job\n", who);
        return NULL;
    }
    if (j->pid <= 0) {
        espix_eprintf(s, "%s: %s is a builtin; it cannot be stopped or resumed\n",
                      who, j->name);
        return NULL;
    }
    return j;
}

static int cmd_jobs(espix_session_t *s, int argc, char **argv)
{
    (void)argc; (void)argv;

    job_prune(s);

    int n = 0;
    for (int i = 0; i < ESPIX_SESSION_JOBS; i++) {
        espix_job_t *j = &s->jobs[i];
        if (!job_live(j)) {
            continue;
        }
        if (j->pid > 0) {
            espix_printf(s, "[%d] %5d %-4s %s\n", ++n, (int)j->pid,
                         espix_proc_state_str(espix_proc_state_of(j->pid)),
                         j->name);
        } else {
            /* A builtin task has no pid and no proc state; it is running
             * whenever it is listed at all. */
            espix_printf(s, "[%d]     - Run  %s\n", ++n, j->name);
        }
    }
    if (n == 0) {
        espix_printf(s, "no jobs\n");
    }
    return 0;
}

static int cmd_bg(espix_session_t *s, int argc, char **argv)
{
    espix_job_t *j = job_pick_process(s, "bg", (argc > 1) ? argv[1] : NULL);
    if (j == NULL) {
        return 1;
    }

    const esp_err_t err = espix_proc_signal(j->pid, SIGCONT);
    if (err != ESP_OK) {
        espix_eprintf(s, "bg: %d: %s\n", (int)j->pid, esp_err_to_name(err));
        espix_shell_job_clear(s, (int)(j - s->jobs));
        return 1;
    }
    espix_printf(s, "[%d] resumed\n", (int)j->pid);
    return 0;
}

static int cmd_fg(espix_session_t *s, int argc, char **argv)
{
    espix_job_t *j = job_pick_process(s, "fg", (argc > 1) ? argv[1] : NULL);
    if (j == NULL) {
        return 1;
    }

    const espix_pid_t pid = j->pid;
    const int         slot = (int)(j - s->jobs);

    (void)espix_proc_signal(pid, SIGCONT);

    int             code = 0;
    const esp_err_t err  = espix_proc_wait(pid, &code, portMAX_DELAY);
    espix_shell_job_clear(s, slot);

    if (err != ESP_OK) {
        espix_eprintf(s, "fg: %d: %s\n", (int)pid, esp_err_to_name(err));
        return 1;
    }
    if (code != 0) {
        espix_printf(s, "[exit %d]\n", code);
    }
    return code;
}

static espix_cmd_t s_run_cmds[] = {
    { .name = "timeout", .fn = cmd_timeout,
      .help = "run a program, ending it if it overruns",
      .usage = "timeout <seconds> <program> [args...]" },
    { .name = "confine", .fn = cmd_confine, .backgrounds = true,
      .help = "run a program that can name nothing outside <dir>",
      .usage = "confine <dir> <path> [args...] [&]" },
    { .name = "kill",  .fn = cmd_kill,
      .help = "send a signal to a process",
      .usage = "kill [-SIG|-l] <pid>..." },
    { .name = "crash", .fn = cmd_crash,
      .help = "fault on purpose, to test fault reporting",
      .usage = "crash [abi]" },
    { .name = "jobs", .fn = cmd_jobs,
      .help = "list this session's background jobs",
      .usage = "jobs" },
    { .name = "fg", .fn = cmd_fg,
      .help = "resume a background job in the foreground",
      .usage = "fg [%n|pid]" },
    { .name = "bg", .fn = cmd_bg,
      .help = "resume a stopped background job",
      .usage = "bg [%n|pid]" },
};

void espix_cmds_register_run(void)
{
    espix_cmds_register_table(s_run_cmds,
                             sizeof(s_run_cmds) / sizeof(s_run_cmds[0]));
}
