/*
 * Session variables, and the environment a process inherits from them.
 *
 * Three tiers, and the boundaries between them are the whole design:
 *
 *   the system environment   newlib's global `environ`. Machine-wide config --
 *                            TZ above all, which espix_time sets for tzset().
 *                            Read-mostly, one clock, one timezone.
 *
 *   a session's table        what `export FOO=bar` and `FOO=bar` write. One per
 *                            login, owned by that login, seen by nobody else.
 *
 *   a process's environment  a flat NUL-terminated `char **` built at spawn from
 *                            the two above, session winning. See copy_env() in
 *                            espix_proc/exec.c.
 *
 * Why the process gets a flat copy rather than a lookup that falls back to the
 * global: falling back lets you override a system variable and does not let you
 * *remove* one -- `unset TZ` would keep finding the system copy underneath.
 * Fixing that needs tombstone entries, which is a concept bought to save memory
 * that is not there to save; the system environment is TZ and little else. One
 * flat array is what POSIX specifies anyway, and `unset` then behaves.
 *
 * Why a session owns its table outright, with no sharing between logins: espix
 * has already paid for the alternative once. Command history is owned per
 * *user*, so two sessions as the same account hold the same list, and the module
 * locked only the lookup -- a double free that surfaced as four unrelated
 * panics, none of them in history.c. Nothing here is shared, so nothing here
 * needs a lock.
 */

#include <stdlib.h>
#include <string.h>

#include "espix_kernel.h"
#include "espix_shell.h"

struct espix_env {
    struct {
        char *name;         /* NULL when the slot is free */
        char *value;
        bool  exported;
    } v[ESPIX_ENV_MAX];
    size_t count;
};

/* ------------------------------------------------------------------ */

