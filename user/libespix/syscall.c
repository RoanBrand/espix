/* syscall(): a Linux number goes straight to the kernel, which answers the calls it has and says ENOSYS to the rest. */
#include <stdarg.h>
#include <sys/syscall.h>

#include "sys.h"

long syscall(long number, ...)
{
    va_list ap;
    va_start(ap, number);
    long a[6];
    for (int i = 0; i < 6; i++) {
        a[i] = va_arg(ap, long);
    }
    va_end(ap);
    return espix_ret(espix_sys6(number, a[0], a[1], a[2], a[3], a[4], a[5]));
}
