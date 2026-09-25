/*
 * C shim over esphome/micro-mp3, a C++ class, so espix's C engine can call it.
 *
 * micro-mp3 is the same OpenCore/Helix fixed-point decoder esp_audio_codec
 * ships, but built from source with per-file optimization flags: the hot
 * transforms and dequant paths at -Os (bound by register spills, not
 * multiplies) and the polyphase synthesis at -O2. That tuning is the only lever
 * for MP3 on this part, since S31's PIE/SIMD is used by LC3 and Opus only.
 */
#include <new>
#include <stddef.h>
#include <stdint.h>

#include "micro_mp3/mp3_decoder.h"

extern "C" {

void *espix_mp3_open(void)
{
    return new (std::nothrow) micro_mp3::Mp3Decoder();
}

void espix_mp3_close(void *h)
{
    delete static_cast<micro_mp3::Mp3Decoder *>(h);
}

int espix_mp3_decode(void *h, const uint8_t *in, size_t in_len,
                     uint8_t *out, size_t out_len,
                     size_t *consumed, size_t *samples)
{
    if (h == NULL || consumed == NULL || samples == NULL) {
        return -1;   /* micro_mp3::MP3_INPUT_INVALID */
    }
    *consumed = 0;
    *samples = 0;
    return (int)static_cast<micro_mp3::Mp3Decoder *>(h)->decode(
        in, in_len, out, out_len, *consumed, *samples);
}

int espix_mp3_sample_rate(void *h)
{
    return h ? (int)static_cast<micro_mp3::Mp3Decoder *>(h)->get_sample_rate() : 0;
}

int espix_mp3_channels(void *h)
{
    return h ? (int)static_cast<micro_mp3::Mp3Decoder *>(h)->get_channels() : 0;
}

int espix_mp3_bit_depth(void *h)
{
    return h ? (int)static_cast<micro_mp3::Mp3Decoder *>(h)->get_bit_depth() : 0;
}

}   /* extern "C" */
