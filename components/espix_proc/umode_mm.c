/*
 * U-mode address spaces: one Sv32 table per process, frames from PSRAM, and
 * the kernel's access to user memory. Every frame is owned by one table, or is
 * kernel memory lent to it (docs/USER-MODE.md). A process's threads share the
 * table, so every change to it is under the table's lock.
 */
#include "sdkconfig.h"

#if CONFIG_ESPIX_UMODE

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "riscv/encoding.h"

#include "espix_umode.h"

#define PG 4096u

/* RSW: the frame is the kernel's, lent to the app, and never freed with the table. */
#define PTE_LENT 0x100u

/* A range whose pages are mapped, zeroed, when first touched: bss, the heap, a stack; or one mapped whole to lent memory. */
typedef struct {
    uint32_t start, end;
    uint32_t flags;
    bool     lent;
} region_t;

#define REGIONS 16

/* Lent memory is mapped between the heap's limit and the thread stacks. */
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
    region_t          regions[REGIONS];
    uint32_t          brk_base, brk;    /* the heap is regions[0] */
    uint32_t          tstacks;          /* thread stack spans in use, one bit each */
    uint32_t          pins;             /* kernel calls holding a pointer into a frame; atomic */
    dead_t           *dead;             /* freed when the last pin goes */
};

/*
 * Frames are carved from chunks of the PSRAM heap. A 4 KiB block aligned to 4 KiB leaves the heap's
 * header in the page before it, so frames allocated one by one cost two pages each. A chunk is
 * smaller when the heap has no room for a whole one, and goes back when its last frame does.
 */
#define CHUNK_FRAMES 64
#define CHUNKS       192

typedef struct {
    uint8_t  *base;
    uint64_t  free;     /* a bit per frame */
    uint8_t   n;        /* its frames */
} chunk_t;

static EXT_RAM_BSS_ATTR chunk_t s_chunks[CHUNKS];     /* by base */
static int               s_nchunks;
static SemaphoreHandle_t s_frames_lock;

void espix_um_frames_init(void)
{
    s_frames_lock = xSemaphoreCreateMutex();
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
        s_chunks[i] = (chunk_t){ base, chunk_mask(n), (uint8_t)n };
        return &s_chunks[i];
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
        f = c->base + (size_t)b * PG;
    }
    xSemaphoreGive(s_frames_lock);
    if (f != NULL) {
        memset(f, 0, PG);
    }
    return f;
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

static void frame_free(void *frame)
{
    uint8_t *const f = frame;
    void *empty = NULL;
    if (f == NULL) {
        return;
    }
    xSemaphoreTake(s_frames_lock, portMAX_DELAY);
    for (int lo = 0, hi = s_nchunks - 1; lo <= hi;) {
        const int mid = (lo + hi) / 2;
        chunk_t *const c = &s_chunks[mid];
        if (f < c->base) {
            hi = mid - 1;
        } else if (f >= c->base + (size_t)c->n * PG) {
            lo = mid + 1;
        } else {
            c->free |= 1ull << ((f - c->base) / PG);
            if (c->free == chunk_mask(c->n)) {
                empty = c->base;
                memmove(c, c + 1, (size_t)(s_nchunks - mid - 1) * sizeof(*c));
                s_nchunks--;
            }
            break;
        }
    }
    xSemaphoreGive(s_frames_lock);
    heap_caps_free(empty);
}

static uint32_t pte_of(const void *pa, uint32_t flags)
{
    return (((uint32_t)pa >> 12) << 10) | flags;
}

static void *pte_frame(uint32_t pte)
{
    return (void *)((pte >> 10) << 12);
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
    as->asid = asid & 0x1ff;
    return as;
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
            if ((l2[j] & (PTE_V | PTE_LENT)) == PTE_V) {
                frame_free(pte_frame(l2[j]));
            }
        }
        frame_free(l2);
    }
    frame_free(as->root);
    frames_free(as->dead);
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
    const uint32_t flags = pte_flags | PTE_V | PTE_U | PTE_A | ((pte_flags & PTE_W) ? PTE_D : 0);
    if (*pte & PTE_V) {
        *pte |= flags;
        return pte_frame(*pte);
    }
    void *frame = frame_alloc();
    if (frame == NULL) {
        return NULL;
    }
    __atomic_store_n(pte, pte_of(frame, flags), __ATOMIC_RELEASE);
    return frame;
}

bool espix_um_region_add(espix_um_as_t *as, uint32_t start, uint32_t end, uint32_t pte_flags)
{
    for (int i = 1; i < REGIONS; i++) {
        if (as->regions[i].end == 0) {
            as->regions[i] = (region_t){ start & ~(PG - 1), (end + PG - 1) & ~(PG - 1), pte_flags, false };
            return true;
        }
    }
    return false;
}

/* The PTE flags a reserved page at va gets when touched, or 0 if nothing reserves it. Under the lock. */
static uint32_t reserved(const espix_um_as_t *as, uint32_t va)
{
    for (int i = 0; i < REGIONS; i++) {
        if (va >= as->regions[i].start && va < as->regions[i].end) {
            return as->regions[i].lent ? 0 : as->regions[i].flags;
        }
    }
    if (va >= TSTACK_LOW && va < TSTACK_HIGH) {
        const uint32_t k   = (TSTACK_HIGH - 1 - va) / TSTACK_SPAN;
        const uint32_t top = TSTACK_HIGH - k * TSTACK_SPAN - PG;
        if ((as->tstacks & (1u << k)) && va < top && va >= top - TSTACK_SPAN + 2 * PG) {
            return PTE_R | PTE_W;
        }
    }
    return 0;
}

