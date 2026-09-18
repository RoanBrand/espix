/*
 * espix test app -- levers for the test suite to pull.
 *
 * Not an example (apps/hello is that) and not part of the firmware. Every
 * subcommand here exists because something was being checked by hand, usually
 * repeatedly: the process root, the chmod seam that bypasses the VFS, exit
 * statuses reaching the client, and output volume large enough to provoke the
 * SSH transport.
 *
 * Everything it uses has to resolve against the tables the firmware exports at
 * load time, so it doubles as an ABI test: if it loads at all, every symbol
 * below was published.
 *
 * Prints one line per result, machine-readable enough for a shell test to
 * assert on with a substring match and no parsing.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

/* errno by name where a test would otherwise be asserting on a number, and the
 * distinction between "not there" and "not allowed" is the whole point of
 * several of them. */
static const char *errno_name(int e)
{
    switch (e) {
    case ENOENT:  return "ENOENT";
    case EACCES:  return "EACCES";
    case EPERM:   return "EPERM";
    case EISDIR:  return "EISDIR";
    case EEXIST:  return "EEXIST";
    case ENOTDIR: return "ENOTDIR";
    case EINVAL:  return "EINVAL";
    case ENOMEM:  return "ENOMEM";
    default:      break;
    }

    /*
     * Anything else by number rather than "?".
     *
     * A run failed `chmod <path> ?` under load and that was the whole report:
     * the suite established that chmod had failed and then declined to say why,
     * on a test that only fails under load -- which is exactly the case where
     * nobody can reproduce it afterwards to ask again.
     *
     * Static buffer, because the caller wants a string. This app is
     * single-threaded and prints the result immediately, so the one buffer is
     * enough; a second call before the first is printed would not be.
     */
    static char other[24];
    snprintf(other, sizeof(other), "errno %d", e);
    return other;
}

static int cmd_probe(int argc, char **argv)
{
    for (int i = 0; i < argc; i++) {
        FILE *f = fopen(argv[i], "r");
        if (f != NULL) {
            fclose(f);
            printf("probe %s ok\n", argv[i]);
        } else {
            printf("probe %s %s\n", argv[i], errno_name(errno));
        }
    }
    return 0;
}

static int cmd_chmod(const char *path, const char *octal)
{
    const mode_t mode = (mode_t)strtol(octal, NULL, 8);

    if (chmod(path, mode) == 0) {
        printf("chmod %s ok\n", path);
        return 0;
    }
    printf("chmod %s %s\n", path, errno_name(errno));
    return 1;
}

/*
 * Ownership as an app sees it: stat() by path and fstat() by descriptor. The
 * expected uid comes from the suite: this reports, it does not judge.
 */
/*
 * Who the app is, as the ABI answers it. The shell's `id` is the other side of
 * the comparison a suite makes.
 */
static int cmd_id(void)
{
    printf("uid=%u gid=%u euid=%u egid=%u\n", (unsigned)getuid(),
           (unsigned)getgid(), (unsigned)geteuid(), (unsigned)getegid());
    return 0;
}

static int cmd_stat(const char *path)
{
    struct stat st;

    if (stat(path, &st) != 0) {
        printf("stat %s %s\n", path, errno_name(errno));
        return 1;
    }

    printf("stat %s uid=%u gid=%u\n", path, (unsigned)st.st_uid,
           (unsigned)st.st_gid);

    /* fstat() has no path to answer from, which is where the two part company. */
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        struct stat by_fd;
        if (fstat(fd, &by_fd) == 0) {
            printf("fstat %s uid=%u gid=%u\n", path, (unsigned)by_fd.st_uid,
                   (unsigned)by_fd.st_gid);
        }
        close(fd);
    }

    return 0;
}

static int cmd_cd(const char *path)
{
    char cwd[128];

    if (chdir(path) != 0) {
        printf("cd %s %s\n", path, errno_name(errno));
    } else {
        printf("cd %s ok\n", path);
    }
    printf("cwd %s\n", getcwd(cwd, sizeof(cwd)) ? cwd : "(failed)");
    return 0;
}

