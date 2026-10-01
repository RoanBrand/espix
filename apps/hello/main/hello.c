/*
 * espix example app.
 *
 * This is NOT part of the espix firmware. It is cross-compiled on the host into
 * a standalone ELF, copied onto the device's filesystem, and executed at
 * runtime as `hello` -- which is the whole point of the exercise.
 *
 * It is deliberately the smallest thing that shows the shape of an espix app:
 * it is handed argv, it prints, and it returns an exit status the shell
 * reports. Everything further an app can do -- the filesystem, the process
 * root, signals, the published ABI by name -- is exercised by tests/app, which
 * exists for that and is allowed to be as blunt as a test needs to be. Keeping
 * the two apart is the point: a showcase that grows test scaffolding stops
 * showing anything.
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

    /* A non-zero status is worth showing, because it is what the shell reports
     * as `[exit N]` and what `$?` holds afterwards. */
    if (argc > 1 && strcmp(argv[1], "fail") == 0) {
        printf("exiting with status 3\n");
        return 3;
    }

    return 0;
}
