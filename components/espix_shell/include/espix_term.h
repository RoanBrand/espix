/*
 * A terminal: a grid of cells, a parser for the escape sequences that address
 * it, and a shell session on top -- with the drawing left to the caller.
 *
 * This is what the on-screen console used to be, minus the part that was about
 * the canvas. A terminal does not know what it is being drawn into, and the two
 * things that want one here are drawn into different places entirely: the
 * console into the display canvas, and the desktop's terminal window into its
 * own surface. Both want the same CSI handling, the same UTF-8 decoding, the
 * same editor, and the same session, and none of that is about pixels.
 *
 * The seam is `espix_term_view_t`: three functions that draw a cell, a row, and
 * a blank screen. Everything above them -- the grid, the cursor, the parser, the
 * editor, the session -- is shared.
 *
 * Ownership stays with the caller. A terminal is created, run in the caller's
 * own task, and freed when that task ends; it does not start tasks or claim the
 * screen, because those are the caller's business (one is a screen owner, the
 * other is a window).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "espix_shell.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A row longer than this is a size somebody got wrong, not a screen. */
#define ESPIX_TERM_MAX_COLS 160
#define ESPIX_TERM_MAX_ROWS 100

/*
 * Where a terminal's pixels go.
 *
 * `row` is the one that matters for speed -- a whole row of cells in one call,
 * which is how both targets draw text -- and `cell` is for the single-character
 * updates a keystroke or a cursor move makes. `clear` blanks everything the grid
 * covers, including any margin outside it.
 *
 * An implementation is free to draw nothing: the console's checks whether it is
 * still the thing on screen, because output from a console that has been taken
 * over must not be painted over its replacement.
 */
typedef struct {
    void *ctx;
    void (*cell)(void *ctx, int row, int col, char ch);
    void (*row)(void *ctx, int row, const char *cells, int len);
    void (*clear)(void *ctx);
} espix_term_view_t;

typedef struct espix_term espix_term_t;

/*
 * A terminal of `cols` x `rows` cells, drawing through `view` (copied). `user`
 * names the command history's owner, which is per-account rather than per-screen.
 */
espix_term_t *espix_term_new(const espix_term_view_t *view, int cols, int rows,
                             const char *user);

/* Frees the grid, the input queue and the editor. Not reentrant with run(). */
void espix_term_free(espix_term_t *t);

/*
 * The session the shell runs on, with the four callbacks that reach this
 * terminal already filled in. The caller sets the identity it wants -- name,
 * uid, gid, user, home, cwd -- before espix_term_run(), because that is a
 * policy about who is at the keyboard and not a property of a terminal.
 */
espix_session_t *espix_term_session(espix_term_t *t);

/*
 * Run the shell on this terminal until it is stopped: the motd, then a session,
 * then another, because `exit` ends the session rather than the terminal. Runs
 * in the caller's task and does not return until espix_term_stop().
 */
void espix_term_run(espix_term_t *t);

/* Ask run() to return. Idempotent, and safe from any task. */
void espix_term_stop(espix_term_t *t);
bool espix_term_stopping(const espix_term_t *t);

/*
 * A key event, already translated: keysym to byte, and Ctrl-<letter> to the
 * control code it means. Both a VNC viewer and a local keyboard arrive here the
 * same way, so the translation lives with the terminal rather than with each
 * source of keys.
 */
void espix_term_key(espix_term_t *t, uint32_t keysym, bool down);

/* The grid and the cursor, for anything that needs to draw them again. */
void espix_term_repaint(espix_term_t *t);

/*
 * The model itself, for a target that has to redraw from it rather than be told
 * to. A window is the case: the desktop paints a window's frame and then asks
 * the window to draw its content, and at that moment the surface lock is
 * already held -- so the content has to come from the grid directly rather than
 * through the view, which would take the lock again.
 *
 * The returned row is `cols` characters and is not NUL-terminated.
 */
int         espix_term_cols(const espix_term_t *t);
int         espix_term_rows(const espix_term_t *t);
const char *espix_term_row_text(const espix_term_t *t, int row);

/* Bytes into the parser, as if a program had written them. */
void espix_term_write(espix_term_t *t, const char *data, size_t len);

#ifdef __cplusplus
}
#endif
