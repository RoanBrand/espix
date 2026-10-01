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

#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

    /*
     * And the other way out, which is the one a real program uses.
     *
     * exit() resolved to the firmware's until abi_exit.c claimed it, and the
     * firmware's ends at IDF's _exit() == abort(): `hello exit 5` did not print
     * [exit 5], it reset the board. Now the status reaches the shell exactly as
     * a return does, and the device stays up -- which is what this case pins.
     */
    if (argc > 2 && strcmp(argv[1], "exit") == 0) {
        const int n = atoi(argv[2]);
        printf("calling exit(%d)\n", n);
        exit(n);
    }

    /*
     * abort() and a failing assert(). Both used to reboot the board, and the
     * assert reached it by a route a symbol override cannot close on its own:
     * the failing assert calls the *firmware's* __assert_func, which calls the
     * firmware's abort at its own link time. abi_exit.c claims both names.
     *
     * 134 is 128 + SIGABRT, which is what a shell reports for a process killed
     * by that signal.
     */
    if (argc > 1 && strcmp(argv[1], "abort") == 0) {
        printf("calling abort()\n");
        abort();
    }

    if (argc > 1 && strcmp(argv[1], "assert") == 0) {
        printf("about to fail an assertion\n");
        assert(argc < 0);
        printf("assert did not fire, which is its own bug\n");
        return 1;
    }

    return 0;
}
