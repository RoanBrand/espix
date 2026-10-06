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
 * allocation pass through code espix owns.
 *
 * Why the resolver and not a table
 * --------------------------------
 * elf_loader's own libc table answers for malloc, calloc, realloc and free, and
 * it is searched before any table espix registers -- so a registered entry could
 * never shadow them. The resolver runs first. Same seam sleep, getenv and exit
 * already use; see abi_resolver.c.
 *
 * The arena (R-P1.2)
 * -------------------
 * A global heap cannot give back what one process took, so each process gets
 * regions of its own: a PSRAM block registered as a private multi_heap, made
 * lazily on the first allocation and sized from the request that failed. The
 * membership test for free() is a range comparison, not a record per
 * allocation, which is the whole reason to prefer regions over a side table --
 * see docs/APP-MEMORY.md for the design and the hazards.
 *
 * The one hazard that shaped this file: espix_proc_self() keys on the calling
 * *task*, so an app's pthread finds no slot. A free() from that thread must
 * still find the app's region, or it would hand a region pointer to the global
 * heap -- corruption, not a leak. region_locate() therefore walks every
 * process's regions when there is no slot of our own.
 *
 * Note that free() still frees anything: ESP-IDF's heap is one system with
 * several pools, so a pointer from the internal heap frees correctly through
 * the same call as a PSRAM one. The range test is for *membership*, not a
 * prerequisite for freeing at all.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_heap_caps.h"
#include "multi_heap.h"

#include "espix_display.h"
#include "espix_kernel.h"
#include "espix_proc_priv.h"

#define TAG "abi"

/*
 * Free PSRAM must stay above what the display would need to rebuild itself,
 * because that is the largest thing espix asks for at once and the moment it is
 * asked for is an app's exit: the app owned the screen, and the display comes
 * back when it leaves. Below the floor an app's request to grow is refused; see
 * docs/APP-MEMORY.md, "The ceiling".
 *
 * The canvas and the RFB staging buffer beside it, each w*h*2 bytes. At 1280x800
 * that is the 4 MB the entry above describes; at 320x240 it is 300 KB. It was
 * the 1280x800 figure whatever the canvas was, so a small one reserved 4 MB it
 * would never ask for -- and on an S3 with 4.7 MB free, Doom's 4.35 MB arena
 * request was refused for the sake of 3.7 MB nothing wanted.
 */
#define PSRAM_FLOOR_MIN_BYTES (256u * 1024u)

static size_t psram_floor_bytes(void)
{
    const espix_canvas_t *c = espix_display_canvas();
    size_t                frame = (size_t)ESPIX_DISPLAY_W * ESPIX_DISPLAY_H * 2;

    if (c != NULL) {
        frame = (size_t)espix_canvas_width(c) * (size_t)espix_canvas_height(c) * 2;
    }

    const size_t need = 2 * frame;      /* the canvas and the staging beside it */
    return (need < PSRAM_FLOOR_MIN_BYTES) ? PSRAM_FLOOR_MIN_BYTES : need;
}

/* No region smaller than this, so a first small malloc does not carve a
 * straw-sized heap; and a granule to round the request to. */
#define REGION_MIN_BYTES  (32u * 1024u)
#define REGION_GRANULE    (4u * 1024u)

/* Reported once, because an app pushed into internal RAM is worth knowing about
 * and is not worth a line per allocation. */
static bool s_reported_fallback;

/*
 * Serialises every region operation, including multi_heap_* on a region: a
 * region is registered without a lock of its own, so concurrent allocation from
 * an app's main task and a pthread it made would race inside tlsf. Created once
 * in espix_proc_abi_alloc_register(); NULL disables the arena and drops every
 * app back to the global heap, which is the pre-R-P1.2 behaviour and therefore
 * a safe failure.
 */
static SemaphoreHandle_t s_region_lock;

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

/* ------------------------------------------------------------------ */
/* The arena                                                           */
/* ------------------------------------------------------------------ */

/* Whether this build has PSRAM to make regions from at all. A board without it
 * keeps the global heap and the leak that comes with it; see APP-MEMORY. */
static bool regions_available(void)
{
    return heap_caps_get_total_size(MALLOC_CAP_SPIRAM) != 0;
}

/*
 * How much PSRAM to ask for a request of n bytes.
 *
 * multi_heap cannot serve n from a region of exactly n -- the pool carries a
 * header and tlsf needs its own metadata -- so a little slack stops a region
 * from being created one allocation too small. Proportional above the granular
 * base because a large request has proportionally more blocks.
 */
