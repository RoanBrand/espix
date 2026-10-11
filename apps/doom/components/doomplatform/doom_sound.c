/*
 * Doom's sound effects, on espix's audio stream API.
 *
 * The engine tracks what is playing on which channel and calls the platform
 * module; the mixing is the module's job. Nothing here is a port of anything
 * upstream -- the SDL backend hands its mixing to SDL_mixer, and there is no
 * sample loop in the tree to copy -- so this is the DMX format parsed directly
 * and sixteen channels summed into one espix stream.
 *
 * That stream is the whole of what espix provides: open it in the sink's
 * format, write PCM, close it. No device, no sink selection, no policy. Doom is
 * a client of the audio server exactly the way an app is, which is what the ABI
 * exists to make true.
 */
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

#include "deh_str.h"
#include "i_sound.h"
#include "m_misc.h"
#include "w_wad.h"
#include "z_zone.h"

#include "espix_audio.h"

/*
 * The DMX lumps are 8-bit unsigned mono at 11 kHz and the sink wants 16-bit
 * stereo at 44.1 kHz, so the module does the conversion: a Q16 position step
 * per output frame is a repeat-4 upsample, which is all a 1993 sound effect
 * needs and costs one add.
 */
#define SFX_RATE     11025
#define OUT_RATE     44100
#define NUM_CHANNELS 16
#define MIX_FRAMES   512                    /* ~12 ms at 44.1 kHz */

typedef struct {
    uint8_t *samples;                       /* after the header, tail trimmed */
    unsigned length;                        /* in samples */
} sfx_cache_t;

typedef struct {
    const uint8_t *samples;
    unsigned       length;
    uint32_t       pos;                     /* Q16, into samples */
    uint32_t       step;                    /* Q16 per output frame */
    int            left;                    /* 0-255 each, from volume and sep */
    int            right;
    volatile bool  active;
} sfx_channel_t;

static sfx_channel_t s_chan[NUM_CHANNELS];
static int32_t       s_acc[MIX_FRAMES * 2]; /* the sum, before clipping */
static int16_t       s_mix[MIX_FRAMES * 2];

static espix_audio_stream_t *s_stream;
static pthread_t             s_thread;
static volatile bool         s_running;
static volatile bool         s_thread_up;
static volatile bool         s_thread_done;   /* the thread sets this as it leaves */
static bool                  s_initialised;
static bool                  s_sfx_prefix;

/*
 * The two symbols i_sound.c binds as console variables but which live in the
 * backend this build filters out (i_sdlsound.c). Zero is what "no libsamplerate"
 * means: the variables are bound and then consulted by nothing.
 */
int   use_libsamplerate   = 0;
float libsamplerate_scale = 0.65f;

static void sfx_lump_name(sfxinfo_t *sfx, char *buf, size_t buf_len)
{
    if (sfx->link != NULL) {
        sfx = sfx->link;
    }
    if (s_sfx_prefix) {
        M_snprintf(buf, buf_len, "ds%s", DEH_String(sfx->name));
    } else {
        M_StringCopy(buf, DEH_String(sfx->name), buf_len);
    }
}

/*
 * Parse a DMX lump and copy the samples out of the WAD.
 *
 * Header: format (0x03, 0x00), sample rate (LE16), sample count (LE32). The
 * first and last 16 bytes are then skipped, because DMX did -- the same
 * arithmetic, and the same sanity checks, as the SDL backend's CacheSFX.
 *
 * Copied rather than pointed at: a lump pointer is only valid until the WAD's
 * zone is purged, and the mixer reads it on another thread.
 */
static sfx_cache_t *sfx_cache(sfxinfo_t *sfx)
{
    if (sfx->driver_data != NULL) {
        return (sfx_cache_t *)sfx->driver_data;
    }
    if (sfx->lumpnum < 0) {
        char name[9];
        sfx_lump_name(sfx, name, sizeof(name));
        sfx->lumpnum = W_CheckNumForName(name);
        if (sfx->lumpnum < 0) {
            return NULL;
        }
    }

    const uint8_t *data = W_CacheLumpNum(sfx->lumpnum, PU_STATIC);
    const unsigned len  = W_LumpLength(sfx->lumpnum);
    if (data == NULL || len < 8 || data[0] != 0x03 || data[1] != 0x00) {
        return NULL;
    }
    const unsigned samples = (unsigned)data[4] | ((unsigned)data[5] << 8)
                           | ((unsigned)data[6] << 16) | ((unsigned)data[7] << 24);
    if (samples <= 48 || samples > len - 8) {
        return NULL;
    }

    const unsigned n = samples - 32;
    sfx_cache_t *c = Z_Malloc(sizeof(*c), PU_STATIC, NULL);
    c->samples = Z_Malloc((int)n, PU_STATIC, NULL);
    memcpy(c->samples, data + 16, n);
    c->length = n;
    sfx->driver_data = c;
    return c;
}

