/*
 * espix session: dispatch and the read-eval-print loop.
 */

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"

#include "esp_console.h"

#include "espix_fs.h"
#include "espix_kernel.h"
#include "espix_shell.h"
#include "espix_task.h"

#define TAG "shell"

static const espix_shell_proc_ops_t *s_proc;

espix_session_t *espix_shell_current(void)
{
    return (espix_session_t *)pvTaskGetThreadLocalStoragePointer(
        NULL, ESPIX_TLS_SESSION_IDX);
}

void espix_shell_set_current(espix_session_t *s)
{
    vTaskSetThreadLocalStoragePointer(NULL, ESPIX_TLS_SESSION_IDX, s);
}

int espix_shell_term_size(int *cols, int *rows)
{
    espix_session_t *s = espix_shell_current();

    if (s == NULL || s->term_size == NULL || cols == NULL || rows == NULL) {
        return -1;
    }
    s->term_size(s, cols, rows);
    return 0;
}

static bool proc_stopping(void);

static int session_out(espix_session_t *s, const char *data, size_t len)
{
    if (s != NULL && s->redirect != NULL) {
        return (int)fwrite(data, 1, len, s->redirect);
    }
    if (s == NULL || s->write == NULL) {
        return (int)fwrite(data, 1, len, stdout);
    }
    /* A process asked to stop may outlive its session; it does not get to
     * write into a transport that may be gone. */
    if (proc_stopping()) {
        return -1;
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
    if (proc_stopping()) {
        return -1;
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
    FILE *in;               /* `<`, or NULL */
    bool  err_to_out;       /* `2>&1` */
    const char *out_name;   /* the target as typed, for a failed close */
    const char *err_name;
    const char *in_name;
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
/*
 * Split an attached redirection -- 2>/dev/null, >out, >>out, <in -- into the
 * two words the separated form already uses.
 *
 * POSIX writes the descriptor against the operator with no space, and that is
 * the form people type; the separated one (2> /dev/null) was all that was
 * recognised, so the attached word became an argument and the command was told
 * about a file called "2>/dev/null".
 *
 * The operator is replaced by one of the canonical literals take_redirects()
 * matches, and the target is inserted after it, so no byte of the caller's
 * buffer has to be overwritten to terminate the operator. Returns 1 when it
 * split one, 0 when the word is not an attached redirection, and -1 on a
 * descriptor espix does not redirect.
 */
static int take_attached(espix_session_t *s, char **argv, int *argc, int i)
{
    char *p = argv[i];
    char *o = p;
    int   fd = -1;

    if (*o >= '0' && *o <= '9') {
        fd = *o - '0';
        o++;
    }

    const char *op;
    char       *target;

    if (o[0] == '<') {
        if (fd > 0) {
            goto bad_fd;
        }
        op     = "<";
        target = o + 1;
    } else if (o[0] == '>' && o[1] == '>') {
        if (fd > 2 || fd == 0) {
            goto bad_fd;
        }
        op     = (fd == 2) ? "2>>" : ">>";
        target = o + 2;
    } else if (o[0] == '>') {
        if (o[1] == '&') {
            return 0;                   /* 2>&1 is a word of its own */
        }
        if (fd > 2 || fd == 0) {
            goto bad_fd;
        }
        op     = (fd == 2) ? "2>" : ">";
        target = o + 1;
    } else {
        return 0;
    }

    if (*target == '\0') {
        return 0;                       /* ">" alone: the separated form */
    }
    if (*argc + 1 >= ESPIX_ARGS_MAX) {
        espix_eprintf(s, "espix: too many words for a redirection\n");
        return -1;
    }

    for (int k = *argc; k > i + 1; k--) {
        argv[k] = argv[k - 1];
    }
    argv[i]     = (char *)op;
    argv[i + 1] = target;
    (*argc)++;
    return 1;

bad_fd:
    espix_eprintf(s, "espix: only descriptors 0, 1 and 2 can be redirected\n");
    return -1;
}

static int take_redirects(espix_session_t *s, int argc, char **argv,
                          redirects_t *r)
{
    memset(r, 0, sizeof(*r));

    int  first = argc;
    bool seen  = false;

    for (int i = 0; i < argc; i++) {
        if (take_attached(s, argv, &argc, i) < 0) {
            return -1;
        }

        const char *tok = argv[i];

        const bool to_out = (strcmp(tok, ">") == 0 || strcmp(tok, ">>") == 0);
        const bool to_err = (strcmp(tok, "2>") == 0 || strcmp(tok, "2>>") == 0);
        const bool to_in  = (strcmp(tok, "<") == 0);
        const bool dup    = (strcmp(tok, "2>&1") == 0);

        if (!to_out && !to_err && !to_in && !dup) {
            if (seen) {
                /* The shell strips a trailing & after this, but a redirection
                 * is a FILE that this call would close when the line returns,
                 * so it cannot be lent to a job that outlives the line. Say so
                 * rather than treating the & as a stray word. R-P2.8 keeps
                 * redirection with & for later. */
                if (strcmp(tok, "&") == 0 && i + 1 == argc) {
                    espix_eprintf(s, "espix: redirection with & is not supported "
                                     "yet\n");
                    return -1;
                }
                espix_eprintf(s, "espix: %s: unexpected after a redirection\n",
                              tok);
                return -1;
            }
            continue;
        }

        if (!seen) {
            first = i;
            seen  = true;
        }

        if (dup) {
            r->err_to_out = true;
            continue;
        }

        if (i + 1 >= argc) {
            espix_eprintf(s, "espix: %s needs a target\n", tok);
            return -1;
        }

        char abs[ESPIX_PATH_MAX];
        if (espix_fs_resolve(s != NULL ? s->cwd : "/", argv[i + 1],
                             abs, sizeof(abs)) != ESP_OK) {
            espix_eprintf(s, "espix: %s: path too long\n", argv[i + 1]);
            return -1;
        }

        if (to_in) {
            if (r->in != NULL) {
                espix_eprintf(s, "espix: %s: input redirected twice\n", tok);
                return -1;
            }
            r->in = fopen(abs, "rb");
            if (r->in == NULL) {
                espix_eprintf(s, "espix: %s: cannot open for reading: %s\n", abs,
                              strerror(errno));
                return -1;
            }
            r->in_name = argv[i + 1];
            i++;                        /* the target */
            continue;
        }

        const bool append = (strlen(tok) > 1 && tok[strlen(tok) - 2] == '>');

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
    /*
     * Only what the line redirected. A pipeline stage inherits the pipe ends
     * its caller put on the session copy, and a command that wrote no `>` of
     * its own must not clear them -- see run_pipeline().
     */
    if (r->out != NULL) { s->redirect     = r->out; }
    if (r->err != NULL) { s->redirect_err = r->err; }
    if (r->in  != NULL) { s->redirect_in  = r->in;  }
    s->err_to_out = r->err_to_out;
}

/* And take them away again, closing what was opened. Always paired with
 * redirects_apply(), including on the paths that never ran a command. */
static void redirects_release(espix_session_t *s, redirects_t *r)
{
    if (s != NULL) {
        /* Only what this line set: a stage's inherited ends are its caller's
         * to clear. */
        if (r->out != NULL) { s->redirect     = NULL; }
        if (r->err != NULL) { s->redirect_err = NULL; }
        if (r->in  != NULL) { s->redirect_in  = NULL; }
        s->err_to_out = false;
    }
    if (r->in != NULL) {
        fclose(r->in);
        r->in = NULL;
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

        /* $? -- the status of the last command; $$ -- the shell's pid. */
        if (p[1] == '?' || p[1] == '$') {
            int v = 0;
            if (s != NULL) {
                v = (p[1] == '?') ? s->last_status
                                  : (s->pid > 0 ? (int)s->pid : 0);
            }
            char num[12];
            const int n = snprintf(num, sizeof(num), "%d", v);
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
 * A command as a process of its own, so ^C, kill and ps reach it. In the
 * foreground it runs against the session itself while the shell only waits, so
 * a redirect, `sudo cd` and the creds sudo raises all apply; a job gets a copy,
 * because the shell carries on.
 */
typedef struct {
    espix_session_t *s;
    espix_cmd_fn     fn;
    int              argc;
    char           **argv;
    bool             owned;     /* a job's, in one allocation; freed at the end */
} cmd_proc_t;

static int cmd_proc(void *arg)
{
    cmd_proc_t *const c = arg;

    espix_shell_set_current(c->s);
    const int status = c->fn(c->s, c->argc, c->argv);
    espix_shell_set_current(NULL);

    if (c->owned) {
        free(c);
    }
    return status;
}

/* Under the real session, whatever c->s is, so a hangup reaches it. */
static esp_err_t cmd_spawn(espix_session_t *s, const espix_cmd_t *cmd,
                           cmd_proc_t *c, espix_pid_t *pid)
{
    espix_shell_spawn_t sp = {
        .name           = cmd->name,
        .fn             = cmd_proc,
        .arg            = c,
        .stack          = (cmd->stack != 0) ? cmd->stack : ESPIX_CMD_STACK,
        .internal_stack = cmd->internal_stack,
    };
    return s_proc->spawn(s, &sp, pid);
}

static int run_as_process(espix_session_t *s, const espix_cmd_t *cmd,
                          int argc, char **argv)
{
    cmd_proc_t  c   = { .s = s, .fn = cmd->fn, .argc = argc, .argv = argv };
    espix_pid_t pid = ESPIX_PID_NONE;

    const esp_err_t err = cmd_spawn(s, cmd, &c, &pid);
    if (err != ESP_OK) {
        espix_eprintf(s, "espix: %s: cannot start: %s\n", cmd->name,
                      esp_err_to_name(err));
        return 1;
    }
    /* Returns only once the process has ended, so `c` outlives it. */
    const int status = s_proc->wait(s, pid, cmd->tty);
    return (status < 0) ? 1 : status;
}

void espix_shell_jobs_prune(espix_session_t *s)
{
    if (s == NULL) {
        return;
    }
    for (int i = 0; i < ESPIX_SESSION_JOBS; i++) {
        const espix_pid_t pid = s->jobs[i].pid;
        if (pid > 0 && (s_proc == NULL || !s_proc->alive(pid))) {
            espix_shell_job_clear(s, i);
        }
    }
}

bool espix_shell_job_add(espix_session_t *s, espix_pid_t pid, const char *name)
{
    if (s == NULL || pid <= 0) {
        return false;
    }
    espix_shell_jobs_prune(s);

    for (int i = 0; i < ESPIX_SESSION_JOBS; i++) {
        espix_job_t *j = &s->jobs[i];
        if (j->pid <= 0) {
            j->pid = pid;
            strlcpy(j->name, (name != NULL) ? name : "?", ESPIX_JOB_NAME_MAX);
            return true;
        }
    }
    return false;
}

void espix_shell_job_clear(espix_session_t *s, int slot)
{
    if (s == NULL || slot < 0 || slot >= ESPIX_SESSION_JOBS) {
        return;
    }
    memset(&s->jobs[slot], 0, sizeof(s->jobs[slot]));
}

bool espix_shell_stopping(const espix_session_t *s)
{
    (void)s;
    return espix_shell_sigcheck();
}

/*
 * A background builtin: a process with a heap copy of the session -- the shell
 * runs the next command meanwhile, so the printf buffer, the redirects and the
 * cwd have to belong to one of the two -- and of its argv, which points into
 * the shell scratch. Returns the status exec_one() reports.
 */
static int start_background_job(espix_session_t *s, const espix_cmd_t *cmd,
                                int argc, char **argv)
{
    size_t bytes = 0;
    for (int i = 0; i < argc; i++) {
        bytes += strlen(argv[i]) + 1;
    }

    /* cmd_proc_t, the session copy, the argv pointers, the strings. */
    cmd_proc_t *c = malloc(sizeof(*c) + sizeof(espix_session_t) +
                           ((size_t)argc + 1) * sizeof(char *) + bytes);
    if (c == NULL) {
        espix_eprintf(s, "espix: %s: no memory for a job\n", argv[0]);
        return 1;
    }
    espix_session_t *const copy = (espix_session_t *)(c + 1);
    char **const           av   = (char **)(copy + 1);
    char                  *w    = (char *)(av + argc + 1);

    *copy = *s;
    copy->background = false;           /* a job now, not a & line */
    for (int i = 0; i < argc; i++) {
        const size_t n = strlen(argv[i]) + 1;
        memcpy(w, argv[i], n);
        av[i] = w;
        w += n;
    }
    av[argc] = NULL;

    *c = (cmd_proc_t){ .s = copy, .fn = cmd->fn, .argc = argc, .argv = av,
                       .owned = true };

    espix_pid_t     pid = ESPIX_PID_NONE;
    const esp_err_t err = cmd_spawn(s, cmd, c, &pid);
    if (err != ESP_OK) {
        free(c);
        espix_eprintf(s, "espix: %s: cannot start: %s\n", argv[0],
                      esp_err_to_name(err));
        return 1;
    }

    espix_printf(s, "[%d] %s\n", (int)pid, argv[0]);
    if (!espix_shell_job_add(s, pid, argv[0])) {
        espix_eprintf(s, "espix: %d: no job slot; it runs untracked\n",
                      (int)pid);
    }
    return 0;
}

int espix_shell_take_key(espix_session_t *s)
{
    if (s == NULL) {
        return 0;
    }

    const int key = s->last_key;
    s->last_key = 0;
    return key;
}

/*
 * Split a command line into words the way a shell does.
 *
 * esp_console_split_argv() -- what this replaced -- knows only a double quote,
 * and only when one starts a word: echo "a b" gave one word, but echo 'a b'
 * gave two words with the quotes still on them, and echo a'b'c was three
 * literals. Here either quote groups and is removed, a quote may appear
 * anywhere in a word (a'b'c is abc), and a single-quoted $ stays literal -- it
 * is written out as backslash-dollar for expand_token(), which already knows
 * that spelling. Double quotes still expand, as sh does.
 *
 * Words are written back over the input, which is what keeps this reentrant on
 * the caller's scratch buffer: the write pointer never overtakes the read
 * pointer, because every byte written was consumed first.
 */
static int espix_split_argv(char *line, char **argv, int argv_size)
{
    int   argc = 0;
    char *out  = line;

    for (char *p = line; *p != '\0' && argc < argv_size; ) {
        while (*p == ' ' || *p == '\t' || *p == '\n') {
            p++;
        }
        if (*p == '\0') {
            break;
        }

        argv[argc++] = out;

        while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\n') {
            if (*p == '\'') {
                p++;
                while (*p != '\0' && *p != '\'') {
                    if (*p == '$') {
                        *out++ = '\\';      /* keep it literal below */
                    }
                    *out++ = *p++;
                }
                if (*p == '\'') {
                    p++;
                }
            } else if (*p == '"') {
                p++;
                while (*p != '\0' && *p != '"') {
                    if (*p == '\\' && (p[1] == '"' || p[1] == '\\')) {
                        p++;
                    }
                    *out++ = *p++;
                }
                if (*p == '"') {
                    p++;
                }
            } else {
                *out++ = *p++;
            }
        }

        /*
         * The separator is consumed *before* the terminator is written. When
         * nothing was quoted out == p, so writing the NUL first would erase the
         * separator and the scan below would read it as the end of the line --
         * which is what it did the first time.
         */
        if (*p != '\0') {
            p++;
        }
        *out++ = '\0';
    }

    return argc;
}

/*
 * One command, with no operators left in it: what espix_shell_exec() used to
 * be before `;`, `&&` and `||` were parsed. Split out so the sequencer below
 * can run it once per segment.
 */
static int exec_one(espix_session_t *s, const char *line)
{
    if (line == NULL) {
        return ESPIX_SHELL_EMPTY;
    }

    /* Per-session scratch copy: split_argv() writes into the buffer it is
     * given, and this is exactly what makes dispatch reentrant. */
    char  scratch[ESPIX_LINE_MAX];
    char *argv[ESPIX_ARGS_MAX];

    strlcpy(scratch, line, sizeof(scratch));

    int argc = espix_split_argv(scratch, argv, ESPIX_ARGS_MAX);
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

    /*
     * A trailing `&` is the shell's background operator, recognised here so
     * that no command ever sees it as an argument -- `cat f &` used to open a
     * file called `&`. The intent travels on the session because one of the
     * things it applies to is a builtin (`confine`), not only a program.
     */
    const bool prev_background = s->background;
    s->background = false;
    if (argc > 1 && strcmp(av[argc - 1], "&") == 0) {
        av[--argc] = NULL;
        s->background = true;
    }

    const espix_cmd_t *cmd = espix_shell_find(av[0]);

    if (cmd == NULL && s_exec_fallback == NULL) {
        s->background = prev_background;
        redirects_release(s, &redir);
        espix_env_scope_end(s, &scope);
        return ESPIX_SHELL_ENOENT;
    }

    redirects_apply(s, &redir);

    /* Not a builtin: let it be resolved as a program, the way a shell falls
     * through to PATH. */
    int status;

    /* Dispatched by the shell itself, so a command here becomes a process;
     * anywhere else -- a pipe stage, a job, sudo -- the caller already is one. */
    const bool on_shell = s_proc != NULL && s->shell_task != NULL &&
                          s->shell_task == (void *)xTaskGetCurrentTaskHandle();

    if (cmd == NULL) {
        status = s_exec_fallback(s, argc, av);
    } else if (s->background && !cmd->backgrounds && s_proc != NULL) {
        /*
         * A builtin as a background process (R-P2.8). confine is the
         * exception: it spawns a program, and a program is a process with a
         * pid, so it backgrounds itself through the flag.
         *
         * A scoped assignment is undone when this function returns, so it
         * cannot be lent to a job that outlives the line; take_redirects()
         * refuses a redirection with & for the same reason. Both are recorded
         * against R-P2.8.
         */
        if (assigned > 0) {
            espix_eprintf(s, "espix: assignments with & are not supported yet\n");
            status = 1;
        } else {
            status = start_background_job(s, cmd, argc, av);
        }
    } else if (cmd->shell ||
               (!on_shell && (!cmd->internal_stack || s_proc == NULL))) {
        status = cmd->fn(s, argc, av);
    } else {
        /* Including under sudo, for a command that maps flash: the caller's
         * stack is in PSRAM, which the cache freeze makes unreachable. */
        status = run_as_process(s, cmd, argc, av);
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
    s->background = prev_background;
    return status;
}

/*
 * `;`, `&&` and `||` as operators rather than words.
 *
 * A full shell parses the line into a tree before running anything; espix
 * splits it into one flat sequence, which is all these three need and costs no
 * objects. The split happens here, before expansion, because an operator is
 * only an operator when it is unquoted -- `echo 'a;b'` is one command, and the
 * `&` in `2>&1` is not `&&`.
 *
 * The result is a shell's: each segment runs in turn, `&&` skips the next when
 * the last status was non-zero, `||` skips it when the status was zero, and the
 * line's status is the last segment's. A blank segment is skipped, so `a ;; b`
 * is `a` then `b`, and a line of nothing but separators runs nothing -- which
 * leaves $? alone, as an empty line does.
 */
static bool blank_segment(const char *p)
{
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return *p == '\0';
}

/* ------------------------------------------------------------------ */
/* Pipes                                                               */
/* ------------------------------------------------------------------ */

#define PIPE_BUF_BYTES   1024
#define PIPE_MAX_STAGES  4

/*
 * One pipe: a StreamBuffer with a FILE on each end, so a builtin's ordinary
 * espix_printf() and fgets() reach it. A pipe is unidirectional with one
 * reader and one writer, which is exactly a StreamBuffer's contract.
 */
typedef struct {
    StreamBufferHandle_t sb;
    FILE                *wr;
    FILE                *rd;
    volatile bool        wr_closed;
    volatile bool        rd_closed;     /* the reader's command has returned */

    /* Each end waits on its task's notification (ESPIX_NOTIFY_SIG_IDX), so
     * the other end's progress and a signal both wake it. The lock keeps a
     * handle from outliving the wait it names. */
    SemaphoreHandle_t    lock;
    TaskHandle_t         rd_wait;
    TaskHandle_t         wr_wait;
} pipe_t;

static bool pipe_stopping(void)
{
    return espix_shell_stopping(espix_shell_current());
}

static void pipe_park(pipe_t *p, TaskHandle_t *waiter, bool on)
{
    xSemaphoreTake(p->lock, portMAX_DELAY);
    *waiter = on ? xTaskGetCurrentTaskHandle() : NULL;
    xSemaphoreGive(p->lock);
}

static void pipe_poke(pipe_t *p, TaskHandle_t *waiter)
{
    xSemaphoreTake(p->lock, portMAX_DELAY);
    if (*waiter != NULL) {
        (void)xTaskNotifyGiveIndexed(*waiter, ESPIX_NOTIFY_SIG_IDX);
    }
    xSemaphoreGive(p->lock);
}

/*
 * A full pipe whose reader has finished will never drain: `cat big | head -1`
 * left cat blocked here for good, and the line waiting on it with it. So the
 * write fails instead, which is what SIGPIPE/EPIPE would make of it.
 */
static int pipe_write(void *cookie, const char *buf, int len)
{
    pipe_t *p    = cookie;
    int     sent = 0;

    pipe_park(p, &p->wr_wait, true);
    while (sent < len) {
        if (p->rd_closed || pipe_stopping()) {
            pipe_park(p, &p->wr_wait, false);
            errno = EPIPE;
            return -1;
        }
        const size_t n = xStreamBufferSend(p->sb, buf + sent,
                                           (size_t)(len - sent), 0);
        if (n > 0) {
            sent += (int)n;
            pipe_poke(p, &p->rd_wait);
            continue;
        }
        (void)ulTaskNotifyTakeIndexed(ESPIX_NOTIFY_SIG_IDX, pdTRUE,
                                      portMAX_DELAY);
    }
    pipe_park(p, &p->wr_wait, false);
    return sent;
}

static int pipe_read(void *cookie, char *buf, int len)
{
    pipe_t *p = cookie;

    pipe_park(p, &p->rd_wait, true);
    for (;;) {
        /* The writer may close just after a miss: one more look before EOF. */
        const bool   closed = p->wr_closed || pipe_stopping();
        const size_t n      = xStreamBufferReceive(p->sb, buf, (size_t)len, 0);

        if (n > 0 || closed) {
            pipe_park(p, &p->rd_wait, false);
            pipe_poke(p, &p->wr_wait);
            return (int)n;
        }
        (void)ulTaskNotifyTakeIndexed(ESPIX_NOTIFY_SIG_IDX, pdTRUE,
                                      portMAX_DELAY);
    }
}

/* A stage's process. Its session is a copy: the stages run at once, so each
 * needs its own printf buffer and redirects. */
typedef struct {
    espix_session_t s;
    const char     *line;       /* into the caller's buffer, alive to the end */
    pipe_t         *out;        /* the pipe it writes, or NULL for the last */
    pipe_t         *in;         /* the pipe it reads, or NULL for the first */
} pipe_stage_t;

static int pipe_stage(void *arg)
{
    pipe_stage_t *st = arg;

    espix_shell_set_current(&st->s);
    const int status = exec_one(&st->s, st->line);

    /* The writer before this stage must not wait on a reader that is gone. */
    if (st->in != NULL) {
        st->in->rd_closed = true;
        pipe_poke(st->in, &st->in->wr_wait);
    }
    /* Closed here, so the next stage sees EOF as soon as this one is done. */
    if (st->out != NULL) {
        fflush(st->out->wr);
        fclose(st->out->wr);
        st->out->wr        = NULL;
        st->s.redirect     = NULL;
        st->out->wr_closed = true;
        pipe_poke(st->out, &st->out->rd_wait);
    }

    espix_shell_set_current(NULL);
    return status;
}

/* Does this stage end with an & token? A pipeline cannot background a stage:
 * the pipe it reads or writes is the caller stack, and the job would outlive
 * it. 2>&1 ends in 1 and a&b is one word, so neither matches. */
static bool stage_wants_background(const char *stage)
{
    const char *end = stage + strlen(stage);
    while (end > stage && (end[-1] == ' ' || end[-1] == '\t')) {
        end--;
    }
    const size_t len = (size_t)(end - stage);
    return len > 0 && end[-1] == '&' &&
           (len == 1 || end[-2] == ' ' || end[-2] == '\t');
}

/* The first word of a stage, so a pipe can refuse a program operand. */
static void first_token(const char *line, char *out, size_t len)
{
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    size_t i = 0;
    while (*line != '\0' && *line != ' ' && *line != '\t' && i + 1 < len) {
        out[i++] = *line++;
    }
    out[i] = '\0';
}

/*
 * A pipeline: `a | b | c`. Every stage is a process in one group, feeding the
 * next through a pipe, and the shell waits for them all -- so ^C reaches every
 * stage, and nothing outlives the line.
 *
 * A program stage is refused rather than run. An app's stdout is its session
 * stream, and pointing that at a pipe needs the descriptor plumbing R-P2.8
 * covers; running it anyway would put its output on the terminal while the
 * consumer waited for bytes that never came.
 */
static int run_pipeline(espix_session_t *s, char *line)
{
    char *stage[PIPE_MAX_STAGES];
    int   n    = 0;
    char *p    = line;

    stage[n++] = line;

    while (*p != '\0') {
        if (*p == '\'' || *p == '"') {
            const char quote = *p++;
            while (*p != '\0' && *p != quote) {
                if (quote == '"' && *p == '\\' && p[1] != '\0') {
                    p++;
                }
                p++;
            }
            if (*p == quote) {
                p++;
            }
            continue;
        }
        if (*p == '|') {
            *p = '\0';
            if (n >= PIPE_MAX_STAGES) {
                espix_eprintf(s, "espix: too many pipes (max %d)\n",
                              PIPE_MAX_STAGES - 1);
                return 2;
            }
            stage[n++] = p + 1;
        }
        p++;
    }

    if (n == 1) {
        return exec_one(s, line);
    }

    for (int i = 0; i < n; i++) {
        if (stage_wants_background(stage[i])) {
            espix_eprintf(s, "espix: & with a pipeline is not supported yet\n");
            return 2;
        }
    }

    if (s_proc == NULL) {
        espix_eprintf(s, "espix: pipes need the process table\n");
        return 2;
    }

    const espix_cmd_t *cmds[PIPE_MAX_STAGES];
    for (int i = 0; i < n; i++) {
        char word[32];
        first_token(stage[i], word, sizeof(word));
        cmds[i] = espix_shell_find(word);
        if (word[0] != '\0' && cmds[i] == NULL) {
            espix_eprintf(s, "espix: %s: pipes are for builtins for now\n", word);
            return 2;
        }
    }

    pipe_t pipes[PIPE_MAX_STAGES - 1];
    memset(pipes, 0, sizeof(pipes));

    for (int i = 0; i < n - 1; i++) {
        pipes[i].sb   = xStreamBufferCreate(PIPE_BUF_BYTES, 1);
        pipes[i].lock = xSemaphoreCreateMutex();
        if (pipes[i].sb != NULL && pipes[i].lock != NULL) {
            pipes[i].wr = funopen(&pipes[i], NULL, pipe_write, NULL, NULL);
            pipes[i].rd = funopen(&pipes[i], pipe_read, NULL, NULL, NULL);
        }
        if (pipes[i].wr == NULL || pipes[i].rd == NULL) {
            espix_eprintf(s, "espix: no memory for a pipe\n");
            for (int k = 0; k <= i; k++) {
                if (pipes[k].wr != NULL)   { fclose(pipes[k].wr); }
                if (pipes[k].rd != NULL)   { fclose(pipes[k].rd); }
                if (pipes[k].sb != NULL)   { vStreamBufferDelete(pipes[k].sb); }
                if (pipes[k].lock != NULL) { vSemaphoreDelete(pipes[k].lock); }
            }
            return 2;
        }
        /* Line buffered: a stage's output should reach the reader as it is
         * produced, and not only when a kilobyte has accumulated. */
        setvbuf(pipes[i].wr, NULL, _IOLBF, 128);
    }

    pipe_stage_t *st = calloc((size_t)n, sizeof(*st));
    espix_pid_t   pids[PIPE_MAX_STAGES] = { 0 };
    espix_pid_t   pgid    = 0;
    int           started = 0;

    for (int i = 0; st != NULL && i < n; i++) {
        const bool last = (i == n - 1);

        st[i].s             = *s;
        st[i].s.redirect    = last ? s->redirect : pipes[i].wr;
        st[i].s.redirect_in = (i > 0) ? pipes[i - 1].rd : s->redirect_in;
        st[i].s.err_to_out  = last && s->err_to_out;
        st[i].line          = stage[i];
        st[i].out           = last ? NULL : &pipes[i];
        st[i].in            = (i > 0) ? &pipes[i - 1] : NULL;

        /* Room for exec_one()'s own frames on top of the command's. */
        const espix_cmd_t *cmd   = cmds[i];
        uint32_t           stack = ESPIX_CMD_STACK;
        if (cmd != NULL && cmd->stack + 4096 > stack) {
            stack = cmd->stack + 4096;
        }
        espix_shell_spawn_t sp = {
            .name           = (cmd != NULL) ? cmd->name : "sh",
            .fn             = pipe_stage,
            .arg            = &st[i],
            .stack          = stack,
            .internal_stack = cmd != NULL && cmd->internal_stack,
            .pgid           = pgid,
        };
        if (s_proc->spawn(s, &sp, &pids[i]) != ESP_OK) {
            break;
        }
        pgid = sp.pgid;
        started++;
    }

    if (started < n) {
        /* Let the stages already running out of pipes nothing will drain. */
        espix_eprintf(s, "espix: cannot start a pipe stage\n");
        for (int i = 0; i < n - 1; i++) {
            pipes[i].rd_closed = true;
            pipes[i].wr_closed = true;
            pipe_poke(&pipes[i], &pipes[i].rd_wait);
            pipe_poke(&pipes[i], &pipes[i].wr_wait);
        }
    }

    /* The last stage's status is the line's; the rest are waited for so none
     * outlives its pipes. */
    int status = 2;
    for (int i = n - 1; i >= 0; i--) {
        if (pids[i] > 0) {
            const int st_status = s_proc->wait(s, pids[i], false);
            if (i == n - 1) {
                status = (st_status < 0) ? 1 : st_status;
            }
        }
    }

    for (int i = 0; i < n - 1; i++) {
        if (pipes[i].wr != NULL) { fclose(pipes[i].wr); }
        if (pipes[i].rd != NULL) { fclose(pipes[i].rd); }
        if (pipes[i].sb != NULL) { vStreamBufferDelete(pipes[i].sb); }
        vSemaphoreDelete(pipes[i].lock);
    }
    free(st);

    return status;
}

int espix_shell_exec(espix_session_t *s, const char *line)
{
    if (line == NULL) {
        return ESPIX_SHELL_EMPTY;
    }

    /* The line is cut up in place: each operator becomes the terminator of the
     * segment before it, so exec_one() is handed a plain command. */
    char buf[ESPIX_LINE_MAX];
    strlcpy(buf, line, sizeof(buf));

    int  status  = ESPIX_SHELL_EMPTY;
    bool ran     = false;
    int  pending = 0;   /* 0 = run, 1 = run if last succeeded, 2 = if it failed */

    char *seg = buf;
    char *p   = buf;

    for (;;) {
        /* The next unquoted operator, or the end of the line. */
        while (*p != '\0') {
            if (*p == '\'' || *p == '"') {
                const char quote = *p++;
                while (*p != '\0' && *p != quote) {
                    if (quote == '"' && *p == '\\' && p[1] != '\0') {
                        p++;
                    }
                    p++;
                }
                if (*p == quote) {
                    p++;
                }
                continue;
            }
            if (*p == ';' ||
                (*p == '&' && p[1] == '&') ||
                (*p == '|' && p[1] == '|')) {
                break;
            }
            p++;
        }

        const char op = *p;
        *p = '\0';

        const bool skip = (pending == 1) ? (ran && status != 0)
                                         : (pending == 2 ? (ran && status == 0)
                                                         : false);
        if (!skip && !blank_segment(seg)) {
            status = run_pipeline(s, seg);
            ran    = true;

            /*
             * POSIX updates $? after every command, not only at the end of the
             * line, so a later segment of the same line sees this one's status
             * -- false; echo $? prints 1. espix_shell_run_line() sets the
             * final value again for the next line, which is why the cross-line
             * case already worked.
             */
            if (s != NULL) {
                s->last_status = status;
            }
        }

        if (op == '\0') {
            break;
        }
        p += (op == ';') ? 1 : 2;
        pending = (op == '&') ? 1 : ((op == '|') ? 2 : 0);
        seg = p;
    }

    return ran ? status : ESPIX_SHELL_EMPTY;
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

void espix_shell_set_proc_ops(const espix_shell_proc_ops_t *ops)
{
    s_proc = ops;
}

bool espix_shell_sigcheck(void)
{
    return s_proc != NULL && s_proc->sigcheck();
}

static bool proc_stopping(void)
{
    return s_proc != NULL && s_proc->stopping();
}

void espix_shell_session_begin(espix_session_t *s)
{
    if (s_proc != NULL) {
        s_proc->begin(s);
        if (s->pid != ESPIX_PID_NONE) {
            s->shell_task = (void *)xTaskGetCurrentTaskHandle();
        }
    }
}

size_t espix_shell_session_end(espix_session_t *s)
{
    s->shell_task = NULL;
    const size_t orphans = (s_proc != NULL) ? s_proc->end(s) : 0;
    if (orphans > 0) {
        espix_klog(ESPIX_KLOG_INFO, TAG, "%s: killed %u process%s on exit",
                   s->user, (unsigned)orphans, orphans == 1 ? "" : "es");
    }
    return orphans;
}

void espix_shell_session_run(espix_session_t *s)
{
    if (s == NULL || s->read_line == NULL) {
        return;
    }

    espix_shell_set_current(s);
    espix_shell_session_begin(s);

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
        /* A full buffer may be a cut line, and running what is left of one --
         * an rm missing its last names -- is worse than running none of it. */
        if ((size_t)n >= sizeof(line) - 1) {
            espix_eprintf(s, "espix: line too long (max %u)\n",
                          (unsigned)(sizeof(line) - 2));
            continue;
        }

        (void)espix_shell_run_line(s, line);
    }

    /*
     * The session is over, so what it spawned goes with it -- and before the
     * caller tears its transport down, because a process still writing through
     * that transport would be writing into state about to go.
     *
     * Including a background job: that is what Linux does too, where a job
     * started with & still gets SIGHUP when the terminal closes. An escape
     * hatch is a feature nobody has asked for yet.
     */
    (void)espix_shell_session_end(s);

    espix_shell_set_current(NULL);
}