static int cmd_write(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");

    if (f == NULL) {
        printf("write %s %s\n", path, errno_name(errno));
        return 1;
    }
    fputs(text, f);
    fclose(f);
    printf("write %s ok\n", path);
    return 0;
}

static int cmd_read(const char *path)
{
    FILE *f = fopen(path, "r");

    if (f == NULL) {
        printf("read %s %s\n", path, errno_name(errno));
        return 1;
    }

    char line[256] = { 0 };
    const char *got = fgets(line, sizeof(line), f);
    fclose(f);

    line[strcspn(line, "\r\n")] = '\0';
    printf("read %s [%s]\n", path, got ? line : "");
    return 0;
}

/*
 * Bytes at an offset, hex and ASCII.
 *
 * A file, not a device: espix refuses to open a block node from userland on
 * purpose (see the note on espix_dev_open in dev.c), so `hexdump /dev/sda3`
 * answers EOPNOTSUPP and always will. It was written hoping to read an ext4
 * superblock that way, and could not -- worth knowing before trying again. The
 * driver that *can* read one logs it itself; see log_why_not_mounted() in
 * components/espix_fs/ext.c.
 *
 * open/lseek/read rather than stdio, because lseek is what makes an offset
 * possible at all -- and it is also the call under test when a driver's reads
 * are in doubt, so exercising it here is not incidental.
 */
static int cmd_hexdump(const char *path, const char *off_s, const char *len_s)
{
    /* strtoll, not strtol: off_t is 64 bits now, and an offset past 4 GiB is
     * exactly what this tool gets pointed at. */
    const long long off = strtoll(off_s, NULL, 0);
    const long long len = strtoll(len_s, NULL, 0);

    if (off < 0 || len <= 0 || len > 4096) {
        printf("hexdump: offset >= 0, length 1..4096\n");
        return 2;
    }

    const int fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("hexdump %s %s\n", path, errno_name(errno));
        return 1;
    }

    if (lseek(fd, off, SEEK_SET) != off) {
        printf("hexdump lseek %s\n", errno_name(errno));
        close(fd);
        return 1;
    }

    static unsigned char buf[4096];
    const ssize_t got = read(fd, buf, (size_t)len);
    close(fd);

    if (got < 0) {
        printf("hexdump read %s\n", errno_name(errno));
        return 1;
    }

    for (ssize_t i = 0; i < got; i += 16) {
        printf("%08llx  ", (unsigned long long)(off + i));
        for (int j = 0; j < 16; j++) {
            if (i + j < got) {
                printf("%02x ", buf[i + j]);
            } else {
                printf("   ");
            }
            if (j == 7) {
                printf(" ");
            }
        }
        printf(" |");
        for (int j = 0; j < 16 && i + j < got; j++) {
            const unsigned char c = buf[i + j];
            printf("%c", (c >= 32 && c < 127) ? c : '.');
        }
        printf("|\n");
    }

    printf("hexdump %s %lld bytes at %lld\n", path, (long long)got, off);
    return 0;
}

/*
 * n lines of output, on demand.
 *
 * This is the handle on the `Corrupted MAC` failure in docs/KNOWN-ISSUES.md:
 * it takes an app writing several lines to provoke, and reproducing it used to
 * mean running the example app thirty times and counting. A loop over `out 200`
 * gives a rate in one connection instead -- and, importantly, on a build with
 * no tracing in it, which is what that investigation concluded it needed and
 * never had.
 */
