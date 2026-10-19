/*
 * uapp: the U-mode test app (docs/USER-MODE.md, M1). Each mode checks one thing
 * the boundary must hold: a fault ends only the app, kernel memory is out of
 * reach, preemption and the FPU work in U-mode, and exit codes arrive.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long cycles(void)
{
    unsigned long c;
    __asm__ volatile ("rdcycle %0" : "=r"(c));
    return c;
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
    fprintf(stderr, "uapp: unknown mode %s\n", mode);
    return 2;
}
