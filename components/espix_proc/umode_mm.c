/*
 * U-mode address spaces: one Sv32 table per process, frames from PSRAM, and
 * the kernel's access to user memory (docs/USER-MODE.md). A frame is counted:
 * one table maps it, or several share it (an image's text). Every range is a
 * region: zero pages mapped as touched, a file's pages read in as touched, or
 * kernel memory lent whole. A page read before it is written is the one zero
 * frame, read-only; a write to a page its region allows but its PTE does not
 * is copy-on-write. A process's threads share the table, so every change to it
 * is under the table's lock.
 */
#include "sdkconfig.h"

#if CONFIG_ESPIX_UMODE

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "riscv/encoding.h"

#include "espix_fs.h"
#include "espix_proc.h"
#include "espix_umode.h"

#define PG 4096u

/* RSW bits: the frame is the kernel's, lent and never freed with the table; or kept by a page made inaccessible (V clear). */
#define PTE_LENT 0x100u
#define PTE_HELD 0x200u

enum { R_ANON, R_LENT, R_FILE };

typedef struct {
    uint32_t start, end;
    uint32_t flags;     /* PTE_R, PTE_W, PTE_X; none is PROT_NONE */
    uint8_t  kind;
    bool     shared;    /* MAP_SHARED: a file's pages are written back to it */
    int      kfd;       /* R_FILE: a kernel descriptor of its own */
    uint64_t off;       /* R_FILE: the file offset at start */
} region_t;

/* Lent memory and mmap live between the heap's limit and the thread stacks. */
#define MAP_LOW  ESPIX_UM_BRK_MAX

#define TSTACK_SPAN ESPIX_UM_STACK_MAX
#define TSTACK_HIGH (ESPIX_UM_STACK_TOP - ESPIX_UM_STACK_MAX)
#define TSTACK_LOW  (TSTACK_HIGH - ESPIX_UM_TSTACKS * TSTACK_SPAN)
#define MAP_HIGH    TSTACK_LOW

/* A frame unmapped while a kernel call still held a pointer into it; lent memory goes back by release. */
typedef struct dead {
    struct dead *next;
    void        *frame;
    void       (*release)(void *);
} dead_t;

struct espix_um_as {
    uint32_t         *root;
    uint32_t          asid;
    SemaphoreHandle_t lock;
    region_t          heap;             /* [brk_base, the break's page) */
    region_t         *maps;             /* every other region, by start, disjoint */
    int               nmaps, capmaps;
    uint32_t          brk_base, brk;
    uint32_t          tstacks;          /* thread stack spans in use, one bit each */
    uint32_t          pins;             /* kernel calls holding a pointer into a frame; atomic */
    dead_t           *dead;             /* freed when the last pin goes */
    uint32_t         *pages;            /* resident, the zero frame and lent memory aside; the slot's */
    uint32_t          nopages;          /* where pages points until it is told */
    int               users;            /* processes on it: a vfork child borrows its parent's; atomic */
};

static void resident(espix_um_as_t *as, int n)
{
    __atomic_add_fetch(as->pages, (uint32_t)n, __ATOMIC_RELAXED);
}

/* ------------------------------------------------------------------ */
/* Frames                                                              */
/* ------------------------------------------------------------------ */

/*
 * Frames are carved from chunks of the PSRAM heap. A 4 KiB block aligned to 4 KiB leaves the heap's
 * header in the page before it, so frames allocated one by one cost two pages each. A chunk is
 * smaller when the heap has no room for a whole one, and goes back when its last frame does.
 */
#define CHUNK_FRAMES 64
#define CHUNKS       192

typedef struct {
    uint8_t  *base;
    uint64_t  free;                 /* a bit per frame */
    uint64_t  text;                 /* a bit per frame a shared text keeps: never written in place */
    uint8_t   n;                    /* its frames */
    uint8_t   refs[CHUNK_FRAMES];   /* the tables mapping each */
} chunk_t;

static EXT_RAM_BSS_ATTR chunk_t s_chunks[CHUNKS];     /* by base */
static int               s_nchunks;
static SemaphoreHandle_t s_frames_lock;
static uint8_t          *s_zero;        /* mapped by any number of tables, never counted or freed */

static void *frame_alloc(void);

void espix_um_frames_init(void)
{
    s_frames_lock = xSemaphoreCreateMutex();
    s_zero = frame_alloc();
}

static uint64_t chunk_mask(int n)
{
    return (n == 64) ? ~0ull : ((1ull << n) - 1);
}

/* Under the frames lock. */
static chunk_t *chunk_new(void)
{
    if (s_nchunks == CHUNKS) {
        return NULL;
    }
    for (int n = CHUNK_FRAMES; n >= 1; n /= 4) {
        uint8_t *const base = heap_caps_aligned_alloc(PG, (size_t)n * PG, MALLOC_CAP_SPIRAM);
        if (base == NULL) {
            continue;
        }
        int i = s_nchunks++;
        for (; i > 0 && s_chunks[i - 1].base > base; i--) {
            s_chunks[i] = s_chunks[i - 1];
        }
        s_chunks[i] = (chunk_t){ .base = base, .free = chunk_mask(n), .n = (uint8_t)n };
        return &s_chunks[i];
    }
    return NULL;
}

/* The chunk holding f, and its index in the table. Under the frames lock. */
static chunk_t *chunk_of(const uint8_t *f, int *at)
{
    for (int lo = 0, hi = s_nchunks - 1; lo <= hi;) {
        const int mid = (lo + hi) / 2;
        chunk_t *const c = &s_chunks[mid];
        if (f < c->base) {
            hi = mid - 1;
        } else if (f >= c->base + (size_t)c->n * PG) {
            lo = mid + 1;
        } else {
            *at = mid;
            return c;
        }
    }
    return NULL;
}

static void *frame_alloc(void)
{
    uint8_t *f = NULL;
    xSemaphoreTake(s_frames_lock, portMAX_DELAY);
    chunk_t *c = NULL;
    for (int i = 0; i < s_nchunks && c == NULL; i++) {
        if (s_chunks[i].free != 0) {
            c = &s_chunks[i];
        }
    }
    if (c == NULL) {
        c = chunk_new();
    }
    if (c != NULL) {
        const int b = __builtin_ctzll(c->free);
        c->free &= ~(1ull << b);
        c->text &= ~(1ull << b);
        c->refs[b] = 1;
        f = c->base + (size_t)b * PG;
    }
    xSemaphoreGive(s_frames_lock);
    if (f != NULL) {
        memset(f, 0, PG);
    }
    return f;
}

