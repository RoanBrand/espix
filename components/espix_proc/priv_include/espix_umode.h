/*
 * U-mode apps on the S31: address spaces, the trap hooks, exec. docs/USER-MODE.md.
 */
#pragma once

#include "sdkconfig.h"

#if CONFIG_ESPIX_UMODE

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "espix_proc_priv.h"

/* FreeRTOS TLS pointer index of a U-mode thread's espix_um_thread_t. */
#define ESPIX_TLS_UM_IDX 2

typedef struct espix_um_as espix_um_as_t;

/* What the trap exit needs to resume a thread in U-mode; internal RAM, read on the interrupt path. */
typedef struct {
    uint32_t       satp;
    uint32_t       ktp;     /* the task's own tp, which kernel code needs back on every entry */
    espix_um_as_t *as;
} espix_um_thread_t;

void espix_um_init(void);

/* Whether an image is a U-mode executable (ET_EXEC) rather than a relocatable M-mode app. */
bool espix_um_is_exec(const uint8_t *image, size_t size);

/* Loads and enters the app; returns only on failure to start, with a message for the session. */
const char *espix_um_exec(espix_proc_slot_t *slot);

void espix_um_release(espix_proc_slot_t *slot);

/* Address spaces (umode_mm.c). Frames are 4 KiB of PSRAM; a physical address is the pointer. */
espix_um_as_t *espix_um_as_new(uint32_t asid);
void           espix_um_as_free(espix_um_as_t *as);
uint32_t       espix_um_as_satp(const espix_um_as_t *as);
void          *espix_um_map_new(espix_um_as_t *as, uint32_t va, uint32_t pte_flags);
void          *espix_um_user_ptr(espix_um_as_t *as, uint32_t va, uint32_t need);

#endif /* CONFIG_ESPIX_UMODE */
