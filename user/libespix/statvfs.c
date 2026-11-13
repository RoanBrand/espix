/* Filesystem statistics: none to give yet, so every call says so. */
#include <errno.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>

static int none(void)
{
    errno = ENOSYS;
    return -1;
}

int statvfs(const char *path, struct statvfs *buf) { (void)path; (void)buf; return none(); }
int fstatvfs(int fd, struct statvfs *buf)          { (void)fd; (void)buf; return none(); }
int statfs(const char *path, struct statfs *buf)   { (void)path; (void)buf; return none(); }
int fstatfs(int fd, struct statfs *buf)            { (void)fd; (void)buf; return none(); }
