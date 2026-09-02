/*
 * espix console transport — the local UART / USB-Serial-JTAG session.
 *
 * This is the first espix_session_t implementation. It replicates the driver
 * setup that components/console/esp_console_repl_chip.c performs (that file is
 * the reference), but deliberately does not call esp_console_start_repl(): the
 * REPL owns its own task and its own dispatch, and espix shares dispatch with
 * SSH sessions.
 *
 * Line editing is espressif/esp_linenoise, one instance per session, so this
 * console and an SSH session get the same editing with separate histories.
 * IDF's own linenoise could not do that: its history and callbacks are
 * file-scope statics and it reads raw file descriptors.
 */

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_linenoise.h"
#include "esp_timer.h"
#include "esp_vfs_eventfd.h"
#include "sdkconfig.h"

#if CONFIG_ESP_CONSOLE_UART
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#endif

#include "espix_kernel.h"
#include "espix_shell.h"

#define TAG "console"

#define CONSOLE_RX_BUF 512

static espix_session_t s_console;

/* ------------------------------------------------------------------ */
/* Transport setup                                                     */
/* ------------------------------------------------------------------ */

static esp_err_t console_hw_init(void)
{
    /*
     * Drain stdout before touching the UART. Installing the driver runs
     * uart_hal_txfifo_rst(), which throws away anything still shifting out —
     * at 115200 baud the last couple of kernel log lines are typically still
     * in flight, and they get truncated mid-word. fflush() only pushes into
     * the FIFO; fsync() is what waits for the transmitter to go idle.
     * (Same guard as components/console/esp_console_repl_chip.c.)
     */
    fflush(stdout);
    fsync(fileno(stdout));

#if CONFIG_ESP_CONSOLE_UART
    const int uart_num = CONFIG_ESP_CONSOLE_UART_NUM;

    /* Line endings must be set before the driver takes over the VFS, so that
     * Enter arrives as CR and our output leaves as CRLF. */
    uart_vfs_dev_port_set_rx_line_endings(uart_num, ESP_LINE_ENDINGS_CR);
    uart_vfs_dev_port_set_tx_line_endings(uart_num, ESP_LINE_ENDINGS_CRLF);

    const uart_config_t cfg = {
        .baud_rate  = CONFIG_ESP_CONSOLE_UART_BAUDRATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_RETURN_ON_ERROR(uart_driver_install(uart_num, CONSOLE_RX_BUF, 0, 0, NULL, 0),
                        TAG, "uart_driver_install failed");
    ESP_RETURN_ON_ERROR(uart_param_config(uart_num, &cfg),
                        TAG, "uart_param_config failed");

    /* Blocking, interrupt-driven reads instead of the default polling stub. */
    uart_vfs_dev_use_driver(uart_num);
    return ESP_OK;

#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CR);
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);

    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(usb_serial_jtag_driver_install(&cfg),
                        TAG, "usb_serial_jtag_driver_install failed");

    usb_serial_jtag_vfs_use_driver();
    return ESP_OK;

#else
    /* USB CDC or a secondary console: stdio already works, but without a
     * driver behind it the editor cannot do escape sequences. */
    espix_klog(ESPIX_KLOG_WARN, TAG,
               "no console driver for this backend; line editing disabled");
    return ESP_OK;
#endif
}

/* ------------------------------------------------------------------ */
/* Line editing                                                        */
/* ------------------------------------------------------------------ */

static esp_linenoise_handle_t s_editor;
static espix_history_t       *s_history;

/*
 * The editor asks the terminal where the cursor is, twice per prompt, to work
 * out the width. The answer comes back as a burst — "\x1b[35;100R" — and pacing
 * every byte of it costs about 400ms per query. That was the whole of the
 * roughly one-second pause before a serial prompt, and it bought nothing:
 * pacing exists so the *editor's* paste heuristic does not mistake fast typing
 * for a clipboard paste, and a cursor report is consumed by
 * get_cursor_position()'s own loop, which never reaches that heuristic.
 *
 * So the question is watched for on the way out and the answer let through
 * unpaced. Everything a person types is still paced, which is all it was for.
 *
 * The bound on that window used to be a count of 24 reads, and that was the
 * bug behind a console stuck echoing "[A" for arrow keys until reboot. A
 * terminal that stops answering left pacing off for 24 reads per query and 48
 * per line — longer than most commands — and every new prompt re-armed it
 * before it could drain. With pacing off, esp_linenoise's heuristic sees each
 * key as pasted input and insert_pasted_char() puts the raw byte in the line
 * buffer without ever reaching the escape parser, so ESC itself lands in the
 * command.
 *
 * What has to be bounded is the *time* pacing spends switched off, not the
 * number of bytes that happen to arrive in it. A terminal that is going to
 * answer does so in about a millisecond.
 */
