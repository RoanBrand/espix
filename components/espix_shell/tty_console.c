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
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_linenoise.h"
#include "esp_timer.h"
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
#define REPORT_WINDOW_US 60000

/* Size claimed on behalf of a terminal that will not answer. */
#define REPORT_COLS 80
#define REPORT_ROWS 24

static bool    s_expect_report;
static int64_t s_report_deadline_us;

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
        s_report_deadline_us = esp_timer_get_time() + REPORT_WINDOW_US;
    }

    const ssize_t n = write(fd, buf, count);
    if (n == (ssize_t)count) {
        fsync(fd);
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
        espix_pace(&last_us);
        return 1;
    }

    const ssize_t n = read(fd, buf, count);

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

        /* 'R' ends a cursor report, 'n' a device status report. The whole read
         * is scanned: one byte per call is only how it happens to work today. */
        for (ssize_t i = 0; i < n; i++) {
            if (out[i] == 'R' || out[i] == 'n') {
                s_expect_report = false;
                break;
            }
        }

        /* Stamp it even though this was not paced, or the next real keystroke
         * sees a stale timestamp, skips its delay, and is taken for a paste. */
        last_us = esp_timer_get_time();
        return n;                       /* not paced: we asked for this */
    }

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

    if (esp_linenoise_get_line(s_editor, buf, len) != ESP_OK) {
        /* An empty line, or a momentarily unavailable backend (USB host
         * disconnect). Neither ends the session; back off so a detached console
         * cannot spin the CPU. */
        vTaskDelay(pdMS_TO_TICKS(10));
        return 0;
    }

    if (buf[0] != '\0') {
        espix_history_push(s_history, buf);
        espix_history_apply(s_history, s_editor);
    }

    return (int)strlen(buf);
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
        .read_line = console_read_line,
        .write     = console_write,
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
