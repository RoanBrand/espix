/*
 * uapp: the U-mode test app (docs/USER-MODE.md). Each mode checks one thing the
 * boundary must hold: a fault ends only the app, kernel memory is out of reach,
 * preemption and the FPU work in U-mode, exit codes arrive, the syscalls
 * reach the kernel's files, clocks and identities, threads share one
 * process, signals reach handlers wherever the app is, and sockets and poll
 * block until a peer or a signal ends the wait.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <grp.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

static unsigned long cycles(void)
{
    unsigned long c;
    __asm__ volatile ("rdcycle %0" : "=r"(c));
    return c;
}

static uint64_t cycles64(void)
{
    uint32_t hi, lo, again;
    do {
        __asm__ volatile ("rdcycleh %0" : "=r"(hi));
        __asm__ volatile ("rdcycle %0" : "=r"(lo));
        __asm__ volatile ("rdcycleh %0" : "=r"(again));
    } while (hi != again);
    return ((uint64_t)hi << 32) | lo;
}

static long psram_free_kb(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    char line[64];
    long kb = -1;
    while (f != NULL && fgets(line, sizeof(line), f) != NULL) {
        if (sscanf(line, "PsramFree: %ld", &kb) == 1) {
            break;
        }
    }
    if (f != NULL) {
        fclose(f);
    }
    return kb;
}

/* A kilobyte of stack a level, so 1024 levels need a megabyte. */
static int deep(int n)
{
    volatile char frame[1024];
    frame[0] = (char)n;
    return (n == 0) ? frame[0] : 1 + deep(n - 1) + frame[0] - (char)n;
}

#define CHECK(what, cond)                                                 \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("uapp: files: %s failed: %s\n", (what), strerror(errno)); \
            return 1;                                                       \
        }                                                                   \
    } while (0)