/*
 * How long after asking we will still accept a cursor report.
 *
 * This was 60ms, chosen as "a terminal answers in about a millisecond". That is
 * true of a terminal on the other end of a wire, and false of one reached
 * through idf.py monitor, where the reply makes a round trip through a USB
 * serial adapter and a terminal emulator. A real report arriving at ~150ms
 * landed outside the window, was taken for typing, and ran as a command:
 *
 *     espix: [31;1R: command not found
 *
 * A generous window is nearly free now that shape decides what counts as a
 * report (see report_byte_fits): anything that is not report-shaped ends the
 * window immediately and is treated as input, so the only thing a long window
 * costs is a user typing a literal ESC [ digit sequence within a second of a
 * prompt appearing.
 */
#define REPORT_WINDOW_US 1000000

/* Size claimed on behalf of a terminal that will not answer. */
#define REPORT_COLS 80
#define REPORT_ROWS 24

static bool    s_expect_report;
static int64_t s_report_deadline_us;
static unsigned s_report_pos;

/*
 * Set once a query has gone unanswered. After that the query is not sent at
 * all and the answer is synthesised, because an unanswered one also leaves
 * get_cursor_position() blocked in a read loop that swallows up to 31 typed
 * characters hunting for its terminator.
 */
static bool    s_terminal_mute;

/*
 * Answers we generate, handed back before real input. The same shape as
 * inject() in espix_ssh/ssh_channel.c, which has always needed it because an
 * SSH channel has no terminal to ask.
 */
static uint8_t s_injected[24];
static size_t  s_injected_len;
static size_t  s_injected_pos;

/* A real byte read while an answer still had to be delivered ahead of it. */
static int     s_pushback = -1;

/* ------------------------------------------------------------------ */
/* Fitting kernel messages around the prompt                           */
/* ------------------------------------------------------------------ */

/*
 * True while the session task is blocked inside esp_linenoise_get_line(), which
 * is when a prompt is on screen and nothing is watching it.
 */
static volatile bool s_editing;

/*
 * Whether anything has been done to the current line.
 *
 * This is a safety interlock, not a cosmetic one. Ending the line early makes
 * esp_linenoise_edit() return `state->len` -- the buffer as it stands -- and the
 * shell runs whatever comes back. Restarting a line with half a command on it
 * would therefore *execute* that half. So the line is only ever restarted when
 * this is false.
 *
 * Which is why it clears on almost nothing: only the keys that genuinely end or
 * abandon a line. Every other byte sets it, including TAB (completion can
 * insert text with no printable byte behind it) and the bytes of an escape
 * sequence. Erring towards "dirty" costs a redraw that does not happen, at a
 * moment when the user is demonstrably looking at the terminal anyway. Erring
 * the other way runs a command nobody typed.
 */
static volatile bool s_line_dirty;

/*
 * Set when kernel output has landed on a prompt that had nothing typed at it.
 * Consumed by console_read_bytes(), which ends the line so the session loop
 * draws a fresh one.
 *
 * Written by whichever task logged, read by the session task, so volatile --
 * a torn read is impossible on a bool, a cached one is not. Same reasoning as
 * s_last_echo_ms in klog.c.
 */
static volatile bool s_restart_line;

/*
 * An eventfd the logging task writes to, so the read below can wait on the
 * console *and* on "a message just went out" in one select() and still block
 * indefinitely when neither happens. No polling: the console task sleeps until
 * something actually occurs, exactly as it did before any of this.
 *
 * This is the mechanism esp_linenoise uses for its own esp_linenoise_abort()
 * -- state.abort_read_fd, selected on by esp_linenoise_default_read_bytes().
 * espix has to do it here because the abort path is documented as having no
 * effect once a custom read_bytes_cb is supplied, which espix supplies.
 *
 * -1 when unavailable, in which case the read blocks on the console alone and
 * the prompt reappears on the next keystroke instead of by itself.
 */
