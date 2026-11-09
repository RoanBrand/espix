/*
 * U-mode apps on the S31: address spaces, the trap hooks, syscalls, exec. docs/USER-MODE.md.
 */
#pragma once

#include "sdkconfig.h"

#if CONFIG_ESPIX_UMODE

#include "riscv/rvruntime-frames.h"

#include "espix_posix.h"

void espix_um_init(void);

/* Whether an image is a U-mode executable (ET_EXEC) rather than a relocatable M-mode app. */
bool espix_um_is_exec(const uint8_t *image, size_t size);
/* The same question of a file, from its header alone. */
bool espix_um_file_is_exec(const char *path);

/* Loads and enters the app; returns only on failure to start, with a message for the session. */
const char *espix_um_exec(espix_proc_slot_t *slot);

/* proc_task()'s exit_jmp value for an exec in place: the old image is released and the new one loaded. */
#define ESPIX_UM_EXEC_JUMP 2

/* fork and vfork from thread t at its syscall's frame f: the child's pid, or -errno. */
int32_t     espix_um_fork(espix_um_thread_t *t, RvExcFrame *f, bool vfork);
/* A fork's child entering U where its parent's syscall returns; only a failure to start returns. */
const char *espix_um_fork_run(espix_proc_slot_t *slot);
/* exec's first half: path checked, and its program, arguments and environment the process's next; 0 or -errno. */
int32_t     espix_um_exec_prepare(espix_um_thread_t *t, const char *path, int argc, char **argv, char **envp);
/* The second: the process becomes that program. */
void        espix_um_exec_go(espix_um_thread_t *t) __attribute__((noreturn));
/* After an exec's jump home: the old image gone, close-on-exec descriptors closed, caught signals default. */
void        espix_um_exec_reset(espix_proc_slot_t *slot);


void espix_um_release(espix_proc_slot_t *slot);

/* Ends the process with status: the calling thread leaves, and the main one tears the process down. */
void espix_um_exit_group(espix_um_thread_t *t, int status) __attribute__((noreturn));
/* The same for a signal: status 128 + sig, and wait() sees the signal. */
void espix_um_exit_sig(espix_um_thread_t *t, int sig) __attribute__((noreturn));
/* Ends the calling thread only, as pthread_exit() does; the main thread waits for the others first. */
void espix_um_thread_exit(espix_um_thread_t *t) __attribute__((noreturn));
/* A new thread of t's process; its tid, or -errno. */
int32_t espix_um_thread_create(espix_um_thread_t *t, uint32_t entry, uint32_t arg, uint32_t utp,
                               uint32_t ustack, uint32_t ctid);

/* After an unmap: every other core drops asid's translations before the frames go. */
void espix_um_asid_shoot(uint32_t asid);
/* A thread of the address space satp, in U on the other core, takes a signal now rather than at its next tick. */
void espix_um_sig_nudge(uint32_t satp);

/* Futexes (umode_futex.c), private to a process: 0, -EAGAIN, -EINTR or -ETIMEDOUT. */
int32_t espix_um_futex_wait(espix_um_proc_t *p, uint32_t uaddr, uint32_t val, int64_t timeout_us);
/* How many waiters on uaddr were woken, at most n. */
int32_t espix_um_futex_wake(espix_um_proc_t *p, uint32_t uaddr, int32_t n);

/* Signals (umode_sig.c). Run on the thread's own task, in the trap a diverted return to U takes. */
void espix_um_sig_deliver(espix_um_thread_t *t, RvExcFrame *u);
void espix_um_sig_return(espix_um_thread_t *t, RvExcFrame *u);
/* A fault's signal to its handler; false when it is blocked or not caught, and ends the process. */
bool espix_um_sig_fault(espix_um_thread_t *t, RvExcFrame *u, int sig);
/* For proc.c's blocking calls: what ends the caller's wait, claimed from the process's pending set. */
uint32_t espix_um_sig_claim(espix_proc_slot_t *slot);
int      espix_um_termsig(const espix_proc_slot_t *slot);
/* Whether the calling thread blocks sig or its process ignores it. */
bool     espix_um_sig_refused(const espix_proc_slot_t *slot, int sig);
/* SIGCHLD ignored or SA_NOCLDWAIT: no zombies. Under the table lock. */
bool     espix_um_ignores_children(const espix_proc_slot_t *slot);
int32_t  espix_um_sigaction(espix_um_thread_t *t, int sig, uint32_t uact, uint32_t uold);
int32_t  espix_um_sigprocmask(espix_um_thread_t *t, int how, uint32_t uset, uint32_t uold);
int32_t  espix_um_sigpending(espix_um_thread_t *t, uint32_t uout);
int32_t  espix_um_sigsuspend(espix_um_thread_t *t, uint32_t umask);
int32_t  espix_um_raise(espix_um_thread_t *t, int sig);

/* The FPU's registers and fcsr, 33 words. */
void espix_um_fpu_save(void *area);
void espix_um_fpu_restore(const void *area);