static void sfx_set_params(int channel, int vol, int sep)
{
    if (channel < 0 || channel >= NUM_CHANNELS) {
        return;
    }
    sfx_channel_t *c = &s_chan[channel];

    /* The engine's own panning arithmetic, kept as it is written upstream. */
    int left  = ((254 - sep) * vol) / 127;
    int right = (sep * vol) / 127;
    if (left < 0) left = 0; else if (left > 255) left = 255;
    if (right < 0) right = 0; else if (right > 255) right = 255;
    c->left  = left;
    c->right = right;
}

/*
 * The mixer, on its own thread rather than in Update().
 *
 * Update() is called once a tic -- some 35 times a second -- and mixing a
 * second of audio inside one of those calls would stall the game. Writing to
 * the stream blocks while the ring is full, which is where the pacing comes
 * from: the thread runs exactly as fast as the sink consumes.
 */
static void *sfx_thread(void *arg)
{
    (void)arg;

    /*
     * Paced to realtime, and it has to be: writing to the stream only blocks
     * while the ring is full, and the ring is large enough that the producer
     * can otherwise run far ahead of the sink. Left unpaced this thread
     * generated audio as fast as the CPU allowed -- measured at 96% of a core,
     * above the game's own priority, which is what made Doom crawl.
     *
     * The credit is the same shape Bluedroid's A2DP source uses for the same
     * reason: count the frames owed at OUT_RATE and sleep until they are due.
     */
    const int64_t start_us = esp_timer_get_time();
    uint64_t      produced = 0;

    while (s_running) {
        const int64_t due_us = (int64_t)(produced * 1000000ULL / OUT_RATE);
        int64_t       wait_ms = (due_us - (esp_timer_get_time() - start_us)) / 1000;
        if (wait_ms > 0) {
            if (wait_ms > 20) {
                wait_ms = 20;               /* wake often enough to see a stop */
            }
            vTaskDelay(pdMS_TO_TICKS(wait_ms));
            continue;
        }
        produced += MIX_FRAMES;

        memset(s_acc, 0, sizeof(s_acc));
        bool any = false;

        for (int i = 0; i < NUM_CHANNELS; i++) {
            sfx_channel_t *c = &s_chan[i];
            if (!c->active) {
                continue;
            }
            any = true;
            for (int f = 0; f < MIX_FRAMES; f++) {
                const unsigned idx = c->pos >> 16;
                if (idx >= c->length) {
                    c->active = false;      /* finished */
                    break;
                }
                /* 8-bit unsigned mono, centred and widened to 16 bits. */
                const int32_t v = ((int32_t)c->samples[idx] - 128) << 8;
                s_acc[2 * f]     += (v * c->left)  >> 8;
                s_acc[2 * f + 1] += (v * c->right) >> 8;
                c->pos += c->step;
            }
        }

        for (int i = 0; i < MIX_FRAMES * 2; i++) {
            const int32_t v = s_acc[i];
            s_mix[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
        }

        if (!any) {
            /* Nothing sounding. Do not feed the sink -- the same reasoning as
             * the engine's mixer -- and wake soon enough that the next shot is
             * not late. */
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        size_t off = 0;
        while (off < sizeof(s_mix) && s_running) {
            const size_t w = espix_audio_stream_write(s_stream,
                                                      (uint8_t *)s_mix + off,
                                                      sizeof(s_mix) - off);
            if (w == 0) {
                vTaskDelay(pdMS_TO_TICKS(10));  /* ring full, not a stop */
                continue;
            }
            off += w;
        }
    }

    s_thread_up   = false;
    s_thread_done = true;
    return NULL;
}

static boolean I_Espix_InitSound(boolean use_sfx_prefix)
{
    if (s_initialised) {
        return true;
    }
    s_sfx_prefix = use_sfx_prefix;
    memset(s_chan, 0, sizeof(s_chan));

    const espix_audio_format_t fmt = { OUT_RATE, 2, 16 };
    s_stream = espix_audio_stream_open(&fmt);
    if (s_stream == NULL) {
        /* No sink connected, or every slot taken. The game carries on silent,
         * which is what "no sound card" looks like on a real machine too. */
        return false;
    }

    s_running   = true;
    s_thread_up = true;
    if (pthread_create(&s_thread, NULL, sfx_thread, NULL) != 0) {
        s_running   = false;
        s_thread_up = false;
        (void)espix_audio_stream_close(s_stream);
        s_stream = NULL;
        return false;
    }
    s_initialised = true;
    return true;
}

static void I_Espix_ShutdownSound(void)
{
    if (!s_initialised) {
        return;
    }
    /*
     * Wait for the thread to say it has left, and only then touch the stream.
     *
     * This was a fault in the thread on exit: pthread_join alone was not enough
     * of a guarantee, and closing the stream while the mixer was still inside
     * stream_write freed the ring under it -- the coredump named the faulting
     * task as this one. The flag is set as the thread's last act, so once it is
     * seen nothing here can be reached by it.
     */
    s_running = false;
    for (int i = 0; i < 200 && !s_thread_done; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_thread_up) {
        pthread_join(s_thread, NULL);
        s_thread_up = false;
    }
    /*
     * In this order: the thread is gone, so nothing is writing, and close()
     * then drains what is queued before suspending the sink. Closing first
     * would free the ring under the writer.
     */
    if (s_stream != NULL) {
        (void)espix_audio_stream_close(s_stream);
        s_stream = NULL;
    }
    s_initialised = false;
}

static int I_Espix_GetSfxLumpNum(sfxinfo_t *sfx)
{
    char name[9];

    sfx_lump_name(sfx, name, sizeof(name));
    return W_GetNumForName(name);
}

static void I_Espix_UpdateSound(void)
{
    /* All the work is on the mixer's own thread; there is nothing to do per
     * tic except let the finished channels reap themselves, which they do. */
}

static void I_Espix_UpdateSoundParams(int channel, int vol, int sep)
{
    if (!s_initialised) {
        return;
    }
    sfx_set_params(channel, vol, sep);
}

static int I_Espix_StartSound(sfxinfo_t *sfxinfo, int channel, int vol, int sep)
{
    if (!s_initialised) {
        return -1;
    }
    if (channel < 0) {
        /* The engine can ask for any channel. Take the first free one; if they
         * are all busy, drop this sound rather than cut another off, which is
         * what the priority field would be for and this module does not do. */
        for (int i = 0; i < NUM_CHANNELS; i++) {
            if (!s_chan[i].active) {
                channel = i;
                break;
            }
        }
        if (channel < 0) {
            return -1;
        }
    }
    if (channel >= NUM_CHANNELS) {
        return -1;
    }

    sfx_cache_t *c = sfx_cache(sfxinfo);
    if (c == NULL) {
        return -1;                          /* not a DMX lump we can play */
    }

    sfx_channel_t *ch = &s_chan[channel];
    ch->samples = c->samples;
    ch->length  = c->length;
    ch->pos     = 0;
    ch->step    = (uint32_t)(((uint64_t)SFX_RATE << 16) / OUT_RATE);
    sfx_set_params(channel, vol, sep);
    ch->active  = true;                     /* last: the mixer may be reading */
    return channel;
}

static void I_Espix_StopSound(int channel)
{
    if (channel >= 0 && channel < NUM_CHANNELS) {
        s_chan[channel].active = false;
    }
}

static boolean I_Espix_SoundIsPlaying(int channel)
{
    if (channel < 0 || channel >= NUM_CHANNELS) {
        return false;
    }
    return s_chan[channel].active;
}

static void I_Espix_CacheSounds(sfxinfo_t *sounds, int num_sounds)
{
    /* Deliberately lazy. Preloading every effect is what libsamplerate needed
     * here and we do not use it; loading each on first play keeps the whole
     * set out of the zone until it is asked for, which is the same rule the
     * rest of espix follows. */
    (void)sounds;
    (void)num_sounds;
}

/*
 * Sound Blaster, because that is what snd_sfxdevice defaults to: the engine
 * matches the requested device against this list, so claiming SB is claiming
 * the default.
 */
static snddevice_t s_espix_devices[] = { SNDDEVICE_SB };

sound_module_t DG_sound_module = {
    .sound_devices     = s_espix_devices,
    .num_sound_devices = 1,
    .Init              = I_Espix_InitSound,
    .Shutdown          = I_Espix_ShutdownSound,
    .GetSfxLumpNum     = I_Espix_GetSfxLumpNum,
    .Update            = I_Espix_UpdateSound,
    .UpdateSoundParams = I_Espix_UpdateSoundParams,
    .StartSound        = I_Espix_StartSound,
    .StopSound         = I_Espix_StopSound,
    .SoundIsPlaying    = I_Espix_SoundIsPlaying,
    .CacheSounds       = I_Espix_CacheSounds,
};

/*
 * Music: not yet, and it must exist all the same.
 *
 * FEATURE_SOUND makes i_sound.c reference DG_music_module, so a build with
 * sound effects and no music still has to provide one. This one claims no
 * devices, so nothing selects it, and doom_espix.c keeps
 * snd_musicdevice = SNDDEVICE_NONE -- which is what stops the engine swapping
 * the intro for its OPL variant and dying on "d_introa not found!" in a WAD
 * that has no such lump. The real one is dbopl plus MUS/GENMIDI, later.
 */
static boolean music_none_init(void)              { return false; }
static void    music_none_void(void)              { }
static void    music_none_volume(int v)           { (void)v; }
static void   *music_none_register(void *d, int l) { (void)d; (void)l; return NULL; }
static void    music_none_unregister(void *h)     { (void)h; }
static void    music_none_play(void *h, boolean lo) { (void)h; (void)lo; }
static boolean music_none_playing(void)           { return false; }

music_module_t DG_music_module = {
    .sound_devices     = NULL,
    .num_sound_devices = 0,
    .Init              = music_none_init,
    .Shutdown          = music_none_void,
    .SetMusicVolume    = music_none_volume,
    .PauseMusic        = music_none_void,
    .ResumeMusic       = music_none_void,
    .RegisterSong      = music_none_register,
    .UnRegisterSong    = music_none_unregister,
    .PlaySong          = music_none_play,
    .StopSong          = music_none_void,
    .MusicIsPlaying    = music_none_playing,
    .Poll              = music_none_void,
};
