/*
 * record: capture from the registered source (the board's microphone) to a WAV.
 *
 * Capture and the disk are decoupled by a ring in PSRAM and a writer task, and
 * that is not an optimisation -- it is the difference between capturing and
 * dropping. The I2S has no backpressure: if nothing reads it, it overruns and
 * the samples are simply gone. A write to a slow stick stalls for tens or
 * hundreds of milliseconds (the first ones, while the FAT and the directory are
 * being built, are the worst), and a loop that reads and writes in turn spends
 * that time not draining the I2S. Measured on an old USB stick: writes averaged
 * 21 ms per 32 kB but peaked at 336 ms, and a serial loop lost ~2.5% of a 20 s
 * take, all at the start.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "espix_cmds_priv.h"
#include "espix_shell.h"
#include "espix_audio_source.h"

#if CONFIG_ESPIX_AUDIO

/* One write per chunk: 32 kB is several FAT clusters on any plausible volume. */
#define REC_CHUNK (32 * 1024)

/*
 * The slack between the I2S and the disk. Half a megabyte is ~3 s of 44.1 kHz
 * stereo, which absorbs even the worst write stall measured; it lives in PSRAM,
 * where there is no reason to be stingy.
 */
#define REC_RING  (512 * 1024)

typedef struct {
    StreamBufferHandle_t sb;
    int                  fd;
    volatile bool        stop;
    volatile uint64_t    total;
    volatile uint32_t    wr_max;
    TaskHandle_t         task;
} rec_t;

static rec_t s_rec;

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void writer_task(void *arg)
{
    rec_t   *w   = (rec_t *)arg;
    uint8_t *buf = malloc(REC_CHUNK);

    while (buf != NULL) {
        const size_t n = xStreamBufferReceive(w->sb, buf, REC_CHUNK,
                                              pdMS_TO_TICKS(200));
        if (n > 0) {
            const int64_t t0 = esp_timer_get_time();
            const ssize_t r  = write(w->fd, buf, n);
            const uint32_t wr = (uint32_t)(esp_timer_get_time() - t0);
            if (r < 0) {
                break;
            }
            w->total += (uint64_t)n;
            if (wr > w->wr_max) {
                w->wr_max = wr;
            }
        } else if (w->stop) {
            break;              /* the ring is drained and nothing more comes */
        }
    }

    free(buf);
    w->task = NULL;
    vTaskDelete(NULL);
}

static int cmd_record(espix_session_t *s, int argc, char **argv)
{
    if (argc < 2) {
        espix_eprintf(s, "usage: record <file.wav> [seconds]\n");
        return 1;
    }
    int seconds = argc > 2 ? atoi(argv[2]) : 5;
    if (seconds <= 0) {
        seconds = 5;
    }

    const espix_audio_source_ops_t *src = espix_audio_source_default();
    if (src == NULL || (src->connected != NULL && !src->connected())) {
        espix_eprintf(s, "record: no audio source available\n");
        return 1;
    }
    const espix_audio_format_t f = src->format();
    if (f.channels == 0) {
        espix_eprintf(s, "record: source reports no format\n");
        return 1;
    }

    char path[320];
    if (!espix_cmd_path(s, argv[1], path, sizeof(path))) {
        return 1;
    }

    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        espix_eprintf(s, "record: %s: cannot create\n", argv[1]);
        return 1;
    }

    /* A 16-bit PCM WAV header, its two sizes patched once the capture ends. */
    uint8_t hdr[44];
    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr, "RIFF", 4);
    memcpy(hdr + 8, "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    put_le32(hdr + 16, 16);
    put_le16(hdr + 20, 1);                                   /* PCM */
    put_le16(hdr + 22, f.channels);
    put_le32(hdr + 24, f.rate);
    put_le32(hdr + 28, f.rate * f.channels * 2);             /* byte rate */
    put_le16(hdr + 32, (uint16_t)(f.channels * 2));          /* block align */
    put_le16(hdr + 34, 16);
    memcpy(hdr + 36, "data", 4);
    (void)!write(fd, hdr, sizeof(hdr));

    s_rec.fd    = fd;
    s_rec.stop  = false;
    s_rec.total = 0;
    s_rec.wr_max = 0;
    s_rec.sb    = xStreamBufferCreateWithCaps(REC_RING, REC_CHUNK,
                                              MALLOC_CAP_SPIRAM);
    if (s_rec.sb == NULL) {
        espix_eprintf(s, "record: no memory for the capture ring\n");
        close(fd);
        return 1;
    }

    if (xTaskCreate(writer_task, "recwrite", 6144, &s_rec, 4,
                    &s_rec.task) != pdPASS) {
        espix_eprintf(s, "record: cannot start the writer\n");
        vStreamBufferDeleteWithCaps(s_rec.sb);
        close(fd);
        return 1;
    }

    src->start();

    uint8_t *buf = malloc(REC_CHUNK);
    uint32_t rd_max = 0, sent_stalls = 0;
    const int64_t end = esp_timer_get_time() + (int64_t)seconds * 1000000;

    espix_printf(s, "record: %s, %u Hz, %u ch, %d s\n", argv[1],
                 (unsigned)f.rate, (unsigned)f.channels, seconds);

    while (buf != NULL && esp_timer_get_time() < end) {
        const int64_t t0 = esp_timer_get_time();
        const size_t  n  = src->read(buf, REC_CHUNK);
        const uint32_t rd = (uint32_t)(esp_timer_get_time() - t0);
        if (n == 0) {
            continue;
        }
        if (rd > rd_max) {
            rd_max = rd;
        }
        /*
         * Always wait: a full ring means the disk is behind, and the right
         * answer is to hold the samples here rather than drop them. The ring is
         * sized so this should not happen, and it is counted if it does.
         */
        if (xStreamBufferSend(s_rec.sb, buf, n, portMAX_DELAY) != n) {
            sent_stalls++;
        }
    }

    src->stop();
    free(buf);

    s_rec.stop = true;
    while (s_rec.task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vStreamBufferDeleteWithCaps(s_rec.sb);

    const uint64_t total = s_rec.total;
    put_le32(hdr + 4, (uint32_t)(36 + total));
    put_le32(hdr + 40, (uint32_t)total);
    (void)lseek(fd, 0, SEEK_SET);
    (void)!write(fd, hdr, sizeof(hdr));
    close(fd);

    espix_printf(s, "record: %llu bytes, %.1f s; read max %u us, write max %u us, "
                    "ring stalls %u\n",
                 (unsigned long long)total,
                 (double)total / (double)(f.rate * f.channels * 2),
                 (unsigned)rd_max, (unsigned)s_rec.wr_max,
                 (unsigned)sent_stalls);
    return 0;
}

static espix_cmd_t s_record_cmds[] = {
    { .name = "record", .fn = cmd_record,
      .help = "record from the audio source (microphone) to a WAV file",
      .usage = "record <file.wav> [seconds]" },
};

#endif /* CONFIG_ESPIX_AUDIO */

void espix_cmds_register_record(void)
{
#if CONFIG_ESPIX_AUDIO
    espix_cmds_register_table(s_record_cmds,
                              sizeof(s_record_cmds) / sizeof(s_record_cmds[0]));
#endif
}
