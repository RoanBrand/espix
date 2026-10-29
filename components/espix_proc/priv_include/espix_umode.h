/*
 * U-mode apps on the S31: address spaces, the trap hooks, syscalls, exec. docs/USER-MODE.md.
 */
#pragma once

#include "sdkconfig.h"

#if CONFIG_ESPIX_UMODE

#include <dirent.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "riscv/rvruntime-frames.h"

#include "espix_proc_priv.h"

typedef struct espix_um_as espix_um_as_t;

#define ESPIX_UM_FDS 32

enum { ESPIX_UM_FD_FREE, ESPIX_UM_FD_STD, ESPIX_UM_FD_FILE, ESPIX_UM_FD_DIR };

/* One of a process's descriptors: a number only it can name, behind which is a kernel object. */
typedef struct {
    uint8_t kind;
    bool    cloexec;
    union {
        int  std;   /* which of the process's three streams */
        int  kfd;   /* the kernel's descriptor */
        DIR *dir;
    };
} espix_um_fd_t;

/* A U-mode process: what its threads share. */
typedef struct {
    espix_um_as_t *as;
    espix_um_fd_t  fds[ESPIX_UM_FDS];
} espix_um_proc_t;

/* What the trap exit needs to resume a thread in U-mode; internal RAM, read on the interrupt path. */
typedef struct {
    uint32_t           satp;
    uint32_t           ktp;   /* the task's own tp, which kernel code needs back on every entry */
    espix_um_proc_t   *proc;
    espix_proc_slot_t *slot;
    void              *task;  /* its FreeRTOS task, the FPU owner to compare with */
    int                kick_sig;    /* a fatal signal the next return to U turns into a trap */
} espix_um_thread_t;

void espix_um_init(void);

/* Whether an image is a U-mode executable (ET_EXEC) rather than a relocatable M-mode app. */
bool espix_um_is_exec(const uint8_t *image, size_t size);
/* The same question of a file, from its header alone. */
bool espix_um_file_is_exec(const char *path);

/* Loads and enters the app; returns only on failure to start, with a message for the session. */
const char *espix_um_exec(espix_proc_slot_t *slot);

/* Before the owned-fd sweep: the process's descriptors, closed while their numbers are still its own. */
void espix_um_close_fds(espix_proc_slot_t *slot);

void espix_um_release(espix_proc_slot_t *slot);

/* Ends the process from task context, as an M-mode app's exit() does. */
void espix_um_exited(int status) __attribute__((noreturn));

/* Syscalls (umode_sys.c): the result for a0. */
int32_t espix_um_syscall(espix_um_thread_t *t, RvExcFrame *f);
void    espix_um_fds_init(espix_um_proc_t *p);

/* The heap grows by brk up to here; mmap will live above it. */
#define ESPIX_UM_BRK_MAX 0x60000000u

/* Address spaces (umode_mm.c). Frames are 4 KiB of PSRAM; a physical address is the pointer. */
espix_um_as_t *espix_um_as_new(uint32_t asid);
void           espix_um_as_free(espix_um_as_t *as);
uint32_t       espix_um_as_satp(const espix_um_as_t *as);
void          *espix_um_map_new(espix_um_as_t *as, uint32_t va, uint32_t pte_flags);

/* A range mapped zeroed page by page as it is touched; false when the table of them is full. */
bool     espix_um_region_add(espix_um_as_t *as, uint32_t start, uint32_t end, uint32_t pte_flags);
void     espix_um_brk_init(espix_um_as_t *as, uint32_t base);
/* Moves the break to want if it can; the break either way, as Linux's brk returns it. */
uint32_t espix_um_brk(espix_um_as_t *as, uint32_t want);

/* The kernel's pointer to user va, mapping a reserved page on the way; NULL if need is not allowed. */
void *espix_um_user_ptr(espix_um_as_t *as, uint32_t va, uint32_t need);
/* A page fault the app took: true when it was a reserved page, now mapped, and the app can resume. */
bool  espix_um_fault(espix_um_as_t *as, uint32_t va, uint32_t need);

/* User memory, a page at a time: 0 or -EFAULT; a string's length, or -EFAULT or -ENAMETOOLONG. */
int espix_um_copyin(espix_um_as_t *as, void *dst, uint32_t uva, size_t n);
int espix_um_copyout(espix_um_as_t *as, uint32_t uva, const void *src, size_t n);
int espix_um_strin(espix_um_as_t *as, char *dst, uint32_t uva, size_t size);

#endif /* CONFIG_ESPIX_UMODE */
