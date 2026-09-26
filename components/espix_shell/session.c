/*
 * espix session: dispatch and the read-eval-print loop.
 */

#include <stdio.h>
#include <errno.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_console.h"
#include "esp_log.h"

#include "espix_fs.h"
#include "espix_kernel.h"
#include "espix_shell.h"

#define TAG "shell"

espix_session_t *espix_shell_current(void)
{
    return (espix_session_t *)pvTaskGetThreadLocalStoragePointer(
        NULL, ESPIX_TLS_SESSION_IDX);
}

void espix_shell_set_current(espix_session_t *s)
{
    vTaskSetThreadLocalStoragePointer(NULL, ESPIX_TLS_SESSION_IDX, s);
}

static int session_out(espix_session_t *s, const char *data, size_t len)
{
    if (s != NULL && s->redirect != NULL) {
        return (int)fwrite(data, 1, len, s->redirect);
    }
    if (s == NULL || s->write == NULL) {
        return (int)fwrite(data, 1, len, stdout);
    }
    return s->write(s, data, len);
}

/*
 * Diagnostics. Note what this does *not* consult: s->redirect.
 *
 * That single omission is the point of the whole split. `cmd > file` used to
 * put espix's own error messages in the file, so a command that failed wrote
 * nothing to the terminal and left the reason somewhere nobody looked.
 *
 * `2>&1` is the one case where diagnostics do follow output, and it routes
 * through session_out() rather than duplicating the FILE * -- two handles on
 * one stream would be closed twice.
 */
static int session_err(espix_session_t *s, const char *data, size_t len)
{
    if (s == NULL) {
        return (int)fwrite(data, 1, len, stderr);
    }
    if (s->err_to_out) {
        return session_out(s, data, len);
    }
    if (s->redirect_err != NULL) {
        return (int)fwrite(data, 1, len, s->redirect_err);
    }
    if (s->write_err != NULL) {
        return s->write_err(s, data, len);
    }
    /* No separate error path: the console has one descriptor and both streams
     * belong on it, which is what a real terminal does too. */
    if (s->write == NULL) {
        return (int)fwrite(data, 1, len, stderr);
    }
    return s->write(s, data, len);
}

int espix_puts(espix_session_t *s, const char *str)
{
    if (str == NULL) {
        return 0;
    }
    return session_out(s, str, strlen(str));
}

/* Shared by espix_printf() and espix_eprintf(), which differ only in where the
 * formatted line goes. */
static int session_vprintf(espix_session_t *s, bool is_err,
                           const char *fmt, va_list ap)
{
    /* The session's own buffer, not this frame's -- see the field's note. A NULL
     * session, which is a command with no terminal attached, keeps the local. */
    char  local[ESPIX_LINE_MAX];
    char *buf = (s != NULL) ? s->printf_buf : local;

    const int n = vsnprintf(buf, ESPIX_LINE_MAX, fmt, ap);
    if (n < 0) {
        return n;
    }

    /* Truncation is reported as-written rather than retried on the heap: no
     * espix command legitimately emits a single line this long. */
    const size_t len = ((size_t)n < ESPIX_LINE_MAX) ? (size_t)n : ESPIX_LINE_MAX - 1;

    return is_err ? session_err(s, buf, len) : session_out(s, buf, len);
}

int espix_printf(espix_session_t *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const int n = session_vprintf(s, false, fmt, ap);
    va_end(ap);
    return n;
}

int espix_eprintf(espix_session_t *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const int n = session_vprintf(s, true, fmt, ap);
    va_end(ap);
    return n;
}

int espix_session_write(espix_session_t *s, const char *data, size_t len,
                        bool err)
{
    if (data == NULL || len == 0) {
        return 0;
    }
    return err ? session_err(s, data, len) : session_out(s, data, len);
}

/* What one command line's redirections resolved to. The names are kept so a
 * failure to close can name the file: the close is where a buffered write is
 * actually attempted, and a silent failure there loses data.
 *
 * Pointers into argv rather than copies, because this struct lives on the stack
 * of every command -- and over SSH that stack is the connection task's budget.
 * The strings outlive the command, and redirects_release() runs inside it. */