static int s_wake_fd = -1;

static void note_typed(uint8_t c)
{
    if (c == '\r' || c == '\n' || c == 0x03 /* Ctrl-C */ || c == 0x15 /* Ctrl-U */) {
        s_line_dirty = false;
    } else {
        s_line_dirty = true;
    }
}

/*
 * Kernel output is about to be written from another task: take the prompt off
 * the screen so the message does not land on it.
 *
 * Erasing alone is what the first version of this did, and it was wrong -- the
 * editor's multi-line refresh clears the rows it last used by walking *upward*
 * from a private, sticky row count anchored to where it believes its prompt is
 * (esp_linenoise.c:221), so moving the prompt to a row it does not know about
 * makes the next refresh erase rows now holding kernel output. Pressing Up was
 * enough to see it.
 *
 * What makes the erase safe now is that it is always followed by a *restart*:
 * output_done() below ends the input line, so esp_linenoise_edit() runs again
 * and resets its row count, cursor positions and column width on entry
 * (esp_linenoise.c:732-735) before drawing the prompt wherever the cursor has
 * ended up. The stale belief never survives long enough to be acted on.
 *
 * Only when the line is untouched -- see s_line_dirty. With something typed,
 * nothing is erased and nothing is restarted: the message lands beside the
 * input as it always did, which is ugly but cannot lose what was typed.
 */
static void console_klog_output_begin(void)
{
    if (!s_editing || s_line_dirty) {
        return;
    }

    /* Not on a terminal that has proven it ignores escape sequences: ESC[K
     * would be rubbish on screen. The message just appends there. */
    if (s_terminal_mute) {
        return;
    }

    fputs("\r\033[K", stdout);
    fflush(stdout);
}

/*
 * Kernel output has been written. Wake the input read so the line restarts and
 * the prompt is drawn again below the message.
 *
 * Signalled rather than done here: drawing from this task would race the
 * editor's own writes, and only the editor can leave its row bookkeeping
 * correct.
 */
static void console_klog_output_done(void)
{
    if (!s_editing || s_line_dirty || s_wake_fd < 0) {
        return;
    }

    s_restart_line = true;

    /* eventfd counts, so the value is irrelevant and a coalesced burst of
     * messages costs one wakeup. Must be an 8-byte write. */
    const uint64_t one = 1;
    (void)write(s_wake_fd, &one, sizeof(one));
}

static const espix_klog_console_hooks_t k_console_hooks = {
    .output_begin = console_klog_output_begin,
    .output_done  = console_klog_output_done,
};

static void inject(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf((char *)s_injected, sizeof(s_injected), fmt, ap);
    va_end(ap);

    if (n <= 0) {
        return;
    }
    s_injected_len = ((size_t)n < sizeof(s_injected)) ? (size_t)n
                                                      : sizeof(s_injected) - 1;
    s_injected_pos = 0;
}

/* A fresh session re-probes: the terminal on the other end may have changed. */
static void console_input_reset(void)
{
    s_expect_report = false;
    s_terminal_mute = false;
    s_injected_len  = 0;
    s_injected_pos  = 0;
    s_pushback      = -1;
}

/* Set only around esp_linenoise_create_instance(); see console_write_bytes. */
static bool s_probe_in_progress;

/*
 * Does this byte continue a cursor/status report -- ESC [ digits ; digits R, or
 * ESC [ digits n -- at the given position?
 *
 * Shape is what distinguishes the terminal answering us from the user typing,
 * and it does so without relying on timing. It also cleanly separates a report
 * from an arrow key: both start ESC [, but 'A' is not a digit, so ESC [ A ends
 * the window and is delivered as the keystroke it is.
 */
static bool report_byte_fits(uint8_t c, unsigned pos)
{
    switch (pos) {
    case 0:  return c == 0x1b;
    case 1:  return c == '[';
    default: return (c >= '0' && c <= '9') || c == ';' || c == 'R' || c == 'n';
    }
}

