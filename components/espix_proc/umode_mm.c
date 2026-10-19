/*
 * U-mode address spaces: one Sv32 table per process, frames from PSRAM.
 *
 * M1 keeps it simple: every frame is its own 4 KiB aligned allocation and is
 * owned by exactly one table. Refcounted frames, chunked allocation and
 * map-on-touch come with M2 and M7 (docs/USER-MODE.md).
 */
#include "sdkconfig.h"

#if CONFIG_ESPIX_UMODE

#include <stdlib.h>

#include "esp_heap_caps.h"
#include "riscv/encoding.h"

#include "espix_umode.h"

#define PG 4096u

struct espix_um_as {
    uint32_t *root;
    uint32_t  asid;
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

void *espix_um_user_ptr(espix_um_as_t *as, uint32_t va, uint32_t need)
{
    const uint32_t *pte = leaf(as, va, false);
    if (pte == NULL || (*pte & (PTE_V | PTE_U | need)) != (PTE_V | PTE_U | need)) {
        return NULL;
    }
    return (uint8_t *)pte_frame(*pte) + (va & (PG - 1));
}

#endif /* CONFIG_ESPIX_UMODE */
