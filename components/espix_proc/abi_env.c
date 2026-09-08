/*
 * getenv, setenv, unsetenv and putenv, for a loaded app.
 *
 * An app must not reach newlib's. newlib keeps one `environ` for the whole
 * firmware, so an app calling libc's getenv() would read the machine's
 * environment and miss everything its own session exported -- and worse, an app
 * calling libc's setenv() would write into a table every task on the device
 * shares. Per-process is the whole point.
 *
 * Why these are prefixed and mapped by the resolver rather than simply named
 * getenv() and friends: newlib already defines those names in this firmware, so
 * defining them here is a duplicate symbol at link time. The same reason
 * abi_signal.c prefixes sleep() and getpid(). And ESP_ELFSYM_EXPORT(getenv)
 * would publish *newlib's* address, which is precisely backwards -- the macro
 * expands to { "getenv", &getenv }.
 *
 * What an app gets is a snapshot. Its environment was packed at spawn by
 * copy_env() and belongs to it; setenv() here edits that copy, and neither the
 * session it came from nor any other process sees the change. That is what
 * fork/exec gives on Unix, and doing anything else would mean sharing a mutable
 * table between tasks, which is the bug this project already paid for once in
 * command history.
 *
 * The snapshot is fixed-size, so setenv() can add a name only while there is a
 * free slot -- see the note on growth below.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "espix_kernel.h"
#include "espix_proc.h"
#include "espix_proc_priv.h"

#define TAG "abi"

/*
 * The cap on what an app may add is ESPIX_PROC_ENV_ADDED_MAX, declared with the
 * slot it lives in. copy_env() packs the inherited set into one allocation
 * sized exactly for it, because that is the common case and costs nothing when
 * an app never calls setenv(); anything the app adds goes in the separate table
 * beside it, so the packed block keeps its interior pointers valid.
 */

/* "NAME=value" starts with "NAME=" ? */
static bool entry_is(const char *entry, const char *name, size_t nlen)
{
    return strncmp(entry, name, nlen) == 0 && entry[nlen] == '=';
}

static char *espix_abi_getenv(const char *name)
{
    espix_proc_slot_t *slot = espix_proc_self();
    if (slot == NULL || name == NULL || name[0] == '\0') {
        return NULL;
    }
    const size_t nlen = strlen(name);

    /* Anything the app added wins over what it inherited: a later setenv()
     * replaces an earlier value, and the added table is searched first. */
    for (int i = 0; i < slot->env_added_count; i++) {
        if (entry_is(slot->env_added[i], name, nlen)) {
            return slot->env_added[i] + nlen + 1;
        }
    }
    for (char **e = slot->envp; e != NULL && *e != NULL; e++) {
        if (entry_is(*e, name, nlen)) {
            return *e + nlen + 1;
        }
    }
    return NULL;
}

static int espix_abi_setenv(const char *name, const char *value, int overwrite)
{
    espix_proc_slot_t *slot = espix_proc_self();
    if (slot == NULL || name == NULL || value == NULL || name[0] == '\0' ||
        strchr(name, '=') != NULL) {
        errno = EINVAL;
        return -1;
    }

    const size_t nlen = strlen(name);

    if (!overwrite && espix_abi_getenv(name) != NULL) {
        return 0;
    }

    char *entry = malloc(nlen + 1 + strlen(value) + 1);
    if (entry == NULL) {
        errno = ENOMEM;
        return -1;
    }
    sprintf(entry, "%s=%s", name, value);

    /* Replace in place if the app has already set this one. */
    for (int i = 0; i < slot->env_added_count; i++) {
        if (entry_is(slot->env_added[i], name, nlen)) {
            free(slot->env_added[i]);
            slot->env_added[i] = entry;
            return 0;
        }
    }

    if (slot->env_added_count >= ESPIX_PROC_ENV_ADDED_MAX) {
        free(entry);
        errno = ENOMEM;
        return -1;
    }
    slot->env_added[slot->env_added_count++] = entry;
    return 0;
}

static int espix_abi_unsetenv(const char *name)
{
    espix_proc_slot_t *slot = espix_proc_self();
    if (slot == NULL || name == NULL || name[0] == '\0' ||
        strchr(name, '=') != NULL) {
        errno = EINVAL;
        return -1;
    }
    const size_t nlen = strlen(name);

    for (int i = 0; i < slot->env_added_count; i++) {
        if (entry_is(slot->env_added[i], name, nlen)) {
            free(slot->env_added[i]);
            slot->env_added[i] = slot->env_added[--slot->env_added_count];
            slot->env_added[slot->env_added_count] = NULL;
            break;
        }
    }

    /*
     * Removing an inherited name is done by hiding it, not by rewriting the
     * packed block: entries there are interior pointers into one allocation
     * that must stay intact to be freed. Shifting the vector down is enough --
     * the strings stay where they are and the block still frees in one call.
     */
    if (slot->envp != NULL) {
        for (char **e = slot->envp; *e != NULL; e++) {
            if (entry_is(*e, name, nlen)) {
                for (char **q = e; *q != NULL; q++) {
                    q[0] = q[1];
                }
                break;
            }
        }
    }
    return 0;
}

/*
 * putenv() takes ownership of the caller's string in POSIX, which is a trap
 * worth not importing: the app would then have to keep that buffer alive for as
 * long as the variable exists, and a stack buffer is the obvious mistake.
 * espix copies instead, which is what most people assume it does anyway.
 */
static int espix_abi_putenv(char *string)
{
    if (string == NULL) {
        errno = EINVAL;
        return -1;
    }
    char *eq = strchr(string, '=');
    if (eq == NULL || eq == string) {
        return espix_abi_unsetenv(string);
    }

    char name[64];
    const size_t nlen = (size_t)(eq - string);
    if (nlen >= sizeof(name)) {
        errno = EINVAL;
        return -1;
    }
    memcpy(name, string, nlen);
    name[nlen] = '\0';

    return espix_abi_setenv(name, eq + 1, 1);
}

/* ------------------------------------------------------------------- */

static const abi_sym_t s_env_syms[] = {
    ABI_SYM("getenv",   espix_abi_getenv),
    ABI_SYM("setenv",   espix_abi_setenv),
    ABI_SYM("unsetenv", espix_abi_unsetenv),
    ABI_SYM("putenv",   espix_abi_putenv),
};

void espix_proc_abi_env_register(void)
{
    espix_abi_resolver_add(s_env_syms,
                           sizeof(s_env_syms) / sizeof(s_env_syms[0]));

    espix_klog(ESPIX_KLOG_DEBUG, TAG, "environment published to apps");
}