typedef struct {
    FILE *out;              /* `>` / `>>`, or NULL */
    FILE *err;              /* `2>` / `2>>`, or NULL */
    bool  err_to_out;       /* `2>&1` */
    const char *out_name;   /* the target as typed, for a failed close */
    const char *err_name;
} redirects_t;

/*
 * Strip trailing redirections from argv and open their targets.
 *
 * Handles `>`, `>>`, `2>`, `2>>` and `2>&1`, in any combination, so
 * `cmd > out 2> err` and `cmd > both 2>&1` both work. Returns the new argc, or
 * a negative value if the redirection is malformed.
 *
 * Redirections must be trailing, which is what the single-target version
 * enforced too: everything from the first operator to the end of the line has
 * to be redirection, or it is a mistake worth reporting rather than guessing
 * at.
 *
 * `2>&1` is order-insensitive here, unlike a real shell where `2>&1 > file`
 * differs from `> file 2>&1`. Ordering matters there because the shell dups
 * descriptors as it goes; espix has flags rather than a descriptor table, so
 * the distinction has nothing to attach to. The common form does the common
 * thing, and the rare one is not silently wrong -- it simply behaves as the
 * common one.
 */
static int take_redirects(espix_session_t *s, int argc, char **argv,
                          redirects_t *r)
{
    memset(r, 0, sizeof(*r));

    int first = argc;

    for (int i = 0; i < argc; i++) {
        const char *tok = argv[i];

        const bool to_out = (strcmp(tok, ">") == 0 || strcmp(tok, ">>") == 0);
        const bool to_err = (strcmp(tok, "2>") == 0 || strcmp(tok, "2>>") == 0);
        const bool dup    = (strcmp(tok, "2>&1") == 0);

        if (!to_out && !to_err && !dup) {
            if (first != argc) {
                espix_eprintf(s, "espix: %s: unexpected after a redirection\n",
                              tok);
                return -1;
            }
            continue;
        }

        if (first == argc) {
            first = i;
        }

        if (dup) {
            r->err_to_out = true;
            continue;
        }

        if (i + 1 >= argc) {
            espix_eprintf(s, "espix: %s needs a target\n", tok);
            return -1;
        }

        const bool append = (strlen(tok) > 1 && tok[strlen(tok) - 2] == '>');

        char abs[ESPIX_PATH_MAX];
        if (espix_fs_resolve(s != NULL ? s->cwd : "/", argv[i + 1],
                             abs, sizeof(abs)) != ESP_OK) {
            espix_eprintf(s, "espix: %s: path too long\n", argv[i + 1]);
            return -1;
        }

        FILE **slot = to_out ? &r->out : &r->err;
        if (*slot != NULL) {
            espix_eprintf(s, "espix: %s: redirected twice\n", tok);
            return -1;
        }

        *slot = fopen(abs, append ? "ab" : "wb");
        if (*slot == NULL) {
            espix_eprintf(s, "espix: %s: cannot open for writing: %s\n", abs,
                          strerror(errno));
            return -1;
        }
        /* Kept for redirects_release(): a failed close has to name the file. */
        if (to_out) {
            r->out_name = argv[i + 1];
        } else {
            r->err_name = argv[i + 1];
        }

        i++;                            /* the target */
    }

    return first;
}

/* Point the session at the opened targets for the duration of one command. */
static void redirects_apply(espix_session_t *s, const redirects_t *r)
{
    if (s == NULL) {
        return;
    }
    s->redirect     = r->out;
    s->redirect_err = r->err;
    s->err_to_out   = r->err_to_out;
}

/* And take them away again, closing what was opened. Always paired with
 * redirects_apply(), including on the paths that never ran a command. */
