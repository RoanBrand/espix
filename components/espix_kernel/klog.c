/*
 * espix kernel log ring — backs `dmesg`.
 *
 * A fixed-size array of lines rather than a byte ring: wraparound then costs
 * nothing but an index bump, and a reader can never observe a torn line. The
 * whole thing is statically allocated so it is usable before the heap and the
 * filesystem exist.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "espix_kernel.h"

#define KLOG_LINES CONFIG_ESPIX_KLOG_LINES

/*
 * In PSRAM, with CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY. The ring is 12.4
 * kB of internal RAM (96 x ~132 B) and internal is the pool Bluetooth needs;
 * nothing touches it before app_main, by which point PSRAM is up. The "before
 * the heap exists" note in the file header still holds: this is a static
 * placement, not an allocation.
 */
EXT_RAM_BSS_ATTR static espix_klog_entry_t s_ring[KLOG_LINES];
static uint32_t           s_next;       /* total lines ever written */
static portMUX_TYPE       s_lock = portMUX_INITIALIZER_UNLOCKED;

/* Plain uint32 read/written without the lock: a torn read is impossible on a
 * 32-bit aligned word, and the only consumer wants "roughly when", not exactly. */
static volatile uint32_t  s_last_echo_ms;

/* Whoever owns the terminal, so kernel output can be fitted around a prompt
 * rather than dropped on top of one. See espix_kernel.h. */
static const espix_klog_console_hooks_t *s_console;

/*
 * The console stream, captured once, because `stdout` is not a global here.
 *
 * newlib gives every task its own struct _reent, and `stdout` expands to the
 * *calling task's* copy. espix_proc points a loaded app's at the session it was
 * launched from, so inside an app's task printf() is not the console -- for an
 * SSH session it is the encrypted channel.
 *
 * A kernel log echoing through that plain printf() therefore re-entered
 * whatever transport the current task happened to own. In the SSH server that
 * meant klog -> printf -> chan_write -> ssh_packet_write -> a klog on the same
 * path -> unbounded recursion, on an 8KB app stack, with the inner packet
 * rebuilding the shared out_buf and bumping seq_out underneath the outer send.
 * The transmit lock does not catch it: it is recursive by design, so the same
 * task is admitted rather than deadlocked.
 *
 * Captured lazily on the first echo, which happens during boot on the main
 * task, before any app exists to have redirected anything.
 */
static FILE *s_console_out;

/*
 * The console flusher.
 *
 * The ring was always asynchronous; the console write was not. klog_store()
 * used to fprintf + fflush to a line-buffered tty in the *caller's* context,
 * which is a blocking UART write (~8-10 ms per line at 115200), and every
 * ESP_LOGx in the tree reached the console the same way. That is a latency
 * spike in whatever happened to be running -- it does not show up as task CPU,
 * because the task is blocked, not busy. It was audible: playback telemetry
 * logged at INFO once every 2-3 s stalled the audio producer into a burst of
 * static.
 *
 * So the caller now only formats and queues, and this task drains the ring to
 * the console. Linux's printk-to-ring with a separate console context, same
 * shape. Before it starts (early boot) the inline echo is kept, because there
 * is no task to hand the work to.
 */
static TaskHandle_t s_flusher;
static uint32_t     s_flushed;      /* next seq this task will echo */

/* What reaches the console, as `dmesg -n` sets it; the ring keeps everything
 * regardless. See espix_klog_set_console_level(). */
static espix_klog_level_t s_console_level = ESPIX_KLOG_INFO;

void espix_klog_set_console_level(espix_klog_level_t level)
{
    s_console_level = level;
}

espix_klog_level_t espix_klog_console_level(void)
{
    return s_console_level;
}

void espix_klog_set_console_hooks(const espix_klog_console_hooks_t *hooks)
{
    s_console = hooks;
}

/* Read the pointer once into a local at each site: it can be cleared by another
 * task between the test and the call. */
static void console_output_begin(void)
{
    const espix_klog_console_hooks_t *h = s_console;

    if (h != NULL && h->output_begin != NULL) {
        h->output_begin();
    }
}

static void console_output_done(void)
{
    const espix_klog_console_hooks_t *h = s_console;

    if (h != NULL && h->output_done != NULL) {
        h->output_done();
    }
}

/*
 * Copy `src` into `dst`, dropping ANSI escape sequences and trailing newlines.
 * ESP_LOG output arrives colourised and newline-terminated; neither is wanted
 * in a stored log line, and stripping here keeps this independent of whatever
 * CONFIG_LOG_COLORS happens to be set to.
 */