static int cmd_out(const char *count, const char *width)
{
    static const char filler_default[] =
        "the quick brown fox jumps over the lazy dog";
    static const char filler[] =
        "the quick brown fox jumps over the lazy dog, and then does it again "
        "and again and again until the line is as long as it needs to be, "
        "which is the entire point of this particular piece of padding here.";

    const long n = strtol(count, NULL, 10);
    const long w = (width != NULL) ? strtol(width, NULL, 10) : 0;

    /*
     * `width` is what separates two variables that are otherwise welded
     * together.
     *
     * An app's stdout is line-buffered with a 128-byte buffer (see
     * chan_open_stream in ssh_channel.c), so every newline flushes and each
     * line leaves as its own CHANNEL_DATA packet. `out 200` at the default
     * width is therefore ~12KB *and* ~200 packets, and the cliff between 100
     * and 200 could be caused by either.
     *
     * Padding the lines changes bytes without changing packet count, so the two
     * can be measured apart:
     *
     *     out 200        ~12KB, 200 packets   the known-bad case
     *     out 100 120    ~12KB, 100 packets   same bytes, half the packets
     *     out 200 30      ~6KB, 200 packets   same packets, half the bytes
     *
     * setvbuf() would have been the tidier way to do this, and it is not
     * available: neither setvbuf nor fflush is in the loader's exported symbol
     * table, so an app cannot change its own buffering at all.
     */
    for (long i = 0; i < n; i++) {
        if (w <= 0) {
            printf("line %ld of %ld: %s\n", i + 1, n, filler_default);
        } else {
            /* One printf, so one newline and therefore one flush and one
             * packet. The precision does the padding: snprintf would have been
             * the obvious way and is not exported to apps either. */
            long pad = w - 20;

            if (pad < 1) {
                pad = 1;
            }
            if ((size_t)pad > sizeof(filler) - 1) {
                pad = (long)sizeof(filler) - 1;
            }
            printf("line %ld of %ld: %.*s\n", i + 1, n, (int)pad, filler);
        }
    }
    return 0;
}

/*
 * Write to both streams, so a test can tell them apart.
 *
 * An app's stdout and stderr are two separate funopen() streams over the
 * session -- and over SSH they land in CHANNEL_DATA and CHANNEL_EXTENDED_DATA
 * respectively, so this is what proves an app's own fprintf(stderr, ...)
 * reaches the client's stderr rather than being mixed into its output.
 */
static int cmd_both(void)
{
    printf("this-is-stdout\n");
    fflush(stdout);
    fprintf(stderr, "this-is-stderr\n");
    fflush(stderr);
    return 0;
}

/*
 * The same output as `out`, in far fewer packets.
 *
 * An app's stdout is line-buffered with a 128-byte buffer (chan_open_stream in
 * ssh_channel.c), so every newline flushes and each line leaves as its own
 * ~60-byte CHANNEL_DATA packet -- nowhere near the 2048-byte maximum, so
 * nothing batches. `out 200` is therefore ~12KB *and* ~200 packets, and the
 * cliff between 100 and 200 could be either.
 *
 * Full buffering sends the identical bytes in three or four packets, so the two
 * can be told apart. The output must stay byte-for-byte identical to `out` or
 * the comparison measures two things at once.
 *
 * This did not work until setvbuf was exported to apps -- see
 * components/espix_proc/abi_libc.c, which exists because of this function.
 */
static int cmd_outbuf(const char *count)
{
    if (setvbuf(stdout, NULL, _IOFBF, 4096) != 0) {
        printf("outbuf: setvbuf failed -- this run would silently measure the "
               "same thing as `out`\n");
        return 1;
    }

    const int rc = cmd_out(count, NULL);

    fflush(stdout);
    return rc;
}

/*
 * Standard input, echoed back with a count.
 *
 * `ssh host 'testapp cat' < file` is what this is for, and it is the check that
 * an app's stdin is real: espix hands the process a funopen() stream over the
 * queue that the connection task fills from CHANNEL_DATA. Before that existed
 * the readfn was NULL, so stdin returned EOF immediately -- which looks exactly
 * like an empty file, hence the byte count rather than only the text.
 */
static int cmd_cat(void)
{
    char          buf[128];
    unsigned long total = 0;

    while (fgets(buf, sizeof(buf), stdin) != NULL) {
        fputs(buf, stdout);
        total += (unsigned long)strlen(buf);
    }

    printf("\ncat: %lu bytes\n", total);
    return 0;
}

