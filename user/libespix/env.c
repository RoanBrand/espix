/*
 * The environment of an app without an MMU: environ is the app's own, as libc keeps it on
 * Linux, so what setenv changes is what exec passes on. It starts as the block the kernel
 * hands the entry, and becomes an array of the app's own at the first change.
 */
#if ESPIX_NOMMU_APP

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

extern char **environ;

static char **s_own;        /* environ, once it is ours */
static char **s_made;       /* the strings setenv made, freed when replaced */
static int    s_nmade;

static int env_count(void)
{
    int n = 0;
    while (environ != NULL && environ[n] != NULL) {
        n++;
    }
    return n;
}

static int env_find(const char *name, size_t n)
{
    for (int i = 0; environ != NULL && environ[i] != NULL; i++) {
        if (strncmp(environ[i], name, n) == 0 && environ[i][n] == '=') {
            return i;
        }
    }
    return -1;
}

static void drop(char *s)
{
    for (int i = 0; i < s_nmade; i++) {
        if (s_made[i] == s) {
            free(s);
            s_made[i] = s_made[--s_nmade];
            return;
        }
    }
}

/* environ as our own array with room for extra more; its count, or -1. */
static int own(int extra)
{
    const int n = env_count();
    char **v = (environ == s_own) ? realloc(s_own, (size_t)(n + 1 + extra) * sizeof(*v))
                                  : malloc((size_t)(n + 1 + extra) * sizeof(*v));
    if (v == NULL) {
        return -1;
    }
    if (environ != s_own && n > 0) {
        memcpy(v, environ, (size_t)n * sizeof(*v));
    }
    v[n] = NULL;
    s_own = environ = v;
    return n;
}

/* NAME=value in place of NAME's entry, or added. */
static int put(char *entry, size_t n)
{
    const int i = env_find(entry, n);
    const int count = own(i < 0);
    if (count < 0) {
        return -1;
    }
    if (i >= 0) {
        char *const old = environ[i];
        environ[i] = entry;
        drop(old);
    } else {
        environ[count]     = entry;
        environ[count + 1] = NULL;
    }
    return 0;
}

char *getenv(const char *name)
{
    if (name == NULL || strchr(name, '=') != NULL) {
        return NULL;
    }
    const size_t n = strlen(name);
    const int i = env_find(name, n);
    return (i < 0) ? NULL : environ[i] + n + 1;
}

int setenv(const char *name, const char *value, int overwrite)
{
    if (name == NULL || name[0] == '\0' || strchr(name, '=') != NULL) {
        errno = EINVAL;
        return -1;
    }
    const size_t n = strlen(name);
    if (!overwrite && env_find(name, n) >= 0) {
        return 0;
    }
    const size_t vl = (value != NULL) ? strlen(value) : 0;
    char **const m = realloc(s_made, (size_t)(s_nmade + 1) * sizeof(*m));
    char *const e = malloc(n + vl + 2);
    if (m != NULL) {
        s_made = m;
    }
    if (m == NULL || e == NULL) {
        free(e);
        errno = ENOMEM;
        return -1;
    }
    memcpy(e, name, n);
    e[n] = '=';
    memcpy(e + n + 1, (value != NULL) ? value : "", vl + 1);
    if (put(e, n) != 0) {
        free(e);
        errno = ENOMEM;
        return -1;
    }
    s_made[s_nmade++] = e;
    return 0;
}

int unsetenv(const char *name)
{
    if (name == NULL || name[0] == '\0' || strchr(name, '=') != NULL) {
        errno = EINVAL;
        return -1;
    }
    const size_t n = strlen(name);
    if (env_find(name, n) < 0) {
        return 0;
    }
    if (own(0) < 0) {
        errno = ENOMEM;
        return -1;
    }
    int out = 0;
    for (int i = 0; environ[i] != NULL; i++) {
        if (strncmp(environ[i], name, n) == 0 && environ[i][n] == '=') {
            drop(environ[i]);
        } else {
            environ[out++] = environ[i];
        }
    }
    environ[out] = NULL;
    return 0;
}

/* The string itself becomes the entry, as POSIX has it; without '=' it unsets, as glibc's does. */
int putenv(char *s)
{
    const char *const eq = (s != NULL) ? strchr(s, '=') : NULL;
    if (eq == NULL) {
        return (s != NULL) ? unsetenv(s) : (errno = EINVAL, -1);
    }
    if (eq == s || put(s, (size_t)(eq - s)) != 0) {
        errno = (eq == s) ? EINVAL : ENOMEM;
        return -1;
    }
    return 0;
}

int clearenv(void)
{
    while (s_nmade > 0) {
        free(s_made[--s_nmade]);
    }
    free(s_own);
    s_own   = NULL;
    environ = NULL;
    return 0;
}

#endif /* ESPIX_NOMMU_APP */
