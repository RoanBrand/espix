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
static int cmd_out(const char *count)
{
    const long n = strtol(count, NULL, 10);

    for (long i = 0; i < n; i++) {
        printf("line %ld of %ld: the quick brown fox jumps over the lazy dog\n",
               i + 1, n);
    }
    return 0;
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
           "  out <n>             print n lines\n"
           "  sleep <secs>        sleep, for signal and job-control tests\n");
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
    if (strcmp(cmd, "out") == 0 && argc > 2) {
        return cmd_out(argv[2]);
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