static int find(const espix_env_t *e, const char *name)
{
    if (e == NULL) {
        return -1;
    }
    for (size_t i = 0; i < ESPIX_ENV_MAX; i++) {
        if (e->v[i].name != NULL && strcmp(e->v[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* A name the shell will accept. POSIX's rule, and the one that makes
 * `FOO=bar cmd` decidable: anything else is a command, not an assignment. */
bool espix_env_name_ok(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    if (!((name[0] >= 'A' && name[0] <= 'Z') ||
          (name[0] >= 'a' && name[0] <= 'z') || name[0] == '_')) {
        return false;
    }
    for (const char *p = name + 1; *p != '\0'; p++) {
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
              (*p >= '0' && *p <= '9') || *p == '_')) {
            return false;
        }
    }
    return strlen(name) <= ESPIX_ENV_NAME_MAX;
}

/* ------------------------------------------------------------------ */

const char *espix_env_get(const espix_session_t *s, const char *name)
{
    if (s == NULL || name == NULL) {
        return NULL;
    }
    const int i = find(s->env, name);
    if (i >= 0) {
        return s->env->v[i].value;
    }

    /* Then the system environment. A session that has not set a name sees the
     * machine's answer for it, which is how TZ reaches everybody. */
    return getenv(name);
}

esp_err_t espix_env_set(espix_session_t *s, const char *name,
                        const char *value, bool exported)
{
    if (s == NULL || !espix_env_name_ok(name) || value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(value) > ESPIX_ENV_VALUE_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (s->env == NULL) {
        /* Lazily, so a session that never sets a variable costs nothing. */
        s->env = calloc(1, sizeof(*s->env));
        if (s->env == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    char *copy = strdup(value);
    if (copy == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const int at = find(s->env, name);
    if (at >= 0) {
        free(s->env->v[at].value);
        s->env->v[at].value = copy;
        /* `export FOO` on an existing variable exports it; `FOO=bar` on an
         * already-exported one leaves it exported, as sh does. */
        s->env->v[at].exported = s->env->v[at].exported || exported;
        return ESP_OK;
    }

    for (size_t i = 0; i < ESPIX_ENV_MAX; i++) {
        if (s->env->v[i].name == NULL) {
            s->env->v[i].name = strdup(name);
            if (s->env->v[i].name == NULL) {
                free(copy);
                return ESP_ERR_NO_MEM;
            }
            s->env->v[i].value    = copy;
            s->env->v[i].exported = exported;
            s->env->count++;
            return ESP_OK;
        }
    }

    free(copy);
    return ESP_ERR_NO_MEM;      /* table full; the caller says so */
}

bool espix_env_unset(espix_session_t *s, const char *name)
{
    if (s == NULL || name == NULL) {
        return false;
    }
    const int i = find(s->env, name);
    if (i < 0) {
        return false;
    }
    free(s->env->v[i].name);
    free(s->env->v[i].value);
    s->env->v[i].name     = NULL;
    s->env->v[i].value    = NULL;
    s->env->v[i].exported = false;
    s->env->count--;
    return true;
}

bool espix_env_at(const espix_session_t *s, size_t slot, const char **name,
                  const char **value, bool *exported)
{
    if (s == NULL || s->env == NULL || slot >= ESPIX_ENV_MAX) {
        return false;
    }
    if (s->env->v[slot].name == NULL) {
        return false;
    }
    if (name != NULL)     { *name     = s->env->v[slot].name; }
    if (value != NULL)    { *value    = s->env->v[slot].value; }
    if (exported != NULL) { *exported = s->env->v[slot].exported; }
    return true;
}

size_t espix_env_count(const espix_session_t *s)
{
    return (s == NULL || s->env == NULL) ? 0 : s->env->count;
}

void espix_env_free(espix_session_t *s)
{
    if (s == NULL || s->env == NULL) {
        return;
    }
    for (size_t i = 0; i < ESPIX_ENV_MAX; i++) {
        free(s->env->v[i].name);
        free(s->env->v[i].value);
    }
    free(s->env);
    s->env = NULL;
}

/* ------------------------------------------------------------------ */
/* One-shot assignments: `FOO=bar cmd`                                 */
/* ------------------------------------------------------------------ */
/*
 * Applied to the session's own table for the duration of one command and then
 * undone, rather than carried alongside it. That keeps one source of truth --
 * a spawned process reads the session table and nothing else -- at the cost of
 * having to put back exactly what was there, which is what the scope is for.
 *
 * A shadowed variable is *detached* rather than copied: its heap strings move
 * into the scope and back again, so a one-shot over an existing name costs no
 * allocation for the old value and cannot fail halfway.
 */

void espix_env_scope_init(espix_env_scope_t *scope)
{
    if (scope != NULL) {
        memset(scope, 0, sizeof(*scope));
    }
}

esp_err_t espix_env_scope_set(espix_session_t *s, espix_env_scope_t *scope,
                              const char *name, const char *value)
{
    if (s == NULL || scope == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (scope->n >= ESPIX_ENV_SCOPE_MAX) {
        return ESP_ERR_NO_MEM;
    }

    const int at = find(s->env, name);
    if (at >= 0) {
        /* Detach: the table forgets these strings, the scope owns them. */
        scope->saved[scope->n].name     = s->env->v[at].name;
        scope->saved[scope->n].value    = s->env->v[at].value;
        scope->saved[scope->n].exported = s->env->v[at].exported;
        scope->saved[scope->n].existed  = true;
        s->env->v[at].name     = NULL;
        s->env->v[at].value    = NULL;
        s->env->v[at].exported = false;
        s->env->count--;
    } else {
        scope->saved[scope->n].existed = false;
        scope->saved[scope->n].name    = strdup(name);
        if (scope->saved[scope->n].name == NULL) {
            return ESP_ERR_NO_MEM;
        }
        scope->saved[scope->n].value = NULL;
    }
    scope->n++;

    /* Exported, because the point of `FOO=bar cmd` is that cmd sees it. */
    const esp_err_t err = espix_env_set(s, name, value, true);
    if (err != ESP_OK) {
        return err;
    }
    return ESP_OK;
}

/*
 * Keep the new value instead of undoing it: `FOO=bar` with no command, which sh
 * treats as setting a shell variable rather than as a discarded one-shot.
 *
 * The export flag reverts to whatever it was, which is not the same as false --
 * assigning to an already-exported variable leaves it exported, and only a name
 * that did not exist becomes a plain unexported shell variable.
 */
esp_err_t espix_env_scope_keep(espix_session_t *s, espix_env_scope_t *scope,
                               const char *name)
{
    if (s == NULL || scope == NULL || name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (int i = 0; i < scope->n; i++) {
        if (scope->saved[i].name == NULL ||
            strcmp(scope->saved[i].name, name) != 0) {
            continue;
        }

        const int at = find(s->env, name);
        if (at >= 0) {
            s->env->v[at].exported = scope->saved[i].existed
                                         ? scope->saved[i].exported
                                         : false;
        }

        /* Consumed: scope_end must not put the old value back over it. */
        free(scope->saved[i].name);
        free(scope->saved[i].value);
        scope->saved[i].name    = NULL;
        scope->saved[i].value   = NULL;
        scope->saved[i].existed = false;
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

void espix_env_scope_end(espix_session_t *s, espix_env_scope_t *scope)
{
    if (s == NULL || scope == NULL) {
        return;
    }
    /* Backwards, so a name assigned twice on one line unwinds in order. */
    for (int i = scope->n - 1; i >= 0; i--) {
        if (scope->saved[i].name == NULL) {
            continue;                   /* kept by espix_env_scope_keep() */
        }

        espix_env_unset(s, scope->saved[i].name);

        if (scope->saved[i].existed) {
            /* Re-attach the originals; no allocation, so this cannot fail. */
            for (size_t k = 0; k < ESPIX_ENV_MAX; k++) {
                if (s->env != NULL && s->env->v[k].name == NULL) {
                    s->env->v[k].name     = scope->saved[i].name;
                    s->env->v[k].value    = scope->saved[i].value;
                    s->env->v[k].exported = scope->saved[i].exported;
                    s->env->count++;
                    break;
                }
            }
        } else {
            free(scope->saved[i].name);
        }
        scope->saved[i].name  = NULL;
        scope->saved[i].value = NULL;
    }
    scope->n = 0;
}

/* ------------------------------------------------------------------ */

/*
 * What a fresh login starts with.
 *
 * Called by each transport once the session's identity and cwd are settled,
 * because that is what these are derived from. HOME comes from the session's own
 * `home`, which apply_account() fills from /etc/passwd and leaves empty when
 * that directory does not exist -- so a login with no home gets "/" here for
 * the same reason it starts there, rather than a HOME pointing at nothing.
 *
 * Deliberately few. Every name here is one an app or a script can rely on, and
 * a default nobody asked for is a default nobody can remove -- `unset PATH`
 * should mean something, so PATH is set once here and not re-asserted.
 *
 * TZ is absent on purpose: it belongs to the machine, lives in the system
 * environment where tzset() reads it, and a per-session copy would be a second
 * answer to a question that has one.
 */
void espix_env_set_login_defaults(espix_session_t *s)
{
    if (s == NULL) {
        return;
    }

    (void)espix_env_set(s, "USER", (s->user[0] != '\0') ? s->user : "root", true);
    (void)espix_env_set(s, "HOME", (s->home[0] != '\0') ? s->home : "/", true);
    (void)espix_env_set(s, "PATH", "/bin", true);

    /*
     * The transport already decided this: `ansi` is what esp_linenoise probed
     * on the console and what an SSH session always has. Publishing it as TERM
     * is how a program that is not the shell gets to know.
     */
    (void)espix_env_set(s, "TERM", s->ansi ? "xterm" : "dumb", true);
}