static bool contains_seq(const void *buf, size_t count, const char *lit)
{
    const size_t n = strlen(lit);

    if (count < n) {
        return false;
    }
    for (size_t i = 0; i + n <= count; i++) {
        if (memcmp((const uint8_t *)buf + i, lit, n) == 0) {
            return true;
        }
    }
    return false;
}

/*
 * Normally observes rather than intercepts: the query still goes to the
 * terminal, whose answer is the only source of the real width on a serial line.
 * Once a terminal has proven silent it intercepts instead, and answers.
 */
static ssize_t console_write_bytes(int fd, const void *buf, size_t count)
{
    const bool cursor = contains_seq(buf, count, "\x1b[6n");
    const bool status = contains_seq(buf, count, "\x1b[5n");

    /*
     * create_instance() announces "your terminal does not support escape
     * sequences" when its probe goes unanswered. We override that verdict
     * below, so printing it would be telling the user something untrue on
     * every boot where nothing was attached yet.
     */
    if (s_probe_in_progress &&
        contains_seq(buf, count, "does not support escape sequences")) {
        return (ssize_t)count;
    }

    if (s_terminal_mute) {
        if (cursor) {
            inject("\x1b[%u;%uR", REPORT_ROWS, REPORT_COLS);
            return (ssize_t)count;      /* swallowed: nobody would answer it */
        }
        if (status) {
            inject("\x1b[0n");
            return (ssize_t)count;
        }
        /*
         * The "go to the right margin" that sits between the two queries goes
         * with them. get_columns() only emits a restore when the second report
         * exceeds the first, and both of ours are the same number — so moving
         * the cursor for real would park it at the margin for good.
         */
        if (contains_seq(buf, count, "\x1b[999C")) {
            return (ssize_t)count;
        }
    } else if (cursor || status) {
        s_expect_report      = true;
        s_report_pos         = 0;
        s_report_deadline_us = esp_timer_get_time() + REPORT_WINDOW_US;
    }

    /*
     * esp_linenoise writes its escape sequences with sizeof() rather than
     * strlen(), so each one carries a trailing NUL onto the wire -- three per
     * prompt, from ESC[6n, ESC[999C and ESC[6n again. Terminals are not
     * required to ignore them, and this one renders them, which is the run of
     * blank space that appears before the cursor after a reboot.
     *
     * Trimmed here rather than patched upstream, and only from the end: a
     * terminal write that legitimately ends in NUL does not exist, while
     * stripping them from the middle could corrupt an app's output.
     *
     * The caller is told the whole buffer went out, because it compares the
     * return against the length it passed -- get_cursor_position() gives up
     * and reports failure otherwise.
     */
    size_t out_len = count;
    while (out_len > 0 && ((const uint8_t *)buf)[out_len - 1] == 0x00) {
        out_len--;
    }

    if (out_len == 0) {
        return (ssize_t)count;
    }

    const ssize_t n = write(fd, buf, out_len);
    if (n == (ssize_t)out_len) {
        fsync(fd);
        return (ssize_t)count;
    }
    return n;
}

