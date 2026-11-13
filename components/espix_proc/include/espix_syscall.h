/*
 * espix's syscalls: the U-mode half of the app ABI, shared by the kernel and
 * libespix (docs/USER-MODE.md).
 *
 * A number in a7, arguments in a0-a5, the result in a0: negative is -errno. A
 * call Linux has with the same arguments takes Linux's rv32 number; the rest are
 * numbered from 2048 (tools/syscalls.py checks both, and lists them).
 * Flag, mode and errno values are the toolchain picolibc's, which both sides
 * use; socket and poll values and sockaddrs are lwIP's and IDF's, which the
 * kernel hands on unchanged and user/include mirrors. Structures that cross
 * are the fixed-width ones below.
 */
#pragma once

#include <stdint.h>

/* Linux's calls, by their asm-generic rv32 numbers: the same arguments and meaning (the comment names Linux's, where it differs). */
#define ESPIX_SYSCALLS_LINUX(X) \
    X(GETCWD,        getcwd,          17)        /* (buf, size): the length with its NUL */ \
    X(DUP,           dup,             23)        /* (fd) */ \
    X(FCNTL,         fcntl,           25)        /* (fd, cmd, arg); Linux's fcntl64 */ \
    X(IOCTL,         ioctl,           29)        /* (fd, FIONBIO or FIONREAD, int *) */ \
    X(TRUNCATE,      truncate,        45)        /* (path, len_lo, len_hi); Linux's truncate64 */ \
    X(FTRUNCATE,     ftruncate,       46)        /* (fd, len_lo, len_hi); Linux's ftruncate64 */ \
    X(CHDIR,         chdir,           49)        /* (path) */ \
    X(OPENAT,        openat,          56)        /* (dirfd or AT_FDCWD, path, flags, mode) */ \
    X(CLOSE,         close,           57)        /* (fd) */ \
    X(PIPE2,         pipe2,           59)        /* (int fds[2], O_NONBLOCK | O_CLOEXEC) */ \
    X(READ,          read,            63)        /* (fd, buf, len) */ \
    X(WRITE,         write,           64)        /* (fd, buf, len) */ \
    X(PREAD,         pread,           67)        /* (fd, buf, len, off_lo, off_hi); Linux's pread64 */ \
    X(PWRITE,        pwrite,          68)        /* (fd, buf, len, off_lo, off_hi); Linux's pwrite64 */ \
    X(READLINKAT,    readlinkat,      78)        /* (dirfd or AT_FDCWD, path, buf, len): the target's length, no NUL */ \
    X(FSYNC,         fsync,           82)        /* (fd) */ \
    X(CAPGET,        capget,          90)        /* (cap_user_header_t, cap_user_data_t): uid 0 holds every capability */ \
    X(EXIT,          exit,            94)        /* (status); Linux's exit_group */ \
    X(SCHED_YIELD,   sched_yield,    124)        /* () */ \
    X(KILL,          kill,           129)        /* (pid, sig) */ \
    X(SIGRETURN,     sigreturn,      139)        /* (): a handler's restorer; its frame is at sp; Linux's rt_sigreturn */ \
    X(SETPGID,       setpgid,        154)        /* (pid, pgid) */ \
    X(GETPGID,       getpgid,        155)        /* (pid) */ \
    X(GETSID,        getsid,         156)        /* (pid) */ \
    X(SETSID,        setsid,         157)        /* (): the new session's id */ \
    X(GETGROUPS,     getgroups,      158)        /* (size, uint32_t *list): the count; size 0 only counts */ \
    X(UNAME,         uname,          160)        /* (struct espix_utsname *): Linux's struct new_utsname */ \
    X(UMASK,         umask,          166)        /* (mask): the old one */ \
    X(GETPID,        getpid,         172)        /* () */ \
    X(GETPPID,       getppid,        173)        /* () */ \
    X(GETUID,        getuid,         174)        /* () */ \
    X(GETEUID,       geteuid,        175)        /* () */ \
    X(GETGID,        getgid,         176)        /* () */ \
    X(GETEGID,       getegid,        177)        /* () */ \
    X(GETTID,        gettid,         178)        /* () */ \
    X(SOCKET,        socket,         198)        /* (domain, type | SOCK_NONBLOCK | SOCK_CLOEXEC, protocol) */ \
    X(BIND,          bind,           200)        /* (fd, addr, len) */ \
    X(LISTEN,        listen,         201)        /* (fd, backlog) */ \
    X(CONNECT,       connect,        203)        /* (fd, addr, len) */ \
    X(GETSOCKNAME,   getsockname,    204)        /* (fd, addr, socklen_t *len) */ \
    X(GETPEERNAME,   getpeername,    205)        /* (fd, addr, socklen_t *len) */ \
    X(SENDTO,        sendto,         206)        /* (fd, buf, len, flags, to or 0, tolen) */ \
    X(RECVFROM,      recvfrom,       207)        /* (fd, buf, len, flags, from or 0, socklen_t *fromlen) */ \
    X(SETSOCKOPT,    setsockopt,     208)        /* (fd, level, name, val, len) */ \
    X(GETSOCKOPT,    getsockopt,     209)        /* (fd, level, name, val, socklen_t *len) */ \
    X(SHUTDOWN,      shutdown,       210)        /* (fd, how) */ \
    X(BRK,           brk,            214)        /* (addr): the break, moved there if it could be */ \
    X(MUNMAP,        munmap,         215)        /* (addr, len) */ \
    X(EXECVE,        execve,         221)        /* (path, argv, envp): returns only with an error */ \
    X(MMAP,          mmap,           222)        /* (addr, len, prot, flags, fd, offset in pages); Linux's mmap2 */ \
    X(MPROTECT,      mprotect,       226)        /* (addr, len, prot) */ \
    X(MSYNC,         msync,          227)        /* (addr, len, flags) */ \
    X(MINCORE,       mincore,        232)        /* (addr, len, uint8_t *vec): 1 a page resident */ \
    X(MADVISE,       madvise,        233)        /* (addr, len, advice) */ \
    X(ACCEPT,        accept,         242)        /* (fd, addr or 0, socklen_t *len, SOCK_* flags); Linux's accept4 */ \
    X(GETRANDOM,     getrandom,      278)        /* (buf, len, flags) */ \
    X(CLOCK_GETTIME, clock_gettime,  403)        /* (clock, struct espix_timespec *); Linux's clock_gettime64 */ \
    X(FUTEX,         futex,          422)        /* (uaddr, op, val, const struct espix_timespec *); Linux's futex_time64 */

