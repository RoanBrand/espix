/*
 * U-mode address spaces: one Sv32 table per process, frames from PSRAM, and
 * the kernel's access to user memory. Every frame is its own 4 KiB aligned
 * allocation, owned by one table (docs/USER-MODE.md).
 */
#include "sdkconfig.h"

#if CONFIG_ESPIX_UMODE

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "esp_heap_caps.h"
#include "riscv/encoding.h"

#include "espix_umode.h"

#define PG 4096u

/* A range whose pages are mapped, zeroed, when first touched: bss, the heap, a stack. */
typedef struct {
    uint32_t start, end;
    uint32_t flags;
} region_t;

#define REGIONS 8

struct espix_um_as {
    uint32_t *root;
    uint32_t  asid;
    region_t  regions[REGIONS];
    uint32_t  brk_base, brk;    /* the heap is regions[0] */
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
    if (as->root == NULL) {
        free(as);
        return NULL;
    }
    as->asid = asid & 0x1ff;
    return as;
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

static const region_t *region_of(const espix_um_as_t *as, uint32_t va)
{
    for (int i = 0; i < REGIONS; i++) {
        if (va >= as->regions[i].start && va < as->regions[i].end) {
            return &as->regions[i];
        }
    }
    return NULL;
}

void espix_um_brk_init(espix_um_as_t *as, uint32_t base)
{
    as->brk_base = as->brk = (base + PG - 1) & ~(PG - 1);
    as->regions[0] = (region_t){ as->brk_base, as->brk_base, PTE_R | PTE_W };
}

static void unmap(espix_um_as_t *as, uint32_t va)
{
    uint32_t *pte = leaf(as, va, false);
    if (pte != NULL && (*pte & PTE_V)) {
        heap_caps_free(pte_frame(*pte));
        *pte = 0;
        asm volatile ("sfence.vma %0, %1" :: "r"(va), "r"(as->asid) : "memory");
    }
}

uint32_t espix_um_brk(espix_um_as_t *as, uint32_t want)
{
    if (want < as->brk_base || want > ESPIX_UM_BRK_MAX) {
        return as->brk;
    }
    const uint32_t old_end = as->regions[0].end;
    const uint32_t new_end = (want + PG - 1) & ~(PG - 1);
    for (uint32_t va = new_end; va < old_end; va += PG) {
        unmap(as, va);
    }
    as->regions[0].end = new_end;
    as->brk = want;
    return want;
}

/* The page at va, mapped now if a region reserves it; NULL if the app may not reach it so. */
void *espix_um_user_ptr(espix_um_as_t *as, uint32_t va, uint32_t need)
{
    const uint32_t *pte = leaf(as, va, false);
    if (pte == NULL || !(*pte & PTE_V)) {
        const region_t *r = region_of(as, va);
        if (r == NULL || (r->flags & need) != need) {
            return NULL;
        }
        uint8_t *frame = espix_um_map_new(as, va & ~(PG - 1), r->flags);
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

bool espix_um_fault(espix_um_as_t *as, uint32_t va, uint32_t need)
{
    const uint32_t *pte = leaf(as, va, false);
    return (pte == NULL || !(*pte & PTE_V)) && espix_um_user_ptr(as, va, need) != NULL;
}

int espix_um_copyin(espix_um_as_t *as, void *dst, uint32_t uva, size_t n)
{
    uint8_t *d = dst;
    while (n > 0) {
        const uint8_t *k = espix_um_user_ptr(as, uva, PTE_R);
        if (k == NULL) {
            return -EFAULT;
        }
        const size_t c = MIN(n, PG - (uva & (PG - 1)));
        memcpy(d, k, c);
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
        uint8_t *k = espix_um_user_ptr(as, uva, PTE_W);
        if (k == NULL) {
            return -EFAULT;
        }
        const size_t c = MIN(n, PG - (uva & (PG - 1)));
        memcpy(k, s, c);
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
        const char *k = espix_um_user_ptr(as, uva, PTE_R);
        if (k == NULL) {
            return -EFAULT;
        }
        const size_t c = MIN(size - len, PG - (uva & (PG - 1)));
        for (size_t i = 0; i < c; i++) {
            if ((dst[len++] = k[i]) == '\0') {
                return (int)(len - 1);
            }
        }
        uva += c;
    }
    return -ENAMETOOLONG;
}

#endif /* CONFIG_ESPIX_UMODE */