/* A file's life in dir, through every file syscall: create, write, seek, read, stat, rename, list, remove. */
static int files(const char *dir)
{
    char sub[96], a[128], b[128], buf[32];
    snprintf(sub, sizeof(sub), "%s/uapp-d", dir);
    snprintf(a, sizeof(a), "%s/f", sub);
    snprintf(b, sizeof(b), "%s/g", sub);

    CHECK("mkdir", mkdir(sub, 0755) == 0);
    int fd = open(a, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK("open for write", fd >= 3);
    CHECK("write", write(fd, "hello, file", 11) == 11);
    CHECK("close", close(fd) == 0);

    fd = open(a, O_RDONLY);
    CHECK("open for read", fd >= 3);
    CHECK("lseek", lseek(fd, 7, SEEK_SET) == 7);
    CHECK("read", read(fd, buf, sizeof(buf)) == 4 && memcmp(buf, "file", 4) == 0);
    struct stat st;
    CHECK("fstat", fstat(fd, &st) == 0 && st.st_size == 11 && S_ISREG(st.st_mode));
    CHECK("close", close(fd) == 0);

    CHECK("rename", rename(a, b) == 0);
    CHECK("stat after rename", stat(b, &st) == 0 && st.st_size == 11);
    CHECK("the old name is gone", stat(a, &st) != 0 && errno == ENOENT);
    CHECK("truncate", truncate(b, 5) == 0 && stat(b, &st) == 0 && st.st_size == 5);

    DIR *d = opendir(sub);
    CHECK("opendir", d != NULL);
    int seen = 0;
    for (struct dirent *e; (e = readdir(d)) != NULL;) {
        seen += strcmp(e->d_name, "g") == 0;
    }
    CHECK("closedir", closedir(d) == 0);
    CHECK("readdir finds the file", seen == 1);

    CHECK("unlink", unlink(b) == 0);
    CHECK("rmdir", rmdir(sub) == 0);
    CHECK("a closed descriptor is refused", close(fd) != 0 && errno == EBADF);
    printf("uapp: files ok\n");
    return 0;
}

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_cond = PTHREAD_COND_INITIALIZER;
static long            s_count;
static int             s_turn;

static void *bump(void *arg)
{
    const long n = (long)arg;
    for (long i = 0; i < n; i++) {
        pthread_mutex_lock(&s_lock);
        s_count++;
        pthread_mutex_unlock(&s_lock);
    }
    return (void *)n;
}

/* Two threads take turns through a condition variable. */
static void *pingpong(void *arg)
{
    const int me = (int)(intptr_t)arg;
    for (int i = 0; i < 1000; i++) {
        pthread_mutex_lock(&s_lock);
        while (s_turn != me) {
            pthread_cond_wait(&s_cond, &s_lock);
        }
        s_turn = !me;
        pthread_cond_broadcast(&s_cond);
        pthread_mutex_unlock(&s_lock);
    }
    return NULL;
}

/* Four threads add to one counter under a mutex, two play ping-pong, and a timed wait ends at its time. */
static int threads(long n)
{
    enum { T = 4 };
    pthread_t t[T];
    for (int i = 0; i < T; i++) {
        if (pthread_create(&t[i], NULL, bump, (void *)n) != 0) {
            printf("uapp: threads: pthread_create failed\n");
            return 1;
        }
    }
    long want = 0;
    for (int i = 0; i < T; i++) {
        void *r;
        pthread_join(t[i], &r);
        want += (long)r;
    }
    pthread_t a, b;
    pthread_create(&a, NULL, pingpong, (void *)0);
    pthread_create(&b, NULL, pingpong, (void *)1);
    pthread_join(a, NULL);
    pthread_join(b, NULL);

    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_nsec += 100000000L;
    if (until.tv_nsec >= 1000000000L) {
        until.tv_nsec -= 1000000000L;
        until.tv_sec++;
    }
    pthread_mutex_lock(&s_lock);
    const int r = pthread_cond_timedwait(&s_cond, &s_lock, &until);
    pthread_mutex_unlock(&s_lock);
    printf("uapp: threads: counter %ld of %ld, ping-pong done, timed wait %s, tid %s\n", s_count, want,
           (r == ETIMEDOUT) ? "timed out" : "wrong", (gettid() == getpid()) ? "is the pid" : "wrong");
    return 0;
}

static void *exit_from_thread(void *arg)
{
    usleep(50000);
    exit((int)(intptr_t)arg);
}

static void *finish_late(void *arg)
{
    (void)arg;
    usleep(100000);
    printf("uapp: the detached thread finished\n");
    return NULL;
}

static void *deep_in_thread(void *arg)
{
    (void)arg;
    printf("uapp: deep stack in a thread %s\n", deep(1024) == 1024 ? "ok" : "wrong");
    return NULL;
}

static void *churn(void *arg)
{
    const int me = (int)(intptr_t)arg;
    for (int i = 0; i < 2000; i++) {
        const size_t n = 16 + (size_t)((i * 37 + me * 101) % 4000);
        char *p = malloc(n);
        if (p == NULL) {
            printf("uapp: churn: malloc failed\n");
            return (void *)1;
        }
        memset(p, me, n);
        free(p);
        if (i % 500 == 0) {
            printf("uapp: churn %d at %d\n", me, i);
        }
    }
    return NULL;
}

/* Asleep for good, in a condition wait or a sleep, until the process is ended. */
static void *sleep_forever(void *arg)
{
    if (arg != NULL) {
        pthread_mutex_lock(&s_lock);
        for (;;) {
            pthread_cond_wait(&s_cond, &s_lock);
        }
    }
    for (;;) {
        sleep(100);
    }
    return NULL;
}

/*
 * An add on one word from two cores, as an AMO and as LR/SC. A U-mode AMO is not
 * always atomic on this S31, so the kernel does them (CONFIG_ESPIX_UM_AMO_TRAP); with
 * that off this measures the hardware. The loops are their own functions so their
 * layout, which sets the AMO's loss rate, stays fixed.
 */
void uapp_amo_loop(volatile uint32_t *w, long n);
void uapp_lrsc_loop(volatile uint32_t *w, long n);
__asm__("    .text\n"
        "    .balign 64\n"
        "    .global uapp_amo_loop\n"
        "uapp_amo_loop:\n"
        "    li t1, 1\n"
        "1:  amoadd.w.aqrl zero, t1, (a0)\n"
        "    addi a1, a1, -1\n"
        "    bnez a1, 1b\n"
        "    ret\n"
        "    .balign 64\n"
        "    .global uapp_lrsc_loop\n"
        "uapp_lrsc_loop:\n"
        "1:  lr.w.aqrl t2, (a0)\n"
        "    addi t2, t2, 1\n"
        "    sc.w.rl t3, t2, (a0)\n"
        "    bnez t3, 1b\n"
        "    addi a1, a1, -1\n"
        "    bnez a1, 1b\n"
        "    ret\n");

static volatile uint32_t s_amo_word __attribute__((aligned(64)));
static volatile int s_amo_go, s_amo_ready;
static long s_amo_n;
static void (*s_amo_fn)(volatile uint32_t *, long);

static void *amo_thread(void *arg)
{
    *(int *)arg = sched_getcpu();
    __atomic_fetch_add(&s_amo_ready, 1, __ATOMIC_SEQ_CST);
    while (s_amo_go == 0) {
    }
    if (s_amo_go == 1) {
        s_amo_fn(&s_amo_word, s_amo_n);
    }
    return NULL;
}

/* The words lost from 2n increments, with the two threads on different cores; -1 if they never were. */
static long amo_lost(void (*fn)(volatile uint32_t *, long), int cpu[2])
{
    for (int tries = 0; tries < 50; tries++) {
        pthread_t t[2];
        s_amo_word = 0;
        s_amo_go = 0;
        s_amo_ready = 0;
        s_amo_fn = fn;
        for (int i = 0; i < 2; i++) {
            pthread_create(&t[i], NULL, amo_thread, &cpu[i]);
        }
        while (s_amo_ready < 2) {
        }
        const int split = cpu[0] != cpu[1];
        s_amo_go = split ? 1 : 2;
        for (int i = 0; i < 2; i++) {
            pthread_join(t[i], NULL);
        }
        if (split) {
            return 2 * s_amo_n - (long)s_amo_word;
        }
    }
    return -1;
}

static void *spin_forever(void *arg)
{
    volatile float x = (float)(intptr_t)arg;
    for (;;) {
        x = x * 1.0001f;
    }
    return NULL;
}

/* ------------------------------------------------------------------ signals */

static volatile int s_got, s_wrong_thread, s_main_cpu, s_sender_cpu, s_sender_any_cpu;
static volatile unsigned long s_sent_at, s_worst, s_total;
static volatile float s_sink;

/* Runs between any two instructions of the loop it interrupts, and clobbers what its frame must restore. */
static unsigned long now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)ts.tv_sec * 1000000ul + (unsigned long)ts.tv_nsec / 1000ul;
}

