/* Linux's statfs(): as statvfs.h, ENOSYS until the VFS keeps filesystem statistics (libespix). */
#pragma once

struct statfs {
    long          f_type, f_bsize;
    unsigned long f_blocks, f_bfree, f_bavail, f_files, f_ffree;
    int           f_fsid[2];
    long          f_namelen, f_frsize, f_flags, f_spare[4];
};

int statfs(const char *path, struct statfs *buf);
int fstatfs(int fd, struct statfs *buf);
