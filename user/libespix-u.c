/*
 * libespix-u: picolibc's porting layer as espix syscalls, and the start of a
 * U-mode app (docs/USER-MODE.md).
 */
#include <errno.h>
#include <picotls.h>
#include <stdio-bufio.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "espix_syscall.h"

static inline long espix_syscall3(long n, long a, long b, long c)
{
    register long a0 __asm__("a0") = a;
    register long a1 __asm__("a1") = b;
    register long a2 __asm__("a2") = c;
    register long a7 __asm__("a7") = n;
    __asm__ volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}

static long sys_ret(long r)
{
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    return r;
}

ssize_t write(int fd, const void *buf, size_t n)
{
    return sys_ret(espix_syscall3(ESPIX_SYS_WRITE, fd, (long)buf, (long)n));
}

ssize_t read(int fd, void *buf, size_t n)
{
    (void)fd;
    (void)buf;
    (void)n;
    errno = ENOSYS;
    return -1;
}

off_t lseek(int fd, off_t off, int whence)
{
    (void)fd;
    (void)off;
    (void)whence;
    errno = ESPIPE;
    return -1;
}

int close(int fd)
{
    (void)fd;
    return 0;
}

_Noreturn void _exit(int status)
{
    espix_syscall3(ESPIX_SYS_EXIT, status, 0, 0);
    for (;;) {
    }
}

static char s_inbuf[64], s_outbuf[256], s_errbuf[64];
static struct __file_bufio s_stdin  = FDEV_SETUP_BUFIO(0, s_inbuf, sizeof(s_inbuf), read, write,
                                                       lseek, close, _FDEV_SETUP_READ, 0);
static struct __file_bufio s_stdout = FDEV_SETUP_BUFIO(1, s_outbuf, sizeof(s_outbuf), read, write,
                                                       lseek, close, _FDEV_SETUP_WRITE, __BLBF);
static struct __file_bufio s_stderr = FDEV_SETUP_BUFIO(2, s_errbuf, sizeof(s_errbuf), read, write,
                                                       lseek, close, _FDEV_SETUP_WRITE, __BLBF);
FILE *stdin  = &s_stdin.xfile.cfile.file;
FILE *stdout = &s_stdout.xfile.cfile.file;
FILE *stderr = &s_stderr.xfile.cfile.file;

/* The main thread's TLS block; picolibc keeps errno there. */
static char s_tls[256] __attribute__((aligned(16)));

int main(int argc, char **argv, char **envp);
void __libc_init_array(void);

static void flush_std(void)
{
    fflush(stdout);
    fflush(stderr);
}

void __espix_start(int argc, char **argv, char **envp)
{
    if (_tls_size() > sizeof(s_tls)) {
        static const char msg[] = "libespix-u: TLS larger than the main thread's block\n";
        write(2, msg, sizeof(msg) - 1);
        _exit(127);
    }
    _init_tls(s_tls);
    _set_tls(s_tls);
    environ = envp;
    __libc_init_array();
    atexit(flush_std);
    exit(main(argc, argv, envp));
}