static size_t region_size_for(size_t n)
{
    size_t want = n + (n / 16) + 1024u;
    if (want < REGION_MIN_BYTES) {
        want = REGION_MIN_BYTES;
    }
    return (want + (REGION_GRANULE - 1)) & ~(size_t)(REGION_GRANULE - 1);
}

/* The region of 'slot' that contains p, or NULL. This is the classification the
 * whole design rests on: inside one of my regions, or not mine. */
static espix_app_region_t *region_owning(espix_proc_slot_t *slot, const void *p)
{
    if (slot == NULL || p == NULL) {
        return NULL;
    }

    const uintptr_t a = (uintptr_t)p;
    for (int i = 0; i < slot->nregions; i++) {
        const uintptr_t b = (uintptr_t)slot->regions[i].base;
        if (a >= b && a < b + slot->regions[i].size) {
            return &slot->regions[i];
        }
    }
    return NULL;
}

/*
 * p's region and, when it can be known, the process it belongs to.
 *
 * With a slot of our own the answer is one test. Without one -- an app's
 * pthread, or a task espix owns -- every process's regions are searched before
 * giving up, because a free() from an app thread of a pointer the app's main
 * task allocated must find it. Forty-eight range checks on this path is the
 * price; see the note at the top of the file.
 */
static espix_app_region_t *region_locate(const void *p, espix_proc_slot_t **out_slot)
{
    espix_proc_slot_t *const self = espix_proc_self();
    if (self != NULL) {
        if (out_slot != NULL) {
            *out_slot = self;
        }
        return region_owning(self, p);
    }

    for (int i = 0; i < ESPIX_PROC_MAX; i++) {
        espix_proc_slot_t *const s = &g_espix_proc_table.slots[i];
        if (s->nregions == 0) {
            continue;
        }
        espix_app_region_t *const r = region_owning(s, p);
        if (r != NULL) {
            if (out_slot != NULL) {
                *out_slot = s;
            }
            return r;
        }
    }
    return NULL;
}

/*
 * Make room in the process's region list for one more entry, growing the index
 * when it is full. The index is espix's bookkeeping rather than the app's, so
 * espix_proc_regions_release() frees it separately from the regions.
 *
 * PSRAM first, because a process with many regions should not be spending
 * scarce internal RAM on their index; internal is the fallback, so the index
 * cannot fail merely because the pool it prefers is full.
 */
