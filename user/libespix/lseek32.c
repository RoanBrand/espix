/* The lseek picolibc's own stdio calls, with libc's 32-bit off_t. */
#include <stdint.h>
#include <unistd.h>

#include "sys.h"

off_t lseek(int fd, off_t off, int whence)
{
    uint64_t pos = 0;
    const long r = espix_sys5(ESPIX_SYS_LSEEK, fd, (uint32_t)off, (uint32_t)((int64_t)off >> 32), whence, &pos);
    if (r < 0) {
        return espix_ret(r);
    }
    if (pos > INT32_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    return (off_t)pos;
}

