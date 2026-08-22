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

#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "espix_kernel.h"

#define KLOG_LINES CONFIG_ESPIX_KLOG_LINES

static espix_klog_entry_t s_ring[KLOG_LINES];
static uint32_t           s_next;       /* total lines ever written */
static portMUX_TYPE       s_lock = portMUX_INITIALIZER_UNLOCKED;

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
    /* Console gets INFO and above; DEBUG stays in the ring for `dmesg`. Same
     * split Linux draws with its console loglevel — routine per-event chatter
     * should not be on the terminal you are trying to work in. */
    if (echo && level <= ESPIX_KLOG_INFO) {
        /* Outside the critical section: this is stdio, not a quick memcpy. */
        printf("espix: %s\n", staged.text);
        fflush(stdout);
    }
#else
    (void)echo;
#endif
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
    if (vsnprintf(line, sizeof(line), fmt, ap_copy) > 0) {
        klog_store(level_from_esp_log(line), line, false);
    }
    va_end(ap_copy);

    return s_prev_vprintf ? s_prev_vprintf(fmt, ap) : vprintf(fmt, ap);
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