static void copy_sanitised(char *dst, size_t dst_len, const char *src)
{
    size_t o = 0;

    while (*src && o + 1 < dst_len) {
        if (*src == '\033') {
            /* Skip CSI sequence: ESC [ ... <final byte 0x40-0x7E> */
            src++;
            if (*src == '[') {
                src++;
                while (*src && (*src < '@' || *src > '~')) {
                    src++;
                }
                if (*src) {
                    src++;
                }
            }
            continue;
        }
        if (*src == '\r' || *src == '\n') {
            src++;
            continue;
        }
        dst[o++] = *src++;
    }

    dst[o] = '\0';
}

/*
 * `echo` mirrors the line to the console, the way a Linux kernel prints to the
 * console as well as to the kmsg ring. Lines arriving from the esp_log hook
 * pass false: they are already on their way to the console by definition, and
 * echoing them would double every ESP_LOGx.
 */
static void klog_echo(const espix_klog_entry_t *e);
static void klog_wake(void);

static void klog_store(espix_klog_level_t level, const char *line, bool echo)
{
    if (line == NULL) {
        return;
    }

    /* Format outside the critical section; only the ring update is guarded.
     *
     * esp_log_timestamp() rather than esp_timer_get_time(): the two count from
     * different origins (the former from CPU reset, the latter from systimer
     * init, ~750ms apart on a PSRAM board), and dmesg lines are read alongside
     * the ESP_LOGx lines they interleave with, so they have to agree. */
    espix_klog_entry_t staged = {
        .ts_ms = esp_log_timestamp(),
        .level = (uint8_t)level,
    };
    copy_sanitised(staged.text, sizeof(staged.text), line);

    if (staged.text[0] == '\0') {
        return;
    }

    portENTER_CRITICAL_SAFE(&s_lock);
    staged.seq = s_next;
    s_ring[s_next % KLOG_LINES] = staged;
    s_next++;
    portEXIT_CRITICAL_SAFE(&s_lock);

#if !CONFIG_ESPIX_KLOG_QUIET
    if (echo) {
        if (s_flusher != NULL) {
            klog_wake();
        } else {
            /* No flusher yet: this runs during early boot, before the scheduler
             * or before the task is created. Echo inline, as always. */
            klog_echo(&staged);
        }
    }
#else
    (void)echo;
#endif
}

/*
 * One line to the console. Only ever called from the flusher task (or from the
 * caller during early boot), so the stdio here is off every hot path.
 */
static void klog_echo(const espix_klog_entry_t *e)
{
    /* Console gets INFO and above by default; DEBUG stays in the ring for
     * `dmesg`. Same split Linux draws with its console loglevel -- routine
     * per-event chatter should not be on the terminal you are trying to work
     * in -- and `dmesg -n` moves the line at runtime. */
    if (e->level > s_console_level) {
        return;
    }

    if (s_console_out == NULL) {
        s_console_out = stdout;     /* boot, on the main task */
    }

    /* To the captured console, never to the caller's stdout -- see the note on
     * s_console_out for what that cost the SSH server. */
    console_output_begin();
    fprintf(s_console_out, "%s\n", e->text);
    fflush(s_console_out);
    console_output_done();

    s_last_echo_ms = e->ts_ms;
}

static void klog_wake(void)
{
    if (s_flusher != NULL && !xPortInIsrContext()) {
        xTaskNotifyGive(s_flusher);
    }
}

/* Echo every line stored since the last flush, oldest first. */
static void klog_flush_pending(void)
{
    portENTER_CRITICAL_SAFE(&s_lock);
    const uint32_t next = s_next;
    portEXIT_CRITICAL_SAFE(&s_lock);

    const uint32_t oldest = (next > KLOG_LINES) ? next - KLOG_LINES : 0;
    if (s_flushed < oldest) {
        s_flushed = oldest;     /* fell behind; the ring already dropped them */
    }

    for (; s_flushed < next; s_flushed++) {
        espix_klog_entry_t copy;

        portENTER_CRITICAL_SAFE(&s_lock);
        copy = s_ring[s_flushed % KLOG_LINES];
        portEXIT_CRITICAL_SAFE(&s_lock);

        if (copy.seq == s_flushed) {
            klog_echo(&copy);
        }
    }
}

static void klog_flusher_task(void *arg)
{
    (void)arg;

    for (;;) {
        /* Wake on demand, with a timeout as a backstop: a missed notification
         * (an ISR, or a line queued in a race) still reaches the console. */
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        klog_flush_pending();
    }
}

