/*
 * espix example app.
 *
 * This is NOT part of the espix firmware. It is cross-compiled on the host into
 * a standalone ELF, copied onto the device's filesystem, and executed at
 * runtime by `run /bin/hello` — the whole point of the exercise.
 *
 * It links against nothing but libc. Symbols are resolved at load time against
 * the tables the firmware's elf_loader exports (CONFIG_ELF_LOADER_LIBC_SYMBOLS
 * and CONFIG_ELF_LOADER_ESPIDF_SYMBOLS), so anything used here has to be in
 * one of those tables.
 */

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    printf("hello from an espix app\n");
    printf("  argc = %d\n", argc);

    for (int i = 0; i < argc; i++) {
        printf("  argv[%d] = %s\n", i, argv[i]);
    }

    /* Return a non-zero status when asked, so `run` can be seen reporting it. */
    if (argc > 1 && strcmp(argv[1], "fail") == 0) {
        printf("exiting with status 3\n");
        return 3;
    }

    return 0;
}