/*
 * Signals -- moved here from what used to be apps/sigtest.
 *
 * It lived in apps/ as though it were an example, which meant tools/build-apps.sh
 * built it on every firmware build and shipped it in every rootfs image, and
 * apps/README.md's table never listed it. It is a lever for the test suite, so
 * it belongs beside the others.
 *
 * Almost all of this is ordinary POSIX, which is the point: signal(), getpid()
 * and sleep() mean here what they mean anywhere. A handler runs in this task,
 * synchronously, at the point the app next calls into espix -- and then
 * returns, and execution carries on at the next line.
 *
 * What is not POSIX is espix_sigcheck(), needed only by `sig spin`: POSIX
 * delivers signals asynchronously and espix delivers them when a process calls
 * in, so a loop that blocks on nothing has to say when that is.
 */

/* Set by the handler, read by the loop. sig_atomic_t because that is what a
 * handler may touch, and only flags are set here: the printing happens back in
 * the loop, which is the idiom worth copying. */
static volatile sig_atomic_t s_stop;
static volatile sig_atomic_t s_usr1;
static volatile sig_atomic_t s_last;

/* espix's delivery point. Only `sig spin` needs it. */
extern bool espix_sigcheck(void);

static void on_signal(int sig)
{
    s_last = sig;

    if (sig == SIGUSR1) {
        s_usr1++;               /* handled and survivable: keep going */
        return;
    }
    s_stop = 1;
}

static const char *signame(int sig)
{
    switch (sig) {
    case SIGTERM: return "SIGTERM";
    case SIGINT:  return "SIGINT";
    case SIGHUP:  return "SIGHUP";
    case SIGUSR1: return "SIGUSR1";
    case SIGUSR2: return "SIGUSR2";
    default:      return "a signal";
    }
}

static void sig_cleanup(void)
{
    /* Where an app would put its hardware back. Reaching this line at all is
     * the thing being tested: a hard kill never does. */
    printf("sig: cleaning up\n");
    fflush(stdout);
}

static int sig_spin(void)
{
    printf("sig: pid %d, spinning with no blocking call\n", (int)getpid());
    fflush(stdout);

    unsigned long long n = 0;

    for (;;) {
        /* Some work with no call into espix in it. */
        for (int i = 0; i < 200000; i++) {
            n += (unsigned long long)i;
        }

        /* Without this the loop above would be uninterruptible: nothing else
         * here gives espix a chance to run a handler. */
        if (espix_sigcheck() || s_stop) {
            printf("sig: stopping after %llu\n", n);
            sig_cleanup();
            return 0;
        }
    }
}

static int sig_ignore(void)
{
    signal(SIGTERM, SIG_IGN);
    signal(SIGINT,  SIG_IGN);
    signal(SIGHUP,  SIG_IGN);

    printf("sig: pid %d, ignoring SIGTERM/SIGINT/SIGHUP\n", (int)getpid());
    fflush(stdout);

    /*
     * Nothing here ends on its own -- that is the point of this mode. SIGKILL,
     * or the shell's third Ctrl-C, is the only way out. The return below is
     * unreachable and exists because -Werror=return-type asks for it.
     */
    for (;;) {
        (void)sleep(60);
    }

    return 0;
}

static int sig_handlers(void)
{
    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);
    signal(SIGHUP,  on_signal);
    signal(SIGUSR1, on_signal);

    printf("sig: pid %d, handlers installed\n", (int)getpid());
    fflush(stdout);

    for (;;) {
        /*
         * A long sleep on purpose. Nothing should ever wait 60 seconds here:
         * espix wakes a sleeping process when it signals it, so `left` below
         * reports how much of the sleep was cut short.
         */
        const unsigned left = sleep(60);

        if (s_stop) {
            printf("sig: %s, %u seconds of sleep left\n",
                   signame((int)s_last), left);
            sig_cleanup();
            return 0;
        }

        if (s_usr1) {
            /* The handler already ran, returned, and the sleep returned after
             * it. Carrying on from here is the whole demonstration. */
            printf("sig: SIGUSR1 x%d, %u seconds left; carrying on\n",
                   (int)s_usr1, left);
            fflush(stdout);
            s_usr1 = 0;
            continue;
        }

        printf("sig: slept the full 60s, nothing arrived\n");
        fflush(stdout);
    }
}