void espix_um_brk_init(espix_um_as_t *as, uint32_t base)
{
    as->brk_base = as->brk = (base + PG - 1) & ~(PG - 1);
    as->regions[0] = (region_t){ as->brk_base, as->brk_base, PTE_R | PTE_W, false };
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
        if ((*pte & (PTE_V | PTE_LENT)) != PTE_V) {
            continue;
        }
        dead_t *const d = malloc(sizeof(*d));
        if (d == NULL) {
            continue;       /* left mapped: a leak, never a frame in use freed */
        }
        *d = (dead_t){ *dead, pte_frame(*pte), NULL };
        *dead = d;
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
        const uint32_t old_end = as->regions[0].end;
        const uint32_t new_end = (want + PG - 1) & ~(PG - 1);
        dead_t *dead = NULL;
        unmap_range(as, new_end, old_end, &dead);
        unmapped(as, dead);
        as->regions[0].end = new_end;
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

/* The highest free gap of len bytes for lent memory, a guard page above it; 0 if none. Under the lock. */
static uint32_t map_gap(const espix_um_as_t *as, uint32_t len)
{
    uint32_t end = MAP_HIGH - PG;
    bool moved = true;
    while (moved && end >= MAP_LOW + len) {
        moved = false;
        for (int i = 1; i < REGIONS; i++) {
            const region_t *r = &as->regions[i];
            if (r->lent && r->start < end + PG && r->end + PG > end - len) {
                end = r->start - PG;
                moved = true;
            }
        }
    }
    return (end >= MAP_LOW + len) ? end - len : 0;
}

uint32_t espix_um_map_lent(espix_um_as_t *as, void *k, size_t len, uint32_t pte_flags)
{
    len = (len + PG - 1) & ~(PG - 1);
    if (((uint32_t)k & (PG - 1)) != 0 || len == 0) {
        return 0;
    }
    const uint32_t flags = pte_flags | PTE_V | PTE_U | PTE_A | ((pte_flags & PTE_W) ? PTE_D : 0) | PTE_LENT;
    xSemaphoreTake(as->lock, portMAX_DELAY);
    int slot = -1;
    for (int i = 1; i < REGIONS && slot < 0; i++) {
        if (as->regions[i].end == 0) {
            slot = i;
        }
    }
    const uint32_t va = (slot < 0) ? 0 : map_gap(as, len);
    uint32_t off = 0;
    for (; va != 0 && off < len; off += PG) {
        uint32_t *pte = leaf(as, va + off, true);
        if (pte == NULL) {
            break;
        }
        __atomic_store_n(pte, pte_of((uint8_t *)k + off, flags), __ATOMIC_RELEASE);
    }
    if (va == 0 || off < len) {
        for (uint32_t o = 0; va != 0 && o < off; o += PG) {
            __atomic_store_n(leaf(as, va + o, false), 0, __ATOMIC_SEQ_CST);
        }
        xSemaphoreGive(as->lock);
        return 0;
    }
    as->regions[slot] = (region_t){ va, va + len, pte_flags, true };
    xSemaphoreGive(as->lock);
    asm volatile ("sfence.vma x0, %0" :: "r"(as->asid) : "memory");
    return va;
}

bool espix_um_unmap_lent(espix_um_as_t *as, uint32_t va, void (*release)(void *), void *obj)
{
    dead_t *const d = malloc(sizeof(*d));
    xSemaphoreTake(as->lock, portMAX_DELAY);
    int i = 1;
    while (i < REGIONS && !(as->regions[i].lent && as->regions[i].start == va)) {
        i++;
    }
    if (i == REGIONS || d == NULL) {
        xSemaphoreGive(as->lock);
        free(d);
        return false;
    }
    for (uint32_t p = va; p < as->regions[i].end; p += PG) {
        __atomic_store_n(leaf(as, p, false), 0, __ATOMIC_SEQ_CST);
        asm volatile ("sfence.vma %0, %1" :: "r"(p), "r"(as->asid) : "memory");
    }
    as->regions[i] = (region_t){ 0 };
    *d = (dead_t){ NULL, obj, release };
    unmapped(as, d);
    xSemaphoreGive(as->lock);
    return true;
}

/* The page at va, mapped now if something reserves it. Under the lock. */
static uint8_t *page_of(espix_um_as_t *as, uint32_t va, uint32_t need)
{
    const uint32_t *pte = leaf(as, va, false);
    if (pte == NULL || !(*pte & PTE_V)) {
        const uint32_t flags = reserved(as, va);
        if (flags == 0 || (flags & need) != need) {
            return NULL;
        }
        uint8_t *frame = espix_um_map_new(as, va & ~(PG - 1), flags);
        if (frame == NULL) {
            return NULL;
        }
        asm volatile ("sfence.vma %0, %1" :: "r"(va), "r"(as->asid) : "memory");
        return frame + (va & (PG - 1));
    }
    if ((*pte & (PTE_U | need)) != (PTE_U | need)) {
        return NULL;
    }
    return (uint8_t *)pte_frame(*pte) + (va & (PG - 1));
}

void *espix_um_user_pin(espix_um_as_t *as, uint32_t va, uint32_t need)
{
    /* A page mapped already needs no lock: the tables only grow, and unmapped() honours the pin. */
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