static void on_async(int sig)
{
    const unsigned long late = now_us() - s_sent_at;
    if (late > s_worst) {
        s_worst = late;
    }
    s_total += late;
    volatile float f = 3.0f;
    for (int i = 0; i < 50; i++) {
        f = f * 1.5f - 1.0f;
    }
    s_sink = f;
    if (gettid() != getpid()) {
        s_wrong_thread++;
    }
    s_got++;
    (void)sig;
}

/* The sender: on the other core, and blocking the signal itself, so only the main thread, computing, can take it. */
static void *send_usr1(void *arg)
{
    const int n = (int)(intptr_t)arg;
    s_sender_cpu = sched_getcpu();
    if (s_sender_cpu == s_main_cpu && !s_sender_any_cpu) {
        return NULL;
    }
    sigset_t m;
    sigemptyset(&m);
    sigaddset(&m, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &m, NULL);
    for (int i = 0; i < n; i++) {
        const int before = s_got;
        s_sent_at = now_us();
        kill(getpid(), SIGUSR1);
        for (int ms = 0; s_got == before && ms < 1000; ms++) {
            usleep(1000);
        }
    }
    return NULL;
}

/* An FNV hash and a float count: what a handler that broke a register would change. */
static int sig_async(int n)
{
    signal(SIGUSR1, on_async);
    uint32_t want = 0;
    int wrong = 0, rounds = 0;
    pthread_t t;
    s_main_cpu = sched_getcpu();
    for (int tries = 0;; tries++) {
        s_sender_cpu     = -1;
        s_sender_any_cpu = tries == 20;
        pthread_create(&t, NULL, send_usr1, (void *)(intptr_t)n);
        while (s_sender_cpu < 0) {
        }
        if (s_sender_cpu != s_main_cpu || s_sender_any_cpu) {
            break;
        }
        pthread_join(t, NULL);
    }
    while (s_got < n && rounds < 200000) {
        uint32_t h = 2166136261u;
        float f = 0.0f;
        for (int i = 0; i < 4096; i++) {
            h = (h ^ (uint32_t)i) * 16777619u;
            f += 1.0f;
        }
        if (rounds++ == 0) {
            want = h;
        }
        wrong += (h != want || f != 4096.0f);
    }
    pthread_join(t, NULL);
    printf("uapp: %d of %d handlers ran in a compute loop, %d wrong results, %d on another thread, "
           "%lu us on average and at most %lu after a kill from core %d to core %d\n", s_got, n, wrong,
           s_wrong_thread, s_got ? s_total / (unsigned long)s_got : 0, s_worst, s_sender_cpu, s_main_cpu);
    return (s_got == n && wrong == 0 && s_wrong_thread == 0) ? 0 : 1;
}

static void on_count(int sig)
{
    (void)sig;
    s_got++;
}

static sigset_t only(int sig)
{
    sigset_t m;
    sigemptyset(&m);
    sigaddset(&m, sig);
    return m;
}

/* A blocked signal waits; unblocked, it runs before sigprocmask returns; SA_RESETHAND; sigsuspend. */
static int sig_mask(void)
{
    const sigset_t usr1 = only(SIGUSR1), alrm = only(SIGALRM);
    signal(SIGUSR1, on_count);
    sigprocmask(SIG_BLOCK, &usr1, NULL);
    raise(SIGUSR1);
    const int held = s_got;
    sigset_t p;
    sigpending(&p);
    sigprocmask(SIG_UNBLOCK, &usr1, NULL);
    const int ran = s_got;

    struct sigaction sa = { .sa_handler = on_count, .sa_flags = SA_RESETHAND }, now;
    sigaction(SIGUSR2, &sa, NULL);
    raise(SIGUSR2);
    sigaction(SIGUSR2, NULL, &now);

    signal(SIGALRM, on_count);
    sigprocmask(SIG_BLOCK, &alrm, NULL);
    alarm(1);
    sigset_t during, after;
    sigprocmask(SIG_BLOCK, NULL, &during);
    sigdelset(&during, SIGALRM);
    const int before = s_got;
    const int rc = sigsuspend(&during), e = errno;
    sigprocmask(SIG_BLOCK, NULL, &after);

    const bool ok = held == 0 && sigismember(&p, SIGUSR1) && ran == 1 && now.sa_handler == SIG_DFL &&
                    rc == -1 && e == EINTR && s_got == before + 1 && sigismember(&after, SIGALRM);
    printf("uapp: mask: held %d, pending %d, ran %d, reset %d, suspend %d errno %d woke %d, mask back %d: %s\n",
           held, sigismember(&p, SIGUSR1), ran, now.sa_handler == SIG_DFL, rc, e, s_got - before,
           sigismember(&after, SIGALRM), ok ? "ok" : "wrong");
    return ok ? 0 : 1;
}