/* espix's own, from ESPIX_SYS_OWN: calls Linux has under other arguments, or not at all. */
#define ESPIX_SYS_OWN 2048
#define ESPIX_SYSCALLS_OWN(X) \
    X(OPEN,          open,          2048)        /* (path, flags, mode) */ \
    X(LSEEK,         lseek,         2049)        /* (fd, off_lo, off_hi, whence, uint64_t *pos) */ \
    X(FSTAT,         fstat,         2050)        /* (fd, struct espix_stat *) */ \
    X(STAT,          stat,          2051)        /* (path, struct espix_stat *) */ \
    X(UNLINK,        unlink,        2052)        /* (path) */ \
    X(RENAME,        rename,        2053)        /* (from, to) */ \
    X(MKDIR,         mkdir,         2054)        /* (path, mode) */ \
    X(RMDIR,         rmdir,         2055)        /* (path) */ \
    X(UTIME,         utime,         2056)        /* (path, const struct espix_utimbuf *) */ \
    X(CHMOD,         chmod,         2057)        /* (path, mode) */ \
    X(DUP2,          dup2,          2058)        /* (fd, newfd) */ \
    X(GETDENTS,      getdents,      2059)        /* (fd, buf, len): struct espix_dirent records */ \
    X(ISATTY,        isatty,        2060)        /* (fd) */ \
    X(NANOSLEEP,     nanosleep,     2061)        /* (const struct espix_timespec *, rem) */ \
    X(GETPGRP,       getpgrp,       2062)        /* () */ \
    X(THREAD_CREATE, thread_create, 2063)        /* (entry, arg, tls, stack or 0, tid word or 0): the tid */ \
    X(THREAD_EXIT,   thread_exit,   2064)        /* (): its tid word is zeroed and woken */ \
    X(GETCPU,        getcpu,        2065)        /* (): the core the caller runs on */ \
    X(SIGACTION,     sigaction,     2066)        /* (sig, const struct espix_sigaction *, old) */ \
    X(SIGPROCMASK,   sigprocmask,   2067)        /* (how, const uint32_t *set, uint32_t *old): the thread's */ \
    X(SIGPENDING,    sigpending,    2068)        /* (uint32_t *): pending and blocked */ \
    X(SIGSUSPEND,    sigsuspend,    2069)        /* (const uint32_t *mask): -EINTR once a handler is due */ \
    X(RAISE,         raise,         2070)        /* (sig): to the calling thread */ \
    X(SETITIMER,     setitimer,     2071)        /* (which, const struct espix_itimerval *, old) */ \
    X(TERM_SIZE,     term_size,     2072)        /* (int *cols, int *rows) */ \
    X(POLL,          poll,          2073)        /* (struct pollfd *, nfds, ms or -1) */ \
    X(GETADDRINFO,   getaddrinfo,   2074)        /* (node, service, hints, out, max): a count, -errno or -EAI_* */ \
    X(GETHOSTNAME,   gethostname,   2075)        /* (buf, len) */ \
    X(GETPW,         getpw,         2076)        /* (name or 0, uid, struct espix_pwent *, len) */ \
    X(GETGR,         getgr,         2077)        /* (name or 0, gid, struct espix_pwent *, len) */ \
    X(SYSLOG,        syslog,        2078)        /* (priority, ident or 0, message): to the kernel log */ \
    X(GETIFADDRS,    getifaddrs,    2079)        /* (struct espix_ifaddr *, max): how many */ \
    X(GFX_OPEN,      gfx_open,      2080)        /* (full_w, full_h): claims the screen; 0,0 keeps its size */ \
    X(GFX_CLOSE,     gfx_close,     2081)        /* () */ \
    X(GFX_SURFACE,   gfx_surface,   2082)        /* (w, h, struct espix_gfx_fb *): a handle; 0,0 is the screen's size */ \
    X(GFX_FREE,      gfx_free,      2083)        /* (handle): its pixels are unmapped */ \
    X(GFX_PRESENT,   gfx_present,   2084)        /* (handle, x, y, w, h): w 0 scales it to fill the screen */ \
    X(GFX_POLL,      gfx_poll,      2085)        /* (struct espix_input *): 1 with an event, 0 without */ \
    X(AUDIO_OPEN,    audio_open,    2086)        /* (const struct espix_audio_fmt *): a handle */ \
    X(AUDIO_WRITE,   audio_write,   2087)        /* (handle, buf, len): bytes taken, blocking while the ring is full */ \
    X(AUDIO_CTL,     audio_ctl,     2088)        /* (handle, ESPIX_AUDIO_*, arg) */ \
    X(AUDIO_CLOSE,   audio_close,   2089)        /* (handle): what is queued plays out */ \
    X(MEMINFO,       meminfo,       2090)        /* (struct espix_meminfo *) */ \
    X(WAIT4,         wait4,         2091)        /* (pid, int *status or 0, WNOHANG | WUNTRACED): a child's pid, 0 for none yet */ \
    X(FORK,          fork,          2092)        /* (): the child's pid, and 0 in the child */ \
    X(VFORK,         vfork,         2093)        /* (): fork, the child borrowing the caller's memory and the caller waiting until it execs or ends */

