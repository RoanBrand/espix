/*
 * A POSIX app's process in the kernel, with an MMU or without: its descriptor table, sockets,
 * surfaces and streams, and the user-memory access the syscalls make. A U-mode app on the S31
 * (espix_umode.h) has an address space behind it; an app without one passes its own pointers,
 * which are checked for range and used as they are. docs/USER-MODE.md.
 */
#pragma once

#include "sdkconfig.h"

#include <dirent.h>
#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "espix_proc_priv.h"
#include "espix_syscall.h"

typedef struct espix_um_as espix_um_as_t;

#define ESPIX_UM_FDS ESPIX_OPEN_MAX

enum { ESPIX_UM_FD_FREE, ESPIX_UM_FD_STD, ESPIX_UM_FD_FILE, ESPIX_UM_FD_DIR, ESPIX_UM_FD_SOCK };

/* A socket, shared by the descriptors dup made of it. lwIP's is always non-blocking: the app's blocking is ours. */
typedef struct {
    int      kfd;
    int      refs;          /* atomic */
    int      type;          /* SOCK_STREAM, SOCK_DGRAM or SOCK_RAW */
    bool     nonblock;
    bool     connecting;    /* a blocking connect a signal cut short, finished by the next */
    bool     connected;     /* once was, so lwIP's ENOTCONN means the peer has gone */
    int64_t  rcv_us, snd_us;    /* SO_RCVTIMEO, SO_SNDTIMEO; 0 waits for ever */
} espix_um_sock_t;

/* One of a process's descriptors: a number only it can name, behind which is a kernel object. */
typedef struct {
    uint8_t kind;
    bool    cloexec;
    union {
        int  std;   /* which of the process's three streams */
        int  kfd;   /* the kernel's descriptor */
        DIR *dir;
        espix_um_sock_t *sock;
    };
} espix_um_fd_t;

typedef struct espix_um_waiter espix_um_waiter_t;
struct espix_um_text;

/* The read-only segments of a process's image it shares with others running it. */
#define ESPIX_UM_TEXTS 2

enum { ESPIX_UM_OBJ_FREE, ESPIX_UM_OBJ_SURFACE, ESPIX_UM_OBJ_AUDIO };

/* A gfx surface or an audio stream the process holds, by handle; a call using it holds a reference. */
typedef struct {
    uint8_t  kind;
    bool     closed;    /* its handle is gone; the last reference ends it */
    int      refs;
    void    *obj;
    uint32_t va;        /* a surface's pixels in the app */
} espix_um_obj_t;

#define ESPIX_UM_OBJS 8

/* A U-mode process: what its threads share. */
typedef struct {
    espix_um_as_t     *as;
    SemaphoreHandle_t  lock;        /* the descriptor table, the futex waiters, the thread count and the exit */
    espix_um_fd_t      fds[ESPIX_UM_FDS];
    int                threads;     /* live, the main one included; a leaving thread notifies the main one */
    bool               exiting;     /* every thread is ending; the main one ends the process */
    int                exit_status;
    uint8_t            exit_sig;    /* the signal behind exit_status, or 0 */
    bool               exec;        /* the exit is an exec: the main thread loads the new image */
    struct espix_um_vfork *vfork;   /* a vfork's child: its parent waits until this process lets go of its memory */
    espix_um_waiter_t *futex_waiters;
    uint32_t          *amos;        /* each AMO the loader replaced: its pc and the instruction, by pc */
    uint32_t           amo_count;
    struct espix_sigaction act[NSIG];   /* under lock */
    espix_um_obj_t     objs[ESPIX_UM_OBJS];     /* under lock */
    void              *gfx;         /* the screen (espix_gfx_t), while it holds it */
    struct espix_um_text *text[ESPIX_UM_TEXTS];
    uint8_t            ntext;
} espix_um_proc_t;