static jmp_buf s_jb;
static volatile uintptr_t s_fault_at, s_bad_addr = 16;

static void on_segv(int sig, siginfo_t *si, void *uc)
{
    (void)sig;
    (void)uc;
    s_fault_at = (uintptr_t)si->si_addr;
    longjmp(s_jb, 1);
}

/* A fault reaches its handler with the address; with SIGSEGV still blocked, the next one ends the app. */
static int sig_fault(void)
{
    struct sigaction sa = { .sa_sigaction = on_segv, .sa_flags = SA_SIGINFO };
    sigaction(SIGSEGV, &sa, NULL);
    if (setjmp(s_jb) == 0) {
        *(volatile int *)s_bad_addr = 1;
        printf("uapp: the fault did not happen\n");
        return 1;
    }
    printf("uapp: caught SIGSEGV at 0x%x\n", (unsigned)s_fault_at);
    fflush(stdout);
    *(volatile int *)s_bad_addr = 1;
    return 0;
}

/* A read interrupted by a handler: restarted under SA_RESTART, EINTR otherwise. */
static int sig_read(bool restart)
{
    struct sigaction sa = { .sa_handler = on_count, .sa_flags = restart ? SA_RESTART : 0 };
    sigaction(SIGALRM, &sa, NULL);
    alarm(1);
    char buf[32];
    const ssize_t n = read(0, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[strcspn(buf, "\n")] = '\0';
        printf("uapp: read '%.*s', handler ran %d\n", (int)n, buf, s_got);
    } else {
        printf("uapp: read %d errno %d, handler ran %d\n", (int)n, errno, s_got);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Sockets                                                             */
/* ------------------------------------------------------------------ */

static long ms_since(const struct timespec *t0)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long)(t.tv_sec - t0->tv_sec) * 1000 + (t.tv_nsec - t0->tv_nsec) / 1000000;
}

/* A socket bound to 127.0.0.1 and a port the stack picks, listening if it is a stream; its address at a. */
static int bound_lo(int type, struct sockaddr_in *a)
{
    const int fd = socket(AF_INET, type, 0);
    *a = (struct sockaddr_in){ .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t len = sizeof(*a);
    if (fd < 0 || bind(fd, (struct sockaddr *)a, sizeof(*a)) != 0 ||
        (type == SOCK_STREAM && listen(fd, 2) != 0) || getsockname(fd, (struct sockaddr *)a, &len) != 0) {
        printf("uapp: socket: %s\n", strerror(errno));
        return -1;
    }
    return fd;
}

/* An accepted loopback connection: the server's end, with the client's at *client. */
static int pair_lo(int *client)
{
    struct sockaddr_in a;
    const int lfd = bound_lo(SOCK_STREAM, &a);
    *client = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0 || *client < 0 || connect(*client, (struct sockaddr *)&a, sizeof(a)) != 0) {
        printf("uapp: connect: %s\n", strerror(errno));
        return -1;
    }
    const int s = accept(lfd, NULL, NULL);
    close(lfd);
    return s;
}

static char s_out[20000], s_in[20000];
static int  s_tx;

static void *sender(void *arg)
{
    const int fd = *(int *)arg;
    size_t done = 0;
    while (done < sizeof(s_out)) {
        const ssize_t n = send(fd, s_out + done, sizeof(s_out) - done, 0);
        if (n <= 0) {
            break;
        }
        done += (size_t)n;
    }
    shutdown(fd, SHUT_WR);
    return NULL;
}

/* 20000 bytes across page boundaries, more than the stack buffers: the sender blocks until they drain. */
static int net_tcp(void)
{
    int c;
    const int s = pair_lo(&c);
    if (s < 0) {
        return 1;
    }
    for (size_t i = 0; i < sizeof(s_out); i++) {
        s_out[i] = (char)(i * 7 + 3);
    }
    pthread_t th;
    s_tx = c;
    pthread_create(&th, NULL, sender, &s_tx);
    size_t got = 0;
    ssize_t n;
    while ((n = recv(s, s_in + got, sizeof(s_in) - got, 0)) > 0) {
        got += (size_t)n;
    }
    pthread_join(th, NULL);
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    char ip[INET_ADDRSTRLEN] = "?";
    if (getpeername(s, (struct sockaddr *)&peer, &plen) == 0) {
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
    }
    printf("uapp: tcp %u bytes %s, then end of stream %d, peer %s\n", (unsigned)got,
           (got == sizeof(s_in) && memcmp(s_in, s_out, got) == 0) ? "intact" : "WRONG", (int)n, ip);
    close(s);
    close(c);
    return 0;
}

