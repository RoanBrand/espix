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
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
    default:      return "?";
    }
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

static void usage(void)
{
    printf("usage: testapp <command> [args]\n"
           "  exit <n>            exit with status n\n"
           "  argv [args...]      echo argc and each argument\n"
           "  probe <path>...     open each path, report ok or errno\n"
           "  chmod <path> <oct>  chmod, report ok or errno\n"
           "  cd <path>           chdir then getcwd\n"
           "  write <path> <text> create and write\n"
           "  read <path>         read the first line back\n"
           "  out <n> [width]     print n lines, each padded to width bytes\n"
           "  outbuf <n>          the same lines, 4KB-buffered (few packets)\n"
           "  both                one line to stdout, one to stderr\n"
           "  cat                 echo stdin, then its byte count\n"
           "  sink                read stdin, report the byte count only\n"
           "  sig [mode]          handlers (default) | ignore | spin\n"
           "  sleep <secs>        sleep, for signal and job-control tests\n"
           "  env get <NAME>      print a variable as the app sees it\n"
           "  env set <N=V>       setenv in this process, then read it back\n"
           "  env unset <NAME>    unsetenv, then read it back\n");
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
    if (strcmp(cmd, "cd") == 0 && argc > 2) {
        return cmd_cd(argv[2]);
    }
    if (strcmp(cmd, "write") == 0 && argc > 3) {
        return cmd_write(argv[2], argv[3]);
    }
    if (strcmp(cmd, "read") == 0 && argc > 2) {
        return cmd_read(argv[2]);
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

    printf("testapp: unknown command '%s'\n", cmd);
    usage();
    return 2;
}