/* What the trap exit needs to resume a thread in U-mode; internal RAM, read on the interrupt path. */
typedef struct {
    uint32_t           satp;
    uint32_t           ktp;   /* the task's own tp, which kernel code needs back on every entry */
    espix_um_proc_t   *proc;
    espix_proc_slot_t *slot;
    void              *task;  /* its FreeRTOS task, the FPU owner to compare with */
    bool               diverted;    /* the return to U went to UM_KICK_PC instead of sig_pc */
    bool               gone;        /* the main thread after pthread_exit: takes no signal */
    bool               suspended;   /* in sigsuspend, with sig_saved the mask to go back to */
    bool               sys_intr;    /* the syscall just made returned -EINTR */
    uint32_t           sig_pc;
    uint32_t           sig_blocked;
    uint32_t           sig_pending; /* this thread's alone: raised, or claimed from the process's */
    uint32_t           sig_saved;
    uint32_t           sys_nr, sys_a0;  /* the last syscall, to restart it under SA_RESTART */
    espix_pid_t        tid;
    uint32_t           clear_tid;   /* holds the tid while the thread runs, zeroed and woken when it ends */
    uint32_t           stack;       /* the top of the stack the kernel gave it, or 0 */
    jmp_buf           *exit_jmp;    /* a thread's way out; NULL for the main thread, which ends the process */
    uint32_t           entry, arg, usp, utp;    /* where a new thread starts */
} espix_um_thread_t;

/* What a pointer is pinned for, as the PTE bits name it; RISC-V's own where they exist. */
#if CONFIG_IDF_TARGET_ARCH_RISCV
#include "riscv/encoding.h"
#else
#define PTE_R     0x002
#define PTE_W     0x004
#define PTE_X     0x008
#endif

/* Before the owned-fd sweep: the process's descriptors, closed while their numbers are still its own. */
void espix_um_close_fds(espix_proc_slot_t *slot);
/* Its surfaces and audio streams, ended once its threads are gone (umode_gfx.c). */
void espix_um_objs_release(espix_um_proc_t *p);
void    espix_um_fds_init(espix_um_proc_t *p);
/* A fork's descriptors: each file its own kernel descriptor, sockets and pipes shared; 0 or -errno. */
int32_t espix_um_fds_fork(espix_um_proc_t *from, espix_um_proc_t *to);
/* Every descriptor of p closed; those marked close-on-exec. */
void    espix_um_fds_drop(espix_um_proc_t *p);
void    espix_um_fds_cloexec(espix_um_proc_t *p);
/*
 * The kernel's pointer to user va, mapping a reserved page on the way; NULL if need is not allowed.
 * The frame stays allocated, even if another thread unmaps it, until espix_um_user_unpin().
 */
void *espix_um_user_pin(espix_um_as_t *as, uint32_t va, uint32_t need);
void  espix_um_user_unpin(espix_um_as_t *as);
/*
 * User memory, a page at a time: 0 or -EFAULT; a string's length, or -EFAULT or -ENAMETOOLONG.
 * A NULL address space is an app without one, whose pointers are range-checked and used as they are.
 */
int espix_um_copyin(espix_um_as_t *as, void *dst, uint32_t uva, size_t n);
int espix_um_copyout(espix_um_as_t *as, uint32_t uva, const void *src, size_t n);
int espix_um_strin(espix_um_as_t *as, char *dst, uint32_t uva, size_t size);

/* A process's syscall state: a U-mode app's main thread, or an M-mode POSIX app's (posix.c); NULL for neither. */
espix_um_thread_t *espix_posix_thread_of(espix_proc_slot_t *slot);
/* The calling M-mode app's, made on its first syscall; NULL without memory or a process. */
espix_um_thread_t *espix_posix_self(void);
/* An M-mode POSIX app's, after its descriptors are closed. */
void               espix_posix_release(espix_proc_slot_t *slot);
/* Descriptor fd's read (or with out, write) of len bytes, for stdio routed through the table; bytes or -errno. */
int32_t espix_posix_rw(espix_um_thread_t *t, int fd, void *buf, size_t len, bool out);
/* The syscall an app without an address space makes: a direct call, the number then a0-a5. */
int32_t espix_posix_syscall(uint32_t nr, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4,
                            uint32_t a5);

/* An app without an address space: whether [va, va + n) is memory it may name; its pointers, checked. */
bool  espix_posix_range_ok(uint32_t va, size_t n);
void *espix_posix_pin(uint32_t va);
int   espix_posix_copyin(void *dst, uint32_t uva, size_t n);
int   espix_posix_copyout(uint32_t uva, const void *src, size_t n);
int   espix_posix_strin(char *dst, uint32_t uva, size_t size);