static bool region_list_reserve(espix_proc_slot_t *slot)
{
    if (slot->nregions < slot->region_cap) {
        return true;
    }

    const uint32_t want = (slot->region_cap == 0)
                              ? 4u : (uint32_t)slot->region_cap * 2u;
    if (want > 0xffffu) {
        return false;
    }

    const size_t bytes = (size_t)want * sizeof(espix_app_region_t);
    espix_app_region_t *grown = heap_caps_realloc(slot->regions, bytes,
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (grown == NULL) {
        grown = realloc(slot->regions, bytes);
    }
    if (grown == NULL) {
        return false;
    }

    slot->regions    = grown;
    slot->region_cap = (uint16_t)want;
    return true;
}

/*
 * Carve one more region for 'slot', sized for the request that did not fit, and
 * append it. NULL when the board has no PSRAM, when the index cannot be grown,
 * or when taking the memory would drop free PSRAM below the floor -- the last of
 * which is the deliberate budget a caller sees as "out of memory".
 *
 * There is no cap on the number of regions. There was one, and it was the bug
 * this list exists without: a request that will not fit beside an earlier block
 * needs a region of its own, so five 1 MB blocks wanted five regions, and a
 * fixed list of four failed the fifth malloc with 12 MB of PSRAM free.
 */
static espix_app_region_t *region_grow(espix_proc_slot_t *slot, size_t n)
{
    if (!regions_available()) {
        return NULL;
    }

    const size_t want       = region_size_for(n);
    const size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t floor      = psram_floor_bytes();

    if (free_psram < want + floor) {
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "pid %d: refusing a %u KB arena region -- PSRAM free is "
                   "%u KB and the floor is %u KB",
                   (int)slot->info.pid, (unsigned)(want / 1024),
                   (unsigned)(free_psram / 1024),
                   (unsigned)(floor / 1024));
        return NULL;
    }

    /* The index first, so a failure here does not leak the region below. */
    if (!region_list_reserve(slot)) {
        return NULL;
    }

    /* multi_heap_register() aligns the pool it makes from the address it is
     * given, but tlsf wants it aligned to a pointer -- ask for 8 and let the
     * caps allocator refuse only if it cannot. */
    void *const base = heap_caps_aligned_alloc(8, want,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (base == NULL) {
        return NULL;
    }

    multi_heap_handle_t const heap = multi_heap_register(base, want);
    if (heap == NULL) {
        heap_caps_free(base);
        return NULL;
    }

    espix_app_region_t *const r = &slot->regions[slot->nregions++];
    r->heap = heap;
    r->base = base;
    r->size = want;

    /* Named as it happens, because "how many regions did that app take" is the
     * first question any arena surprise raises and there is no other way to
     * see it: a region is not individually visible in ps. One line per region
     * created, and a process normally creates one or two. */
    espix_klog(ESPIX_KLOG_INFO, TAG,
               "pid %d: arena region %u is %u KB, for a %u byte request",
               (int)slot->info.pid, (unsigned)slot->nregions,
               (unsigned)(want / 1024), (unsigned)n);
    return r;
}

/* Allocate n from the process's arena, registering a region if none fits.
 * Caller holds s_region_lock. NULL only when a region was refused at the floor,
 * or when the list's own index could not be grown -- both real memory limits. */
static void *region_alloc(espix_proc_slot_t *slot, size_t n)
{
    if (n == 0) {
        n = 1;
    }

    for (int i = 0; i < slot->nregions; i++) {
        void *const p = multi_heap_malloc(slot->regions[i].heap, n);
        if (p != NULL) {
            return p;
        }
    }

    espix_app_region_t *const r = region_grow(slot, n);
    if (r == NULL) {
        return NULL;
    }
    return multi_heap_malloc(r->heap, n);
}

/* realloc() within the arena. Caller holds s_region_lock. On failure the old
 * block is untouched and still valid, as realloc() promises. */
static void *region_realloc(espix_proc_slot_t *slot, espix_app_region_t *r,
                            void *p, size_t n)
{
    const size_t old = multi_heap_get_allocated_size(r->heap, p);

    void *const same = multi_heap_realloc(r->heap, p, n);
    if (same != NULL) {
        return same;
    }

    /* The owning region cannot grow it. Anywhere else in the same arena is
     * still somewhere a later free() will find, which is what matters. */
    for (int i = 0; i < slot->nregions; i++) {
        if (&slot->regions[i] == r) {
            continue;
        }
        void *const q = multi_heap_malloc(slot->regions[i].heap, n);
        if (q != NULL) {
            memcpy(q, p, (old < n) ? old : n);
            multi_heap_free(r->heap, p);
            return q;
        }
    }

    espix_app_region_t *const nr = region_grow(slot, n);
    if (nr != NULL) {
        void *const q = multi_heap_malloc(nr->heap, n);
        if (q != NULL) {
            memcpy(q, p, (old < n) ? old : n);
            multi_heap_free(r->heap, p);
            return q;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* The names an app calls                                              */
/* ------------------------------------------------------------------ */

void *espix_abi_alloc(size_t n)
{
    espix_proc_slot_t *const slot = espix_proc_self();

    /* Not a process (the loader, a command task), an app thread that has no
     * slot, or a lock that never came up: the global heap, as before. */
    if (slot == NULL || s_region_lock == NULL || !regions_available()) {
        return alloc_psram(n);
    }

    xSemaphoreTake(s_region_lock, portMAX_DELAY);
    void *const p = region_alloc(slot, n);
    xSemaphoreGive(s_region_lock);

    /*
     * NULL is a region refused at the floor -- a deliberate budget, so the app
     * sees the failure -- or the index itself failing to grow, which is PSRAM
     * genuinely exhausted. There is no "the list is full" case: the list has no
     * fixed size, and every allocation that succeeds lives in a region, so every
     * allocation is given back at exit. See docs/APP-MEMORY.md.
     */
    return p;
}

void *espix_abi_calloc(size_t n, size_t size)
{
    if (n != 0 && size > SIZE_MAX / n) {
        return NULL;
    }
    const size_t total = n * size;

    void *const p = espix_abi_alloc(total);
    if (p != NULL) {
        memset(p, 0, total);
    }
    return p;
}

void *espix_abi_realloc(void *p, size_t n)
{
    if (p == NULL) {
        return espix_abi_alloc(n);
    }
    if (n == 0) {
        espix_abi_free(p);
        return NULL;
    }

    espix_proc_slot_t *slot = NULL;
    espix_app_region_t *r = NULL;

    if (s_region_lock != NULL) {
        xSemaphoreTake(s_region_lock, portMAX_DELAY);
        r = region_locate(p, &slot);
        if (r != NULL) {
            void *const q = region_realloc(slot, r, p, n);
            xSemaphoreGive(s_region_lock);
            return q;
        }
        xSemaphoreGive(s_region_lock);
    }

    /* A pointer from before the arena -- the loader's, or one a library made
     * during relocation. Growing it would strand the original in a pool with no
     * owner, so it stays global. */
    void *const q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return (q != NULL) ? q : realloc(p, n);
}

void espix_abi_free(void *p)
{
    if (p == NULL) {
        return;
    }

    espix_proc_slot_t *slot = NULL;

    if (s_region_lock != NULL) {
        xSemaphoreTake(s_region_lock, portMAX_DELAY);
        espix_app_region_t *const r = region_locate(p, &slot);
        if (r != NULL) {
            multi_heap_free(r->heap, p);
            xSemaphoreGive(s_region_lock);
            return;
        }
        xSemaphoreGive(s_region_lock);
    }

    /* Not from any arena: memory from before the regions existed, or memory
     * espix itself handed the app. It has to free -- the rule is "this
     * succeeds", not "this is correct" -- and a foreign pointer is not an
     * error. The first one is still worth a line, because it is how a leak the
     * arena cannot see announces itself. */
    if (slot != NULL && slot->foreign_frees++ == 0) {
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "pid %d: free() of %p outside its arena; freeing it globally",
                   (int)slot->info.pid, p);
    }
    free(p);
}

static char *abi_strdup(const char *s)
{
    if (s == NULL) {
        return NULL;
    }

    const size_t n = strlen(s) + 1;
    char *const  d = espix_abi_alloc(n);
    if (d != NULL) {
        memcpy(d, s, n);
    }
    return d;
}

/* ------------------------------------------------------------------ */
/* Teardown and reporting                                              */
/* ------------------------------------------------------------------ */

void espix_proc_regions_release(espix_proc_slot_t *slot)
{
    if (slot == NULL || (slot->nregions == 0 && slot->regions == NULL)) {
        return;
    }

    if (s_region_lock != NULL) {
        xSemaphoreTake(s_region_lock, portMAX_DELAY);
    }

    size_t released = 0;
    const int count = slot->nregions;

    for (int i = 0; i < slot->nregions; i++) {
        if (slot->regions[i].base != NULL) {
            heap_caps_free(slot->regions[i].base);
            released += slot->regions[i].size;
        }
        slot->regions[i].heap = NULL;
        slot->regions[i].base = NULL;
        slot->regions[i].size = 0;
    }
    /* The index is espix's, not the arena's, so it frees here rather than with
     * the regions above -- and even when no region was ever made, a failed grow
     * may have left one behind. */
    free(slot->regions);
    slot->regions    = NULL;
    slot->nregions   = 0;
    slot->region_cap = 0;
    slot->foreign_frees = 0;

    if (s_region_lock != NULL) {
        xSemaphoreGive(s_region_lock);
    }

    espix_klog(ESPIX_KLOG_DEBUG, TAG,
               "pid %d: %d arena region(s) released, %u KB of PSRAM returned",
               (int)slot->info.pid, count, (unsigned)(released / 1024));
}

size_t espix_proc_heap_used(espix_pid_t pid)
{
    if (s_region_lock == NULL) {
        return 0;
    }

    size_t total = 0;

    xSemaphoreTake(s_region_lock, portMAX_DELAY);
    for (int i = 0; i < ESPIX_PROC_MAX; i++) {
        const espix_proc_slot_t *const s = &g_espix_proc_table.slots[i];
        if (s->nregions == 0 || s->info.pid != pid) {
            continue;
        }
        for (int j = 0; j < s->nregions; j++) {
            multi_heap_info_t info;
            multi_heap_get_info(s->regions[j].heap, &info);
            total += info.total_allocated_bytes;
        }
    }
    xSemaphoreGive(s_region_lock);

    return total;
}

/* ------------------------------------------------------------------ */
/* Publication                                                         */
/* ------------------------------------------------------------------ */

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
    /*
     * Before the table, because the first app allocation may follow the first
     * resolution immediately. A failure here is not fatal: s_region_lock stays
     * NULL and every app keeps the global heap.
     */
    if (s_region_lock == NULL) {
        s_region_lock = xSemaphoreCreateMutex();
    }

    espix_abi_resolver_add(s_alloc_syms,
                           sizeof(s_alloc_syms) / sizeof(s_alloc_syms[0]));

    espix_klog(ESPIX_KLOG_DEBUG, TAG,
               "the allocator published to apps (per-process arena: %s)",
               (s_region_lock != NULL) ? "on" : "off");
}