#define ESPIX_SYSCALLS(X) ESPIX_SYSCALLS_LINUX(X) ESPIX_SYSCALLS_OWN(X)

/* The longest path a syscall takes, with its NUL: the kernel's ESPIX_PATH_MAX. */
#define ESPIX_SYS_PATH_MAX 128

/* The clock /proc counts in, and sysconf(_SC_CLK_TCK): Linux's USER_HZ, whatever the kernel's tick. */
#define ESPIX_USER_HZ 100

/* The app ABI an app is built for, in its .note.espix (tools/build-libc.sh); another is refused at exec. */
#define ESPIX_ABI_VERSION 3

enum {
#define ESPIX_SYS_ENUM_(name, lname, nr) ESPIX_SYS_##name = nr,
    ESPIX_SYSCALLS(ESPIX_SYS_ENUM_)
#undef ESPIX_SYS_ENUM_
};

/* uname's fields: Linux's struct new_utsname. */
#define ESPIX_UTS_LEN 65
struct espix_utsname {
    char sysname[ESPIX_UTS_LEN];
    char nodename[ESPIX_UTS_LEN];
    char release[ESPIX_UTS_LEN];
    char version[ESPIX_UTS_LEN];
    char machine[ESPIX_UTS_LEN];
    char domainname[ESPIX_UTS_LEN];
};

