/* Memory mappings as syscalls. Memory is never swapped, so locking it is already done. */
#include <errno.h>
#include <stdint.h>
#include <sys/mman.h>

#include "sys.h"

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    if (off < 0 || (off & 4095) != 0 || (uint64_t)off / 4096 > UINT32_MAX) {
        errno = EINVAL;
        return MAP_FAILED;
    }
    const long r = espix_sys6(ESPIX_SYS_MMAP, (long)addr, (long)len, prot, flags, fd, (long)(off / 4096));
    if (r < 0 && r > -4096) {
        errno = (int)-r;
        return MAP_FAILED;
    }
    return (void *)r;
}

int munmap(void *addr, size_t len)
{
    return espix_ret(espix_sys2(ESPIX_SYS_MUNMAP, addr, len));
}

int mprotect(void *addr, size_t len, int prot)
{
    return espix_ret(espix_sys3(ESPIX_SYS_MPROTECT, addr, len, prot));
}

int msync(void *addr, size_t len, int flags)
{
    return espix_ret(espix_sys3(ESPIX_SYS_MSYNC, addr, len, flags));
}

int madvise(void *addr, size_t len, int advice)
{
    return espix_ret(espix_sys3(ESPIX_SYS_MADVISE, addr, len, advice));
}

int mincore(void *addr, size_t len, unsigned char *vec)
{
    return espix_ret(espix_sys3(ESPIX_SYS_MINCORE, addr, len, vec));
}

int posix_madvise(void *addr, size_t len, int advice)
{
    const long r = espix_sys3(ESPIX_SYS_MADVISE, addr, len, advice);
    return (r < 0) ? (int)-r : 0;
}

int mlock(const void *addr, size_t len)   { (void)addr; (void)len; return 0; }
int munlock(const void *addr, size_t len) { (void)addr; (void)len; return 0; }
int mlockall(int flags)                   { (void)flags; return 0; }
int munlockall(void)                      { return 0; }
