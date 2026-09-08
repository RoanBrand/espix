/*
 * espix session: dispatch and the read-eval-print loop.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_console.h"

#include "espix_fs.h"
#include "espix_kernel.h"
#include "espix_shell.h"

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
    char buf[ESPIX_LINE_MAX];

    const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n < 0) {
        return n;
    }

    /* Truncation is reported as-written rather than retried on the heap: no
     * espix command legitimately emits a single line this long. */
    const size_t len = ((size_t)n < sizeof(buf)) ? (size_t)n : sizeof(buf) - 1;

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

/* What one command line's redirections resolved to. */
typedef struct {
    FILE *out;          /* `>` / `>>`, or NULL */
    FILE *err;          /* `2>` / `2>>`, or NULL */
    bool  err_to_out;   /* `2>&1` */
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
            espix_eprintf(s, "espix: %s: cannot open for writing\n", abs);
            return -1;
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
        fclose(r->out);
        r->out = NULL;
    }
    if (r->err != NULL) {
        fclose(r->err);
        r->err = NULL;
    }
}

static espix_exec_fallback_fn s_exec_fallback;

void espix_shell_set_exec_fallback(espix_exec_fallback_fn fn)
{
    s_exec_fallback = fn;
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

    redirects_t redir;
    argc = take_redirects(s, argc, argv, &redir);
    if (argc < 1) {
        redirects_release(s, &redir);
        return (argc < 0) ? 1 : ESPIX_SHELL_EMPTY;
    }
    argv[argc] = NULL;

    const espix_cmd_t *cmd = espix_shell_find(argv[0]);

    if (cmd == NULL && s_exec_fallback == NULL) {
        redirects_release(s, &redir);
        return ESPIX_SHELL_ENOENT;
    }

    redirects_apply(s, &redir);

    /* Not a builtin: let it be resolved as a program, the way a shell falls
     * through to PATH. */
    const int status = (cmd != NULL) ? cmd->fn(s, argc, argv)
                                     : s_exec_fallback(s, argc, argv);

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
        espix_eprintf(s, "espix: %s: command not found\n", argv[0]);
    }

    redirects_release(s, &redir);
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