static ssize_t console_read_bytes(int fd, void *buf, size_t count)
{
    static int64_t last_us;
    uint8_t       *out = buf;

    if (count == 0) {
        return 0;
    }

    /* Our own answers first, unpaced: they are ours and already correct. */
    if (s_injected_pos < s_injected_len) {
        out[0] = s_injected[s_injected_pos++];
        return 1;
    }

    /* Then anything held back while an answer went ahead of it. */
    if (s_pushback >= 0) {
        out[0]     = (uint8_t)s_pushback;
        s_pushback = -1;
        note_typed(out[0]);
        espix_pace(&last_us);
        return 1;
    }

    /*
     * Wait on the console and on the wake-up descriptor together, with no
     * timeout: the task sleeps until a key is pressed or a kernel message goes
     * out, and burns nothing in between. A poll would have been simpler and is
     * not worth a wakeup several times a second on a device that spends most of
     * its life idle at a prompt.
     */
    ssize_t n;

    for (;;) {
        fd_set    rfds;
        const int maxfd = (s_wake_fd > fd) ? s_wake_fd : fd;

        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        if (s_wake_fd >= 0) {
            FD_SET(s_wake_fd, &rfds);
        }

        const int r = select(maxfd + 1, &rfds, NULL, NULL, NULL);
        if (r < 0) {
            return -1;
        }

        if (s_wake_fd >= 0 && FD_ISSET(s_wake_fd, &rfds)) {
            /* Drain it whatever happens next, or select returns immediately
             * for ever. eventfd reads are 8 bytes and reset the count. */
            uint64_t sink = 0;
            (void)read(s_wake_fd, &sink, sizeof(sink));

            /*
             * Returning 0 makes esp_linenoise_edit() return state->len, which
             * for an untouched line is 0 (esp_linenoise.c:772): get_line()
             * hands back an empty string, the session loop runs nothing and
             * calls read_line() again, and the editor redraws its prompt with
             * its row bookkeeping reset.
             *
             * Deliberately not the ENTER path, which would also work and would
             * tidy up the editor's history placeholder for us: its refresh
             * draws a prompt *before* returning, and the session loop then
             * draws another. Two prompts is the very artifact this is meant to
             * remove. The placeholder is dealt with in console_read_line().
             *
             * Not while a cursor report is outstanding: get_cursor_position()
             * is inside its own read loop then, and cutting it short costs the
             * terminal width for the rest of the line. The request is simply
             * dropped -- the next message will make another.
             */
            const bool restart = s_restart_line && !s_expect_report;
            s_restart_line = false;

            if (restart) {
                return 0;
            }
            if (!FD_ISSET(fd, &rfds)) {
                continue;       /* nothing typed alongside it */
            }
        }

        n = read(fd, buf, count);
        break;
    }

    if (n <= 0) {
        return n;
    }

    if (s_expect_report) {
        if (esp_timer_get_time() > s_report_deadline_us) {
            /*
             * Too late to be a report, so this is someone typing. Answer the
             * outstanding query so the editor's read loop stops swallowing
             * input, and keep this byte for the next read.
             */
            s_expect_report = false;
            s_terminal_mute = true;
            s_pushback      = out[0];
            inject("\x1b[%u;%uR", REPORT_ROWS, REPORT_COLS);

            espix_klog(ESPIX_KLOG_DEBUG, TAG,
                       "terminal does not answer cursor queries; assuming %ux%u",
                       REPORT_COLS, REPORT_ROWS);

            out[0] = s_injected[s_injected_pos++];
            return 1;
        }

        if (!report_byte_fits(out[0], s_report_pos)) {
            /*
             * Not part of a report, so the terminal is not answering and this
             * is the user. End the window and let it through the normal paced
             * path -- pacing matters, or the editor takes it for pasted input.
             */
            s_expect_report = false;
            s_report_pos    = 0;
            note_typed(out[0]);
            espix_pace(&last_us);
            return n;
        }

        s_report_pos++;

        /* 'R' ends a cursor report, 'n' a device status report. */
        if (out[0] == 'R' || out[0] == 'n') {
            s_expect_report = false;
            s_report_pos    = 0;
        }

        /* Stamp it even though this was not paced, or the next real keystroke
         * sees a stale timestamp, skips its delay, and is taken for a paste. */
        last_us = esp_timer_get_time();
        return n;                       /* not paced: we asked for this */
    }

    note_typed(out[0]);
    espix_pace(&last_us);
    return n;
}

/* ------------------------------------------------------------------ */
/* Session callbacks                                                  */
/* ------------------------------------------------------------------ */

