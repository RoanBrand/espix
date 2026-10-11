/*
 * The audio engine, published to apps.
 *
 * Apps get the stream API and nothing else: open a stream in a format, write
 * PCM into it, close it. Sinks, sources and the registry stay firmware-side --
 * which speaker the sound comes out of is policy the engine owns, and an app
 * that could choose one would have to know what a sink is. Keeping that out of
 * the ABI is the same argument that keeps espix_bt out of espix_audio.
 *
 * Deliberately absent: espix_audio_play() and the tone generators. The tones
 * are the engine's own test signal, and a program that wants to play a file is
 * a client of the server the way `play` is, not a caller of its command layer.
 *
 * The volume pair is here because a player must be able to offer one, and the
 * state string because a UI has to be able to say whether anything is playing.
 */
#include "esp_elf.h"

#include "espix_audio.h"
#include "espix_kernel.h"
#include "espix_proc_priv.h"

#define TAG "abi"

static const struct esp_elfsym s_audio_syms[] = {
    ESP_ELFSYM_EXPORT(espix_audio_stream_open),
    ESP_ELFSYM_EXPORT(espix_audio_stream_write),
    ESP_ELFSYM_EXPORT(espix_audio_stream_volume),
    ESP_ELFSYM_EXPORT(espix_audio_stream_mute),
    ESP_ELFSYM_EXPORT(espix_audio_stream_close),

    /* The other direction: an app reading the microphone. */
    ESP_ELFSYM_EXPORT(espix_audio_capture_open),
    ESP_ELFSYM_EXPORT(espix_audio_capture_read),
    ESP_ELFSYM_EXPORT(espix_audio_capture_close),
    ESP_ELFSYM_EXPORT(espix_audio_set_volume),
    ESP_ELFSYM_EXPORT(espix_audio_get_volume),
    ESP_ELFSYM_EXPORT(espix_audio_state),
    ESP_ELFSYM_END
};

void espix_proc_abi_audio_register(void)
{
    if (esp_elf_register_symbol(s_audio_syms) != 0) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "could not publish audio to apps");
        return;
    }

    espix_klog(ESPIX_KLOG_INFO, TAG, "audio streams published to apps");
}