/* Non-blocking accept, connect and recv, and poll's readiness and timeout. */
static int net_nb(void)
{
    struct sockaddr_in a;
    const int lfd = bound_lo(SOCK_STREAM, &a);
    if (lfd < 0) {
        return 1;
    }
    fcntl(lfd, F_SETFL, fcntl(lfd, F_GETFL) | O_NONBLOCK);
    const int acc = accept(lfd, NULL, NULL);
    const int acc_err = errno;
    const int c = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    const int con = connect(c, (struct sockaddr *)&a, sizeof(a));
    const int con_err = errno;
    struct pollfd p[2] = { { .fd = c, .events = POLLOUT }, { .fd = lfd, .events = POLLIN } };
    const int ready = poll(p, 2, 2000);
    int soerr = -1;
    socklen_t len = sizeof(soerr);
    getsockopt(c, SOL_SOCKET, SO_ERROR, &soerr, &len);
    const int s = accept4(lfd, NULL, NULL, SOCK_NONBLOCK);
    char b;
    const ssize_t r = recv(s, &b, 1, 0);
    const int r_err = errno;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    struct pollfd q = { .fd = s, .events = POLLIN };
    const int idle = poll(&q, 1, 300);
    printf("uapp: nb accept %d/%s, connect %d/%s, poll %d (out %d, in %d), so_error %d, recv %d/%s, "
           "idle poll %d after %ld ms\n",
           acc, strerror(acc_err), con, strerror(con_err), ready, !!(p[0].revents & POLLOUT),
           !!(p[1].revents & POLLIN), soerr, (int)r, strerror(r_err), idle, ms_since(&t0));
    return 0;
}

/* Blocks SIGALRM, so the main thread's wait is the one the alarm ends. */
static void *late_sender(void *arg)
{
    sigset_t m;
    sigemptyset(&m);
    sigaddset(&m, SIGALRM);
    pthread_sigmask(SIG_BLOCK, &m, NULL);
    usleep(1500 * 1000);
    send(*(int *)arg, "late", 4, 0);
    return NULL;
}

/* A blocking recv or poll and SIGALRM at 1 s: EINTR, or the recv restarted under SA_RESTART. */
static int net_intr(const char *how)
{
    int c;
    const int s = pair_lo(&c);
    if (s < 0) {
        return 1;
    }
    const bool restart = strcmp(how, "recv") != 0;
    struct sigaction sa = { .sa_handler = on_count, .sa_flags = restart ? SA_RESTART : 0 };
    sigaction(SIGALRM, &sa, NULL);
    pthread_t th;
    s_tx = c;
    pthread_create(&th, NULL, late_sender, &s_tx);
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    alarm(1);
    char buf[8];
    int rc;
    if (strcmp(how, "poll") == 0) {
        struct pollfd p = { .fd = s, .events = POLLIN };
        rc = poll(&p, 1, 5000);
    } else {
        rc = (int)recv(s, buf, sizeof(buf), 0);
    }
    const int err = (rc < 0) ? errno : 0;
    printf("uapp: %s rc=%d errno=%d after %ld ms, handler ran %d\n", how, rc, err, ms_since(&t0), s_got);
    pthread_join(th, NULL);
    return 0;
}

/* A datagram over a page boundary, and who sent it. */
static int net_udp(void)
{
    struct sockaddr_in a, from;
    const int fd = bound_lo(SOCK_DGRAM, &a);
    const int c = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0 || c < 0) {
        return 1;
    }
    for (int i = 0; i < 3000; i++) {
        s_out[4090 + i] = (char)i;
    }
    const ssize_t sent = sendto(c, s_out + 4090, 3000, 0, (struct sockaddr *)&a, sizeof(a));
    socklen_t flen = sizeof(from);
    const ssize_t got = recvfrom(fd, s_in + 4090, 4000, 0, (struct sockaddr *)&from, &flen);
    char ip[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
    printf("uapp: udp sent %d, got %d %s from %s\n", (int)sent, (int)got,
           (got == 3000 && memcmp(s_in + 4090, s_out + 4090, 3000) == 0) ? "intact" : "WRONG", ip);
    return 0;
}

/* Name and address conversions, and the identities and entropy that come with them. */
static int net_names(void)
{
    static const char *v6[] = { "fe80::1", "::1", "::", "::ffff:10.0.0.7", "2001:db8::8:800:200c:417a",
                                "1:2:3:4:5:6:7:8" };
    for (size_t i = 0; i < sizeof(v6) / sizeof(v6[0]); i++) {
        struct in6_addr x;
        char back[INET6_ADDRSTRLEN];
        const int ok = inet_pton(AF_INET6, v6[i], &x) == 1 && inet_ntop(AF_INET6, &x, back, sizeof(back)) != NULL;
        printf("uapp: v6 %s -> %s\n", v6[i], ok ? back : "FAILED");
    }
    struct in_addr v4;
    printf("uapp: v4 %s, bad %d\n", inet_pton(AF_INET, "192.168.1.10", &v4) == 1 ? inet_ntoa(v4) : "FAILED",
           inet_pton(AF_INET, "192.168.01.1", &v4));

    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res;
    int r = getaddrinfo("127.0.0.1", "1883", &hints, &res);
    if (r == 0) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)res->ai_addr;
        printf("uapp: getaddrinfo port %u type %d\n", ntohs(sin->sin_port), res->ai_socktype);
        freeaddrinfo(res);
    } else {
        printf("uapp: getaddrinfo: %s\n", gai_strerror(r));
    }
    hints = (struct addrinfo){ .ai_flags = AI_PASSIVE, .ai_family = AF_INET };
    int n = 0;
    if (getaddrinfo(NULL, "8080", &hints, &res) == 0) {
        for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
            n++;
        }
        freeaddrinfo(res);
    }
    printf("uapp: passive, any type: %d results\n", n);

    char host[65];
    gethostname(host, sizeof(host));
    const struct passwd *pw = getpwuid(getuid());
    const struct group *gr = getgrgid(getgid());
    unsigned char rnd[16] = { 0 };
    int set = 0;
    getrandom(rnd, sizeof(rnd), 0);
    for (size_t i = 0; i < sizeof(rnd); i++) {
        set |= rnd[i];
    }
    printf("uapp: host %s, user %s home %s, group %s, random %s\n", host, pw ? pw->pw_name : "?",
           pw ? pw->pw_dir : "?", gr ? gr->gr_name : "?", set ? "yes" : "zeros");
    return 0;
}

