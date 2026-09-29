/*
 * The on-screen console: an espix terminal rendered into the display canvas.
 *
 * It is the third transport for the same shell, after the UART console and the
 * SSH channel, and it exists so that a viewer always has something useful to
 * look at -- a board with no desktop program still gives you a prompt, which is
 * the point.
 *
 * On demand. It is started when a viewer attaches and nothing else owns the
 * screen, and stopped when the viewer goes, so a headless board allocates none
 * of it. That is also why it lives in espix_shell rather than in the display
 * service: it is a shell transport that happens to draw to a canvas, and a
 * display should not need a shell to exist.
 *
 * What is left here is the part that is about the canvas: the screen owner, the
 * three functions that draw a terminal into it, and the task. The terminal
 * itself -- the grid, the parser, the editor, the session -- is espix_term, and
 * the desktop's terminal window is the same object drawn somewhere else.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"

#include "espix_display.h"
#include "espix_kernel.h"
#include "espix_shell.h"
#include "espix_term.h"

#define TAG "vnc0"

/*
 * An inset, because a grid flush to the edge reads as a bug rather than as a
 * terminal -- the first glyph's left edge sits on the bezel and the last row
 * has nowhere to breathe. Sixteen pixels is two cells: enough to look
 * deliberate without wasting screen.
 */
#define CON_MARGIN 16
#define CON_COLS ((ESPIX_DISPLAY_W - 2 * CON_MARGIN) / 8)
#define CON_ROWS ((ESPIX_DISPLAY_H - 2 * CON_MARGIN) / 8)

#define CON_FG 0xC618   /* light grey on black, the usual terminal look */
#define CON_BG 0x0000

typedef struct {
    espix_term_t *term;
    TaskHandle_t  task;
    bool          up;
    /* Set while the console is shutting down, so that a release -- which asks
     * the display for its fallback -- does not start this console again. */
    bool          leaving;
    uint32_t      gen;
} canvas_console_t;

static canvas_console_t s_con;

/* Bumped for each console, so a task that outlives its own can tell. */
static uint32_t s_gen;

static void con_input(void *ctx, const espix_input_event_t *ev);
static void con_repaint(void *ctx);

static const espix_screen_t s_console_screen = {
    .name    = "vnc-console",
    .input   = con_input,
    .repaint = con_repaint,
    /*
     * The grid is a fixed CON_COLS x CON_ROWS (the terminal has no reflow), so
     * a resize cannot give the console more room -- but the canvas it drew into
     * is gone, so it must at least draw its grid again or the screen stays
     * blank until the next output.
     */
    .resized = con_repaint,
    .ctx     = NULL,
};

/*
 * Whether this console is the one being rendered.
 *
 * The owner slot means "whose model is on the screen", so being taken over does
 * not stop the console: it keeps its grid, its history and its session, and
 * stops being drawn. Every drawing entry point therefore checks this --
 * without it, output from a console that is no longer on the screen is painted
 * over whatever replaced it. That is what "the console shines through the
 * desktop" is: `desktop start` typed on the console, and then the console's
 * own `desktop: up` and prompt drawn on top of the desktop.
 */
