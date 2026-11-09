/*
 * A POSIX app without an address space (an M-mode app built by espix-cc): its descriptor
 * table and the rest of a U-mode process's syscall state, made on its first syscall. The
 * syscalls are the ones a U-mode app makes (umode_sys.c), reached by a direct call to
 * espix_posix_syscall(); its threads and signals are an M-mode app's, through the ABI.
 */
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "espix_fs.h"
#include "espix_posix.h"
#include "espix_proc_priv.h"
#include "espix_shell.h"
#if CONFIG_ESPIX_UMODE
#include "espix_umode.h"
#endif

/* A mapping without an MMU: a block of the process's arena; a shared file's is written back to it. */
typedef struct {
    uint32_t addr, len;
    bool     shared, writable, anon;
    int      kfd;           /* a shared writable file's own descriptor, else -1 */
    uint64_t off;
} nmap_t;

/* Where a vfork's parent resumes: the app setjmps into it, and its task frees it at its next vfork or the end. */
typedef struct vjb {
    struct vjb  *next;
    TaskHandle_t task;
    jmp_buf      jb;
} vjb_t;

/* The state and its stdio: stdin, stdout and stderr as streams over descriptors 0-2, so dup2 onto them redirects. */
typedef struct {
    espix_um_thread_t t;
    FILE             *std[3];
    nmap_t           *maps;     /* under t.proc->lock */
    int               nmaps, capmaps;
    bool              threads_ended;    /* by an exec: a stream's lock may have gone with one */
    vjb_t            *spent;            /* resumed from; under t.proc->lock */
} posix_t;

static int std_io(void *cookie, char *buf, int len, bool out)
{
    espix_um_thread_t *const t = espix_posix_thread_of(espix_proc_self());
    if (t == NULL || len <= 0) {
        return 0;
    }
    const int32_t r = espix_posix_rw(t, (int)(intptr_t)cookie - 1, buf, (size_t)len, out);
    return (r < 0) ? (errno = (int)-r, -1) : (int)r;
}

static int std_read(void *cookie, char *buf, int len)
{
    return std_io(cookie, buf, len, false);
}

static int std_write(void *cookie, const char *buf, int len)
{
    return std_io(cookie, (char *)buf, len, true);
}

/* The process's streams become the routed ones; the session's stay behind fds 0-2 as their STD entries. */
static void std_route(posix_t *x, espix_proc_slot_t *slot)
{
    for (int fd = 0; fd < 3; fd++) {
        FILE *const f = funopen((void *)(intptr_t)(fd + 1), fd == 0 ? std_read : NULL, fd > 0 ? std_write : NULL,
                                NULL, NULL);
        if (f == NULL) {
            continue;
        }
        setvbuf(f, NULL, (fd == 1) ? _IOLBF : _IONBF, (fd == 1) ? 128 : 0);
        x->std[fd]     = f;
        slot->std[fd]  = f;
    }
}

/* A process's state with an empty table, its own yet. */
static posix_t *state_new(void)
{
    posix_t *const x = calloc(1, sizeof(*x));
    espix_um_proc_t *const p = calloc(1, sizeof(*p));
    SemaphoreHandle_t lock = xSemaphoreCreateMutex();
    if (x == NULL || p == NULL || lock == NULL) {
        free(x);
        free(p);
        if (lock != NULL) {
            vSemaphoreDelete(lock);
        }
        return NULL;
    }
    p->lock    = lock;
    p->threads = 1;
    espix_um_fds_init(p);
    x->t.proc = p;
    return x;
}

static void state_free(posix_t *x)
{
    vSemaphoreDelete(x->t.proc->lock);
    free(x->t.proc);
    free(x);
}

espix_um_thread_t *espix_posix_thread_of(espix_proc_slot_t *slot)
{
    if (slot == NULL || slot->app == NULL) {
        return NULL;
    }
#if CONFIG_ESPIX_UMODE
    if (slot->app->um != NULL) {
        return slot->app->um;
    }
#endif
    return slot->app->posix;
}