/* syslog into the kernel log, the interface list, and a dlopen that says why not. */
static int net_posix(void)
{
    openlog("uapp", LOG_PID, LOG_USER);
    syslog(LOG_WARNING, "syslog from U-mode %d", 42);
    closelog();
    struct ifaddrs *ifs;
    if (getifaddrs(&ifs) != 0) {
        printf("uapp: getifaddrs: %s\n", strerror(errno));
        return 1;
    }
    for (struct ifaddrs *i = ifs; i != NULL; i = i->ifa_next) {
        char a[INET6_ADDRSTRLEN] = "-";
        if (i->ifa_addr != NULL && i->ifa_addr->sa_family == AF_INET) {
            inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, a, sizeof(a));
        } else if (i->ifa_addr != NULL && i->ifa_addr->sa_family == AF_INET6) {
            inet_ntop(AF_INET6, &((struct sockaddr_in6 *)i->ifa_addr)->sin6_addr, a, sizeof(a));
        }
        printf("uapp: if %s %s%s %s\n", i->ifa_name, (i->ifa_flags & IFF_UP) ? "up" : "down",
               (i->ifa_flags & IFF_LOOPBACK) ? " loopback" : "", a);
    }
    freeifaddrs(ifs);
    printf("uapp: dlopen %p: %s\n", dlopen("x.so", RTLD_NOW), dlerror());
    return 0;
}

/* Blocks in accept until something ends it: a signal's way into a socket wait. */
static int net_wait(void)
{
    struct sockaddr_in a;
    const int lfd = bound_lo(SOCK_STREAM, &a);
    if (lfd < 0) {
        return 1;
    }
    printf("uapp: waiting on port %u\n", ntohs(a.sin_port));
    fflush(stdout);
    const int s = accept(lfd, NULL, NULL);
    printf("uapp: accept %d errno %d\n", s, errno);
    return 0;
}

