/*
 * espix example app.
 *
 * This is NOT part of the espix firmware. It is cross-compiled on the host into
 * a standalone ELF, copied onto the device's filesystem, and executed at
 * runtime by `run /bin/hello` — the whole point of the exercise.
 *
 * It links against nothing but libc. Symbols are resolved at load time against
 * the tables the firmware's elf_loader exports (CONFIG_ELF_LOADER_LIBC_SYMBOLS
 * and CONFIG_ELF_LOADER_ESPIDF_SYMBOLS) plus the ones espix publishes itself,
 * so anything used here has to be in one of those tables. An unresolved symbol
 * fails the *load*, which is why this app is also the test for espix's ABI:
 * if it runs at all, every name below resolved.
 */

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * `hello probe <path>...` -- open each path and say what happened.
 *
 * This exists to test something only a *process* can test. `run -R <dir>` gives
 * a process a root it cannot name a path outside of, and shell builtins are
 * never confined, so `cat` proves nothing about it: the check has to be made by
 * a loaded app, from inside the confinement.
 *
 * The two answers that matter are different on purpose. A path a confined
 * process may not name is ENOENT -- not there, as far as it can tell -- while
 * EACCES is an ordinary permission refusal on a path it can see. Reporting
 * "denied" for the first would confirm the file exists to something that is
 * supposed to be unable to find out.
 *
 * errno is printed by number with the two interesting names spelled out rather
 * than through strerror(). Every symbol here has to resolve out of the loader's
 * export tables at load time or the app does not run at all, and this file is
 * the ABI test as much as anything else; a numeric comparison needs nothing.
 */
static int probe(int argc, char **argv)
{
    for (int i = 0; i < argc; i++) {
        FILE *f = fopen(argv[i], "r");

        if (f != NULL) {
            fclose(f);
            printf("  %s: ok\n", argv[i]);
            continue;
        }

        const int e = errno;
        const char *name = (e == ENOENT)  ? " (ENOENT)"
                           : (e == EACCES) ? " (EACCES)"
                                           : "";
        printf("  %s: errno %d%s\n", argv[i], e, name);
    }
    return 0;
}

/*
 * Everything below needs espix's filesystem ABI (see
 * components/espix_proc/abi_fs.c). None of it was callable from an app before
 * that existed -- an app that touched a file failed to load.
 */
static void show_filesystem(void)
{
    char cwd[128];

    /* Where are we? Inherited from whoever ran us, so `cd /tmp` then `hello`
     * reports /tmp -- espix gives each process its own working directory. */
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        printf("  cwd = %s\n", cwd);
    } else {
        printf("  cwd = (getcwd failed)\n");
    }

    /* A relative path, which is the whole point of having a cwd: espix's VFS
     * resolves it against the line above before touching the filesystem. */
    FILE *f = fopen("hello.tmp", "w");
    if (f == NULL) {
        printf("  fopen(\"hello.tmp\", \"w\") failed\n");
        return;
    }
    fputs("written by an espix app\n", f);
    fclose(f);

    f = fopen("hello.tmp", "r");
    if (f != NULL) {
        char line[64] = { 0 };
        if (fgets(line, sizeof(line), f) != NULL) {
            printf("  read back: %s", line);
        }
        fclose(f);
    }

    /* stat() reports the real mode, because espix's VFS fills it in -- the same
     * nine bits `ls -l` shows. */
    struct stat st;
    if (stat("hello.tmp", &st) == 0) {
        printf("  hello.tmp: %ld bytes, mode %03o\n",
               (long)st.st_size, (unsigned)(st.st_mode & 0777));
    }

    /* And a real chmod, not libc's no-op. */
    if (chmod("hello.tmp", 0600) == 0 && stat("hello.tmp", &st) == 0) {
        printf("  after chmod 600: mode %03o\n",
               (unsigned)(st.st_mode & 0777));
    }

    remove("hello.tmp");

    /* Directories, so readdir is exercised too. */
    DIR *d = opendir("/etc");
    if (d != NULL) {
        int n = 0;
        while (readdir(d) != NULL) {
            n++;
        }
        closedir(d);
        printf("  /etc holds %d entries\n", n);
    }
}

int main(int argc, char **argv)
{
    if (argc > 2 && strcmp(argv[1], "probe") == 0) {
        return probe(argc - 2, argv + 2);
    }

    printf("hello from an espix app\n");
    printf("  argc = %d\n", argc);

    for (int i = 0; i < argc; i++) {
        printf("  argv[%d] = %s\n", i, argv[i]);
    }

    show_filesystem();

    /* Return a non-zero status when asked, so `run` can be seen reporting it. */
    if (argc > 1 && strcmp(argv[1], "fail") == 0) {
        printf("exiting with status 3\n");
        return 3;
    }

    return 0;
}