espix_um_thread_t *espix_posix_self(void)
{
    espix_proc_slot_t *const slot = espix_proc_self();
    if (slot == NULL || slot->app == NULL || slot->info.kind != ESPIX_PROC_APP) {
        return NULL;
    }
    espix_um_thread_t *t = __atomic_load_n((espix_um_thread_t **)&slot->app->posix, __ATOMIC_ACQUIRE);
    if (t != NULL) {
        return t;
    }
    posix_t *const x = state_new();
    if (x == NULL) {
        return NULL;
    }
    t = &x->t;
    t->slot = slot;
    t->tid  = slot->info.pid;
    /* Two threads' first calls at once: one state wins, the other is given back. */
    espix_um_thread_t *none = NULL;
    if (!__atomic_compare_exchange_n((espix_um_thread_t **)&slot->app->posix, &none, t, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        state_free(x);
        return none;
    }
    std_route(x, slot);
    return t;
}

void espix_posix_release(espix_proc_slot_t *slot)
{
    espix_um_thread_t *const t = (slot->app != NULL) ? slot->app->posix : NULL;
    if (t == NULL) {
        return;
    }
    posix_t *const x = (posix_t *)t;
    espix_posix_unmap_all(t);
    free(x->maps);
    while (x->spent != NULL) {
        vjb_t *const v = x->spent;
        x->spent = v->next;
        free(v);
    }
    /* Flushed already by an end the process made itself. A killed one's task may have died holding a
     * stream's lock, and a close would wait on it for ever: those are left, as a kill leaves its others. */
    for (int fd = 0; fd < 3; fd++) {
        if (x->std[fd] != NULL) {
            if (slot->std[fd] == x->std[fd]) {
                slot->std[fd] = (fd == 0) ? stdin : (fd == 1) ? stdout : stderr;
            }
            if (slot->exiting) {
                fclose(x->std[fd]);
            }
        }
    }
    slot->app->posix = NULL;
    vSemaphoreDelete(t->proc->lock);
    free(t->proc);
    free(x);
}

/* ------------------------------------------------------------------ */
/* mmap without an MMU                                                 */
/* ------------------------------------------------------------------ */

/* A shared file's bytes written back from the block, up to the file's end; synced first, for littlefs's size. */
static void writeback(const nmap_t *m)
{
    struct stat st;
    if (m->kfd < 0 || fsync(m->kfd) != 0 || fstat(m->kfd, &st) != 0 || m->off >= (uint64_t)st.st_size) {
        return;
    }
    const size_t n = (size_t)MIN((uint64_t)m->len, (uint64_t)st.st_size - m->off);
    (void)pwrite(m->kfd, (const void *)m->addr, n, (off_t)m->off);
    (void)fsync(m->kfd);
}

#define PG_UP(n) (((n) + 4095u) & ~4095u)

/* The mapping holding [addr, addr + len), or -1; a block counts to its last page's end, as lengths are rounded. Under the lock. */
static int map_holding(const posix_t *x, uint32_t addr, uint32_t len)
{
    for (int i = 0; i < x->nmaps; i++) {
        const nmap_t *m = &x->maps[i];
        const uint32_t span = PG_UP(m->len);
        if (addr >= m->addr && addr - m->addr <= span && len <= span - (addr - m->addr)) {
            return i;
        }
    }
    return -1;
}

int32_t espix_posix_mmap(espix_um_thread_t *t, uint32_t len, bool shared, bool writable, int kfd, uint64_t off)
{
    const bool anon = kfd < 0;
    posix_t *const x = (posix_t *)t;
    uint8_t *const block = espix_abi_calloc(1, len);
    if (block == NULL) {
        if (kfd >= 0) {
            close(kfd);
        }
        return -ENOMEM;
    }
    for (uint32_t done = 0; kfd >= 0 && done < len;) {
        const ssize_t n = pread(kfd, block + done, len - done, (off_t)(off + done));
        if (n <= 0) {
            break;      /* past the end stays zero */
        }
        done += (uint32_t)n;
    }
    /* Only a shared writable file is written back; the others need their descriptor no more. */
    if (kfd >= 0 && !(shared && writable)) {
        close(kfd);
        kfd = -1;
    }
    bool room = true;
    xSemaphoreTake(t->proc->lock, portMAX_DELAY);
    if (x->nmaps == x->capmaps) {
        const int cap = x->capmaps ? x->capmaps * 2 : 8;
        nmap_t *const m = realloc(x->maps, (size_t)cap * sizeof(*m));
        room = m != NULL;
        if (room) {
            x->maps    = m;
            x->capmaps = cap;
        }
    }
    if (room) {
        x->maps[x->nmaps++] = (nmap_t){ (uint32_t)block, len, shared, writable, anon, kfd, off };
    }
    xSemaphoreGive(t->proc->lock);
    if (!room) {
        espix_abi_free(block);
        if (kfd >= 0) {
            close(kfd);
        }
        return -ENOMEM;
    }
    return (int32_t)(uint32_t)block;
}

int32_t espix_posix_munmap(espix_um_thread_t *t, uint32_t addr, uint32_t len)
{
    posix_t *const x = (posix_t *)t;
    nmap_t m = { .kfd = -1 };
    xSemaphoreTake(t->proc->lock, portMAX_DELAY);
    const int i = map_holding(x, addr, len);
    /* A block goes whole: there are no pages to give back a part of. */
    const bool whole = i >= 0 && x->maps[i].addr == addr && PG_UP(len) == PG_UP(x->maps[i].len);
    if (whole) {
        m = x->maps[i];
        x->maps[i] = x->maps[--x->nmaps];
    }
    xSemaphoreGive(t->proc->lock);
    if (!whole) {
        return -EINVAL;
    }
    writeback(&m);
    if (m.kfd >= 0) {
        close(m.kfd);
    }
    espix_abi_free((void *)m.addr);
    return 0;
}

/* msync, and with drop madvise(DONTNEED): a private anonymous range is zero again. */
int32_t espix_posix_mrange(espix_um_thread_t *t, uint32_t addr, uint32_t len, bool sync, bool drop)
{
    posix_t *const x = (posix_t *)t;
    xSemaphoreTake(t->proc->lock, portMAX_DELAY);
    const int i = map_holding(x, addr, len);
    const nmap_t m = (i >= 0) ? x->maps[i] : (nmap_t){ .kfd = -1 };
    xSemaphoreGive(t->proc->lock);
    if (i < 0) {
        return -ENOMEM;
    }
    if (sync) {
        writeback(&m);
    }
    if (drop && !m.shared && m.anon && addr - m.addr < m.len) {
        memset((void *)addr, 0, MIN(len, m.len - (addr - m.addr)));
    }
    return 0;
}

void espix_posix_unmap_all(espix_um_thread_t *t)
{
    posix_t *const x = (posix_t *)t;
    for (int i = 0; i < x->nmaps; i++) {
        writeback(&x->maps[i]);
        if (x->maps[i].kfd >= 0) {
            close(x->maps[i].kfd);
        }
    }
    x->nmaps = 0;
}

#if !CONFIG_ESPIX_UMODE
/* Surfaces and streams are U-mode's handles; an M-mode app's are its own to close. */
void espix_um_objs_release(espix_um_proc_t *p)
{
    (void)p;
}
#endif

/* ------------------------------------------------------------------ */
/* exec without an MMU                                                 */
/* ------------------------------------------------------------------ */

/* An app this kernel loads on a task of its own: an ELF for this architecture, and on the S31 not a U-mode one. */
static bool mmode_image(const char *abs)
{
    uint8_t h[20];
    const int fd = open(abs, O_RDONLY);
    const bool got = fd >= 0 && read(fd, h, sizeof(h)) == (ssize_t)sizeof(h);
    if (fd >= 0) {
        close(fd);
    }
#if CONFIG_IDF_TARGET_ARCH_RISCV
    const uint16_t mine = 243;      /* EM_RISCV */
#else
    const uint16_t mine = 94;       /* EM_XTENSA */
#endif
    if (!got || memcmp(h, "\x7f" "ELF", 4) != 0 || (uint16_t)(h[18] | (h[19] << 8)) != mine) {
        return false;
    }
#if CONFIG_ESPIX_UMODE
    return !espix_um_file_is_exec(abs);
#else
    return true;
#endif
}

int32_t espix_posix_exec_prepare(espix_um_thread_t *t, const char *path, int argc, char **argv, char **envp)
{
    espix_proc_slot_t *const slot = t->slot;
    /* The new program loads on the frame proc_task() keeps on the process's own task, or a vfork's child's new one. */
    if (slot->info.task != xTaskGetCurrentTaskHandle() || (slot->exit_jmp == NULL && slot->app->vfork_jb == NULL)) {
        return -ENOTSUP;
    }
    char abs[ESPIX_PATH_MAX];
    struct stat st;
    if (espix_fs_resolve(espix_proc_cwd(), path, abs, sizeof(abs)) != ESP_OK) {
        return -ENAMETOOLONG;
    }
    if (stat(abs, &st) != 0) {
        return -errno;
    }
    if (!S_ISREG(st.st_mode) || !(st.st_mode & S_IXUSR)) {
        return -EACCES;
    }
    if (!mmode_image(abs)) {
        return -ENOEXEC;
    }
    if (slot->stop_requested) {
        return -EINTR;
    }
    return espix_proc_exec_commit(slot, abs, argc, argv, envp);
}

static void vfork_exec(espix_proc_slot_t *slot) __attribute__((noreturn));

void espix_posix_exec_go(espix_um_thread_t *t)
{
    espix_proc_slot_t *const slot = t->slot;
    if (slot->app->vfork_jb != NULL) {
        vfork_exec(slot);
    }
    ((posix_t *)t)->threads_ended = espix_pthread_reap(slot, true) > 0;
    longjmp(*slot->exit_jmp, ESPIX_PROC_EXEC_JUMP);
}

void espix_posix_exec_reset(espix_proc_slot_t *slot)
{
    posix_t *const x = (posix_t *)slot->app->posix;
    if (x == NULL) {
        return;
    }
    espix_posix_unmap_all(&x->t);
    espix_um_fds_cloexec(x->t.proc);
    /* A thread ended mid-write may hold a stream's lock: the new program gets fresh ones, and those are left. */
    if (x->threads_ended) {
        x->threads_ended = false;
        std_route(x, slot);
    }
}

/* ------------------------------------------------------------------ */
/* vfork without an MMU                                                */
/* ------------------------------------------------------------------ */

/*
 * The child runs on the parent's task, in the parent's memory, until it execs or exits; libespix's
 * vfork() setjmps in the caller's frame first, and the parent resumes there with the child's pid.
 * The child is a process of its own meanwhile: its pid, descriptors, signal actions and identity.
 */

int32_t espix_posix_vfork_prepare(espix_um_thread_t *t)
{
    posix_t *const px = (posix_t *)t;
    const TaskHandle_t me = xTaskGetCurrentTaskHandle();

    /* This task's earlier ones: only it reads its own, so only it frees them. */
    xSemaphoreTake(t->proc->lock, portMAX_DELAY);
    vjb_t *mine = NULL;
    for (vjb_t **pp = &px->spent; *pp != NULL;) {
        vjb_t *const v = *pp;
        if (v->task == me) {
            *pp = v->next;
            v->next = mine;
            mine = v;
        } else {
            pp = &v->next;
        }
    }
    xSemaphoreGive(t->proc->lock);
    while (mine != NULL) {
        vjb_t *const v = mine;
        mine = v->next;
        free(v);
    }

    vjb_t *const v = calloc(1, sizeof(*v));
    posix_t *const cx = state_new();
    int32_t r = (v != NULL && cx != NULL) ? espix_um_fds_fork(t->proc, cx->t.proc) : -ENOMEM;
    espix_proc_slot_t *child = NULL;
    if (r == 0) {
        r = espix_proc_vfork_child(t->slot, &cx->t, &v->jb, &child);
        if (r != 0) {
            espix_um_fds_drop(cx->t.proc);
        }
    }
    if (r != 0) {
        free(v);
        if (cx != NULL) {
            state_free(cx);
        }
        return r;
    }
    v->task = me;
    cx->t.slot = child;
    cx->t.tid  = child->info.pid;
    std_route(cx, child);
    return (int32_t)(uintptr_t)&v->jb;
}

int32_t espix_posix_vfork_enter(espix_um_thread_t *t, uint32_t jb)
{
    espix_proc_slot_t *child = NULL;
    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);
    for (int i = 0; i < ESPIX_PROC_MAX && child == NULL; i++) {
        espix_proc_slot_t *const s = &g_espix_proc_table.slots[i];
        if (s->info.state != ESPIX_PROC_FREE && s->app != NULL && s->app->vfork_jb == (void *)jb &&
            s->app->vfork_from == t->slot) {
            child = s;
        }
    }
    xSemaphoreGive(g_espix_proc_lock);
    if (child == NULL) {
        return -EINVAL;
    }
    vTaskSetThreadLocalStoragePointer(NULL, ESPIX_TLS_PROC_IDX, child);
    return 0;
}

