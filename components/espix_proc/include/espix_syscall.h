/*
 * espix's syscalls: the U-mode half of the app ABI, shared by the kernel and
 * libespix (docs/USER-MODE.md).
 *
 * A number in a7, arguments in a0-a5, the result in a0: negative is -errno.
 * Flag, mode and errno values are the toolchain picolibc's, which both sides
 * use; structures that cross are the fixed-width ones below.
 */
#pragma once

#include <stdint.h>

#define ESPIX_SYSCALLS(X)       \
    X(EXIT,          exit,           1)        /* (status) */                                  \
    X(WRITE,         write,          2)        /* (fd, buf, len) */                            \
    X(READ,          read,           3)        /* (fd, buf, len) */                            \
    X(OPEN,          open,           4)        /* (path, flags, mode) */                       \
    X(CLOSE,         close,          5)        /* (fd) */                                      \
    X(LSEEK,         lseek,          6)        /* (fd, off_lo, off_hi, whence, uint64_t *pos) */ \
    X(FSTAT,         fstat,          7)        /* (fd, struct espix_stat *) */                 \
    X(STAT,          stat,           8)        /* (path, struct espix_stat *) */               \
    X(UNLINK,        unlink,         9)        /* (path) */                                    \
    X(RENAME,        rename,        10)        /* (from, to) */                                \
    X(MKDIR,         mkdir,         11)        /* (path, mode) */                              \
    X(RMDIR,         rmdir,         12)        /* (path) */                                    \
    X(TRUNCATE,      truncate,      13)        /* (path, len_lo, len_hi) */                    \
    X(FTRUNCATE,     ftruncate,     14)        /* (fd, len_lo, len_hi) */                      \
    X(FSYNC,         fsync,         15)        /* (fd) */                                      \
    X(UTIME,         utime,         16)        /* (path, const struct espix_utimbuf *) */      \
    X(CHDIR,         chdir,         17)        /* (path) */                                    \
    X(GETCWD,        getcwd,        18)        /* (buf, size): the length with its NUL */      \
    X(CHMOD,         chmod,         19)        /* (path, mode) */                              \
    X(DUP,           dup,           20)        /* (fd) */                                      \
    X(DUP2,          dup2,          21)        /* (fd, newfd) */                               \
    X(FCNTL,         fcntl,         22)        /* (fd, cmd, arg) */                            \
    X(GETDENTS,      getdents,      23)        /* (fd, buf, len): struct espix_dirent records */ \
    X(ISATTY,        isatty,        24)        /* (fd) */                                      \
    X(CLOCK_GETTIME, clock_gettime, 25)        /* (clock, struct espix_timespec *) */          \
    X(NANOSLEEP,     nanosleep,     26)        /* (const struct espix_timespec *, rem) */      \
    X(GETPID,        getpid,        27)        /* () */                                        \
    X(GETPPID,       getppid,       28)        /* () */                                        \
    X(GETUID,        getuid,        29)        /* () */                                        \
    X(GETEUID,       geteuid,       30)        /* () */                                        \
    X(GETGID,        getgid,        31)        /* () */                                        \
    X(GETEGID,       getegid,       32)        /* () */                                        \
    X(KILL,          kill,          33)        /* (pid, sig) */                                \
    X(GETPGRP,       getpgrp,       34)        /* () */                                        \
    X(GETSID,        getsid,        35)        /* (pid) */                                     \
    X(BRK,           brk,           36)        /* (addr): the break, moved there if it could be */

enum {
#define ESPIX_SYS_ENUM_(name, lname, nr) ESPIX_SYS_##name = nr,
    ESPIX_SYSCALLS(ESPIX_SYS_ENUM_)
#undef ESPIX_SYS_ENUM_
};

struct espix_stat {
    uint64_t size;
    int64_t  atime, mtime, ctime;   /* seconds */
    uint32_t dev, ino, mode, nlink, uid, gid, rdev, blksize, blocks, pad;
};

struct espix_timespec {
    int64_t sec;
    int32_t nsec;
    int32_t pad;
};

struct espix_utimbuf {
    int64_t actime, modtime;
};

/* One getdents record; the next starts reclen bytes on, 4-aligned. */
struct espix_dirent {
    uint32_t ino;
    uint16_t reclen;
    uint8_t  type;
    char     name[];
};
