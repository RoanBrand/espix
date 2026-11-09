/*
 * A large allocation is its own anonymous mapping, as glibc's is: it costs only the pages touched,
 * and free() gives them all back. Smaller ones are picolibc's, from the brk heap. espix-cc links
 * these over picolibc's with --wrap, so libc's own calls come here too.
 */
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

#define BIG 0x20000u       /* glibc's M_MMAP_THRESHOLD */
#define HDR 16u            /* the mapping's length, keeping 16-byte alignment */

void  *__real_malloc(size_t n);
void   __real_free(void *p);
void  *__real_calloc(size_t n, size_t size);
void  *__real_realloc(void *p, size_t n);
size_t __real_malloc_usable_size(void *p);

/* Mappings live above the brk heap's limit (docs/USER-MODE.md). */
static int mapped(const void *p)
{
    return (uintptr_t)p >= 0x60000000u && (uintptr_t)p < 0x70000000u;
}

static void *big(size_t n)
{
    if (n > SIZE_MAX - HDR - 4095) {
        errno = ENOMEM;
        return NULL;
    }
    const size_t len = (n + HDR + 4095) & ~(size_t)4095;
    char *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) {
        errno = ENOMEM;
        return NULL;
    }
    *(size_t *)m = len;
    return m + HDR;
}

static size_t big_size(void *p)
{
    return *(size_t *)((char *)p - HDR) - HDR;
}

void *__wrap_malloc(size_t n)
{
    return (n >= BIG) ? big(n) : __real_malloc(n);
}

void __wrap_free(void *p)
{
    if (p != NULL && mapped(p)) {
        munmap((char *)p - HDR, big_size(p) + HDR);
    } else {
        __real_free(p);
    }
}

void *__wrap_calloc(size_t n, size_t size)
{
    size_t bytes;
    if (__builtin_mul_overflow(n, size, &bytes)) {
        errno = ENOMEM;
        return NULL;
    }
    return (bytes >= BIG) ? big(bytes) : __real_calloc(n, size);      /* a mapping is zero already */
}

void *__wrap_realloc(void *p, size_t n)
{
    if (p == NULL) {
        return __wrap_malloc(n);
    }
    const size_t have = mapped(p) ? big_size(p) : __real_malloc_usable_size(p);
    if (!mapped(p) && n < BIG) {
        return __real_realloc(p, n);
    }
    if (mapped(p) && n <= have && n >= BIG) {
        return p;
    }
    void *q = __wrap_malloc(n);
    if (q != NULL) {
        memcpy(q, p, (n < have) ? n : have);
        __wrap_free(p);
    }
    return q;
}

size_t __wrap_malloc_usable_size(void *p)
{
    return (p != NULL && mapped(p)) ? big_size(p) : __real_malloc_usable_size(p);
}