/* Another table maps it; false when its count is full. */
static bool frame_get(void *frame)
{
    int at;
    bool ok = false;
    if (frame == s_zero) {
        return true;
    }
    xSemaphoreTake(s_frames_lock, portMAX_DELAY);
    chunk_t *const c = chunk_of(frame, &at);
    if (c != NULL) {
        uint8_t *const r = &c->refs[((uint8_t *)frame - c->base) / PG];
        ok = *r < UINT8_MAX;
        *r += ok;
    }
    xSemaphoreGive(s_frames_lock);
    return ok;
}

/* How many tables map it, for copy-on-write: the zero frame counts as many, and a shared text's frame as one more. */
static unsigned frame_refs(void *frame)
{
    int at;
    if (frame == s_zero) {
        return UINT8_MAX;
    }
    xSemaphoreTake(s_frames_lock, portMAX_DELAY);
    chunk_t *const c = chunk_of(frame, &at);
    unsigned n = 0;
    if (c != NULL) {
        const int b = ((uint8_t *)frame - c->base) / PG;
        n = c->refs[b] + ((c->text >> b) & 1);
    }
    xSemaphoreGive(s_frames_lock);
    return n;
}

void espix_um_frame_text(void *frame, bool on)
{
    int at;
    xSemaphoreTake(s_frames_lock, portMAX_DELAY);
    chunk_t *const c = chunk_of(frame, &at);
    if (c != NULL) {
        const uint64_t bit = 1ull << (((uint8_t *)frame - c->base) / PG);
        c->text = on ? (c->text | bit) : (c->text & ~bit);
    }
    xSemaphoreGive(s_frames_lock);
}

/* One table fewer maps it; freed with the last. */
static void frame_free(void *frame)
{
    void *empty = NULL;
    int at;
    if (frame == NULL || frame == s_zero) {
        return;
    }
    xSemaphoreTake(s_frames_lock, portMAX_DELAY);
    chunk_t *const c = chunk_of(frame, &at);
    if (c != NULL) {
        const int b = ((uint8_t *)frame - c->base) / PG;
        if (c->refs[b] > 0 && --c->refs[b] == 0) {
            c->free |= 1ull << b;
            if (c->free == chunk_mask(c->n)) {
                empty = c->base;
                memmove(c, c + 1, (size_t)(s_nchunks - at - 1) * sizeof(*c));
                s_nchunks--;
            }
        }
    }
    xSemaphoreGive(s_frames_lock);
    heap_caps_free(empty);
}

uint32_t espix_um_frames_spare(void)
{
    uint32_t n = 0;
    xSemaphoreTake(s_frames_lock, portMAX_DELAY);
    for (int i = 0; i < s_nchunks; i++) {
        n += (uint32_t)__builtin_popcountll(s_chunks[i].free);
    }
    xSemaphoreGive(s_frames_lock);
    return n;
}

void espix_proc_user_mem(uint32_t *pages, uint32_t *shared)
{
    uint32_t used = 0, many = 0;
    if (s_frames_lock != NULL) {
        xSemaphoreTake(s_frames_lock, portMAX_DELAY);
        for (int i = 0; i < s_nchunks; i++) {
            for (int b = 0; b < s_chunks[i].n; b++) {
                used += s_chunks[i].refs[b] > 0;
                many += s_chunks[i].refs[b] > 1;
            }
        }
        used -= (s_zero != NULL);
        xSemaphoreGive(s_frames_lock);
    }
    *pages  = used;
    *shared = many;
}

void espix_um_frame_put(void *frame)
{
    frame_free(frame);
}

/* ------------------------------------------------------------------ */
/* Tables                                                              */
/* ------------------------------------------------------------------ */

static uint32_t pte_of(const void *pa, uint32_t flags)
{
    return (((uint32_t)pa >> 12) << 10) | flags;
}

static void *pte_frame(uint32_t pte)
{
    return (void *)((pte >> 10) << 12);
}

/* A leaf's bits for a region's permissions. */
static uint32_t leaf_flags(uint32_t flags)
{
    return flags | PTE_V | PTE_U | PTE_A | ((flags & PTE_W) ? PTE_D : 0);
}

