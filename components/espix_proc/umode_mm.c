/*
 * U-mode address spaces: one Sv32 table per process, frames from PSRAM, and
 * the kernel's access to user memory. Every frame is its own 4 KiB aligned
 * allocation, owned by one table (docs/USER-MODE.md). A process's threads share
 * the table, so every change to it is under the table's lock.
 */
#include "sdkconfig.h"

#if CONFIG_ESPIX_UMODE

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "riscv/encoding.h"

#include "espix_umode.h"

#define PG 4096u

/* A range whose pages are mapped, zeroed, when first touched: bss, the heap, a stack. */
typedef struct {
    uint32_t start, end;
    uint32_t flags;
} region_t;

#define REGIONS 8

#define TSTACK_SPAN ESPIX_UM_STACK_MAX
#define TSTACK_HIGH (ESPIX_UM_STACK_TOP - ESPIX_UM_STACK_MAX)
#define TSTACK_LOW  (TSTACK_HIGH - ESPIX_UM_TSTACKS * TSTACK_SPAN)

/* A frame unmapped while a kernel call still held a pointer into it. */
typedef struct dead {
    struct dead *next;
    void        *frame;
} dead_t;

struct espix_um_as {
    uint32_t         *root;
    uint32_t          asid;
    SemaphoreHandle_t lock;
    region_t          regions[REGIONS];
    uint32_t          brk_base, brk;    /* the heap is regions[0] */
    uint32_t          tstacks;          /* thread stack spans in use, one bit each */
    uint32_t          pins;             /* kernel calls holding a pointer into a frame */
    dead_t           *dead;             /* freed when the last pin goes */
};

static void *frame_alloc(void)
{
    return heap_caps_aligned_calloc(PG, 1, PG, MALLOC_CAP_SPIRAM);
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
        heap_caps_free(as->root);
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
        heap_caps_free(d->frame);
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
            if (l2[j] & PTE_V) {
                heap_caps_free(pte_frame(l2[j]));
            }
        }
        heap_caps_free(l2);
    }
    heap_caps_free(as->root);
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
        *l1 = pte_of(l2, PTE_V);
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
    *pte = pte_of(frame, flags);
    return frame;
}

bool espix_um_region_add(espix_um_as_t *as, uint32_t start, uint32_t end, uint32_t pte_flags)
{
    for (int i = 1; i < REGIONS; i++) {
        if (as->regions[i].end == 0) {
            as->regions[i] = (region_t){ start & ~(PG - 1), (end + PG - 1) & ~(PG - 1), pte_flags };
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
            return as->regions[i].flags;
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
    as->regions[0] = (region_t){ as->brk_base, as->brk_base, PTE_R | PTE_W };
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
        if (!(*pte & PTE_V)) {
            continue;
        }
        dead_t *const d = malloc(sizeof(*d));
        if (d == NULL) {
            continue;       /* left mapped: a leak, never a frame in use freed */
        }
        *d = (dead_t){ *dead, pte_frame(*pte) };
        *dead = d;
        *pte = 0;
        asm volatile ("sfence.vma %0, %1" :: "r"(va), "r"(as->asid) : "memory");
    }
}

/* The frames of an unmap: no core translates them any more; freed now, or when the last pin goes. */
static void unmapped(espix_um_as_t *as, dead_t *dead)
{
    if (dead == NULL) {
        return;
    }
    espix_um_asid_shoot(as->asid);
    if (as->pins > 0) {
        dead_t *tail = dead;
        while (tail->next != NULL) {
            tail = tail->next;
        }
        tail->next = as->dead;
        as->dead   = dead;
    } else {
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
    xSemaphoreTake(as->lock, portMAX_DELAY);
    uint8_t *const k = page_of(as, va, need);
    if (k != NULL) {
        as->pins++;
    }
    xSemaphoreGive(as->lock);
    return k;
}

void espix_um_user_unpin(espix_um_as_t *as)
{
    dead_t *dead = NULL;
    xSemaphoreTake(as->lock, portMAX_DELAY);
    if (--as->pins == 0) {
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