static int cmd_sig(const char *mode)
{
    if (mode == NULL || strcmp(mode, "handlers") == 0) {
        return sig_handlers();
    }
    if (strcmp(mode, "ignore") == 0) {
        return sig_ignore();
    }
    if (strcmp(mode, "spin") == 0) {
        return sig_spin();
    }
    printf("sig: unknown mode '%s'\n", mode);
    return 2;
}

/*
 * Read stdin to the end and report how much arrived.
 *
 * This is the only thing that exercises the inbound path espix gained with a
 * process stdin -- chan_pump -> pending -> drain_pending_to_stdin -> stream
 * buffer -> chan_stream_read. scp reaches none of it: an sftp channel gets no
 * stdin queue at all, so the throughput suite's scp measurements would pass
 * with that whole path broken.
 *
 * The buffer is large deliberately. malloc above
 * CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL (16384) is served from PSRAM, so asking
 * for 64KB keeps this off the internal heap without the app naming a capability
 * it has no ABI for -- and it makes each fread a real block read rather than a
 * measurement of per-call overhead.
 *
 * The count is the point as much as the speed: a transfer that ends early is a
 * failure, and without it a truncated read looks like a fast one.
 */
static int cmd_sink(void)
{
    enum { SINK_BUF = 64 * 1024 };

    char *buf = malloc(SINK_BUF);
    if (buf == NULL) {
        printf("sink: out of memory\n");
        return 1;
    }

    unsigned long total = 0;
    for (;;) {
        const size_t got = fread(buf, 1, SINK_BUF, stdin);
        if (got == 0) {
            break;
        }
        total += (unsigned long)got;
    }

    free(buf);
    printf("sink: %lu bytes\n", total);
    return 0;
}

/*
 * The environment, from the app's side of the ABI.
 *
 * `env get NAME` is the assertion that matters: it must reach espix's getenv
 * and not newlib's. A variable exported by the session is in the process's own
 * copy and *not* in the firmware's global `environ`, so a value coming back
 * here proves which implementation answered -- newlib's would print nothing.
 *
 * `env set NAME=value` then `env get NAME` proves the app can change its own
 * copy, and the shell checking afterwards proves the change did not leak back.
 */
static int cmd_env(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[0], "get") == 0) {
        const char *v = getenv(argv[1]);
        printf("env %s=%s\n", argv[1], (v != NULL) ? v : "(unset)");
        return (v != NULL) ? 0 : 1;
    }

    if (argc > 1 && strcmp(argv[0], "set") == 0) {
        char *eq = strchr(argv[1], '=');
        if (eq == NULL) {
            printf("env: set wants NAME=value\n");
            return 2;
        }
        *eq = '\0';
        if (setenv(argv[1], eq + 1, 1) != 0) {
            printf("env: setenv failed\n");
            return 1;
        }
        const char *v = getenv(argv[1]);
        printf("env %s=%s\n", argv[1], (v != NULL) ? v : "(unset)");
        return 0;
    }

    if (argc > 1 && strcmp(argv[0], "unset") == 0) {
        (void)unsetenv(argv[1]);
        const char *v = getenv(argv[1]);
        printf("env %s=%s\n", argv[1], (v != NULL) ? v : "(unset)");
        return (v == NULL) ? 0 : 1;
    }

    printf("env: want get|set|unset\n");
    return 2;
}

