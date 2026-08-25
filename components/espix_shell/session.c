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

int espix_puts(espix_session_t *s, const char *str)
{
    if (str == NULL) {
        return 0;
    }
    return session_out(s, str, strlen(str));
}

int espix_printf(espix_session_t *s, const char *fmt, ...)
{
    char    buf[ESPIX_LINE_MAX];
    va_list ap;

    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n < 0) {
        return n;
    }

    /* Truncation is reported as-written rather than retried on the heap: no
     * espix command legitimately emits a single line this long. */
    const size_t len = ((size_t)n < sizeof(buf)) ? (size_t)n : sizeof(buf) - 1;

    return session_out(s, buf, len);
}

/*
 * Strip a trailing `> file` / `>> file` from argv and open the target.
 * Returns the new argc, or a negative value if the redirection is malformed.
 * On success *out_file is the stream to write to (NULL if none was requested).
 */
static int take_redirect(espix_session_t *s, int argc, char **argv, FILE **out_file)
{
    *out_file = NULL;

    for (int i = 0; i < argc; i++) {
        const bool append = (strcmp(argv[i], ">>") == 0);
        if (!append && strcmp(argv[i], ">") != 0) {
            continue;
        }

        if (i + 2 != argc) {
            espix_printf(s, "espix: redirection needs exactly one target\n");
            return -1;
        }

        char abs[ESPIX_PATH_MAX];
        if (espix_fs_resolve(s != NULL ? s->cwd : "/", argv[i + 1],
                             abs, sizeof(abs)) != ESP_OK) {
            espix_printf(s, "espix: %s: path too long\n", argv[i + 1]);
            return -1;
        }

        FILE *f = fopen(abs, append ? "ab" : "wb");
        if (f == NULL) {
            espix_printf(s, "espix: %s: cannot open for writing\n", abs);
            return -1;
        }

        *out_file = f;
        return i;                       /* argv[i..] dropped from the command */
    }

    return argc;
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

    FILE *redirect = NULL;
    argc = take_redirect(s, argc, argv, &redirect);
    if (argc < 1) {
        if (redirect != NULL) {
            fclose(redirect);
        }
        return (argc < 0) ? 1 : ESPIX_SHELL_EMPTY;
    }
    argv[argc] = NULL;

    const espix_cmd_t *cmd = espix_shell_find(argv[0]);
    if (cmd == NULL) {
        /* Not a builtin: let it be resolved as a program, the way a shell
         * falls through to PATH. */
        if (s_exec_fallback != NULL) {
            if (redirect != NULL && s != NULL) {
                s->redirect = redirect;
            }
            const int status = s_exec_fallback(s, argc, argv);
            if (redirect != NULL) {
                if (s != NULL) {
                    s->redirect = NULL;
                }
                fclose(redirect);
            }
            return status;
        }
        if (redirect != NULL) {
            fclose(redirect);
        }
        return ESPIX_SHELL_ENOENT;
    }

    if (redirect != NULL && s != NULL) {
        s->redirect = redirect;
    }

    const int status = cmd->fn(s, argc, argv);

    if (redirect != NULL) {
        if (s != NULL) {
            s->redirect = NULL;
        }
        fclose(redirect);
    }

    return status;
}

static void build_prompt(const espix_session_t *s, char *buf, size_t len)
{
    snprintf(buf, len, "espix:%s# ", s->cwd[0] != '\0' ? s->cwd : "/");
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

        const int status = espix_shell_exec(s, line);

        if (status == ESPIX_SHELL_ENOENT) {
            char *sp = strchr(line, ' ');
            if (sp != NULL) {
                *sp = '\0';
            }
            espix_printf(s, "espix: %s: command not found\n", line);
            s->last_status = 127;
        } else if (status != ESPIX_SHELL_EMPTY) {
            s->last_status = status;
        }
    }

    espix_shell_set_current(NULL);
}
