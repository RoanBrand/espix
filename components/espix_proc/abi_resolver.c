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
 * Why a resolver rather than another symbol table: the resolver runs before
 * every table -- the loader's own and espix's registered ones alike -- which is
 * the loader's documented hook for "symbol interception and hooking". That
 * ordering is what lets one subsystem own a name another table also lists, and
 * it is why tools/check-abi.py calls a table entry whose name an earlier table
 * answers for dead while a resolver entry is exactly the point.
 */

#include <string.h>

#include "esp_elf.h"
#include "private/elf_symbol.h"

#include "espix_kernel.h"
#include "espix_proc_priv.h"

#if CONFIG_ESPIX_PROC_ABI_WATCHPOINT
#include "esp_cpu.h"
#include "esp_ipc.h"
#endif

#define TAG "abi"

/* Small and fixed: one entry per subsystem that publishes overrides, which is
 * two today and will not be many. A miss costs a walk of both. */
#define ABI_TABLES_MAX 8

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

#if CONFIG_ESPIX_PROC_ABI_WATCHPOINT
/*
 * Watch this file's own table state for writes from anywhere else.
 *
 * It was found damaged on a running board: getenv() stopped resolving while
 * getpid() -- published by the table registered one line earlier -- kept
 * working, which is the signature of the resolver searching one table instead
 * of two. Nothing here ever rewrites s_table_count after boot, so whatever did
 * it was outside this file, and the board stayed broken until it was rebooted.
 *
 * The first word past the process table is now the table's own guard member
 * rather than s_table_count, which only sat there by linker accident -- and on
 * RISC-V does not sit there at all. That is the leading theory about the reach
 * and not a finding about the writer: the watchpoint exists to name the writer
 * rather than to confirm a guess, and it catches a wild store from anywhere.
 *
 * Armed after registration, so the legitimate setup writes do not trip it.
 */
static void abi_watch_arm_this_core(void *unused)
{
    (void)unused;

    /* Both 4 bytes and naturally aligned, which the debug registers require.
     * Not one wide watchpoint over the whole region: the nearest power-of-two
     * window that would span it reaches back into g_espix_procs's last slot,
     * and ordinary writes there would fire it continuously. */
    /*
     * The word just past the process table -- not s_table_count.
     *
     * s_table_count was the word after that table when this was written, and on
     * Xtensa it still is. On RISC-V it lands in .sbss, nowhere near, so a
     * watchpoint aimed at it would never fire for the overrun it exists to
     * catch. The guard is a member of the table's own struct, so this address is
     * right on every target and stays right when .bss moves again.
     */
    /* The cast is the volatile: the debug register takes a plain address, and
     * nothing here dereferences it. */
    esp_cpu_set_watchpoint(0, (void *)&g_espix_proc_table.guard,
                           sizeof(g_espix_proc_table.guard),
                           ESP_CPU_WATCHPOINT_STORE);
    esp_cpu_set_watchpoint(1, &s_tables[1], sizeof(s_tables[1].syms),
                           ESP_CPU_WATCHPOINT_STORE);
}

/*
 * Trip the watchpoint on purpose, for `crash abi`.
 *
 * Stores the value that is already there, so the only thing it changes is that
 * the watchpoint fires -- the ABI state is left correct either way. A guard
 * nobody has watched fire is not known to work, and this one guards a
 * corruption that took an afternoon to characterise; being able to ask it to
 * prove itself is worth the dozen lines.
 */
void espix_proc_abi_watch_selftest(void)
{
    *(volatile size_t *)&s_table_count = s_table_count;
}

void espix_proc_abi_watch_arm(void)
{
    /* Watchpoint registers are per-CPU, so arming only on the core that runs
     * init would miss everything the other core does -- which is half the
     * system, and app tasks float between them. */
    abi_watch_arm_this_core(NULL);

#if !CONFIG_FREERTOS_UNICORE
    const uint32_t other = (xPortGetCoreID() == 0) ? 1 : 0;
    if (esp_ipc_call_blocking(other, abi_watch_arm_this_core, NULL) != ESP_OK) {
        espix_klog(ESPIX_KLOG_ERROR, TAG,
                   "could not arm the ABI watchpoint on core %u",
                   (unsigned)other);
    }
#endif
}
#endif /* CONFIG_ESPIX_PROC_ABI_WATCHPOINT */
