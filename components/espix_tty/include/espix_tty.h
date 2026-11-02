/*
 * A session's terminal, as Linux's tty is: the input a transport delivers, the
 * line discipline between it and the readers (echo, line editing, the signal
 * characters), termios, the foreground process group and the window size.
 *
 * A transport (SSH, the console, the VNC terminal) feeds it with
 * espix_tty_input() and takes its output through ops.write. The shell and its
 * apps read and write it directly; select() and poll() wait on its one VFS
 * descriptor, espix_tty_fileno(), which is also what tcgetattr() and ioctl()
 * reach. An exec channel's stdin is a pipe: the same object with no line
 * discipline, and not a terminal.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <sys/termios.h>
#include <sys/types.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Linux's numbers; IDF defines none of these. */
#ifndef TIOCGWINSZ
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#endif
#ifndef TIOCGPGRP
#define TIOCGPGRP  0x540F
#define TIOCSPGRP  0x5410
#endif

#ifndef _ESPIX_WINSIZE_DEFINED
#define _ESPIX_WINSIZE_DEFINED
struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};
#endif

typedef struct espix_tty espix_tty_t;

typedef struct {
    /* Bytes for the terminal, after output processing; may block. Never called with the tty locked. */
    ssize_t (*write)(void *ctx, const void *buf, size_t len);
    /* Room in the input again after it filled: a transport that held input back resumes. May be NULL. */
    void    (*room)(void *ctx);
    void     *ctx;
} espix_tty_ops_t;

/* What the process layer lends the tty: espix_proc registers these at init. */
typedef struct {
    int   (*signal_group)(pid_t pgid, int sig);
    /* The caller's process group, or 0 for a caller that is not a process. */
    pid_t (*self_pgid)(void);
    /* After each wake of a blocked read: 0 to wait on, or -EINTR (a handler is due, or the process ends). */
    int   (*interrupted)(void);
} espix_tty_hooks_t;

void espix_tty_set_hooks(const espix_tty_hooks_t *hooks);

/* Registers the driver behind the descriptors, once, at boot. */
esp_err_t espix_tty_init(void);

/* A terminal, or with pipe an exec channel's stdin; inbuf bytes of input. NULL without memory or descriptors. */
espix_tty_t *espix_tty_create(const espix_tty_ops_t *ops, size_t inbuf, bool pipe);
/* The transport is gone: readers see the end of input, writers EIO. Drops the creator's reference. */
void espix_tty_hangup(espix_tty_t *t);
/* References for whoever keeps a pointer past the call that gave it (an app's streams, a U-mode fd). */
espix_tty_t *espix_tty_get(espix_tty_t *t);
void         espix_tty_put(espix_tty_t *t);

/* The producer: through the line discipline into the input; how many bytes it took (the rest: try again on room). */
size_t espix_tty_input(espix_tty_t *t, const void *buf, size_t len);
/* The end of input (an exec channel's EOF): readers see it once what is buffered is read. */
void   espix_tty_input_eof(espix_tty_t *t);

/* The consumers. A blocking read ends with -EINTR when a handler is due; 0 is the end of input. */
ssize_t espix_tty_read(espix_tty_t *t, void *buf, size_t len, bool nonblock);
ssize_t espix_tty_write(espix_tty_t *t, const void *buf, size_t len);
/* Whether a read would return now, and how many bytes it could. */
size_t  espix_tty_readable(espix_tty_t *t);

/* The VFS descriptor select(), poll(), tcgetattr() and ioctl() take; the tty owns it. */
int  espix_tty_fileno(const espix_tty_t *t);
bool espix_tty_is_pipe(const espix_tty_t *t);

void  espix_tty_getattr(espix_tty_t *t, struct termios *tio);
void  espix_tty_setattr(espix_tty_t *t, int when, const struct termios *tio);
/* A terminal for a program that does its own editing: no echo, no canonical lines, no signal keys. */
void  espix_tty_makeraw(struct termios *tio);

/* The foreground process group: its readers read, and the signal keys signal it. */
void  espix_tty_set_fg(espix_tty_t *t, pid_t pgid);
pid_t espix_tty_get_fg(const espix_tty_t *t);
/* The window size; a change sends SIGWINCH to the foreground group. */
void  espix_tty_set_winsize(espix_tty_t *t, int cols, int rows);
void  espix_tty_get_winsize(const espix_tty_t *t, int *cols, int *rows);

/* `ttytest`: the line discipline and the descriptor on a tty of its own; 0 when every case passed. */
int espix_tty_selftest(void (*say)(void *ctx, const char *line), void *ctx);

#ifdef __cplusplus
}
#endif
