/* espix_audio.h's streams as syscalls. */
#include <stdlib.h>

#include "espix_audio.h"
#include "sys.h"

struct espix_audio_stream {
    long handle;
};

espix_audio_stream_t *espix_audio_stream_open(const espix_audio_format_t *fmt)
{
    if (fmt == NULL) {
        return NULL;
    }
    espix_audio_stream_t *st = malloc(sizeof(*st));
    if (st == NULL) {
        return NULL;
    }
    const struct espix_audio_fmt f = { fmt->rate, fmt->channels, fmt->bits };
    st->handle = espix_ret(espix_sys1(ESPIX_SYS_AUDIO_OPEN, &f));
    if (st->handle < 0) {
        free(st);
        return NULL;
    }
    return st;
}

size_t espix_audio_stream_write(espix_audio_stream_t *st, const void *pcm, size_t len)
{
    if (st == NULL || pcm == NULL || len == 0) {
        return 0;
    }
    const long r = espix_sys3(ESPIX_SYS_AUDIO_WRITE, st->handle, pcm, len);
    return (r > 0) ? (size_t)r : 0;
}

static esp_err_t ctl(espix_audio_stream_t *st, int op, long arg)
{
    if (st == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return (espix_sys3(ESPIX_SYS_AUDIO_CTL, st->handle, op, arg) < 0) ? ESP_ERR_INVALID_ARG : ESP_OK;
}

esp_err_t espix_audio_stream_volume(espix_audio_stream_t *st, int percent)
{
    return ctl(st, ESPIX_AUDIO_VOLUME, percent);
}

esp_err_t espix_audio_stream_mute(espix_audio_stream_t *st, bool on)
{
    return ctl(st, ESPIX_AUDIO_MUTE, on);
}

uint32_t espix_audio_stream_queued_us(espix_audio_stream_t *st)
{
    const long r = (st != NULL) ? espix_sys3(ESPIX_SYS_AUDIO_CTL, st->handle, ESPIX_AUDIO_QUEUED, 0) : 0;
    return (r > 0) ? (uint32_t)r : 0;
}

esp_err_t espix_audio_stream_close(espix_audio_stream_t *st)
{
    if (st == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    espix_sys1(ESPIX_SYS_AUDIO_CLOSE, st->handle);
    free(st);
    return ESP_OK;
}