static int console_read_line(espix_session_t *s, const char *prompt,
                             char *buf, size_t len)
{
    (void)s;

    esp_linenoise_set_prompt(s_editor, prompt);

    /* get_line() returns ESP_OK for an empty line without writing the buffer,
     * so anything left here from last time would be run as a command. */
    buf[0] = '\0';

    /*
     * Between these two the prompt is on screen and this task is blocked in
     * read(), so kernel output from anywhere else has to be fitted around it --
     * see console_klog_begin().
     */
    s_editing = true;
    const esp_err_t line_err = esp_linenoise_get_line(s_editor, buf, len);
    s_editing = false;

    /* The editor has consumed the line either way; whatever was typed on it is
     * gone from the screen, so the next message has nothing to protect. */
    s_line_dirty = false;

    if (line_err != ESP_OK) {
        /* An empty line, or a momentarily unavailable backend (USB host
         * disconnect). Neither ends the session; back off so a detached console
         * cannot spin the CPU. */
        vTaskDelay(pdMS_TO_TICKS(10));
        return 0;
    }

    if (buf[0] != '\0') {
        espix_history_push(s_history, buf);
    }

    /*
     * Rebuilt on every line, not just accepted ones. esp_linenoise_edit() adds
     * its own empty placeholder on entry (esp_linenoise.c:744) and only the
     * ENTER path pops it again, so a line ended any other way -- Ctrl-C, or the
     * restart in console_read_bytes() -- leaves one behind. A few dozen of those
     * would push real commands out of a 32-entry history.
     *
     * espix_history_apply() frees the editor's list and writes espix's own back,
     * so espix's copy stays authoritative and the strays never accumulate.
     */
    espix_history_apply(s_history, s_editor);

    return (int)strlen(buf);
}

/*
 * Ctrl-C while a foreground process runs. Nothing else is reading the console
 * at that point -- the shell is blocked waiting on the process -- so this is
 * the only thing standing between the user and a program they cannot stop.
 *
 * Everything waiting is consumed, not just the Ctrl-C: input typed at a program
 * that is not reading has nowhere to go, and leaving it queued means it arrives
 * on the next prompt instead.
 */
static bool console_poll_interrupt(espix_session_t *s)
{
    (void)s;

    const int fd  = fileno(stdin);
    bool      hit = false;

    for (;;) {
        fd_set         rfds;
        struct timeval tv = { .tv_sec = 0, .tv_usec = 0 };

        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);

        if (select(fd + 1, &rfds, NULL, NULL, &tv) <= 0) {
            return hit;                 /* nothing waiting */
        }

        uint8_t     buf[32];
        const ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) {
            return hit;
        }
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == 0x03) {       /* ETX, which is what Ctrl-C sends */
                hit = true;
            }
        }
    }
}

static int console_write(espix_session_t *s, const char *data, size_t len)
{
    (void)s;

    const size_t n = fwrite(data, 1, len, stdout);
    fflush(stdout);
    return (int)n;
}

/* ------------------------------------------------------------------ */

/*
 * Hold the FIRST prompt — and only the first — until boot has settled.
 *
 * Boot kicks off asynchronous work that narrates itself from other tasks.
 * Drawing the prompt while that is in flight means it is immediately
 * overwritten, and since the session task then blocks inside the editor with no
 * way for an outside caller to force a redraw, the prompt stays stranded until
 * the user presses Enter. Every boot, as the first thing anyone sees.
 *
 * An earlier version waited purely for the kernel log to fall silent for 300ms.
 * That releases too early, because the boot log has silent gaps of its own: WiFi
 * association and the DHCP lease are ~1 to 1.4s apart, so the gate let go in the
 * middle and the address lines landed on the fresh prompt. Log silence is a bad
 * proxy for "settled" when the thing being waited on goes quiet mid-way, and no
 * constant window fixes that — DHCP timing belongs to the AP, not to us.
 *
 * So the signal is now declared, not inferred: subsystems hold the kernel's boot
 * barrier across their own asynchronous bring-up. The quiet window survives only
 * as a small tail, so the banner is not glued to the last message.
 *
 * This is the console's own concern, not the shell's. It runs once, before the
 * session loop is entered, and no other transport uses it: an SSH session can
 * only exist after the network is up, and kernel messages never reach it anyway.
 */
#define SETTLE_TAIL_MS 250
#define SETTLE_CAP_MS  5000
#define SETTLE_POLL_MS 50

