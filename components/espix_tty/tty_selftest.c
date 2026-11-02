/*
 * `ttytest`: the line discipline, termios and the descriptor, exercised on a
 * tty of its own with no transport behind it. Each case says ok or what it saw.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <unistd.h>

#include "espix_tty.h"

#ifndef FIONREAD
#define FIONREAD 0x4004667f
#endif

typedef struct {
    char   out[256];
    size_t n;
} capture_t;

static ssize_t capture(void *ctx, const void *buf, size_t len)
{
    capture_t *c = ctx;
    const size_t n = (len < sizeof(c->out) - 1 - c->n) ? len : sizeof(c->out) - 1 - c->n;
    memcpy(c->out + c->n, buf, n);
    c->n += n;
    c->out[c->n] = '\0';
    return (ssize_t)len;
}

typedef struct {
    void (*say)(void *ctx, const char *line);
    void  *ctx;
    int    failed;
} report_t;

static void check(report_t *r, const char *what, bool ok, const char *seen)
{
    char line[160];
    snprintf(line, sizeof(line), "ttytest: %s: %s%s%s", what, ok ? "ok" : "FAILED", ok ? "" : ", saw ",
             ok ? "" : seen);
    r->say(r->ctx, line);
    r->failed += !ok;
}

/* A string with its control bytes shown, for a report. */
static const char *shown(const char *s, size_t n, char *buf, size_t size)
{
    size_t o = 0;
    for (size_t i = 0; i < n && o + 5 < size; i++) {
        const unsigned char c = (unsigned char)s[i];
        o += (c < 0x20 || c == 0x7f) ? (size_t)snprintf(buf + o, size - o, "\\x%02x", c)
                                     : (size_t)snprintf(buf + o, size - o, "%c", c);
    }
    buf[o] = '\0';
    return buf;
}

int espix_tty_selftest(void (*say)(void *ctx, const char *line), void *ctx)
{
    report_t r = { .say = say, .ctx = ctx };
    capture_t cap = { .n = 0 };
    const espix_tty_ops_t ops = { .write = capture, .ctx = &cap };
    espix_tty_t *t = espix_tty_create(&ops, 512, false);
    char buf[64], sh[128];
    if (t == NULL) {
        check(&r, "create", false, "NULL");
        return 1;
    }

    espix_tty_input(t, "ab\x7f" "c\r", 5);
    ssize_t n = espix_tty_read(t, buf, sizeof(buf), true);
    check(&r, "a canonical line, edited", n == 3 && memcmp(buf, "ac\n", 3) == 0,
          shown(buf, n > 0 ? (size_t)n : 0, sh, sizeof(sh)));
    check(&r, "and its echo", strcmp(cap.out, "ab\b \bc\r\n") == 0, shown(cap.out, cap.n, sh, sizeof(sh)));

    espix_tty_input(t, "xyz\x15hi\n", 7);
    n = espix_tty_read(t, buf, sizeof(buf), true);
    check(&r, "^U kills the line", n == 3 && memcmp(buf, "hi\n", 3) == 0, shown(buf, n > 0 ? (size_t)n : 0, sh, sizeof(sh)));

    espix_tty_input(t, "one\ntwo\n", 8);
    n = espix_tty_read(t, buf, sizeof(buf), true);
    check(&r, "a read takes one line", n == 4 && memcmp(buf, "one\n", 4) == 0, shown(buf, n > 0 ? (size_t)n : 0, sh, sizeof(sh)));
    n = espix_tty_read(t, buf, sizeof(buf), true);
    check(&r, "and the next one the next", n == 4 && memcmp(buf, "two\n", 4) == 0, shown(buf, n > 0 ? (size_t)n : 0, sh, sizeof(sh)));

    espix_tty_input(t, "\x04", 1);
    n = espix_tty_read(t, buf, sizeof(buf), true);
    check(&r, "^D on an empty line is the end of input", n == 0, "data");
    espix_tty_input(t, "abc\x04", 4);
    n = espix_tty_read(t, buf, sizeof(buf), true);
    check(&r, "and after text ends the line without a newline", n == 3 && memcmp(buf, "abc", 3) == 0,
          shown(buf, n > 0 ? (size_t)n : 0, sh, sizeof(sh)));

    n = espix_tty_read(t, buf, sizeof(buf), true);
    check(&r, "an empty tty, read without blocking, says EAGAIN", n == -EAGAIN, "something else");

    const int fd = espix_tty_fileno(t);
    fd_set rd;
    struct timeval tv = { 0, 100000 };
    FD_ZERO(&rd);
    FD_SET(fd, &rd);
    int s = select(fd + 1, &rd, NULL, NULL, &tv);
    check(&r, "select times out on it while it is empty", s == 0, "ready");
    espix_tty_input(t, "q\n", 2);
    tv = (struct timeval){ 0, 100000 };
    FD_ZERO(&rd);
    FD_SET(fd, &rd);
    s = select(fd + 1, &rd, NULL, NULL, &tv);
    check(&r, "and sees a line arrive", s == 1 && FD_ISSET(fd, &rd), "not ready");
    int queued = -1;
    check(&r, "FIONREAD counts it", ioctl(fd, FIONREAD, &queued) == 0 && queued == 2, "wrong count");
    n = read(fd, buf, sizeof(buf));
    check(&r, "and read() on the descriptor takes it", n == 2 && memcmp(buf, "q\n", 2) == 0, "wrong bytes");

    struct termios tio;
#ifdef CONFIG_VFS_SUPPORT_TERMIOS
    check(&r, "tcgetattr reaches it", tcgetattr(fd, &tio) == 0 && (tio.c_lflag & ICANON), "no termios");
#else
    espix_tty_getattr(t, &tio);
#endif
    espix_tty_makeraw(&tio);
#ifdef CONFIG_VFS_SUPPORT_TERMIOS
    tcsetattr(fd, TCSANOW, &tio);
#else
    espix_tty_setattr(t, TCSANOW, &tio);
#endif
    cap.n = 0;
    cap.out[0] = '\0';
    espix_tty_input(t, "\x03z", 2);
    n = espix_tty_read(t, buf, sizeof(buf), true);
    check(&r, "raw mode hands over every byte, ^C too", n == 2 && buf[0] == 0x03 && buf[1] == 'z',
          shown(buf, n > 0 ? (size_t)n : 0, sh, sizeof(sh)));
    check(&r, "and echoes nothing", cap.n == 0, shown(cap.out, cap.n, sh, sizeof(sh)));
    espix_tty_write(t, "x\n", 2);
    check(&r, "nor turns a newline into CRLF", strcmp(cap.out, "x\n") == 0, shown(cap.out, cap.n, sh, sizeof(sh)));

    espix_tty_get(t);
    espix_tty_hangup(t);
    n = espix_tty_read(t, buf, sizeof(buf), false);
    check(&r, "a hung-up tty reads as the end of input", n == 0, "data or an error");
    check(&r, "and refuses a write", espix_tty_write(t, "x", 1) == -EIO, "accepted it");
    espix_tty_put(t);

    char line[48];
    snprintf(line, sizeof(line), "ttytest: %s", r.failed ? "FAILED" : "all ok");
    say(ctx, line);
    return r.failed ? 1 : 0;
}
