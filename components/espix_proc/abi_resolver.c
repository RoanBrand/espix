/*
 * The one symbol resolver, and the tables that feed it.
 *
 * elf_set_symbol_resolver() takes a single function, so there is exactly one
 * hook for the whole system. That hook used to live in abi_signal.c because
 * signals needed it first -- but it was never about signals: the comment there
 * already said "everything else, including the whole libc and IDF surface,
 * unchanged", which is a global resolver's job description.
 *
 * Splitting it out means a subsystem can publish a name that shadows libc
 * without filing it under someone else's heading. abi_env.c is the second
 * customer; putting getenv() in a file called abi_signal.c to reuse its table
 * is exactly the drift abi_libc.c's header complains about.
 *
 * Why a resolver rather than another symbol table: elf_find_sym_default()
 * searches the loader's own libc table *first*, and a table registered with
 * esp_elf_register_symbol() is consulted after it -- so a registered entry can
 * never shadow anything the loader already answers for. The resolver runs
 * before all of it, which is the loader's documented hook for "symbol
 * interception and hooking".
 */

#include <string.h>

#include "esp_elf.h"
#include "private/elf_symbol.h"

#include "espix_kernel.h"
#include "espix_proc_priv.h"

#define TAG "abi"

/* Small and fixed: one entry per subsystem that publishes overrides, which is
 * two today and will not be many. A miss costs a walk of both. */
#define ABI_TABLES_MAX 4

static struct {
    const abi_sym_t *syms;
    size_t           count;
} s_tables[ABI_TABLES_MAX];

static size_t s_table_count;

static uintptr_t espix_symbol_resolver(const char *sym_name)
{
    if (sym_name != NULL) {
        for (size_t t = 0; t < s_table_count; t++) {
            for (size_t i = 0; i < s_tables[t].count; i++) {
                if (strcmp(sym_name, s_tables[t].syms[i].name) == 0) {
                    return s_tables[t].syms[i].addr;
                }
            }
        }
    }

    /* Everything else, including the whole libc and IDF surface, unchanged. */
    return elf_find_sym_default(sym_name);
}

void espix_abi_resolver_add(const abi_sym_t *syms, size_t count)
{
    if (syms == NULL || count == 0) {
        return;
    }
    if (s_table_count >= ABI_TABLES_MAX) {
        /* Cannot happen while ABI_TABLES_MAX exceeds the number of callers,
         * and the failure it would cause -- an app resolving to libc's
         * implementation instead of espix's -- is silent and confusing, so it
         * is worth a line rather than an assert nobody reads. */
        espix_klog(ESPIX_KLOG_ERROR, TAG,
                   "no room for another ABI table; %zu names unpublished",
                   count);
        return;
    }

    /*
     * Checked rather than trusted: two tables answering for one name means the
     * first registered wins and the second is silently dead, which is the
     * hardest kind of ABI bug to see. Registration happens once at boot, so the
     * cost is a handful of strcmp at startup.
     */
    for (size_t i = 0; i < count; i++) {
        for (size_t t = 0; t < s_table_count; t++) {
            for (size_t k = 0; k < s_tables[t].count; k++) {
                if (strcmp(syms[i].name, s_tables[t].syms[k].name) == 0) {
                    espix_klog(ESPIX_KLOG_ERROR, TAG,
                               "'%s' is published twice; the first wins",
                               syms[i].name);
                }
            }
        }
    }

    s_tables[s_table_count].syms  = syms;
    s_tables[s_table_count].count = count;
    s_table_count++;
}

void espix_proc_abi_resolver_register(void)
{
    elf_set_symbol_resolver(espix_symbol_resolver);
}
