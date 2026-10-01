/*
 * The allocator an app gets: espix's, published under libc's names.
 *
 * Why this file has to exist at all
 * ---------------------------------
 * An app's malloc() was the firmware's. That is fine until you want to know
 * what an app allocated -- to give it back when the app ends, to attribute it,
 * or to keep one app's fragmentation out of the pool the next one starts in.
 * None of that is possible while the call goes straight to the firmware's heap
 * and nothing records it.
 *
 * So espix answers for the allocator. This is R-P1.1: it changes *where* the
 * memory comes from (PSRAM first, internal as the fallback) and makes every
 * allocation pass through code espix owns. The per-process regions (R-P1.2)
 * attach here, which is why the bodies below are deliberately one line each and
 * name the seam.
 *
 * Why the resolver and not a table
 * --------------------------------
 * elf_loader's own libc table answers for malloc, calloc, realloc and free, and
 * it is searched before any table espix registers -- so a registered entry could
 * never shadow them. The resolver runs first. Same seam sleep, getenv and exit
 * already use; see abi_resolver.c.
 *
 * Note that free() still frees anything: ESP-IDF's heap is one system with
 * several pools, so a pointer from the internal heap frees correctly through
 * the same call as a PSRAM one. That is what makes the range check in R-P1.2 a
 * test for *membership*, not a prerequisite for freeing at all.
 */

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"

#include "espix_kernel.h"
#include "espix_proc_priv.h"

#define TAG "abi"

/* Reported once, because an app pushed into internal RAM is worth knowing about
 * and is not worth a line per allocation. */
static bool s_reported_fallback;

static void *alloc_psram(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p != NULL) {
        return p;
    }

    /* PSRAM is 13 MB and internal is the pool that decides how many sessions
     * fit, so this is the direction we would rather not go -- but an app that
     * cannot allocate is worse than an app that has borrowed the wrong pool. */
    p = malloc(n);
    if (p != NULL && !s_reported_fallback) {
        s_reported_fallback = true;
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "app allocation served from internal RAM: PSRAM is full");
    }
    return p;
}

void *espix_abi_alloc(size_t n)
{
    return alloc_psram(n);
}

void *espix_abi_calloc(size_t n, size_t size)
{
    void *p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p != NULL) {
        return p;
    }

    p = calloc(n, size);
    if (p != NULL && !s_reported_fallback) {
        s_reported_fallback = true;
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "app allocation served from internal RAM: PSRAM is full");
    }
    return p;
}

void *espix_abi_realloc(void *p, size_t n)
{
    /* Grows into PSRAM from wherever the block was; heap_caps_realloc() moves
     * and copies when the caps differ, which is what we want here. */
    void *q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return (q != NULL) ? q : realloc(p, n);
}

void espix_abi_free(void *p)
{
    /* One call for every pool: espix's heap is one system with several regions,
     * so this is already correct for a pointer the app was given rather than
     * one it allocated. R-P1.2 adds the region test ahead of it. */
    free(p);
}

static char *abi_strdup(const char *s)
{
    if (s == NULL) {
        return NULL;
    }

    const size_t n = strlen(s) + 1;
    char *const  d = alloc_psram(n);
    if (d != NULL) {
        memcpy(d, s, n);
    }
    return d;
}

/*
 * The names, and why each is here
 * -------------------------------
 * malloc/calloc/realloc/free are the four the loader's own table answers for,
 * which is the whole reason this is a resolver. strdup is the one an app
 * reaches for immediately afterwards and which is not published at all without
 * this -- an app using it failed to *load*.
 *
 * The C++ allocator is NOT here. abi_cxx.cpp owns those, because they need a
 * C++ translation unit to be spelled at all; see the note there.
 */
static const abi_sym_t s_alloc_syms[] = {
    ABI_SYM("malloc",  espix_abi_alloc),
    ABI_SYM("calloc",  espix_abi_calloc),
    ABI_SYM("realloc", espix_abi_realloc),
    ABI_SYM("free",    espix_abi_free),
    ABI_SYM("strdup",  abi_strdup),
};

void espix_proc_abi_alloc_register(void)
{
    espix_abi_resolver_add(s_alloc_syms,
                           sizeof(s_alloc_syms) / sizeof(s_alloc_syms[0]));

    espix_klog(ESPIX_KLOG_DEBUG, TAG, "the allocator published to apps");
}