/* mmap's protections and flags, and madvise's advice: Linux's (user/include/sys/mman.h). */
#define ESPIX_PROT_READ           0x1
#define ESPIX_PROT_WRITE          0x2
#define ESPIX_PROT_EXEC           0x4
#define ESPIX_MAP_SHARED          0x01
#define ESPIX_MAP_PRIVATE         0x02
#define ESPIX_MAP_SHARED_VALIDATE 0x03
#define ESPIX_MAP_TYPE            0x0f
#define ESPIX_MAP_FIXED           0x10
#define ESPIX_MAP_ANONYMOUS       0x20
#define ESPIX_MAP_POPULATE        0x8000
#define ESPIX_MAP_FIXED_NOREPLACE 0x100000
#define ESPIX_MADV_DONTNEED       4
#define ESPIX_MADV_FREE           8

/* A process's descriptors: sysconf(_SC_OPEN_MAX). */
#define ESPIX_OPEN_MAX 32

/* A terminal's ioctls on fds 0-2, Linux's numbers; the termios they carry is IDF's layout (user/include/sys/termios.h). */
#define ESPIX_TCGETS     0x5401
#define ESPIX_TCSETS     0x5402      /* +1 TCSADRAIN, +2 TCSAFLUSH */
#define ESPIX_TCFLSH     0x540B
#define ESPIX_TIOCGPGRP  0x540F
#define ESPIX_TIOCSPGRP  0x5410
#define ESPIX_TIOCGWINSZ 0x5413
#define ESPIX_TIOCSWINSZ 0x5414

/* FUTEX ops: wait while *uaddr == val (a relative timeout, or none), or wake up to val waiters. */
#define ESPIX_FUTEX_WAIT 0
#define ESPIX_FUTEX_WAKE 1

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

struct espix_itimerval {
    struct espix_timespec interval, value;
};

/* SA_* flags; the handler is SIG_DFL (0), SIG_IGN (1) or a function, which returns to restorer. */
struct espix_sigaction {
    uint32_t handler, flags, mask, restorer;
};

/* picolibc's siginfo_t, field for field. */
struct espix_siginfo {
    int32_t  signo, code, err, pid, uid;
    uint32_t addr;
    int32_t  status, value;
};

/* What a handler finds at sp, and SIGRETURN puts back. */
struct espix_sigframe {
    struct espix_siginfo info;
    uint32_t mask;          /* the thread's mask before the handler */
    uint32_t regs[32];      /* pc, then x1-x31 */
    uint32_t fregs[33];     /* f0-f31, fcsr */
};

/* One getdents record; the next starts reclen bytes on, 4-aligned. */
struct espix_dirent {
    uint32_t ino;
    uint16_t reclen;
    uint8_t  type;
    char     name[];
};

/* One getaddrinfo() result, and its hints: addr holds addrlen bytes of a sockaddr. */
struct espix_addrinfo {
    int32_t  flags, family, socktype, protocol;
    uint32_t addrlen;
    uint8_t  addr[28];
};

/* A user's or a group's record: its ids, then name, home and shell (a group: its name), each NUL-ended. */
struct espix_pwent {
    uint32_t id, gid;
    char     strs[];
};

/* One getifaddrs() entry: an interface and one of its addresses (an AF_UNSPEC addr: none). */
struct espix_ifaddr {
    char     name[16];
    uint32_t flags;             /* IFF_* */
    uint8_t  addr[28];          /* sockaddrs, lwIP's layout */
    uint8_t  netmask[28];
};

/* A surface's pixels, mapped into the app: stride is in pixels. */
struct espix_gfx_fb {
    int32_t  w, h, stride;
    uint32_t pixels;
};

/* espix_input_event_t, fixed-width. */
struct espix_input {
    uint32_t kind;
    int16_t  x, y;
    uint32_t buttons, keysym, down;
};

struct espix_audio_fmt {
    uint32_t rate, channels, bits;
};

/* AUDIO_CTL ops: a volume of 0-100, mute on or off, or the microseconds queued. */
#define ESPIX_AUDIO_VOLUME 0
#define ESPIX_AUDIO_MUTE   1
#define ESPIX_AUDIO_QUEUED 2

/* Pages of the memory apps are given, and of it what is free. */
struct espix_meminfo {
    uint32_t page_size, total, free;
};