static void redirects_release(espix_session_t *s, redirects_t *r)
{
    if (s != NULL) {
        s->redirect     = NULL;
        s->redirect_err = NULL;
        s->err_to_out   = false;
    }
    if (r->out != NULL) {
        if (fclose(r->out) != 0) {
            espix_eprintf(s, "espix: %s: write failed: %s\n", r->out_name,
                          strerror(errno));
        }
        r->out = NULL;
    }
    if (r->err != NULL) {
        if (fclose(r->err) != 0) {
            espix_eprintf(s, "espix: %s: write failed: %s\n", r->err_name,
                          strerror(errno));
        }
        r->err = NULL;
    }
}

static espix_exec_fallback_fn s_exec_fallback;

void espix_shell_set_exec_fallback(espix_exec_fallback_fn fn)
{
    s_exec_fallback = fn;
}

/*
 * Expand $VAR, ${VAR} and $? into `out`.
 *
 * Into a second buffer rather than in place: split_argv() leaves argv[]
 * pointing into the caller's scratch, and an expansion is usually longer than
 * what it replaces -- $HOME is five characters and /home/esp is nine. Writing
 * back would run one token into the next.
 *
 * Not re-split afterwards, deliberately. Re-splitting is where shells get their
 * sharp edges, and espix has no quoting machinery to defend it: a value with a
 * space in it stays one argument, which is the behaviour people actually want
 * and the one they cannot get from sh without quotes.
 *
 * Returns false if the result would not fit, so the caller can refuse the line
 * rather than run a truncated one.
 */
