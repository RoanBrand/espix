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

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_linenoise.h"
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
static espix_history_t        s_history;

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
        espix_history_push(&s_history, buf);
        espix_history_apply(&s_history, s_editor);
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

    ESP_RETURN_ON_ERROR(esp_linenoise_create_instance(&cfg, &s_editor),
                        TAG, "cannot create the line editor");

    /* History is in-memory only; persisting it to the rootfs would mean a
     * flash write per command. esp_linenoise_history_save() is there if that
     * trade ever looks different. */

    /*
     * Do NOT probe here. esp_linenoise_create_instance() already probed the
     * terminal and set its mode from the result; probing again asks a question
     * whose single reply has been consumed, fails, and — if the answer is then
     * acted on — turns off editing on a terminal the library just confirmed as
     * capable. Read the verdict instead.
     *
     * Note the name reads backwards: allow_dumb_mode means "use dumb mode",
     * which is what get_line() branches on.
     */
    bool dumb = false;
    esp_linenoise_is_dumb_mode(s_editor, &dumb);

    const bool ansi = !dumb;
    if (dumb) {
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "terminal lacks escape sequences; line editing disabled");
    }

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
    espix_shell_exec(&s_console, "motd");

    espix_shell_session_run(&s_console);

    /* Only reached if the session asks to exit. */
    espix_klog(ESPIX_KLOG_WARN, TAG, "console session ended");
    return ESP_OK;
}