/* Syscalls (umode_sys.c): the result for a0. */
int32_t espix_um_syscall(espix_um_thread_t *t, RvExcFrame *f);
/* exec's signal state: caught signals back to their default. */
void    espix_um_sig_exec(espix_um_proc_t *p);

/* The heap grows by brk up to here; lent memory, and mmap, live above it. */
#define ESPIX_UM_BRK_MAX 0x60000000u

/* The main thread's stack: 8 MB reserved under the top, mapped as touched; Linux's ulimit -s. */
#define ESPIX_UM_STACK_TOP  0x80000000u
#define ESPIX_UM_STACK_MAX  (8u * 1024u * 1024u)
/* Thread stacks: as many 8 MB spans below it, each with an unmapped page at both ends. */
#define ESPIX_UM_TSTACKS    31

/* Address spaces (umode_mm.c). Frames are 4 KiB of PSRAM; a physical address is the pointer. */
void           espix_um_frames_init(void);
/* Frames carved and not in use, which the heap counts as taken. */
uint32_t       espix_um_frames_spare(void);
/* One table fewer maps frame; it is freed with the last. */
void           espix_um_frame_put(void *frame);
/* A frame a shared text keeps, or no longer: copied before it is written, whoever maps it. */
void           espix_um_frame_text(void *frame, bool on);
espix_um_as_t *espix_um_as_new(uint32_t asid);
/* A copy for a fork's child: private pages copy-on-write, shared ones the same frames. NULL without memory. */
espix_um_as_t *espix_um_as_fork(espix_um_as_t *src, uint32_t asid);
/* Another process on it (a vfork's child); put is true when that freed it, its ASID then the caller's to free. */
espix_um_as_t *espix_um_as_get(espix_um_as_t *as);
bool           espix_um_as_put(espix_um_as_t *as, uint32_t *pages);
uint32_t       espix_um_as_asid(const espix_um_as_t *as);
/* Where the table keeps its count of resident pages, which outlives it: the process's slot. */
void           espix_um_as_count(espix_um_as_t *as, uint32_t *pages);
void           espix_um_as_free(espix_um_as_t *as);
uint32_t       espix_um_as_satp(const espix_um_as_t *as);
void          *espix_um_map_new(espix_um_as_t *as, uint32_t va, uint32_t pte_flags);
/* An existing frame mapped at va too, its count raised: an image's shared text. */
bool           espix_um_map_frame(espix_um_as_t *as, uint32_t va, void *frame, uint32_t pte_flags);

/*
 * Kernel memory lent to the app: len bytes from page-aligned k, mapped whole at a free address
 * above the heap's limit; the address, or 0. Unmapped, release(obj) runs once no kernel call
 * holds a pointer into it; false if nothing is lent at va.
 */
uint32_t espix_um_map_lent(espix_um_as_t *as, void *k, size_t len, uint32_t pte_flags);
bool     espix_um_unmap_lent(espix_um_as_t *as, uint32_t va, void (*release)(void *), void *obj);

/* A range mapped zeroed page by page as it is touched, after any region its first page is in. */
bool     espix_um_region_add(espix_um_as_t *as, uint32_t start, uint32_t end, uint32_t pte_flags);

/*
 * mmap's work, on PTE permissions: how holds ESPIX_MAP_* flags; kfd, the region's own descriptor, or
 * -1 for zero pages. The address, or -errno. A file's pages are read in as touched, and a shared
 * writable one's written back by msync, munmap and the process's end.
 */
int32_t espix_um_mmap(espix_um_as_t *as, uint32_t addr, uint32_t len, uint32_t pte_flags, uint32_t how,
                      int kfd, uint64_t off);
int32_t espix_um_munmap(espix_um_as_t *as, uint32_t start, uint32_t len);
int32_t espix_um_mprotect(espix_um_as_t *as, uint32_t start, uint32_t len, uint32_t pte_flags);
/* With drop, the pages go and are refilled when next touched (MADV_DONTNEED). */
int32_t espix_um_madvise(espix_um_as_t *as, uint32_t start, uint32_t len, bool drop);
int32_t espix_um_msync(espix_um_as_t *as, uint32_t start, uint32_t len);
/* vec[i] 1 for each of npages from start a frame backs; -ENOMEM at a page nothing reserves. */
int32_t espix_um_mincore(espix_um_as_t *as, uint32_t start, uint32_t npages, uint8_t *vec);
/* At the process's end, before the owned-fd sweep: shared files written back, their descriptors closed. */
void    espix_um_unmap_files(espix_um_as_t *as);
void     espix_um_brk_init(espix_um_as_t *as, uint32_t base);
/* Moves the break to want if it can; the break either way, as Linux's brk returns it. */
uint32_t espix_um_brk(espix_um_as_t *as, uint32_t want);

/* A thread's stack: the top of a free span, or 0; given back, its pages are unmapped. */
uint32_t espix_um_tstack_take(espix_um_as_t *as);
void     espix_um_tstack_give(espix_um_as_t *as, uint32_t top);

/* A page fault the app took: true when it was a reserved page, now mapped, and the app can resume. */
bool  espix_um_fault(espix_um_as_t *as, uint32_t va, uint32_t need);


#endif /* CONFIG_ESPIX_UMODE */