/* The task back to the parent: at its vfork with the child's pid, or to its end if it was killed meanwhile. */
static void vfork_return(espix_proc_slot_t *parent, void *jb, espix_pid_t cpid) __attribute__((noreturn));
static void vfork_return(espix_proc_slot_t *parent, void *jb, espix_pid_t cpid)
{
    posix_t *const px = (posix_t *)parent->app->posix;
    vjb_t *const v = (vjb_t *)((uint8_t *)jb - offsetof(vjb_t, jb));
    vTaskSetThreadLocalStoragePointer(NULL, ESPIX_TLS_PROC_IDX, parent);
    xSemaphoreTake(px->t.proc->lock, portMAX_DELAY);
    v->next   = px->spent;
    px->spent = v;
    xSemaphoreGive(px->t.proc->lock);

    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);
    parent->vfork_lent--;
    const bool killed = parent->stop_requested;
    const bool ending = parent->exiting;
    const bool main   = parent->info.task == xTaskGetCurrentTaskHandle() && parent->exit_jmp != NULL;
    xSemaphoreGive(g_espix_proc_lock);

    /* A thread of a parent already ending: its teardown waited for this task, and now ends it. */
    if (ending) {
        vTaskSuspend(NULL);
    }
    if (killed && main) {
        if (parent->term_sig == 0) {
            parent->term_sig = SIGKILL;
        }
        parent->exit_status = 128 + parent->term_sig;
        longjmp(*parent->exit_jmp, 1);
    }
    longjmp(*(jmp_buf *)jb, (int)cpid);
}

