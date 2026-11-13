/* statvfs(): espix's VFS has no filesystem statistics yet, so it fails with ENOSYS (libespix). */
#pragma once

struct statvfs {
    unsigned long f_bsize, f_frsize, f_blocks, f_bfree, f_bavail;
    unsigned long f_files, f_ffree, f_favail, f_fsid, f_flag, f_namemax;
};

#define ST_RDONLY 1
#define ST_NOSUID 2

int statvfs(const char *path, struct statvfs *buf);
int fstatvfs(int fd, struct statvfs *buf);