static int cmp_int(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

/*
 * The published ABI, called by name.
 *
 * Two failures look identical from outside and are not: a symbol missing from
 * the tables stops an app loading at all, and one published over a stub loads
 * and then answers ENOSYS. This calls what the tables claim and prints the
 * answers, so a suite can tell the second from the first -- which is the case
 * the notes on isatty() and access() are about.
 *
 * Nothing here reads stdin. getchar() and getc() are published, but with no
 * stdin on the channel they block, and fgetc() on a file is the same libc path;
 * so they are exercised through the file below rather than left untested or
 * hanging.
 */
static int cmd_abi(const char *path)
{
    char buf[] = "one:two:three";
    char *save = NULL;
    const char *t1 = strtok_r(buf, ":", &save);
    const char *t2 = strtok_r(NULL, ":", &save);
    const char *t3 = strtok_r(NULL, ":", &save);

    printf("abi strtok=%s,%s,%s strspn=%u pbrk=%c memchr=%c strnlen=%u,%u\n",
           t1 ? t1 : "(null)", t2 ? t2 : "(null)", t3 ? t3 : "(null)",
           (unsigned)strspn("...abc", "."), *strpbrk("abc-def", "-"),
           *(const char *)memchr("xyz", 'y', 3), (unsigned)strnlen("abcde", 4),
           (unsigned)strnlen("abcde", 6));

    char *dup = strndup("truncated", 5);
    printf("abi strndup=%s atol=%ld atoll=%lld labs=%ld llabs=%lld\n",
           dup ? dup : "(null)", atol("42"), atoll("99"), labs(-7), llabs(-7));
    free(dup);

    static const int keys[] = { 10, 20, 30, 40 };
    const int want = 30;
    const int *found = bsearch(&want, keys, 4, sizeof(keys[0]), cmp_int);

    int i1 = 0, i2 = 0;
    sscanf("12 34", "%d %d", &i1, &i2);

    /* Determinism as a property rather than a value: the sequence rand()
     * produces for a seed is the implementation's business, that it repeats for
     * the same seed is not. */
    srand(1);
    const int r1 = rand();
    srand(1);
    const int r2 = rand();
    printf("abi sscanf=%d,%d bsearch=%d rand=%s\n", i1, i2, found ? *found : -1,
           (r1 == r2) ? "repeatable" : "not-repeatable");

    /* A fixed instant through gmtime, so the text does not depend on the
     * device's timezone -- the whole point of asserting on it. */
    const time_t t0 = 0;
    const struct tm *g = gmtime(&t0);
    char asc[32] = "(null)";
    if (g != NULL && asctime_r(g, asc) != NULL) {
        asc[strcspn(asc, "\n")] = '\0';
    }
    char ctbuf[32];
    printf("abi asctime=%s ctime=%s\n", asc,
           (ctime_r(&t0, ctbuf) != NULL) ? "ok" : "null");

    /*
     * errno across the boundary, and strerror to name it. A failed fopen is how
     * an app normally gets here, so this is the property the other commands lean
     * on: the errno an app reads after a syscall is the one espix's layer set.
     *
     * Not perror(), which was published and then measured to write nothing: it
     * sends its line to the firmware's own stderr stream rather than the channel
     * the app is writing to. See the note in abi_libc.c.
     */
    FILE *missing = fopen("/nonexistent-abi-probe", "r");
    printf("abi errno=%s strerror=%s\n", errno_name(errno), strerror(errno));
    if (missing != NULL) {
        fclose(missing);
    }

    FILE *f = fopen(path, "r");
    if (f == NULL) {
        printf("abi fs=%s\n", errno_name(errno));
        return 1;
    }
    const int fd = fileno(f);
    const int ch = fgetc(f);
    ungetc(ch, f);
    const int again = fgetc(f);

    /* The same descriptor through the other door. One close, not two: fdopen
     * does not take a copy of it. */
    FILE *by_fd = fdopen(fd, "r");
    printf("abi fs=ok fd=%d fgetc=%c ungetc=%s fdopen=%s\n", fd,
           (ch == EOF) ? '?' : (char)ch, (again == ch) ? "ok" : "differs",
           (by_fd != NULL) ? "ok" : "null");
    if (by_fd != NULL) {
        fclose(by_fd);
    } else {
        fclose(f);
    }

    const struct utimbuf when = { .actime = t0 + 1000, .modtime = t0 + 1000 };
    printf("abi utime=%s\n", (utime(path, &when) == 0) ? "ok" : errno_name(errno));

    return 0;
}

static void usage(void)
{
    printf("usage: testapp <command> [args]\n"
           "  exit <n>            exit with status n\n"
           "  argv [args...]      echo argc and each argument\n"
           "  probe <path>...     open each path, report ok or errno\n"
           "  stat <path>         stat and fstat, with the app own uid\n"
           "  id                  uid, gid, euid and egid as the ABI answers them\n"
           "  chmod <path> <oct>  chmod, report ok or errno\n"
           "  cd <path>           chdir then getcwd\n"
           "  write <path> <text> create and write\n"
           "  read <path>         read the first line back\n"
           "  hexdump <path> <off> <len>\n"
           "                      hex and ascii at an offset, for superblocks\n"
           "  out <n> [width]     print n lines, each padded to width bytes\n"
           "  outbuf <n>          the same lines, 4KB-buffered (few packets)\n"
           "  both                one line to stdout, one to stderr\n"
           "  cat                 echo stdin, then its byte count\n"
           "  sink                read stdin, report the byte count only\n"
           "  sig [mode]          handlers (default) | ignore | spin\n"
           "  sleep <secs>        sleep, for signal and job-control tests\n"
           "  env get <NAME>      print a variable as the app sees it\n"
           "  env set <N=V>       setenv in this process, then read it back\n"
           "  env unset <NAME>    unsetenv, then read it back\n"
           "  abi <path>          call the published ABI by name, on a readable file\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return 2;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "exit") == 0 && argc > 2) {
        return (int)strtol(argv[2], NULL, 10);
    }
    if (strcmp(cmd, "argv") == 0) {
        printf("argc %d\n", argc);
        for (int i = 0; i < argc; i++) {
            printf("argv[%d] %s\n", i, argv[i]);
        }
        return 0;
    }
    if (strcmp(cmd, "probe") == 0 && argc > 2) {
        return cmd_probe(argc - 2, argv + 2);
    }
    if (strcmp(cmd, "chmod") == 0 && argc > 3) {
        return cmd_chmod(argv[2], argv[3]);
    }
    if (strcmp(cmd, "id") == 0) {
        return cmd_id();
    }
    if (strcmp(cmd, "stat") == 0 && argc > 2) {
        return cmd_stat(argv[2]);
    }
    if (strcmp(cmd, "cd") == 0 && argc > 2) {
        return cmd_cd(argv[2]);
    }
    if (strcmp(cmd, "write") == 0 && argc > 3) {
        return cmd_write(argv[2], argv[3]);
    }
    if (strcmp(cmd, "read") == 0 && argc > 2) {
        return cmd_read(argv[2]);
    }
    if (strcmp(cmd, "hexdump") == 0 && argc > 4) {
        return cmd_hexdump(argv[2], argv[3], argv[4]);
    }
    if (strcmp(cmd, "outbuf") == 0 && argc > 2) {
        return cmd_outbuf(argv[2]);
    }
    if (strcmp(cmd, "both") == 0) {
        return cmd_both();
    }
    if (strcmp(cmd, "cat") == 0) {
        return cmd_cat();
    }
    if (strcmp(cmd, "sink") == 0) {
        return cmd_sink();
    }
    if (strcmp(cmd, "sig") == 0) {
        return cmd_sig((argc > 2) ? argv[2] : NULL);
    }
    if (strcmp(cmd, "out") == 0 && argc > 2) {
        return cmd_out(argv[2], (argc > 3) ? argv[3] : NULL);
    }
    if (strcmp(cmd, "env") == 0 && argc > 2) {
        return cmd_env(argc - 2, argv + 2);
    }
    if (strcmp(cmd, "sleep") == 0 && argc > 2) {
        sleep((unsigned)strtol(argv[2], NULL, 10));
        printf("slept %s\n", argv[2]);
        return 0;
    }

    if (strcmp(cmd, "abi") == 0 && argc > 2) {
        return cmd_abi(argv[2]);
    }

    printf("testapp: unknown command '%s'\n", cmd);
    usage();
    return 2;
}