/* Fetched through the I-cache, which refills from PSRAM and may hold the frame's old lines. */
static void make_exec(void *frame)
{
    esp_cache_msync(frame, PG, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
    esp_cache_msync(frame, PG, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_INST);
}

espix_um_as_t *espix_um_as_new(uint32_t asid)
{
    espix_um_as_t *as = heap_caps_calloc(1, sizeof(*as), MALLOC_CAP_INTERNAL);
    if (as == NULL) {
        return NULL;
    }
    as->root = frame_alloc();
    as->lock = xSemaphoreCreateMutex();
    if (as->root == NULL || as->lock == NULL) {
        frame_free(as->root);
        if (as->lock != NULL) {
            vSemaphoreDelete(as->lock);
        }
        free(as);
        return NULL;
    }
    as->asid  = asid & 0x1ff;
    as->pages = &as->nopages;
    as->users = 1;
    return as;
}

void espix_um_as_count(espix_um_as_t *as, uint32_t *pages)
{
    const uint32_t n = *as->pages;
    as->pages = pages;
    *pages    = n;
}

espix_um_as_t *espix_um_as_get(espix_um_as_t *as)
{
    __atomic_add_fetch(&as->users, 1, __ATOMIC_RELAXED);
    return as;
}

uint32_t espix_um_as_asid(const espix_um_as_t *as)
{
    return as->asid;
}

bool espix_um_as_put(espix_um_as_t *as, uint32_t *pages)
{
    if (as == NULL) {
        return false;
    }
    if (__atomic_sub_fetch(&as->users, 1, __ATOMIC_ACQ_REL) == 0) {
        espix_um_as_free(as);
        return true;
    }
    /* Still borrowed: its count leaves with the process that kept it. */
    if (as->pages == pages) {
        as->nopages = *pages;
        as->pages   = &as->nopages;
        *pages      = 0;
    }
    return false;
}

static void frames_free(dead_t *d)
{
    while (d != NULL) {
        dead_t *const next = d->next;
        if (d->release != NULL) {
            d->release(d->frame);
        } else {
            frame_free(d->frame);
        }
        free(d);
        d = next;
    }
}

void espix_um_as_free(espix_um_as_t *as)
{
    if (as == NULL) {
        return;
    }
    for (int i = 0; i < 1024; i++) {
        if (!(as->root[i] & PTE_V)) {
            continue;
        }
        uint32_t *l2 = pte_frame(as->root[i]);
        for (int j = 0; j < 1024; j++) {
            if ((l2[j] & (PTE_V | PTE_HELD)) && !(l2[j] & PTE_LENT)) {
                frame_free(pte_frame(l2[j]));
            }
        }
        frame_free(l2);
    }
    frame_free(as->root);
    frames_free(as->dead);
    __atomic_store_n(as->pages, 0, __ATOMIC_RELAXED);
    for (int i = 0; i < as->nmaps; i++) {
        if (as->maps[i].kind == R_FILE && as->maps[i].kfd >= 0) {
            close(as->maps[i].kfd);
        }
    }
    heap_caps_free(as->maps);
    vSemaphoreDelete(as->lock);
    free(as);
}

uint32_t espix_um_as_satp(const espix_um_as_t *as)
{
    return SATP32_MODE | (as->asid << 22) | ((uint32_t)as->root >> 12);
}

static uint32_t *leaf(espix_um_as_t *as, uint32_t va, bool create)
{
    uint32_t *l1 = &as->root[va >> 22];
    if (!(*l1 & PTE_V)) {
        if (!create) {
            return NULL;
        }
        void *l2 = frame_alloc();
        if (l2 == NULL) {
            return NULL;
        }
        __atomic_store_n(l1, pte_of(l2, PTE_V), __ATOMIC_RELEASE);     /* for user_pin()'s walk */
    }
    return &((uint32_t *)pte_frame(*l1))[(va >> 12) & 0x3ff];
}

void *espix_um_map_new(espix_um_as_t *as, uint32_t va, uint32_t pte_flags)
{
    uint32_t *pte = leaf(as, va, true);
    if (pte == NULL) {
        return NULL;
    }
    /* A page two segments share takes both segments' permissions. */
    const uint32_t flags = leaf_flags(pte_flags);
    if (*pte & PTE_V) {
        *pte |= flags;
        return pte_frame(*pte);
    }
    void *frame = frame_alloc();
    if (frame == NULL) {
        return NULL;
    }
    __atomic_store_n(pte, pte_of(frame, flags), __ATOMIC_RELEASE);
    resident(as, 1);
    return frame;
}

bool espix_um_map_frame(espix_um_as_t *as, uint32_t va, void *frame, uint32_t pte_flags)
{
    uint32_t *pte = leaf(as, va, true);
    if (pte == NULL || (*pte & PTE_V) || !frame_get(frame)) {
        return false;
    }
    __atomic_store_n(pte, pte_of(frame, leaf_flags(pte_flags)), __ATOMIC_RELEASE);
    resident(as, 1);
    return true;
}

/* ------------------------------------------------------------------ */
/* Regions                                                             */
/* ------------------------------------------------------------------ */

/* The region holding va, or -1. Under the lock. */
static int map_find(const espix_um_as_t *as, uint32_t va)
{
    for (int lo = 0, hi = as->nmaps - 1; lo <= hi;) {
        const int mid = (lo + hi) / 2;
        if (va < as->maps[mid].start) {
            hi = mid - 1;
        } else if (va >= as->maps[mid].end) {
            lo = mid + 1;
        } else {
            return mid;
        }
    }
    return -1;
}

/* The first region ending above va. */
static int map_lower(const espix_um_as_t *as, uint32_t va)
{
    int lo = 0, hi = as->nmaps;
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (as->maps[mid].end <= va) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static bool map_insert(espix_um_as_t *as, const region_t *r)
{
    if (as->nmaps == as->capmaps) {
        const int cap = as->capmaps ? as->capmaps * 2 : 8;
        region_t *m = heap_caps_realloc(as->maps, (size_t)cap * sizeof(*m), MALLOC_CAP_SPIRAM);
        if (m == NULL) {
            return false;
        }
        as->maps    = m;
        as->capmaps = cap;
    }
    const int i = map_lower(as, r->start);
    memmove(&as->maps[i + 1], &as->maps[i], (size_t)(as->nmaps - i) * sizeof(*r));
    as->maps[i] = *r;
    as->nmaps++;
    return true;
}

static void map_remove(espix_um_as_t *as, int i)
{
    if (as->maps[i].kind == R_FILE && as->maps[i].kfd >= 0) {
        close(as->maps[i].kfd);
    }
    memmove(&as->maps[i], &as->maps[i + 1], (size_t)(as->nmaps - i - 1) * sizeof(region_t));
    as->nmaps--;
}

/* Region i in two at at, inside it; a file's second half gets its own descriptor. */
static bool map_split(espix_um_as_t *as, int i, uint32_t at)
{
    region_t hi = as->maps[i];
    hi.start = at;
    if (hi.kind == R_FILE) {
        hi.off += at - as->maps[i].start;
        hi.kfd = espix_abi_dup(as->maps[i].kfd);
        if (hi.kfd < 0) {
            return false;
        }
    }
    const uint32_t end = as->maps[i].end;
    as->maps[i].end = at;
    if (!map_insert(as, &hi)) {
        as->maps[i].end = end;
        if (hi.kind == R_FILE) {
            close(hi.kfd);
        }
        return false;
    }
    return true;
}

/* Regions split so none crosses start or end. */
static bool map_bound(espix_um_as_t *as, uint32_t start, uint32_t end)
{
    int i = map_find(as, start);
    if (i >= 0 && as->maps[i].start < start && !map_split(as, i, start)) {
        return false;
    }
    i = map_find(as, end - 1);
    return !(i >= 0 && as->maps[i].end > end && !map_split(as, i, end));
}

/* The highest free gap of len bytes for a mapping, with a guard page above and below; 0 if none. */
static uint32_t map_gap(const espix_um_as_t *as, uint32_t len)
{
    uint32_t hi = MAP_HIGH - PG;
    for (int i = as->nmaps - 1; i >= -1; i--) {
        uint32_t lo = MAP_LOW;
        if (i >= 0) {
            const region_t *r = &as->maps[i];
            if (r->start >= hi + PG) {
                continue;
            }
            lo = MAX(lo, r->end + PG);
        }
        if (hi >= lo && hi - lo >= len) {
            return hi - len;
        }
        if (i < 0 || as->maps[i].start < MAP_LOW + PG) {
            return 0;
        }
        hi = MIN(hi, as->maps[i].start - PG);
    }
    return 0;
}

bool espix_um_region_add(espix_um_as_t *as, uint32_t start, uint32_t end, uint32_t pte_flags)
{
    region_t r = { (start & ~(PG - 1)), (end + PG - 1) & ~(PG - 1), pte_flags, R_ANON, false, -1, 0 };
    /* A page two segments share is the first one's region. */
    const int i = map_find(as, r.start);
    if (i >= 0) {
        r.start = as->maps[i].end;
    }
    if (r.start >= r.end) {
        return true;
    }
    const int j = map_lower(as, r.start);
    return !(j < as->nmaps && as->maps[j].start < r.end) && map_insert(as, &r);
}

/* What reserves va, copied out: false when nothing does or it is lent. Under the lock. */
static bool reserved(const espix_um_as_t *as, uint32_t va, region_t *out)
{
    if (va >= as->heap.start && va < as->heap.end) {
        *out = as->heap;
        return true;
    }
    const int i = map_find(as, va);
    if (i >= 0) {
        *out = as->maps[i];
        return out->kind != R_LENT;
    }
    if (va >= TSTACK_LOW && va < TSTACK_HIGH) {
        const uint32_t k   = (TSTACK_HIGH - 1 - va) / TSTACK_SPAN;
        const uint32_t top = TSTACK_HIGH - k * TSTACK_SPAN - PG;
        if ((as->tstacks & (1u << k)) && va < top && va >= top - TSTACK_SPAN + 2 * PG) {
            *out = (region_t){ .flags = PTE_R | PTE_W, .kind = R_ANON, .kfd = -1 };
            return true;
        }
    }
    return false;
}

void espix_um_brk_init(espix_um_as_t *as, uint32_t base)
{
    as->brk_base = as->brk = (base + PG - 1) & ~(PG - 1);
    as->heap = (region_t){ as->brk_base, as->brk_base, PTE_R | PTE_W, R_ANON, false, -1, 0 };
}

/* Unmaps [start, end) onto *dead. Under the lock; unmapped() finishes the job. */
static void unmap_range(espix_um_as_t *as, uint32_t start, uint32_t end, dead_t **dead)
{
    for (uint32_t va = start; va < end; va += PG) {
        uint32_t *pte = leaf(as, va, false);
        if (pte == NULL) {
            va = (va | (0x400000u - 1)) + 1 - PG;   /* no table for this 4 MB */
            continue;
        }
        if (!(*pte & (PTE_V | PTE_HELD)) || (*pte & PTE_LENT)) {
            continue;
        }
        dead_t *const d = malloc(sizeof(*d));
        if (d == NULL) {
            continue;       /* left mapped: a leak, never a frame in use freed */
        }
        *d = (dead_t){ *dead, pte_frame(*pte), NULL };
        *dead = d;
        resident(as, (d->frame == s_zero) ? 0 : -1);
        __atomic_store_n(pte, 0, __ATOMIC_SEQ_CST);
        asm volatile ("sfence.vma %0, %1" :: "r"(va), "r"(as->asid) : "memory");
    }
}

/*
 * The frames of an unmap: no core translates them any more; freed now, or when the last pin goes.
 * The PTEs were cleared before pins is read, and a pinner counts itself before reading a PTE:
 * either the pinner saw the clear, or this sees its pin.
 */
static void unmapped(espix_um_as_t *as, dead_t *dead)
{
    if (dead == NULL) {
        return;
    }
    espix_um_asid_shoot(as->asid);
    if (__atomic_load_n(&as->pins, __ATOMIC_SEQ_CST) == 0) {
        frames_free(dead);
        return;
    }
    dead_t *tail = dead;
    while (tail->next != NULL) {
        tail = tail->next;
    }
    tail->next = as->dead;
    __atomic_store_n(&as->dead, dead, __ATOMIC_SEQ_CST);
    /* The last pin let go after the check above: it may have missed the list. */
    if (__atomic_load_n(&as->pins, __ATOMIC_SEQ_CST) == 0) {
        dead     = as->dead;
        as->dead = NULL;
        frames_free(dead);
    }
}

uint32_t espix_um_brk(espix_um_as_t *as, uint32_t want)
{
    xSemaphoreTake(as->lock, portMAX_DELAY);
    if (want >= as->brk_base && want <= ESPIX_UM_BRK_MAX) {
        const uint32_t old_end = as->heap.end;
        const uint32_t new_end = (want + PG - 1) & ~(PG - 1);
        dead_t *dead = NULL;
        unmap_range(as, new_end, old_end, &dead);
        unmapped(as, dead);
        as->heap.end = new_end;
        as->brk = want;
    }
    const uint32_t brk = as->brk;
    xSemaphoreGive(as->lock);
    return brk;
}

uint32_t espix_um_tstack_take(espix_um_as_t *as)
{
    uint32_t top = 0;
    xSemaphoreTake(as->lock, portMAX_DELAY);
    for (uint32_t k = 0; k < ESPIX_UM_TSTACKS; k++) {
        if (!(as->tstacks & (1u << k))) {
            as->tstacks |= 1u << k;
            top = TSTACK_HIGH - k * TSTACK_SPAN - PG;
            break;
        }
    }
    xSemaphoreGive(as->lock);
    return top;
}

void espix_um_tstack_give(espix_um_as_t *as, uint32_t top)
{
    const uint32_t k = (TSTACK_HIGH - PG - top) / TSTACK_SPAN;
    dead_t *dead = NULL;
    xSemaphoreTake(as->lock, portMAX_DELAY);
    as->tstacks &= ~(1u << k);
    unmap_range(as, top - TSTACK_SPAN + 2 * PG, top, &dead);
    unmapped(as, dead);
    xSemaphoreGive(as->lock);
}

/* ------------------------------------------------------------------ */
/* Lent memory                                                         */
/* ------------------------------------------------------------------ */

uint32_t espix_um_map_lent(espix_um_as_t *as, void *k, size_t len, uint32_t pte_flags)
{
    len = (len + PG - 1) & ~(PG - 1);
    if (((uint32_t)k & (PG - 1)) != 0 || len == 0) {
        return 0;
    }
    const uint32_t flags = leaf_flags(pte_flags) | PTE_LENT;
    xSemaphoreTake(as->lock, portMAX_DELAY);
    const uint32_t va = map_gap(as, len);
    const region_t r = { va, va + len, pte_flags, R_LENT, false, -1, 0 };
    uint32_t off = 0;
    for (; va != 0 && off < len; off += PG) {
        uint32_t *pte = leaf(as, va + off, true);
        if (pte == NULL) {
            break;
        }
        __atomic_store_n(pte, pte_of((uint8_t *)k + off, flags), __ATOMIC_RELEASE);
    }
    if (va == 0 || off < len || !map_insert(as, &r)) {
        for (uint32_t o = 0; va != 0 && o < off; o += PG) {
            __atomic_store_n(leaf(as, va + o, false), 0, __ATOMIC_SEQ_CST);
        }
        xSemaphoreGive(as->lock);
        return 0;
    }
    xSemaphoreGive(as->lock);
    asm volatile ("sfence.vma x0, %0" :: "r"(as->asid) : "memory");
    return va;
}

bool espix_um_unmap_lent(espix_um_as_t *as, uint32_t va, void (*release)(void *), void *obj)
{
    dead_t *const d = malloc(sizeof(*d));
    xSemaphoreTake(as->lock, portMAX_DELAY);
    const int i = map_find(as, va);
    if (i < 0 || as->maps[i].kind != R_LENT || as->maps[i].start != va || d == NULL) {
        xSemaphoreGive(as->lock);
        free(d);
        return false;
    }
    for (uint32_t p = va; p < as->maps[i].end; p += PG) {
        __atomic_store_n(leaf(as, p, false), 0, __ATOMIC_SEQ_CST);
        asm volatile ("sfence.vma %0, %1" :: "r"(p), "r"(as->asid) : "memory");
    }
    map_remove(as, i);
    *d = (dead_t){ NULL, obj, release };
    unmapped(as, d);
    xSemaphoreGive(as->lock);
    return true;
}

/* ------------------------------------------------------------------ */
/* mmap                                                                */
/* ------------------------------------------------------------------ */

/*
 * A shared file region's mapped pages in [start, end) written back, up to the file's end, and synced.
 * Synced first too: littlefs's fstat reports the size last committed. Under the lock.
 */
static void writeback(espix_um_as_t *as, const region_t *r, uint32_t start, uint32_t end)
{
    struct stat st;
    if (r->kind != R_FILE || !r->shared || !(r->flags & PTE_W) || fsync(r->kfd) != 0 || fstat(r->kfd, &st) != 0) {
        return;
    }
    for (uint32_t va = MAX(start, r->start); va < MIN(end, r->end); va += PG) {
        const uint64_t pos = r->off + (va - r->start);
        if (pos >= (uint64_t)st.st_size) {
            break;
        }
        const uint32_t *pte = leaf(as, va, false);
        if (pte != NULL && (*pte & (PTE_V | PTE_HELD))) {
            (void)pwrite(r->kfd, pte_frame(*pte), (size_t)MIN((uint64_t)PG, (uint64_t)st.st_size - pos), (off_t)pos);
        }
    }
    (void)fsync(r->kfd);
}

/* A frame for va, filled as its region says, then mapped. Under the lock. */
static uint8_t *fill(espix_um_as_t *as, uint32_t va, const region_t *r)
{
    uint32_t *pte = leaf(as, va, true);
    uint8_t *frame = (pte != NULL) ? frame_alloc() : NULL;
    if (frame == NULL) {
        return NULL;
    }
    if (r->kind == R_FILE) {
        (void)pread(r->kfd, frame, PG, (off_t)(r->off + (va - r->start)));     /* past the end stays zero */
    }
    if (r->flags & PTE_X) {
        make_exec(frame);
    }
    __atomic_store_n(pte, pte_of(frame, leaf_flags(r->flags)), __ATOMIC_RELEASE);
    resident(as, 1);
    return frame;
}

/* The zero frame at va, read-only whatever its region allows. Under the lock. */
static uint8_t *map_zero(espix_um_as_t *as, uint32_t va, const region_t *r)
{
    uint32_t *pte = leaf(as, va, true);
    if (pte == NULL) {
        return NULL;
    }
    __atomic_store_n(pte, pte_of(s_zero, leaf_flags(r->flags & ~PTE_W)), __ATOMIC_RELEASE);
    return s_zero;
}

/* A write the region allows to a page its PTE does not: the writer's own copy, unless it is the only mapper. */
static uint8_t *cow(espix_um_as_t *as, uint32_t va, uint32_t *pte, const region_t *r)
{
    uint8_t *const old = pte_frame(*pte);
    if (frame_refs(old) == 1 || (r->shared && old != s_zero)) {
        __atomic_store_n(pte, *pte | PTE_W | PTE_D, __ATOMIC_SEQ_CST);
        return old;
    }
    uint8_t *const copy = frame_alloc();
    dead_t *const d = (old != s_zero) ? malloc(sizeof(*d)) : NULL;
    if (copy == NULL || (old != s_zero && d == NULL)) {
        frame_free(copy);
        free(d);
        return NULL;
    }
    if (old != s_zero) {
        memcpy(copy, old, PG);
    } else {
        resident(as, 1);
    }
    if (r->flags & PTE_X) {
        make_exec(copy);
    }
    __atomic_store_n(pte, pte_of(copy, leaf_flags(r->flags)), __ATOMIC_SEQ_CST);
    asm volatile ("sfence.vma %0, %1" :: "r"(va), "r"(as->asid) : "memory");
    /* Another core may still read the old frame at va: its translation goes first. */
    if (d != NULL) {
        *d = (dead_t){ NULL, old, NULL };
        unmapped(as, d);
    } else {
        espix_um_asid_shoot(as->asid);
    }
    return copy;
}

/* The page at va, mapped now if something reserves it, copied now if written and shared. Under the lock. */
static uint8_t *page_of(espix_um_as_t *as, uint32_t va, uint32_t need)
{
    uint32_t *pte = leaf(as, va, false);
    region_t r;
    if (pte == NULL || !(*pte & PTE_V)) {
        if ((pte != NULL && (*pte & PTE_HELD)) || !reserved(as, va, &r) || r.flags == 0 ||
            (r.flags & need) != need) {
            return NULL;
        }
        /* A shared page is the same frame in every process mapping it, so never the zero frame. */
        uint8_t *frame = (r.kind == R_ANON && !r.shared && !(need & PTE_W)) ? map_zero(as, va & ~(PG - 1), &r)
                                                                          : fill(as, va & ~(PG - 1), &r);
        if (frame == NULL) {
            return NULL;
        }
        asm volatile ("sfence.vma %0, %1" :: "r"(va), "r"(as->asid) : "memory");
        return frame + (va & (PG - 1));
    }
    if (!(*pte & PTE_U)) {
        return NULL;
    }
    if ((need & PTE_W) && !(*pte & PTE_W)) {
        uint8_t *frame = (reserved(as, va, &r) && (r.flags & PTE_W)) ? cow(as, va & ~(PG - 1), pte, &r) : NULL;
        return (frame != NULL) ? frame + (va & (PG - 1)) : NULL;
    }
    if ((*pte & need) != need) {
        return NULL;
    }
    return (uint8_t *)pte_frame(*pte) + (va & (PG - 1));
}

/* Lent memory, the heap and the thread stacks are not the app's to unmap or change. */
static bool map_ours(const espix_um_as_t *as, uint32_t start, uint32_t end)
{
    if (start < as->heap.end && end > as->heap.start) {
        return false;
    }
    if (start < TSTACK_HIGH && end > TSTACK_LOW) {
        return false;
    }
    for (int i = map_lower(as, start); i < as->nmaps && as->maps[i].start < end; i++) {
        if (as->maps[i].kind == R_LENT) {
            return false;
        }
    }
    return true;
}

/* [start, end) out of every region, written back and unmapped onto *dead. Under the lock. */
static int32_t map_cut(espix_um_as_t *as, uint32_t start, uint32_t end, dead_t **dead)
{
    if (!map_ours(as, start, end)) {
        return -EINVAL;
    }
    if (!map_bound(as, start, end)) {
        return -ENOMEM;
    }
    for (int i = map_lower(as, start); i < as->nmaps && as->maps[i].start < end;) {
        writeback(as, &as->maps[i], start, end);
        map_remove(as, i);
    }
    unmap_range(as, start, end, dead);
    return 0;
}

int32_t espix_um_mmap(espix_um_as_t *as, uint32_t addr, uint32_t len, uint32_t pte_flags, uint32_t how,
                      int kfd, uint64_t off)
{
    len = (len + PG - 1) & ~(PG - 1);
    const bool fixed = how & (ESPIX_MAP_FIXED | ESPIX_MAP_FIXED_NOREPLACE);
    if (len == 0 || (fixed && (addr < MAP_LOW || addr > MAP_HIGH || len > MAP_HIGH - addr))) {
        return -EINVAL;
    }
    dead_t *dead = NULL;
    int32_t r = 0;
    xSemaphoreTake(as->lock, portMAX_DELAY);
    uint32_t va = addr;
    if (!fixed) {
        va = map_gap(as, len);
        r  = (va == 0) ? -ENOMEM : 0;
    } else if (how & ESPIX_MAP_FIXED_NOREPLACE) {
        const int i = map_lower(as, va);
        r = (i < as->nmaps && as->maps[i].start < va + len) ? -EEXIST : 0;
    } else {
        r = map_cut(as, va, va + len, &dead);
    }
    const region_t reg = { va, va + len, pte_flags, (kfd >= 0) ? R_FILE : R_ANON,
                           (how & ESPIX_MAP_SHARED) != 0, kfd, off };
    if (r == 0 && !map_insert(as, &reg)) {
        r = -ENOMEM;
    }
    unmapped(as, dead);
    if (r == 0 && (how & ESPIX_MAP_POPULATE) && pte_flags != 0) {
        for (uint32_t p = va; p < va + len && page_of(as, p, pte_flags & PTE_W) != NULL; p += PG) {
        }
    }
    xSemaphoreGive(as->lock);
    return (r == 0) ? (int32_t)va : r;
}

int32_t espix_um_munmap(espix_um_as_t *as, uint32_t start, uint32_t len)
{
    const uint32_t end = start + ((len + PG - 1) & ~(PG - 1));
    if (len == 0 || end <= start || end > ESPIX_UM_STACK_TOP) {
        return -EINVAL;
    }
    dead_t *dead = NULL;
    xSemaphoreTake(as->lock, portMAX_DELAY);
    const int32_t r = map_cut(as, start, end, &dead);
    unmapped(as, dead);
    xSemaphoreGive(as->lock);
    return r;
}

/* Whether regions cover [start, end) with no gap. Under the lock. */
static bool map_covered(const espix_um_as_t *as, uint32_t start, uint32_t end)
{
    uint32_t at = start;
    for (int i = map_lower(as, start); i < as->nmaps && at < end && as->maps[i].start <= at; i++) {
        at = as->maps[i].end;
    }
    return at >= end;
}

int32_t espix_um_mprotect(espix_um_as_t *as, uint32_t start, uint32_t len, uint32_t pte_flags)
{
    const uint32_t end = start + ((len + PG - 1) & ~(PG - 1));
    if (len == 0 || end <= start || end > ESPIX_UM_STACK_TOP) {
        return -EINVAL;
    }
    dead_t *dead = NULL;
    int32_t r = 0;
    xSemaphoreTake(as->lock, portMAX_DELAY);
    if (!map_covered(as, start, end)) {
        r = -ENOMEM;
    } else if (!map_ours(as, start, end)) {
        r = -EACCES;
    } else if (!map_bound(as, start, end)) {
        r = -ENOMEM;
    }
    for (int i = map_lower(as, start); r == 0 && i < as->nmaps && as->maps[i].start < end; i++) {
        as->maps[i].flags = pte_flags;
    }
    for (uint32_t va = start; r == 0 && va < end; va += PG) {
        uint32_t *pte = leaf(as, va, false);
        if (pte == NULL || !(*pte & (PTE_V | PTE_HELD))) {
            continue;
        }
        uint8_t *const frame = pte_frame(*pte);
        if ((pte_flags & PTE_X) && !(*pte & PTE_X)) {
            make_exec(frame);
        }
        /* A frame other tables map stays read-only here, to be copied when written; a shared mapping's is shared. */
        const int m = map_find(as, va);
        const bool shared = m >= 0 && as->maps[m].shared;
        const uint32_t f = (frame_refs(frame) > 1 && !shared) ? (pte_flags & ~PTE_W) : pte_flags;
        __atomic_store_n(pte, f ? pte_of(frame, leaf_flags(f)) : (pte_of(frame, 0) | PTE_HELD), __ATOMIC_SEQ_CST);
    }
    espix_um_asid_shoot(as->asid);
    asm volatile ("sfence.vma x0, %0" :: "r"(as->asid) : "memory");
    unmapped(as, dead);
    xSemaphoreGive(as->lock);
    return r;
}

int32_t espix_um_madvise(espix_um_as_t *as, uint32_t start, uint32_t len, bool drop)
{
    const uint32_t end = start + ((len + PG - 1) & ~(PG - 1));
    if (end <= start || end > ESPIX_UM_STACK_TOP) {
        return -EINVAL;
    }
    dead_t *dead = NULL;
    int32_t r = 0;
    xSemaphoreTake(as->lock, portMAX_DELAY);
    if (!map_covered(as, start, end)) {
        r = -ENOMEM;
    } else if (drop && !map_ours(as, start, end)) {
        r = -EINVAL;
    }
    for (int i = map_lower(as, start); r == 0 && drop && i < as->nmaps && as->maps[i].start < end; i++) {
        const region_t *m = &as->maps[i];
        if (m->shared && m->kind == R_ANON) {
            continue;       /* its pages are its contents, as shared memory's are on Linux */
        }
        writeback(as, m, start, end);
        unmap_range(as, MAX(start, m->start), MIN(end, m->end), &dead);
    }
    unmapped(as, dead);
    xSemaphoreGive(as->lock);
    return r;
}

int32_t espix_um_msync(espix_um_as_t *as, uint32_t start, uint32_t len)
{
    const uint32_t end = start + ((len + PG - 1) & ~(PG - 1));
    if (end <= start || end > ESPIX_UM_STACK_TOP) {
        return -EINVAL;
    }
    xSemaphoreTake(as->lock, portMAX_DELAY);
    const bool covered = map_covered(as, start, end);
    for (int i = map_lower(as, start); covered && i < as->nmaps && as->maps[i].start < end; i++) {
        writeback(as, &as->maps[i], start, end);
    }
    xSemaphoreGive(as->lock);
    return covered ? 0 : -ENOMEM;
}

int32_t espix_um_mincore(espix_um_as_t *as, uint32_t start, uint32_t npages, uint8_t *vec)
{
    int32_t r = 0;
    region_t reg;
    xSemaphoreTake(as->lock, portMAX_DELAY);
    for (uint32_t i = 0; i < npages; i++) {
        const uint32_t va = start + i * PG;
        const uint32_t *pte = leaf(as, va, false);
        const bool present = pte != NULL && (*pte & (PTE_V | PTE_HELD));
        if (!present && !reserved(as, va, &reg)) {
            r = -ENOMEM;
            break;
        }
        vec[i] = present;
    }
    xSemaphoreGive(as->lock);
    return r;
}

void espix_um_unmap_files(espix_um_as_t *as)
{
    xSemaphoreTake(as->lock, portMAX_DELAY);
    for (int i = 0; i < as->nmaps; i++) {
        region_t *m = &as->maps[i];
        if (m->kind == R_FILE && m->kfd >= 0) {
            writeback(as, m, m->start, m->end);
            close(m->kfd);
            m->kfd = -1;
        }
    }
    xSemaphoreGive(as->lock);
}

/* ------------------------------------------------------------------ */
/* fork                                                                */
/* ------------------------------------------------------------------ */

/* A copy of region r for the child: a file's with a descriptor of its own. */
static bool region_copy(espix_um_as_t *as, const region_t *r)
{
    region_t c = *r;
    if (c.kind == R_FILE && c.kfd >= 0) {
        c.kfd = espix_abi_dup(r->kfd);
        if (c.kfd < 0) {
            return false;
        }
        espix_fs_fd_disown(c.kfd);
    }
    if (!map_insert(as, &c)) {
        if (c.kind == R_FILE && c.kfd >= 0) {
            close(c.kfd);
        }
        return false;
    }
    return true;
}

espix_um_as_t *espix_um_as_fork(espix_um_as_t *src, uint32_t asid)
{
    espix_um_as_t *as = espix_um_as_new(asid);
    if (as == NULL) {
        return NULL;
    }
    bool ok = true;
    xSemaphoreTake(src->lock, portMAX_DELAY);
    as->heap     = src->heap;
    as->brk_base = src->brk_base;
    as->brk      = src->brk;
    as->tstacks  = src->tstacks;
    for (int i = 0; ok && i < src->nmaps; i++) {
        const region_t *m = &src->maps[i];
        if (m->kind == R_LENT) {
            continue;       /* a surface stays its holder's */
        }
        ok = region_copy(as, m);
        /* Every page of a shared mapping is a frame of its own before both map it. */
        for (uint32_t va = m->start; ok && m->shared && m->flags != 0 && va < m->end; va += PG) {
            const uint32_t *pte = leaf(src, va, false);
            if (pte == NULL || (!(*pte & (PTE_V | PTE_HELD))) || ((*pte & PTE_V) && pte_frame(*pte) == s_zero)) {
                ok = page_of(src, va, (m->flags & PTE_W) ? PTE_W : PTE_R) != NULL;
            }
        }
    }
    uint32_t n = 0;
    for (uint32_t i = 0; ok && i < 1024; i++) {
        if (!(src->root[i] & PTE_V)) {
            continue;
        }
        uint32_t *const l2 = pte_frame(src->root[i]);
        for (uint32_t j = 0; ok && j < 1024; j++) {
            uint32_t pte = l2[j];
            if (!(pte & (PTE_V | PTE_HELD)) || (pte & PTE_LENT)) {
                continue;
            }
            const uint32_t va = (i << 22) | (j << 12);
            const int m = map_find(src, va);
            if (m >= 0 && src->maps[m].kind == R_LENT) {
                continue;
            }
            /* Private and writable: read-only in both now, each writer's copy made when it writes. */
            if (!(m >= 0 && src->maps[m].shared) && (pte & PTE_W)) {
                pte &= ~PTE_W;
                __atomic_store_n(&l2[j], pte, __ATOMIC_SEQ_CST);
            }
            uint8_t *frame = pte_frame(pte);
            uint32_t cpte = pte;
            if (!frame_get(frame)) {
                /* Counted as far as it goes: this one is the child's own copy. */
                uint8_t *const copy = frame_alloc();
                ok = copy != NULL;
                if (ok) {
                    memcpy(copy, frame, PG);
                    if (pte & PTE_X) {
                        make_exec(copy);
                    }
                    cpte  = pte_of(copy, pte & 0x3ffu);
                    frame = copy;
                }
            }
            uint32_t *const c = ok ? leaf(as, va, true) : NULL;
            if (c == NULL) {
                if (ok) {
                    frame_free(frame);
                }
                ok = false;
                break;
            }
            __atomic_store_n(c, cpte, __ATOMIC_RELEASE);
            n += frame != s_zero;
        }
    }
    *as->pages = n;
    /* No core may write through a translation that predates the copy. */
    asm volatile ("sfence.vma x0, %0" :: "r"(src->asid) : "memory");
    espix_um_asid_shoot(src->asid);
    xSemaphoreGive(src->lock);
    if (!ok) {
        espix_um_as_free(as);
        return NULL;
    }
    return as;
}

/* ------------------------------------------------------------------ */
/* User memory                                                         */
/* ------------------------------------------------------------------ */

void *espix_um_user_pin(espix_um_as_t *as, uint32_t va, uint32_t need)
{
    if (as == NULL) {
        return espix_posix_pin(va);
    }
    /* A page mapped already needs no lock: unmapped() honours the pin. */
    __atomic_add_fetch(&as->pins, 1, __ATOMIC_SEQ_CST);
    const uint32_t l1 = __atomic_load_n(&as->root[va >> 22], __ATOMIC_ACQUIRE);
    if (l1 & PTE_V) {
        const uint32_t pte = __atomic_load_n(&((uint32_t *)pte_frame(l1))[(va >> 12) & 0x3ff], __ATOMIC_SEQ_CST);
        if ((pte & (PTE_V | PTE_U | need)) == (PTE_V | PTE_U | need)) {
            return (uint8_t *)pte_frame(pte) + (va & (PG - 1));
        }
    }
    xSemaphoreTake(as->lock, portMAX_DELAY);
    uint8_t *const k = page_of(as, va, need);
    xSemaphoreGive(as->lock);
    if (k == NULL) {
        espix_um_user_unpin(as);
    }
    return k;
}

void espix_um_user_unpin(espix_um_as_t *as)
{
    if (as == NULL) {
        return;
    }
    if (__atomic_sub_fetch(&as->pins, 1, __ATOMIC_SEQ_CST) != 0 ||
        __atomic_load_n(&as->dead, __ATOMIC_SEQ_CST) == NULL) {
        return;
    }
    dead_t *dead = NULL;
    xSemaphoreTake(as->lock, portMAX_DELAY);
    if (__atomic_load_n(&as->pins, __ATOMIC_SEQ_CST) == 0) {
        dead     = as->dead;
        as->dead = NULL;
    }
    xSemaphoreGive(as->lock);
    frames_free(dead);
}

bool espix_um_fault(espix_um_as_t *as, uint32_t va, uint32_t need)
{
    xSemaphoreTake(as->lock, portMAX_DELAY);
    /* Mapped already, by another thread since the fault: this core's TLB is just behind. */
    const bool ok = page_of(as, va, need) != NULL;
    xSemaphoreGive(as->lock);
    if (ok) {
        asm volatile ("sfence.vma %0, %1" :: "r"(va), "r"(as->asid) : "memory");
    }
    return ok;
}

int espix_um_copyin(espix_um_as_t *as, void *dst, uint32_t uva, size_t n)
{
    if (as == NULL) {
        return espix_posix_copyin(dst, uva, n);
    }
    uint8_t *d = dst;
    while (n > 0) {
        const uint8_t *k = espix_um_user_pin(as, uva, PTE_R);
        if (k == NULL) {
            return -EFAULT;
        }
        const size_t c = MIN(n, PG - (uva & (PG - 1)));
        memcpy(d, k, c);
        espix_um_user_unpin(as);
        uva += c;
        d   += c;
        n   -= c;
    }
    return 0;
}

int espix_um_copyout(espix_um_as_t *as, uint32_t uva, const void *src, size_t n)
{
    if (as == NULL) {
        return espix_posix_copyout(uva, src, n);
    }
    const uint8_t *s = src;
    while (n > 0) {
        uint8_t *k = espix_um_user_pin(as, uva, PTE_W);
        if (k == NULL) {
            return -EFAULT;
        }
        const size_t c = MIN(n, PG - (uva & (PG - 1)));
        memcpy(k, s, c);
        espix_um_user_unpin(as);
        uva += c;
        s   += c;
        n   -= c;
    }
    return 0;
}

int espix_um_strin(espix_um_as_t *as, char *dst, uint32_t uva, size_t size)
{
    if (as == NULL) {
        return espix_posix_strin(dst, uva, size);
    }
    size_t len = 0;
    while (len < size) {
        const char *k = espix_um_user_pin(as, uva, PTE_R);
        if (k == NULL) {
            return -EFAULT;
        }
        const size_t c = MIN(size - len, PG - (uva & (PG - 1)));
        for (size_t i = 0; i < c; i++) {
            if ((dst[len++] = k[i]) == '\0') {
                espix_um_user_unpin(as);
                return (int)(len - 1);
            }
        }
        espix_um_user_unpin(as);
        uva += c;
    }
    return -ENAMETOOLONG;
}

#endif /* CONFIG_ESPIX_UMODE */
