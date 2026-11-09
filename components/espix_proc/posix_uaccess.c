/*
 * User memory for an app without an address space (espix_posix.h): the same calls the
 * MMU's walk answers on the S31, here a range check and the pointer itself. Nothing
 * stops such an app naming the kernel's memory; the check turns a wild pointer into
 * EFAULT rather than a fault.
 */
#include <errno.h>
#include <string.h>
#include <sys/param.h>

#include "esp_memory_utils.h"

#include "espix_posix.h"

static bool usable(const void *p)
{
    return esp_ptr_byte_accessible(p) || esp_ptr_in_drom(p);
}

bool espix_posix_range_ok(uint32_t va, size_t n)
{
    return n == 0 || (va + n - 1 >= va && usable((const void *)va) && usable((const void *)(va + n - 1)));
}

void *espix_posix_pin(uint32_t va)
{
    return espix_posix_range_ok(va, 1) ? (void *)va : NULL;
}

int espix_posix_copyin(void *dst, uint32_t uva, size_t n)
{
    if (!espix_posix_range_ok(uva, n)) {
        return -EFAULT;
    }
    memcpy(dst, (const void *)uva, n);
    return 0;
}

int espix_posix_copyout(uint32_t uva, const void *src, size_t n)
{
    if (!espix_posix_range_ok(uva, n)) {
        return -EFAULT;
    }
    memcpy((void *)uva, src, n);
    return 0;
}

int espix_posix_strin(char *dst, uint32_t uva, size_t size)
{
    for (size_t len = 0; len < size; len++) {
        if (!espix_posix_range_ok(uva + len, 1)) {
            return -EFAULT;
        }
        if ((dst[len] = ((const char *)uva)[len]) == '\0') {
            return (int)len;
        }
    }
    return -ENAMETOOLONG;
}

#if !CONFIG_ESPIX_UMODE

/* Without an MMU every address space is NULL: the calls are the range-checked ones. */
void *espix_um_user_pin(espix_um_as_t *as, uint32_t va, uint32_t need)
{
    (void)as;
    (void)need;
    return espix_posix_pin(va);
}

void espix_um_user_unpin(espix_um_as_t *as)
{
    (void)as;
}

int espix_um_copyin(espix_um_as_t *as, void *dst, uint32_t uva, size_t n)
{
    (void)as;
    return espix_posix_copyin(dst, uva, n);
}

int espix_um_copyout(espix_um_as_t *as, uint32_t uva, const void *src, size_t n)
{
    (void)as;
    return espix_posix_copyout(uva, src, n);
}

int espix_um_strin(espix_um_as_t *as, char *dst, uint32_t uva, size_t size)
{
    (void)as;
    return espix_posix_strin(dst, uva, size);
}

#endif /* !CONFIG_ESPIX_UMODE */