static bool expand_token(const espix_session_t *s, const char *in,
                         char *out, size_t out_len)
{
    size_t o = 0;

    for (const char *p = in; *p != '\0'; p++) {
        /* \$ is a literal dollar; every other backslash is left alone, since
         * espix has no escape processing elsewhere and inventing some here
         * would surprise anyone typing a Windows path. */
        if (p[0] == '\\' && p[1] == '$') {
            if (o + 1 >= out_len) { return false; }
            out[o++] = '$';
            p++;
            continue;
        }
        if (p[0] != '$' || p[1] == '\0') {
            if (o + 1 >= out_len) { return false; }
            out[o++] = *p;
            continue;
        }

        /* $? -- the status of the last command, which the session has always
         * tracked and never been able to say. */
        if (p[1] == '?') {
            char num[12];
            const int n = snprintf(num, sizeof(num), "%d",
                                   (s != NULL) ? s->last_status : 0);
            if (n < 0 || o + (size_t)n >= out_len) { return false; }
            memcpy(out + o, num, (size_t)n);
            o += (size_t)n;
            p++;
            continue;
        }

        char        name[ESPIX_ENV_NAME_MAX + 1];
        size_t      nl    = 0;
        const bool  brace = (p[1] == '{');
        const char *q     = p + (brace ? 2 : 1);

        while (*q != '\0' && nl < sizeof(name) - 1 &&
               ((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z') ||
                (*q >= '0' && *q <= '9') || *q == '_')) {
            name[nl++] = *q++;
        }
        name[nl] = '\0';

        if (nl == 0 || (brace && *q != '}')) {
            /* Not a reference: `$` alone, `${}`, `${bad`. Left as typed, which
             * is what sh does and what makes `echo $` harmless. */
            if (o + 1 >= out_len) { return false; }
            out[o++] = *p;
            continue;
        }

        const char *val = espix_env_get(s, name);
        if (val != NULL) {
            const size_t vl = strlen(val);
            if (o + vl >= out_len) { return false; }
            memcpy(out + o, val, vl);
            o += vl;
        }
        /* Unset expands to nothing, as sh does. */

        p = brace ? q : q - 1;
    }

    out[o] = '\0';
    return true;
}

/*
 * Consume leading NAME=value words as assignments for this command only.
 *
 * The standard rule, and the one that makes `FOO=bar cmd` decidable: words at
 * the front that look like assignments are assignments, and the first word that
 * does not is the command. `espix_env_name_ok()` is what "looks like" means, so
 * `./a=b` and `x-y=1` are commands, not assignments.
 *
 * Returns how many were taken, or -1 if one could not be applied -- reported by
 * the caller, because a refused assignment must not run the command as though
 * it had worked.
 */
static int take_assignments(espix_session_t *s, int argc, char **argv,
                            espix_env_scope_t *scope)
{
    int taken = 0;

    while (taken < argc) {
        char *eq = strchr(argv[taken], '=');
        if (eq == NULL || eq == argv[taken]) {
            break;
        }
        *eq = '\0';
        if (!espix_env_name_ok(argv[taken])) {
            *eq = '=';
            break;
        }

        const esp_err_t err = espix_env_scope_set(s, scope, argv[taken], eq + 1);
        *eq = '=';
        if (err != ESP_OK) {
            espix_eprintf(s, "espix: %s\n",
                          (err == ESP_ERR_INVALID_SIZE)
                              ? "value too long"
                              : "too many variables");
            return -1;
        }
        taken++;
    }
    return taken;
}

/*
 * A command that declared a stack of its own runs on a task created for it.
 *
 * The context is a stack local in the spawner, which waits for the task before
 * returning -- so it outlives the task by construction, and so does the argv it
 * points at, which lives in espix_shell_exec()'s scratch buffer. Nothing else
 * touches that buffer until this returns.
 */
typedef struct {
    espix_session_t *s;
    espix_cmd_fn     fn;
    int              argc;
    char           **argv;
    uint32_t         stack;
    int              status;
    TaskHandle_t     caller;        /* notified when the command is done */
} cmd_task_ctx_t;

static void cmd_task(void *arg)
{
    cmd_task_ctx_t *c = arg;

    /*
     * The session this command belongs to. Thread-local storage does not
     * cross xTaskCreate, and everything that answers "who is asking" reads
     * it: the filesystem permission check, the ownership rule,
     * espix_shell_current() itself. Without this a command with a stack of
     * its own was taken for espix itself -- permission checks were skipped,
     * and a directory it created outside a home was left owned by root, so
     * the user who made it could not write into it. `mkdir /tmp/x` then
     * `echo hi > /tmp/x/f` was "Permission denied".
     */
    espix_shell_set_current(c->s);

    c->status = c->fn(c->s, c->argc, c->argv);

    /* The task is about to be deleted; do not leave the pointer on a task
     * that FreeRTOS may reuse. */
    espix_shell_set_current(NULL);

    /*
     * Reported before the task exits, because afterwards nothing can ask it: a
     * deleted task takes its high-water mark with it, and this figure is where
     * the size in the command table should come from. `used` is what the command
     * actually needed, which is the number worth writing down.
     */
    const uint32_t free_min = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGD(TAG, "cmd: %s: %u of %u bytes used", c->argv[0],
             (unsigned)(c->stack - free_min), (unsigned)c->stack);

    xTaskNotifyGive(c->caller);
    vTaskDelete(NULL);
}

static int run_on_own_task(espix_session_t *s, const espix_cmd_t *cmd,
                           int argc, char **argv)
{
    cmd_task_ctx_t ctx = {
        .s      = s,
        .fn     = cmd->fn,
        .argc   = argc,
        .argv   = argv,
        .stack  = cmd->stack,
        .status = 1,
        .caller = xTaskGetCurrentTaskHandle(),
    };

    /* Named for the command, so `ps` says what is running. FreeRTOS truncates
     * the name at configTASK_NAME_LEN. */
    char name[24];
    snprintf(name, sizeof(name), "cmd:%s", cmd->name);

    /* Anything already pending is somebody else's, and would let this return
     * before the command has run. */
    ulTaskNotifyTake(pdTRUE, 0);

    /*
     * The session's priority, so a command behaves the same either way. PSRAM
     * first: a command that asks for its own stack is doing something long -- a
     * big copy, an update -- not something realtime, and internal is the pool the
     * audio codec needs. Internal stays the fallback.
     */
    if (xTaskCreateWithCaps(cmd_task, name, cmd->stack, &ctx,
                            uxTaskPriorityGet(NULL), NULL,
                            MALLOC_CAP_SPIRAM) != pdPASS &&
        xTaskCreate(cmd_task, name, cmd->stack, &ctx, uxTaskPriorityGet(NULL),
                    NULL) != pdPASS) {
        espix_eprintf(s, "espix: %s: cannot start a task for it\n", cmd->name);
        return 1;
    }

    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    return ctx.status;
}

int espix_shell_exec(espix_session_t *s, const char *line)
{
    if (line == NULL) {
        return ESPIX_SHELL_EMPTY;
    }

    /* Per-session scratch copy: split_argv() writes into the buffer it is
     * given, and this is exactly what makes dispatch reentrant. */
    char  scratch[ESPIX_LINE_MAX];
    char *argv[ESPIX_ARGS_MAX];

    strlcpy(scratch, line, sizeof(scratch));

    int argc = (int)esp_console_split_argv(scratch, argv, ESPIX_ARGS_MAX);
    if (argc == 0) {
        return ESPIX_SHELL_EMPTY;
    }

    /*
     * Expanded before take_redirects() and before assignments are read, because
     * both consume argv entries: expanding afterwards would leave `> $HOME/log`
     * writing to a file literally called `$HOME/log`.
     */
    char expanded[ESPIX_LINE_MAX];
    size_t used = 0;
    for (int i = 0; i < argc; i++) {
        char *dst = expanded + used;
        if (used >= sizeof(expanded) ||
            !expand_token(s, argv[i], dst, sizeof(expanded) - used)) {
            espix_eprintf(s, "espix: line too long after expansion\n");
            return 1;
        }
        argv[i] = dst;
        used += strlen(dst) + 1;
    }

    espix_env_scope_t scope;
    espix_env_scope_init(&scope);

    const int assigned = take_assignments(s, argc, argv, &scope);
    if (assigned < 0) {
        espix_env_scope_end(s, &scope);
        return 1;
    }
    argc -= assigned;
    if (argc == 0) {
        /*
         * `FOO=bar` with no command. sh keeps it as a shell variable rather
         * than discarding it, so the scope must not undo this one: re-set it
         * unexported, which is what a bare assignment means.
         */
        for (int i = 0; i < assigned; i++) {
            char *eq = strchr(argv[i], '=');
            if (eq != NULL) {
                *eq = '\0';
                const esp_err_t err = espix_env_scope_keep(s, &scope, argv[i]);
                if (err != ESP_OK) {
                    espix_eprintf(s, "espix: %s: cannot set\n", argv[i]);
                }
                *eq = '=';
            }
        }
        espix_env_scope_end(s, &scope);
        return 0;
    }
    /* A pointer past the assignments, not `argv += assigned`: argv is an array
     * here and cannot be reseated. */
    char **av = argv + assigned;

    redirects_t redir;
    argc = take_redirects(s, argc, av, &redir);
    if (argc < 1) {
        redirects_release(s, &redir);
        espix_env_scope_end(s, &scope);
        return (argc < 0) ? 1 : ESPIX_SHELL_EMPTY;
    }
    av[argc] = NULL;

    const espix_cmd_t *cmd = espix_shell_find(av[0]);

    if (cmd == NULL && s_exec_fallback == NULL) {
        redirects_release(s, &redir);
        espix_env_scope_end(s, &scope);
        return ESPIX_SHELL_ENOENT;
    }

    redirects_apply(s, &redir);

    /* Not a builtin: let it be resolved as a program, the way a shell falls
     * through to PATH. */
    int status;

    if (cmd == NULL) {
        status = s_exec_fallback(s, argc, av);
    } else if (cmd->stack == 0) {
        /* Declared to fit on the session's own task, which is sized for the
         * protocol plus exactly the commands that declare zero. */
        status = cmd->fn(s, argc, av);
    } else {
        status = run_on_own_task(s, cmd, argc, av);
    }

    /*
     * Reported here, inside the redirection, rather than by the caller.
     *
     * It used to be printed by espix_shell_run_line() from the returned status,
     * which is after redirects_release() has already put the streams back -- so
     * `nosuchcmd 2> log` wrote the message to the terminal and left the log
     * empty, which is precisely the bug the error stream exists to fix. It also
     * means both callers get the message: the REPL and SSH's exec channel.
     */
    if (status == ESPIX_SHELL_ENOENT) {
        espix_eprintf(s, "espix: %s: command not found\n", av[0]);
    }

    redirects_release(s, &redir);

    /* After the command, so `FOO=bar cmd` leaves the session as it found it --
     * including a spawned app, which copied what it needed at spawn. */
    espix_env_scope_end(s, &scope);
    return status;
}

/*
 * `<user>:<cwd>$ `, or `#` for root, with the home directory shown as `~`.
 *
 * The user used to be the literal "espix" -- the OS name sitting where a Unix
 * prompt puts an identity, agreeing with neither `whoami` nor the greeting.
 *
 * The sigil used to be `#` for everyone, and there was a reason: espix had mode
 * bits but enforced only the execute one, because an app reached the filesystem
 * through libc and the VFS underneath could not tell which process was calling.
 * Every session really could read and write anything, so `$` would have been
 * the lie.
 *
 * That is no longer true in any part. espix owns the root VFS, resolves the
 * calling task to a process to a session to a user, and enforces read and write
 * -- an ordinary account cannot read /etc/passwd or write /etc. So the sigil
 * means what it has always meant everywhere else, and `#` is now the misleading
 * one: it says you can break things, and only root can.
 */
static void build_prompt(const espix_session_t *s, char *buf, size_t len)
{
    const char *cwd  = (s->cwd[0] != '\0') ? s->cwd : "/";
    const char *user = (s->user[0] != '\0') ? s->user : "?";
    const char  mark = (s->uid == 0) ? '#' : '$';

    /*
     * Abbreviate the home prefix, but only on a path boundary: /home/esp is ~
     * and /home/esp/x is ~/x, while /home/espionage is left alone.
     */
    if (s->home[0] != '\0') {
        const size_t hlen = strlen(s->home);

        if (strncmp(cwd, s->home, hlen) == 0) {
            if (cwd[hlen] == '\0') {
                snprintf(buf, len, "%s:~%c ", user, mark);
                return;
            }
            if (cwd[hlen] == '/') {
                snprintf(buf, len, "%s:~%s%c ", user, cwd + hlen, mark);
                return;
            }
        }
    }

    snprintf(buf, len, "%s:%s%c ", user, cwd, mark);
}

/*
 * Run one line and report it the way a shell does.
 *
 * espix_shell_exec() answers ESPIX_SHELL_ENOENT for a first word it cannot
 * resolve, and turning that into "command not found" and status 127 used to
 * live in the loop below -- which was fine while the loop was the only caller.
 * SSH's exec channel is a second one, and it hands the number straight to the
 * client as an exit status: without this, `ssh host nosuchcmd` would report
 * -127 and say nothing.
 */
int espix_shell_run_line(espix_session_t *s, const char *line)
{
    if (s == NULL || line == NULL) {
        return 0;
    }

    const int status = espix_shell_exec(s, line);

    if (status == ESPIX_SHELL_ENOENT) {
        /* The message itself is emitted by espix_shell_exec(), which still has
         * the redirections applied; all that is left here is the status. */
        s->last_status = 127;
    } else if (status != ESPIX_SHELL_EMPTY) {
        /* An empty line leaves $? alone, which is what every shell does. */
        s->last_status = status;
    }

    return s->last_status;
}

void espix_shell_session_run(espix_session_t *s)
{
    if (s == NULL || s->read_line == NULL) {
        return;
    }

    espix_shell_set_current(s);

    char line[ESPIX_LINE_MAX];
    char prompt[ESPIX_PATH_MAX + 16];

    while (!s->want_exit) {
        build_prompt(s, prompt, sizeof(prompt));

        const int n = s->read_line(s, prompt, line, sizeof(line));
        if (n < 0) {
            break;                          /* EOF or transport gone */
        }
        if (n == 0) {
            continue;
        }

        (void)espix_shell_run_line(s, line);
    }

    espix_shell_set_current(NULL);
}
