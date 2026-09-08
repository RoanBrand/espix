/*
 * env, export, unset -- the session's variables.
 *
 * Three commands over one table (espix_shell/env.c), and the split between
 * `env` and `export` is the useful part rather than tradition: `export` lists
 * everything the session holds and marks what is exported, `env` lists what a
 * program would actually receive. A variable set with `FOO=bar` and never
 * exported appears in one and not the other, which is the only way to see the
 * difference without spawning something.
 */

#include <stdlib.h>
#include <string.h>

#include "espix_cmds_priv.h"
#include "espix_kernel.h"
#include "espix_shell.h"

/* The system environment, which every process inherits under the session's
 * own variables. newlib's, and on IDF it is lock-protected -- see the note in
 * espix_shell/env.c about why this one global is safe to have a role. */
extern char **environ;

static void report_set_error(espix_session_t *s, const char *name,
                             esp_err_t err)
{
    switch (err) {
    case ESP_ERR_INVALID_ARG:
        espix_eprintf(s, "espix: %s: not a valid variable name\n", name);
        break;
    case ESP_ERR_INVALID_SIZE:
        espix_eprintf(s, "espix: %s: value longer than %d bytes\n", name,
                      ESPIX_ENV_VALUE_MAX);
        break;
    case ESP_ERR_NO_MEM:
        espix_eprintf(s, "espix: %s: no room; %d variables is the limit\n",
                      name, ESPIX_ENV_MAX);
        break;
    default:
        espix_eprintf(s, "espix: %s: cannot set\n", name);
        break;
    }
}

/* ------------------------------------------------------------------ */

static int cmd_env(espix_session_t *s, int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /*
     * The system environment first, then the session's exports over it, which
     * is the order a spawned process sees them merged in. Printing them in that
     * order means the last line for a name is the one that wins, so the listing
     * does not have to explain itself.
     */
    for (char **e = environ; e != NULL && *e != NULL; e++) {
        char name[ESPIX_ENV_NAME_MAX + 1];
        const char *eq = strchr(*e, '=');
        if (eq == NULL) {
            continue;
        }
        const size_t nl = (size_t)(eq - *e);
        if (nl > ESPIX_ENV_NAME_MAX) {
            continue;
        }
        memcpy(name, *e, nl);
        name[nl] = '\0';

        /* Skipped when the session overrides it; its own line comes below. */
        bool shadowed = false;
        for (size_t i = 0; i < ESPIX_ENV_MAX; i++) {
            const char *n = NULL;
            bool exported = false;
            if (espix_env_at(s, i, &n, NULL, &exported) && exported &&
                strcmp(n, name) == 0) {
                shadowed = true;
                break;
            }
        }
        if (!shadowed) {
            espix_printf(s, "%s\n", *e);
        }
    }

    for (size_t i = 0; i < ESPIX_ENV_MAX; i++) {
        const char *name = NULL, *value = NULL;
        bool exported = false;
        if (espix_env_at(s, i, &name, &value, &exported) && exported) {
            espix_printf(s, "%s=%s\n", name, value);
        }
    }
    return 0;
}

static int cmd_export(espix_session_t *s, int argc, char **argv)
{
    if (argc < 2) {
        /* No arguments: list everything the session holds, exported or not.
         * The marker is what makes this different from `env`. */
        for (size_t i = 0; i < ESPIX_ENV_MAX; i++) {
            const char *name = NULL, *value = NULL;
            bool exported = false;
            if (espix_env_at(s, i, &name, &value, &exported)) {
                espix_printf(s, "%s %s=%s\n", exported ? "export" : "      ",
                             name, value);
            }
        }
        return 0;
    }

    int rc = 0;
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');

        if (eq == NULL) {
            /* `export FOO` -- export what is already there. Exporting a name
             * that is not set does nothing, as sh does, rather than creating an
             * empty variable nobody asked for. */
            const char *existing = NULL;
            bool        found    = false;
            for (size_t k = 0; k < ESPIX_ENV_MAX; k++) {
                const char *n = NULL;
                if (espix_env_at(s, k, &n, &existing, NULL) &&
                    strcmp(n, argv[i]) == 0) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                continue;
            }
            const esp_err_t err = espix_env_set(s, argv[i], existing, true);
            if (err != ESP_OK) {
                report_set_error(s, argv[i], err);
                rc = 1;
            }
            continue;
        }

        *eq = '\0';
        const esp_err_t err = espix_env_set(s, argv[i], eq + 1, true);
        if (err != ESP_OK) {
            report_set_error(s, argv[i], err);
            rc = 1;
        }
        *eq = '=';
    }
    return rc;
}

static int cmd_unset(espix_session_t *s, int argc, char **argv)
{
    if (argc < 2) {
        espix_eprintf(s, "usage: unset <name>...\n");
        return 1;
    }

    /*
     * Silent when the name is not set, as sh is. And deliberately unable to
     * remove a system variable: the session's table is the session's, and the
     * machine's environment is not a login's to edit. What `unset TZ` does is
     * remove any session override, which is the useful half.
     */
    for (int i = 1; i < argc; i++) {
        (void)espix_env_unset(s, argv[i]);
    }
    return 0;
}

/* ------------------------------------------------------------------ */

static espix_cmd_t s_env_cmds[] = {
    { .name = "env",    .fn = cmd_env,
      .help = "list the environment a program would get", .usage = "env" },
    { .name = "export", .fn = cmd_export,
      .help = "set a variable, or mark one for export",
      .usage = "export [name[=value]]..." },
    { .name = "unset",  .fn = cmd_unset,
      .help = "remove a session variable",     .usage = "unset <name>..." },
};

void espix_cmds_register_env(void)
{
    espix_cmds_register_table(s_env_cmds,
                              sizeof(s_env_cmds) / sizeof(s_env_cmds[0]));
}
