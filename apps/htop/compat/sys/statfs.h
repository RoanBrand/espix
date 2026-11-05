/* statfs(2), which espix has no filesystem statistics call for. htop uses it
 * only to ask whether /sys/fs/selinux is mounted, and a refusal is "no". */
#pragma once

#include <errno.h>
#include <sys/types.h>

struct statfs {
    long           f_type;
    long           f_bsize;
    unsigned long  f_blocks;
    unsigned long  f_bfree;
    unsigned long  f_bavail;
    unsigned long  f_files;
    unsigned long  f_ffree;
    int            f_fsid[2];
    long           f_namelen;
    long           f_frsize;
    long           f_flags;
    long           f_spare[4];
};

static inline int statfs(const char *path, struct statfs *buf)
{
    (void)path;
    (void)buf;
    errno = ENOSYS;
    return -1;
}

static inline int fstatfs(int fd, struct statfs *buf)
{
    (void)fd;
    (void)buf;
    errno = ENOSYS;
    return -1;
}