void espix_posix_vfork_exit(espix_proc_slot_t *slot, int status)
{
    espix_proc_slot_t *const parent = slot->app->vfork_from;
    void *const jb = slot->app->vfork_jb;
    const espix_pid_t cpid = slot->info.pid;
    xSemaphoreTake(g_espix_proc_lock, portMAX_DELAY);
    slot->exiting       = true;
    slot->app->vfork_jb = NULL;
    slot->std_tty       = NULL;
    for (int fd = 0; fd < 3; fd++) {
        slot->own_std[fd] = NULL;       /* the parent's */
    }
    xSemaphoreGive(g_espix_proc_lock);
    espix_proc_release_resources(slot);
    espix_proc_finish(slot, ESPIX_PROC_EXITED, status);
    vfork_return(parent, jb, cpid);
}

/* The child's exec: its new program on a task of its own; the parent's task goes back to it. */
static void vfork_exec(espix_proc_slot_t *slot)
{
    posix_t *const x = (posix_t *)slot->app->posix;
    espix_proc_slot_t *const parent = slot->app->vfork_from;
    void *const jb = slot->app->vfork_jb;
    const espix_pid_t cpid = slot->info.pid;
    espix_posix_unmap_all(&x->t);
    espix_um_fds_cloexec(x->t.proc);
    espix_proc_sig_exec(slot);
    espix_proc_regions_release(slot);
    if (espix_proc_vfork_start(slot) != 0) {
        espix_eprintf(slot->info.session, "espix: %s: no task to run it\n", slot->app->path);
        slot->term_sig = SIGKILL;
        espix_posix_vfork_exit(slot, 128 + SIGKILL);
    }
    vfork_return(parent, jb, cpid);
}


void espix_posix_std_reroute(espix_proc_slot_t *slot)
{
    posix_t *const x = (slot->app != NULL) ? (posix_t *)slot->app->posix : NULL;
    for (int fd = 0; x != NULL && fd < 3; fd++) {
        if (x->std[fd] != NULL) {
            slot->std[fd] = x->std[fd];
        }
    }
}
