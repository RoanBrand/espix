/*
 * U-mode apps on the S31: address spaces, the trap hooks, syscalls, exec. docs/USER-MODE.md.
 */
#pragma once

#include "sdkconfig.h"

#if CONFIG_ESPIX_UMODE

#include <dirent.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

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

typedef struct espix_um_waiter espix_um_waiter_t;

/* A U-mode process: what its threads share. */
typedef struct {
    espix_um_as_t     *as;
    SemaphoreHandle_t  lock;        /* the descriptor table, the futex waiters, the thread count and the exit */
    espix_um_fd_t      fds[ESPIX_UM_FDS];
    int                threads;     /* live, the main one included; a leaving thread notifies the main one */
    bool               exiting;     /* every thread is ending; the main one ends the process */
    int                exit_status;
    espix_um_waiter_t *futex_waiters;
    uint32_t          *amos;        /* each AMO the loader replaced: its pc and the instruction, by pc */
    uint32_t           amo_count;
} espix_um_proc_t;

/* What the trap exit needs to resume a thread in U-mode; internal RAM, read on the interrupt path. */
typedef struct {
    uint32_t           satp;
    uint32_t           ktp;   /* the task's own tp, which kernel code needs back on every entry */
    espix_um_proc_t   *proc;
    espix_proc_slot_t *slot;
    void              *task;  /* its FreeRTOS task, the FPU owner to compare with */
    int                kick_sig;    /* a fatal signal the next return to U turns into a trap */
    espix_pid_t        tid;
    uint32_t           clear_tid;   /* holds the tid while the thread runs, zeroed and woken when it ends */
    uint32_t           stack;       /* the top of the stack the kernel gave it, or 0 */
    jmp_buf           *exit_jmp;    /* a thread's way out; NULL for the main thread, which ends the process */
    uint32_t           entry, arg, usp, utp;    /* where a new thread starts */
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

/* Ends the process with status: the calling thread leaves, and the main one tears the process down. */
void espix_um_exit_group(espix_um_thread_t *t, int status) __attribute__((noreturn));
/* Ends the calling thread only, as pthread_exit() does; the main thread waits for the others first. */
void espix_um_thread_exit(espix_um_thread_t *t) __attribute__((noreturn));
/* A new thread of t's process; its tid, or -errno. */
int32_t espix_um_thread_create(espix_um_thread_t *t, uint32_t entry, uint32_t arg, uint32_t utp,
                               uint32_t ustack, uint32_t ctid);

/* After an unmap: every other core drops asid's translations before the frames go. */
void espix_um_asid_shoot(uint32_t asid);

/* Futexes (umode_futex.c), private to a process: 0, -EAGAIN, -EINTR or -ETIMEDOUT. */
int32_t espix_um_futex_wait(espix_um_proc_t *p, uint32_t uaddr, uint32_t val, int64_t timeout_us);
/* How many waiters on uaddr were woken, at most n. */
int32_t espix_um_futex_wake(espix_um_proc_t *p, uint32_t uaddr, int32_t n);

/* Syscalls (umode_sys.c): the result for a0. */
int32_t espix_um_syscall(espix_um_thread_t *t, RvExcFrame *f);
void    espix_um_fds_init(espix_um_proc_t *p);

/* The heap grows by brk up to here; mmap will live above it. */
#define ESPIX_UM_BRK_MAX 0x60000000u

/* The main thread's stack: 8 MB reserved under the top, mapped as touched; Linux's ulimit -s. */
#define ESPIX_UM_STACK_TOP  0x80000000u
#define ESPIX_UM_STACK_MAX  (8u * 1024u * 1024u)
/* Thread stacks: as many 8 MB spans below it, each with an unmapped page at both ends. */
#define ESPIX_UM_TSTACKS    31

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

/* A thread's stack: the top of a free span, or 0; given back, its pages are unmapped. */
uint32_t espix_um_tstack_take(espix_um_as_t *as);
void     espix_um_tstack_give(espix_um_as_t *as, uint32_t top);

/*
 * The kernel's pointer to user va, mapping a reserved page on the way; NULL if need is not allowed.
 * The frame stays allocated, even if another thread unmaps it, until espix_um_user_unpin().
 */
void *espix_um_user_pin(espix_um_as_t *as, uint32_t va, uint32_t need);
void  espix_um_user_unpin(espix_um_as_t *as);
/* A page fault the app took: true when it was a reserved page, now mapped, and the app can resume. */
bool  espix_um_fault(espix_um_as_t *as, uint32_t va, uint32_t need);

/* User memory, a page at a time: 0 or -EFAULT; a string's length, or -EFAULT or -ENAMETOOLONG. */
int espix_um_copyin(espix_um_as_t *as, void *dst, uint32_t uva, size_t n);
int espix_um_copyout(espix_um_as_t *as, uint32_t uva, const void *src, size_t n);
int espix_um_strin(espix_um_as_t *as, char *dst, uint32_t uva, size_t size);

#endif /* CONFIG_ESPIX_UMODE */