int main(int argc, char **argv)
{
    const char *mode = (argc > 1) ? argv[1] : "hello";

    if (strcmp(mode, "hello") == 0) {
        printf("uapp: hello from U-mode, argc %d\n", argc);
        return 0;
    }
    if (strcmp(mode, "tcp") == 0) {
        return net_tcp();
    }
    if (strcmp(mode, "nb") == 0) {
        return net_nb();
    }
    if (strcmp(mode, "netintr") == 0 && argc > 2) {
        return net_intr(argv[2]);
    }
    if (strcmp(mode, "udp") == 0) {
        return net_udp();
    }
    if (strcmp(mode, "names") == 0) {
        return net_names();
    }
    if (strcmp(mode, "posix") == 0) {
        return net_posix();
    }
    if (strcmp(mode, "acceptwait") == 0) {
        return net_wait();
    }
    if (strcmp(mode, "exit") == 0) {
        return (argc > 2) ? atoi(argv[2]) : 0;
    }
    if (strcmp(mode, "null") == 0) {
        volatile int *p = NULL;
        *p = 1;
        return 0;
    }
    if (strcmp(mode, "kernel") == 0) {
        volatile unsigned *p = (volatile unsigned *)0x2f000000;
        printf("uapp: kernel memory says 0x%08x\n", *p);
        return 0;
    }
    if (strcmp(mode, "exec-data") == 0) {
        static unsigned code[2] = { 0x00008067u, 0 };
        ((void (*)(void))(void *)code)();
        return 0;
    }
    if (strcmp(mode, "spin") == 0 || strcmp(mode, "spinq") == 0) {
        /* Seconds of floating point in U-mode: the tick preempts it, and the FPU state must survive. */
        const int secs = (argc > 2) ? atoi(argv[2]) : 3;
        const unsigned long until = cycles() + (unsigned long)secs * 320000000ul;
        float x = 1.0f;
        unsigned long n = 0;
        while ((long)(until - cycles()) > 0) {
            x = x * 1.0000001f + 0.5f;
            if (x > 1e6f) {
                x = 1.0f;
            }
            n++;
        }
        if (strcmp(mode, "spin") == 0) {
            printf("uapp: spun %lu iterations, x %d\n", n, (int)x);
        }
        return (int)(n & 1);
    }
    if (strcmp(mode, "pauses") == 0 && argc > 3) {
        /* Computes for secs with no syscall, then writes the longest it went without running: a stop's length. */
        const uint64_t start = cycles64(), end = start + (uint64_t)atoi(argv[2]) * 320000000u;
        uint64_t prev = start, worst = 0;
        volatile float x = 1.0f;
        for (uint64_t now = start; now < end; prev = now) {
            x = x * 1.0001f;
            now = cycles64();
            if (now - prev > worst) {
                worst = now - prev;
            }
        }
        FILE *f = fopen(argv[3], "w");
        if (f == NULL) {
            return 1;
        }
        fprintf(f, "uapp: longest pause %lu ms\n", (unsigned long)(worst / 320000u));
        fclose(f);
        return 0;
    }
    if (strcmp(mode, "ispin") == 0 || strcmp(mode, "ispinp") == 0) {
        /* The spin without floating point. */
        const int secs = (argc > 2) ? atoi(argv[2]) : 3;
        const unsigned long until = cycles() + (unsigned long)secs * 320000000ul;
        volatile unsigned long n = 0;
        while ((long)(until - cycles()) > 0) {
            n++;
        }
        if (strcmp(mode, "ispinp") == 0) {
            printf("uapp: counted %lu\n", (unsigned long)n);
        }
        return (int)(n & 1);
    }
    if (strcmp(mode, "files") == 0 && argc > 2) {
        return files(argv[2]);
    }
    if (strcmp(mode, "read") == 0 && argc > 2) {
        /* The first line of a file, or why not: the kernel's permission check, reached by syscall. */
        FILE *f = fopen(argv[2], "r");
        char line[128];
        if (f == NULL) {
            printf("uapp: %s: %s\n", argv[2], strerror(errno));
            return 1;
        }
        if (fgets(line, sizeof(line), f) != NULL) {
            /* A prefix and a newline: enough to name the file, and no hash in a log. */
            printf("uapp: %.12s\n", line);
        }
        fclose(f);
        return 0;
    }
    if (strcmp(mode, "cwd") == 0 && argc > 2) {
        char buf[128];
        if (chdir(argv[2]) != 0 || getcwd(buf, sizeof(buf)) == NULL) {
            printf("uapp: cwd: %s\n", strerror(errno));
            return 1;
        }
        printf("uapp: cwd %s\n", buf);
        return 0;
    }
    if (strcmp(mode, "ids") == 0) {
        printf("uapp: uid=%d euid=%d gid=%d pid=%d ppid=%d\n", (int)getuid(), (int)geteuid(),
               (int)getgid(), (int)getpid(), (int)getppid());
        return 0;
    }
    if (strcmp(mode, "sleep") == 0 && argc > 2) {
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        usleep((useconds_t)atoi(argv[2]) * 1000);
        clock_gettime(CLOCK_MONOTONIC, &b);
        const long ms = (long)(b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
        printf("uapp: slept %ld ms\n", ms);
        return 0;
    }
    if (strcmp(mode, "fsleep") == 0 && argc > 2) {
        /* Floating point in flight, then a long sleep: a kill lands while the FPU holds its state. */
        volatile float x = 1.5f;
        for (int i = 0; i < 1000; i++) {
            x = x * 1.0001f + 0.25f;
        }
        printf("uapp: fsleep %d\n", (int)x);
        fflush(stdout);
        usleep((useconds_t)atoi(argv[2]) * 1000);
        printf("uapp: woke %d\n", (int)(x * 2.0f));
        return 0;
    }
    if (strcmp(mode, "cat") == 0) {
        char buf[64];
        ssize_t n;
        while ((n = read(0, buf, sizeof(buf))) > 0) {
            write(1, buf, (size_t)n);
        }
        return 0;
    }
    if (strcmp(mode, "redirect") == 0 && argc > 2) {
        /* dup2 onto stdout, as a shell does for >: printf then lands in the file. */
        const int fd = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0 || dup2(fd, 1) != 1) {
            fprintf(stderr, "uapp: redirect: %s\n", strerror(errno));
            return 1;
        }
        close(fd);
        printf("uapp: into the file\n");
        return 0;
    }
    if (strcmp(mode, "bigheap") == 0) {
        /* 4 MB reserved by brk, 64 pages touched: only what is touched may cost memory.
         * Not malloc: picolibc's zero-fills every block it returns, which touches it all. */
        const long before = psram_free_kb();
        volatile char *p = sbrk(4 << 20);
        const long reserved = psram_free_kb();
        if (p == (void *)-1) {
            printf("uapp: bigheap: sbrk failed\n");
            return 1;
        }
        for (unsigned off = 0; off < (4u << 20); off += 64u << 10) {
            p[off] = 1;
        }
        const long touched = psram_free_kb();
        printf("uapp: bigheap: reserving cost %ld KB, touching 64 pages %ld KB\n",
               before - reserved, reserved - touched);
        return 0;
    }
    if (strcmp(mode, "brkshrink") == 0) {
        /* 2 MB grown and touched, then given back: the frames return with it. */
        const long before = psram_free_kb();
        volatile char *p = sbrk(2 << 20);
        if (p == (void *)-1) {
            printf("uapp: brkshrink: sbrk failed\n");
            return 1;
        }
        for (unsigned off = 0; off < (2u << 20); off += 4096) {
            p[off] = 1;
        }
        const long grown = psram_free_kb();
        sbrk(-(2 << 20));
        const long after = psram_free_kb();
        printf("uapp: brkshrink: grew %ld KB, returned %ld KB\n", before - grown, after - grown);
        return 0;
    }
    if (strcmp(mode, "deepstack") == 0) {
        printf("uapp: deep stack %s\n", deep(1024) == 1024 ? "ok" : "wrong");
        return 0;
    }
    if (strcmp(mode, "guard") == 0) {
        /* The page below the stack's reservation is the guard. */
        volatile char *p = (volatile char *)(0x80000000u - (8u << 20) - 16);
        *p = 1;
        return 0;
    }
    if (strcmp(mode, "sysloop") == 0 && argc > 2) {
        /* Seconds of back-to-back syscalls, with floating point in flight so this thread owns the FPU. */
        const unsigned long until = cycles() + (unsigned long)atoi(argv[2]) * 320000000ul;
        volatile float x = 1.0f;
        unsigned long n = 0;
        while ((long)(until - cycles()) > 0) {
            (void)getppid();
            x = x * 1.0001f;
            n++;
        }
        printf("uapp: %lu syscalls, x %d\n", n, (int)x);
        return 0;
    }
    if (strcmp(mode, "bench") == 0) {
        /* The cost of a syscall that does nothing: getppid, timed with the cycle counter. */
        const int n = 10000;
        const unsigned long t0 = cycles();
        for (int i = 0; i < n; i++) {
            (void)getppid();
        }
        const unsigned long per = (cycles() - t0) / (unsigned long)n;
        printf("uapp: a syscall takes %lu cycles, %lu ns\n", per, per * 1000ul / 320ul);
        return 0;
    }
    if (strcmp(mode, "sigasync") == 0) {
        return sig_async((argc > 2) ? atoi(argv[2]) : 50);
    }
    if (strcmp(mode, "sigmask") == 0) {
        return sig_mask();
    }
    if (strcmp(mode, "sigfault") == 0) {
        return sig_fault();
    }
    if (strcmp(mode, "sigread") == 0 && argc > 2) {
        return sig_read(strcmp(argv[2], "restart") == 0);
    }
    if (strcmp(mode, "threads") == 0 && argc > 2) {
        return threads(atol(argv[2]));
    }
    if (strcmp(mode, "texit") == 0 && argc > 2) {
        /* exit() in a thread ends the process, while the main thread waits in a join. */
        pthread_t t;
        pthread_create(&t, NULL, exit_from_thread, (void *)(intptr_t)atoi(argv[2]));
        pthread_join(t, NULL);
        return 99;
    }
    if (strcmp(mode, "tdetach") == 0) {
        /* The main thread leaves first; the process lives until the detached thread ends. */
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        pthread_t t;
        pthread_create(&t, &a, finish_late, NULL);
        printf("uapp: the main thread leaves\n");
        pthread_exit(NULL);
    }
    if (strcmp(mode, "tstack") == 0) {
        pthread_t t;
        pthread_create(&t, NULL, deep_in_thread, NULL);
        pthread_join(t, NULL);
        return 0;
    }
    if (strcmp(mode, "amo") == 0) {
        s_amo_n = (argc > 2) ? atol(argv[2]) : 200000;
        int cpu[2];
        const unsigned long c0 = cycles();
        const long amo = amo_lost(uapp_amo_loop, cpu);
        const unsigned long c1 = cycles();
        const long lrsc = amo_lost(uapp_lrsc_loop, cpu);
        const unsigned long c2 = cycles();
        printf("uapp: adds on cores %d and %d, %ld each: amoadd lost %ld (%lu cycles each), "
               "lr/sc lost %ld (%lu cycles each)\n", cpu[0], cpu[1], s_amo_n, amo, (c1 - c0) / s_amo_n,
               lrsc, (c2 - c1) / s_amo_n);
        return amo != 0 || lrsc != 0;
    }
    if (strcmp(mode, "tchurn") == 0) {
        pthread_t t[4];
        for (int i = 0; i < 4; i++) {
            pthread_create(&t[i], NULL, churn, (void *)(intptr_t)(i + 1));
        }
        int bad = 0;
        for (int i = 0; i < 4; i++) {
            void *r;
            pthread_join(t[i], &r);
            bad += (r != NULL);
        }
        printf("uapp: churn %s\n", bad ? "failed" : "ok");
        return bad;
    }
    if (strcmp(mode, "tsleep") == 0 || strcmp(mode, "tspin") == 0) {
        /* Threads to end with the process: asleep in the kernel, or computing in U. */
        void *(*fn)(void *) = (mode[1] == 's' && mode[2] == 'l') ? sleep_forever : spin_forever;
        pthread_t t[3];
        for (int i = 0; i < 3; i++) {
            pthread_create(&t[i], NULL, fn, (void *)(intptr_t)(i & 1));
        }
        printf("uapp: %s: 3 threads started\n", mode);
        fflush(stdout);
        pthread_join(t[0], NULL);
        return 0;
    }
    fprintf(stderr, "uapp: unknown mode %s\n", mode);
    return 2;
}