static void wait_for_boot_settled(void)
{
    const uint32_t start = esp_log_timestamp();

    for (;;) {
        const uint32_t now = esp_log_timestamp();

        /* Backstop only. Reached when a holder never resolves — an AP that is
         * powered off answers neither association nor disconnect promptly — and
         * a shell that never starts is worse than a clobbered prompt. */
        if ((now - start) >= SETTLE_CAP_MS) {
            return;
        }

        if (espix_kernel_boot_pending() > 0) {
            vTaskDelay(pdMS_TO_TICKS(SETTLE_POLL_MS));
            continue;
        }

        /* Settled. Let the tail of the log drain so the banner starts clean. */
        const uint32_t last = espix_klog_last_echo_ms();
        if (last == 0 || (now - last) >= SETTLE_TAIL_MS) {
            return;
        }

        vTaskDelay(pdMS_TO_TICKS(SETTLE_POLL_MS));
    }
}

esp_err_t espix_console_session_start(void)
{
    ESP_RETURN_ON_ERROR(console_hw_init(), TAG, "console hw init failed");

    /* The editor reads this task's stdin; unbuffered, or it would sit on a
     * full line before we ever see a keystroke. */
    setvbuf(stdin, NULL, _IONBF, 0);

    /*
     * One editor instance, owned by this session. The default read/write
     * callbacks are right here: the console genuinely is a pair of file
     * descriptors, so the library's select()/eventfd path applies unchanged.
     * An SSH session supplies its own callbacks instead — that per-instance
     * split is the whole reason espix uses this rather than IDF's linenoise,
     * whose history and callbacks are file-scope statics.
     */
    esp_linenoise_config_t cfg;
    esp_linenoise_get_instance_config_default(&cfg);

    /*
     * fileno(), not STDIN_FILENO/STDOUT_FILENO. Those constants are 0 and 1 by
     * POSIX convention, but ESP-IDF opens the console streams through the VFS
     * and they land on whatever descriptors it hands out — 2 and 3 on this
     * build. Writing to fd 1 fails, which costs the prompt, the echo and the
     * terminal probe all at once. IDF's own linenoise uses fileno() throughout
     * for exactly this reason.
     */
    cfg.in_fd               = fileno(stdin);
    cfg.out_fd              = fileno(stdout);
    cfg.max_cmd_line_length = ESPIX_LINE_MAX;
    cfg.history_max_length  = 32;
    cfg.allow_multi_line    = true;
    cfg.allow_empty_line    = true;
    cfg.completion_cb       = espix_shell_completion;
    cfg.hints_cb            = espix_shell_hint;
    cfg.read_bytes_cb       = console_read_bytes;
    cfg.write_bytes_cb      = console_write_bytes;

    /* Supplying a read callback means create_instance() no longer forces
     * blocking mode for us, and the editor must block waiting for a key. */
    const int flags = fcntl(cfg.in_fd, F_GETFL, 0);
    fcntl(cfg.in_fd, F_SETFL, flags & ~O_NONBLOCK);

    s_probe_in_progress = true;
    const esp_err_t ed_err = esp_linenoise_create_instance(&cfg, &s_editor);
    s_probe_in_progress = false;
    ESP_RETURN_ON_ERROR(ed_err, TAG, "cannot create the line editor");

    /*
     * The descriptor the logging task pokes to wake the input read. Try to
     * create one before registering the VFS: something else may already have
     * done so, and register() would then fail where eventfd() succeeds.
     */
    s_wake_fd = eventfd(0, 0);
    if (s_wake_fd < 0) {
        const esp_vfs_eventfd_config_t ev = ESP_VFS_EVENTD_CONFIG_DEFAULT();
        if (esp_vfs_eventfd_register(&ev) == ESP_OK) {
            s_wake_fd = eventfd(0, 0);
        }
    }
    if (s_wake_fd < 0) {
        /* Not fatal. The prompt then reappears on the next keystroke rather
         * than on its own, which is where this started. */
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "no eventfd; the prompt will not redraw itself after "
                   "kernel messages");
    }

    /*
     * Ask to be told when kernel output reaches the console, now that there is
     * an editor whose line can be restarted. Safe to register before the
     * greeting: s_editing is false until the first get_line(), so until then
     * nothing is requested and messages print exactly as they always did --
     * which is what wait_for_boot_settled() below still relies on.
     */
    espix_klog_set_console_hooks(&k_console_hooks);

    /* The console has no login, so it is its own principal rather than sharing
     * a list with whoever logs in over SSH. */
    s_history = espix_history_for("");
    espix_history_apply(s_history, s_editor);

    /* History is in-memory only; persisting it to the rootfs would mean a
     * flash write per command. esp_linenoise_history_save() is there if that
     * trade ever looks different. */

    /*
     * Overrule the probe and keep line editing on. Note the name reads
     * backwards: allow_dumb_mode means "use dumb mode".
     *
     * esp_linenoise_create_instance() probes exactly once, as the console
     * starts — which on a device is before anyone has attached a terminal, and
     * always before `idf.py monitor --no-reset` reattaches to a board that is
     * already running. Nothing answers, so the probe fails and the instance
     * latches into dumb mode for the life of the firmware.
     *
     * That is worth spelling out, because dumb mode does not merely disable
     * editing. It drops ESC as a non-printable and keeps what follows, so an
     * arrow key is entered as the literal text "[A"; and
     * esp_linenoise_dumb() terminates the line one byte late --
     * `buffer[count + 1] = '\0'` at src/esp_linenoise.c:1027 -- leaving a
     * stale byte of the previous command on the end of this one, so `df` typed
     * after `whoami` runs as `dfo`. Both were reported as a console that goes
     * strange until reboot, and both are this.
     *
     * Every serial terminal in practical use handles escape sequences.
     * Assuming a capable terminal and being wrong puts escape codes on the
     * screen; assuming a dumb one and being wrong costs line editing, history,
     * and the integrity of every command typed. The trade is not close.
     *
     * Do not probe again here either: the question has one reply and
     * create_instance() consumed it.
     */
    bool probed_dumb = false;
    esp_linenoise_is_dumb_mode(s_editor, &probed_dumb);
    if (probed_dumb) {
        espix_klog(ESPIX_KLOG_DEBUG, TAG,
                   "no answer to the terminal probe; assuming it is capable");
    }
    esp_linenoise_set_dumb_mode(s_editor, false);

    const bool ansi = true;

    s_console = (espix_session_t) {
        .name      = "console",
        .cwd       = "/",
        /*
         * Physical access to the board is the privilege, so the console is
         * root. It is a synthetic identity rather than an account in
         * espix_auth: nothing authenticates here, so there would be nothing to
         * verify a credential against, and inventing one would imply a login
         * step that does not exist. Which is also why `login` is false and
         * `logout` declines -- there was no login to undo.
         */
        .user      = "root",
        .login     = false,
        .read_line = console_read_line,
        .write     = console_write,
        .poll_interrupt = console_poll_interrupt,
        .fg_pid    = ESPIX_PID_NONE,
        .ansi      = ansi,
        /* No stdio rebinding: the editor owns stdin and output goes through
         * stdio, so a spawned app inherits the global streams — which for the
         * console is already the right place. */
        .open_stream = NULL,
    };

    espix_klog(ESPIX_KLOG_INFO, TAG, "console session on %s",
#if CONFIG_ESP_CONSOLE_UART
               "uart"
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
               "usb-serial-jtag"
#else
               "stdio"
#endif
              );

    wait_for_boot_settled();

    /*
     * The same greeting an SSH session gets, reached through the ordinary
     * dispatch so this file needs no knowledge of what is in it.
     *
     * /etc/motd is still not read: the greeting now occupies that slot, and
     * printing the file's directory guide at every login would be noise. It
     * remains available with `cat /etc/motd`, and if it should appear here
     * later it belongs directly under the spec block.
     */
    /*
     * A console session that ends starts another, which is what init does when
     * a login shell exits on a terminal. There is no login to return to here,
     * so `exit` resets the session rather than logging anyone out — and, more
     * to the point, the alternative is a device with no shell until it reboots.
     *
     * History is deliberately not reset: it belongs to the user, and logging
     * out and back in keeps it on any real system.
     */
    for (;;) {
        s_console.want_exit   = false;
        s_console.last_status = 0;
        strlcpy(s_console.cwd, "/", sizeof(s_console.cwd));
        console_input_reset();

        espix_shell_exec(&s_console, "motd");
        espix_shell_session_run(&s_console);

        espix_klog(ESPIX_KLOG_DEBUG, TAG, "console session ended; starting another");
    }

    return ESP_OK;     /* not reached */
}
