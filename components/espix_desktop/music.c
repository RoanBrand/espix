/*
 * The music player: a window over espix_audio_play().
 *
 * There is no decoder here and no buffer -- the engine already owns both, and
 * playback runs on its own task. What this adds is the three things a command
 * line makes awkward: a file named on the screen, a state that updates by
 * itself when the track ends, and volume buttons.
 *
 * The state is polled once a second and painted only when it changes, which is
 * the same bargain the settings panel makes: a repaint is not free, and the
 * interesting moment is the transition, not the interval.
 */

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"

#include "espix_audio.h"
#include "espix_desktop.h"
#include "espix_display.h"
#include "espix_kernel.h"
#include "espix_widgets.h"

#define TAG "music"

#define ROW_H WGT_ROW_H

static espix_window_t *s_win;
static char            s_path[ESPIX_PATH_MAX];
static char            s_err[64];
static char            s_state[24];
static int             s_vol_last = -1;
static esp_timer_handle_t s_timer;

static void base_name(const char *path, char *out, size_t n)
{
    const char *slash = strrchr(path, '/');

    strlcpy(out, (slash != NULL && slash[1] != '\0') ? slash + 1 : path, n);
}

/* ------------------------------------------------------------------ */
/* Geometry                                                            */
/* ------------------------------------------------------------------ */

static espix_rect_t row_rect(const espix_window_t *w, int i)
{
    const espix_rect_t c = espix_window_content(w);

    return (espix_rect_t){ c.x + 8, c.y + 8 + i * ROW_H, c.w - 16, ROW_H };
}

#define BTN_W 96
#define BTN_H 28
#define BTN_GAP 8

static espix_rect_t music_btn(const espix_window_t *w, int which)
{
    const espix_rect_t c     = espix_window_content(w);
    const int          total = 3 * BTN_W + 2 * BTN_GAP;
    const int          x0    = c.x + (c.w - total) / 2;

    return (espix_rect_t){ x0 + which * (BTN_W + BTN_GAP),
                           c.y + c.h - BTN_H - 10, BTN_W, BTN_H };
}

/* ------------------------------------------------------------------ */
/* Playback                                                            */
/* ------------------------------------------------------------------ */

static void music_play(const char *path)
{
    s_err[0] = '\0';

    const esp_err_t err = espix_audio_play(path);

    if (err == ESP_ERR_INVALID_STATE) {
        snprintf(s_err, sizeof(s_err), "no audio sink connected");
    } else if (err != ESP_OK) {
        snprintf(s_err, sizeof(s_err), "cannot play: %s", esp_err_to_name(err));
    }
    espix_klog(ESPIX_KLOG_INFO, TAG, "%s: %s", path,
               (err == ESP_OK) ? "playing" : s_err);
}

static void music_volume(int delta)
{
    int v = espix_audio_get_volume() + delta;

    if (v < 0)   { v = 0; }
    if (v > 100) { v = 100; }

    (void)espix_audio_set_volume(v);
}

/* Paint when what the window says would change, and not otherwise. */
static void music_refresh(bool force)
{
    const char *state = espix_audio_state();
    const int   vol   = espix_audio_get_volume();
    const bool  changed = force || strcmp(state, s_state) != 0
                              || vol != s_vol_last;

    strlcpy(s_state, state, sizeof(s_state));
    s_vol_last = vol;

    if (changed && s_win != NULL) {
        espix_window_repaint(s_win);
    }
}

static void music_tick_desk(void *arg)
{
    (void)arg;

    if (s_win != NULL) {
        music_refresh(false);
    }
}

/* esp_timer context: the refresh is the desktop task's to do. */
static void music_tick(void *arg)
{
    (void)arg;
    (void)espix_desktop_post(music_tick_desk, NULL);
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

static void music_draw(espix_window_t *w, espix_surface_t *s, espix_rect_t r,
                       void *ctx)
{
    (void)r;
    (void)ctx;

    const espix_rect_t c = espix_window_content(w);

    espix_wgt_panel(s, c, WGT_BG);

    char name[40];
    base_name(s_path, name, sizeof(name));

    espix_wgt_heading(s, row_rect(w, 0), s_path[0] != '\0' ? name : "music");

    espix_wgt_field(s, row_rect(w, 1), "State", s_state, true);

    char vol[16];
    snprintf(vol, sizeof(vol), "%d%%", espix_audio_get_volume());
    espix_wgt_field(s, row_rect(w, 2), "Volume", vol, true);

    if (s_err[0] != '\0') {
        espix_wgt_label(s, row_rect(w, 3), s_err, false);
    }

    espix_wgt_button(s, music_btn(w, 0), "Stop", true, false);
    espix_wgt_button(s, music_btn(w, 1), "Vol -", true, false);
    espix_wgt_button(s, music_btn(w, 2), "Vol +", true, false);
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

static void music_pointer(espix_window_t *w, int x, int y, uint8_t buttons,
                          void *ctx)
{
    (void)ctx;

    if (buttons == 0) {
        return;                         /* act on the press, not the release */
    }

    if (espix_wgt_hit(music_btn(w, 0), x, y)) {
        (void)espix_audio_stop();
    } else if (espix_wgt_hit(music_btn(w, 1), x, y)) {
        music_volume(-10);
    } else if (espix_wgt_hit(music_btn(w, 2), x, y)) {
        music_volume(+10);
    } else {
        return;
    }
    music_refresh(true);
}

/* ------------------------------------------------------------------ */
/* The window                                                          */
/* ------------------------------------------------------------------ */

static void music_timer_start(void)
{
    if (s_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = music_tick,
            .name     = "music",
        };
        if (esp_timer_create(&args, &s_timer) != ESP_OK) {
            s_timer = NULL;             /* a still window beats no window */
            return;
        }
    }

    /* Started, not "started if it was not already": forget() stops it and keeps
     * the handle, so a window reopened after being closed had no timer at all
     * and its state stopped following the engine. */
    strlcpy(s_state, espix_audio_state(), sizeof(s_state));
    (void)esp_timer_start_periodic(s_timer, 1000 * 1000);
}

espix_window_t *espix_music_open(const char *path)
{
    if (s_win == NULL) {
        s_win = espix_window_new(180, 150, 420, 210, "music");
        if (s_win == NULL) {
            return NULL;
        }
        espix_window_set_draw(s_win, music_draw);
        espix_window_set_pointer(s_win, music_pointer);

        strlcpy(s_state, espix_audio_state(), sizeof(s_state));
        s_vol_last = espix_audio_get_volume();
        music_timer_start();
    }

    if (path != NULL && path[0] != '\0') {
        strlcpy(s_path, path, sizeof(s_path));
        music_play(path);
    }
    music_refresh(true);
    return s_win;
}

void espix_music_forget(void)
{
    s_win = NULL;                       /* its pixels went with the window */

    if (s_timer != NULL) {
        (void)esp_timer_stop(s_timer);
    }

    /* Closing the player stops the track: leaving it playing out of a window
     * that is gone is a thing nobody can then turn off. */
    (void)espix_audio_stop();
    s_path[0]  = '\0';
    s_err[0]   = '\0';
    s_vol_last = -1;
}

espix_window_t *espix_music_window(void)
{
    return s_win;
}