void espix_klog_start_flusher(void)
{
    if (s_flusher != NULL) {
        return;
    }

    /* Start from whatever is already there; those lines were echoed inline. */
    portENTER_CRITICAL_SAFE(&s_lock);
    s_flushed = s_next;
    portEXIT_CRITICAL_SAFE(&s_lock);

    /* PSRAM first, internal as a fallback: this task is not realtime, and
     * internal is the pool that has to stay spare for Bluetooth. */
    if (xTaskCreateWithCaps(klog_flusher_task, "espix:klog", 4096, NULL, 2,
                            &s_flusher, MALLOC_CAP_SPIRAM) != pdPASS) {
        (void)xTaskCreateWithCaps(klog_flusher_task, "espix:klog", 4096, NULL, 2,
                                  &s_flusher, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
}

void espix_klog_put(espix_klog_level_t level, const char *line)
{
    klog_store(level, line, true);
}

void espix_klog(espix_klog_level_t level, const char *tag, const char *fmt, ...)
{
    char line[ESPIX_KLOG_LINE_MAX + 1];
    int  n = 0;

    if (tag != NULL) {
        n = snprintf(line, sizeof(line), "%s: ", tag);
        if (n < 0 || (size_t)n >= sizeof(line)) {
            return;
        }
    }

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof(line) - n, fmt, ap);
    va_end(ap);

    espix_klog_put(level, line);
}

void espix_klog_foreach(espix_klog_iter_fn cb, void *ctx)
{
    if (cb == NULL) {
        return;
    }

    /* Snapshot the bounds, then read entries without holding the lock: a line
     * that gets overwritten mid-walk is one we were about to drop anyway. */
    portENTER_CRITICAL_SAFE(&s_lock);
    const uint32_t next = s_next;
    portEXIT_CRITICAL_SAFE(&s_lock);

    const uint32_t first = (next > KLOG_LINES) ? next - KLOG_LINES : 0;

    for (uint32_t i = first; i < next; i++) {
        espix_klog_entry_t copy;

        portENTER_CRITICAL_SAFE(&s_lock);
        copy = s_ring[i % KLOG_LINES];
        portEXIT_CRITICAL_SAFE(&s_lock);

        if (copy.seq != i) {
            continue;   /* overwritten while we walked */
        }
        if (!cb(ctx, &copy)) {
            return;
        }
    }
}

size_t espix_klog_count(void)
{
    return (s_next < KLOG_LINES) ? s_next : KLOG_LINES;
}

uint32_t espix_klog_dropped(void)
{
    return (s_next > KLOG_LINES) ? s_next - KLOG_LINES : 0;
}

uint32_t espix_klog_last_echo_ms(void)
{
    return s_last_echo_ms;
}

#if CONFIG_ESPIX_KLOG_CAPTURE_ESP_LOG

static vprintf_like_t s_prev_vprintf;

static espix_klog_level_t level_from_esp_log(const char *line)
{
    /* ESP_LOG lines start with "E (", "W (", "I (", "D (", "V (" — possibly
     * after a colour escape, which we have not stripped at this point. */
    for (const char *p = line; *p != '\0'; p++) {
        if (*p == '\033') {
            p++;
            if (*p == '[') {
                p++;
                while (*p && (*p < '@' || *p > '~')) {
                    p++;
                }
                if (*p == '\0') {
                    break;
                }
            }
            continue;
        }
        switch (*p) {
        case 'E': return ESPIX_KLOG_ERROR;
        case 'W': return ESPIX_KLOG_WARN;
        case 'I': return ESPIX_KLOG_INFO;
        case 'D':
        case 'V': return ESPIX_KLOG_DEBUG;
        default:  return ESPIX_KLOG_INFO;
        }
    }
    return ESPIX_KLOG_INFO;
}

static int klog_vprintf(const char *fmt, va_list ap)
{
    va_list ap_copy;
    va_copy(ap_copy, ap);

    /* Stack buffer, so this stays reentrant across tasks. */
    char line[ESPIX_KLOG_LINE_MAX + 1];
    const int n = vsnprintf(line, sizeof(line), fmt, ap_copy);
    if (n > 0) {
        klog_store(level_from_esp_log(line), line, true);
    }
    va_end(ap_copy);

    /*
     * Once the flusher runs, the console belongs to it: this hook only queues,
     * so an ESP_LOGx in a hot path no longer pays a UART round trip. The cost
     * is that a driver message longer than ESPIX_KLOG_LINE_MAX reaches the
     * console clipped rather than whole, and that colour is dropped with the
     * rest of the escape sequences (both the price of the ring being the one
     * path to the console).
     */
    if (s_flusher != NULL) {
        return n;
    }

    /* Early boot: no flusher yet, so keep the stock synchronous output. */
    console_output_begin();
    const int w = s_prev_vprintf ? s_prev_vprintf(fmt, ap) : vprintf(fmt, ap);
    console_output_done();

    return w;
}

void espix_klog_install_esp_log_hook(void)
{
    s_prev_vprintf = esp_log_set_vprintf(klog_vprintf);
}

#else  /* !CONFIG_ESPIX_KLOG_CAPTURE_ESP_LOG */

void espix_klog_install_esp_log_hook(void)
{
}

#endif