static bool con_on_screen(void)
{
    return espix_display_owns(&s_console_screen);
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

/*
 * The terminal's view, which is the only part of it that knows about the
 * canvas. Each entry point locks the canvas for itself and does not nest --
 * espix_canvas_lock() is a plain mutex, and the drawing calls below it take no
 * lock of their own. A repaint therefore holds the lock per row rather than
 * across the lot; the damage each row marks is what keeps a viewer correct.
 */
static void con_view_cell(void *ctx, int row, int col, char ch)
{
    (void)ctx;

    espix_canvas_t *cv = espix_display_canvas();
    if (cv == NULL || !con_on_screen()) {
        return;
    }
    const char cell[2] = { ch, '\0' };

    espix_canvas_lock(cv);
    espix_canvas_text(cv, CON_MARGIN + col * 8, CON_MARGIN + row * 8,
                      cell, CON_FG, CON_BG);
    espix_canvas_unlock(cv);
}

static void con_view_row(void *ctx, int row, const char *cells, int len)
{
    (void)ctx;

    espix_canvas_t *cv = espix_display_canvas();
    if (cv == NULL || !con_on_screen()) {
        return;
    }
    if (len > ESPIX_TERM_MAX_COLS) {
        len = ESPIX_TERM_MAX_COLS;
    }

    char line[ESPIX_TERM_MAX_COLS + 1];
    memcpy(line, cells, (size_t)len);
    line[len] = '\0';

    espix_canvas_lock(cv);
    espix_canvas_text(cv, CON_MARGIN, CON_MARGIN + row * 8, line, CON_FG, CON_BG);
    espix_canvas_unlock(cv);
}

/* The margin is outside the grid, so it is painted too -- otherwise whatever
 * the previous owner left there shows through. */
static void con_view_clear(void *ctx)
{
    (void)ctx;

    espix_canvas_t *cv = espix_display_canvas();
    if (cv == NULL || !con_on_screen()) {
        return;
    }

    espix_canvas_lock(cv);
    espix_canvas_fill(cv, (espix_rect_t){ 0, 0, ESPIX_DISPLAY_W, ESPIX_DISPLAY_H },
                      CON_BG);
    espix_canvas_unlock(cv);
}

static const espix_term_view_t s_con_view = {
    .ctx   = NULL,
    .cell  = con_view_cell,
    .row   = con_view_row,
    .clear = con_view_clear,
};

/* ------------------------------------------------------------------ */
/* Input and output                                                    */
/* ------------------------------------------------------------------ */

static void con_input(void *ctx, const espix_input_event_t *ev)
{
    (void)ctx;

    if (ev->kind != ESPIX_INPUT_KEY || s_con.term == NULL) {
        return;                         /* a text console has no pointer */
    }

    /* The keysym-to-byte and Ctrl-<key> translation lives with the terminal,
     * because a local keyboard and a VNC viewer arrive the same way. */
    espix_term_key(s_con.term, ev->keysym, ev->down);
}

/* Redraw from the model. This is what makes ownership a repaint rather than a
 * handover of state -- the console keeps its screen whether or not it is the
 * one on display. */
static void con_repaint(void *ctx)
{
    (void)ctx;
    espix_term_repaint(s_con.term);
}

/* ------------------------------------------------------------------ */
/* The session                                                         */
/* ------------------------------------------------------------------ */

static void con_task(void *arg)
{
    /* Which console this task belongs to. A task that would not stop keeps
     * running while a newer console exists, and its teardown must then free
     * nothing -- those buffers are the new console's. */
    const uint32_t gen = (uint32_t)(uintptr_t)arg;
    espix_term_t  *t   = s_con.term;

    if (t != NULL) {
        espix_session_t *s = espix_term_session(t);

        s->name = "vnc0";
        /*
         * The esp account, not root -- the deliberate difference from the
         * serial console. That one is root because whoever is holding the board
         * has already won; a viewer over the network has not, so this console
         * starts unprivileged and escalates with sudo when it means to. The
         * protection against someone who does hold the cable, or the VNC
         * password, is the SSH tunnel rather than RFB's own authentication.
         *
         * 1000 and "esp" are written out rather than named because espix_auth
         * owns the names and this component cannot reach it: espix_auth is
         * above espix_fs, which is above this one. Same reason, and the same
         * shape, as the uid in tty_console.c.
         */
        s->uid   = 1000;            /* esp */
        s->gid   = 1000;
        strlcpy(s->user, "esp", sizeof(s->user));
        strlcpy(s->home, "/home/esp", sizeof(s->home));

        espix_term_run(t);
    }

    s_con.leaving = true;

    /*
     * The terminal owns its grid, its queue and its editor, and this task is the
     * only thing that knows it has stopped touching them -- which is why the
     * freeing was here in the first place. Doing it from stop() left a window in
     * which a task still inside read_line touched a queue that no longer
     * existed, which is the xQueueReceive(NULL) assert that rebooted the board.
     */
    espix_term_free(t);

    if (gen != s_gen) {
        espix_klog(ESPIX_KLOG_INFO, TAG, "stale console task exiting");
        vTaskDeleteWithCaps(NULL);
    }

    s_con.term = NULL;
    s_con.up   = false;
    s_con.task = NULL;

    espix_display_release(&s_console_screen);
    vTaskDeleteWithCaps(NULL);          /* frees the PSRAM stack it was given */
}

esp_err_t espix_console_canvas_start(void)
{
    /*
     * Idempotent, and it re-claims when it is already running: a desktop that
     * took the screen and then gave it back must find the console where it
     * left it, not a dead screen.
     */
    if (s_con.leaving) {
        return ESP_ERR_INVALID_STATE;   /* on its way out; do not restart it */
    }
    if (s_con.up) {
        return espix_display_claim(&s_console_screen);
    }

    memset(&s_con, 0, sizeof(s_con));

    s_con.term = espix_term_new(&s_con_view, CON_COLS, CON_ROWS, "esp");
    if (s_con.term == NULL) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "cannot create the terminal");
        return ESP_ERR_NO_MEM;
    }

    /* Claimed before the task runs, so the first frame is the console's and
     * nothing can slip in between. */
    if (espix_display_claim(&s_console_screen) != ESP_OK) {
        espix_term_free(s_con.term);
        s_con.term = NULL;
        return ESP_ERR_INVALID_STATE;
    }

    s_con.up = true;

    void *const gen = (void *)(uintptr_t)++s_gen;

    /*
     * 8192, not 4096: this task runs commands *inline*, exactly as the SSH
     * connection task does, and that task's stack is sized for the same reason.
     * 4096 tripped the canary on `ps` -- a stack-protection panic inside
     * _svfprintf_r -- which is the same class of failure the SSH stack's own
     * comment records for `lsusb` at 6144.
     *
     * A command that needs more still declares its own stack in the command
     * table, where it costs one command rather than every console.
     */
    if (xTaskCreateWithCaps(con_task, "espix:vnc0", 8192, gen, 4, &s_con.task,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        (void)xTaskCreateWithCaps(con_task, "espix:vnc0", 8192, gen, 4,
                                  &s_con.task,
                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (s_con.task == NULL) {
        espix_display_release(&s_console_screen);
        espix_term_free(s_con.term);
        s_con.term = NULL;
        s_con.up   = false;
        return ESP_ERR_NO_MEM;
    }

    espix_klog(ESPIX_KLOG_INFO, TAG, "console up (%dx%d)", CON_COLS, CON_ROWS);
    return ESP_OK;
}

void espix_console_canvas_stop(void)
{
    if (!s_con.up) {
        return;
    }

    /*
     * Asked, not forced -- and nothing is freed here. The task releases the
     * screen and frees the terminal as its last act, because only it knows when
     * it has stopped using them. Waiting for it to be gone is the whole of this
     * function; anything freed here would be freed while the task might still
     * be in read_line.
     */
    s_con.leaving = true;
    espix_term_stop(s_con.term);

    for (int i = 0; i < 300 && s_con.task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (s_con.task != NULL) {
        /*
         * Better a leaked terminal than a freed one under a live task, so the
         * task keeps its own -- but the screen is released here regardless. A
         * console that is gone must not hold it: that is what left the next
         * connection staring at a frozen image with no console of its own.
         */
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "console task did not stop; releasing the screen anyway");
        espix_display_release(&s_console_screen);
    }

    s_con.up      = false;
    s_con.leaving = false;
    espix_klog(ESPIX_KLOG_INFO, TAG, "console down");
}
