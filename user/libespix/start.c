/* A U-mode app's start: TLS, the standard streams, constructors, then main(). */
#include <picotls.h>
#include <stdio-bufio.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>


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

char **environ;

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
        static const char msg[] = "libespix: TLS larger than the main thread's block\n";
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
