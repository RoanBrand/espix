/*
 * uapp: the U-mode test app (docs/USER-MODE.md). Each mode checks one thing the
 * boundary must hold: a fault ends only the app, kernel memory is out of reach,
 * preemption and the FPU work in U-mode, exit codes arrive, the syscalls
 * reach the kernel's files, clocks and identities, and threads share one
 * process.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static unsigned long cycles(void)
{
    unsigned long c;
    __asm__ volatile ("rdcycle %0" : "=r"(c));
    return c;
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

static void *spin_forever(void *arg)
{
    volatile float x = (float)(intptr_t)arg;
    for (;;) {
        x = x * 1.0001f;
    }
    return NULL;
}

int main(int argc, char **argv)
{
    const char *mode = (argc > 1) ? argv[1] : "hello";

    if (strcmp(mode, "hello") == 0) {
        printf("uapp: hello from U-mode, argc %d\n", argc);
        return 0;
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
