/*
 * Graphics and input, published to apps.
 *
 * The implementation is thin on purpose: it is the display service's owner
 * model with a poll-shaped face on it. A game wants a framebuffer and the keys,
 * not callbacks, so the owner's input callback pushes into a queue the app
 * drains once a frame.
 *
 * One static handle, not one per app, and that is deliberate. The display keeps
 * a pointer to the screen it was handed, and an app's heap does not outlive it:
 * a program killed mid-frame would leave the service holding a pointer into
 * freed memory. The handle lives in the firmware instead, so the worst a killed
 * app leaves behind is an owner that has stopped drawing -- and the next claim
 * (the desktop, or the app run again) takes over from it.
 */

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "esp_heap_caps.h"

#include "esp_elf.h"

#include "espix_display.h"
#include "espix_gfx.h"
#include "espix_kernel.h"
#include "espix_proc.h"
#include "espix_proc_priv.h"

#define TAG "gfx"

#define GFX_EVENTS 32

struct espix_gfx {
    espix_screen_t screen;      /* what the display calls; see the note above */
    QueueHandle_t  events;
    bool           open;
    bool           canvas_locked;
    espix_pid_t    lock_pid;    /* who holds the canvas; see espix_gfx_recover() */
};

struct espix_gfx_surface {
    espix_surface_t *s;
};

static struct espix_gfx s_gfx;

/*
 * The canvas lock, with a note of who holds it.
 *
 * A task deleted between the take and the give leaves the mutex held by a TCB
 * that no longer exists, and every later lock -- the desktop repainting, the
 * VNC task encoding -- waits forever, which on this board is a watchdog reset
 * rather than a stuck picture. The gfx handle is firmware memory and outlives
 * the app, so it can remember the holder and orphan the lock once the process
 * is gone; see espix_gfx_recover(). Keyed on the pid, never the task handle,
 * because a deleted task's TCB is freed.
 */
static void gfx_canvas_take(espix_canvas_t *c)
{
    espix_canvas_lock(c);
    s_gfx.lock_pid      = espix_proc_pid_of_task(xTaskGetCurrentTaskHandle());
    s_gfx.canvas_locked = true;
}

static void gfx_canvas_give(espix_canvas_t *c)
{
    s_gfx.canvas_locked = false;
    espix_canvas_unlock(c);
}

void espix_gfx_recover(espix_pid_t pid)
{
    if (!s_gfx.canvas_locked || s_gfx.lock_pid != pid) {
        return;
    }

    espix_canvas_t *c = espix_display_canvas();
    if (c != NULL) {
        espix_canvas_orphan(c);     /* not give: the holder TCB is gone */
    }
    s_gfx.canvas_locked = false;

    espix_klog(ESPIX_KLOG_WARN, TAG,
               "canvas orphaned: pid %d died holding it", (int)pid);
}

/* Runs in whoever posted the input -- the RFB connection task. Never blocks: a
 * full queue drops the event rather than stalling the viewer. */
static void gfx_input(void *ctx, const espix_input_event_t *ev)
{
    struct espix_gfx *g = ctx;

    if (g->events != NULL) {
        (void)xQueueSend(g->events, ev, 0);
    }
}

/*
 * The app redraws on its own loop, so a repaint request has nothing to do; a
 * resize is the same, because the app asks for the new size on its next frame.
 * Both are required by the owner interface, so they exist and do nothing rather
 * than not existing.
 */
static void gfx_repaint(void *ctx) { (void)ctx; }
static void gfx_resized(void *ctx) { (void)ctx; }

/*
 * Claim the screen, and take the canvas at full_w x full_h while this app owns
 * it. A separate entry point rather than two arguments on espix_gfx_open(),
 * and that is deliberate: the two are resolved by name at load, so an app built
 * against the old one would otherwise hand two registers of whatever happened
 * to be in them to the display as its video mode.
 */
espix_gfx_t *espix_gfx_open_mode(int full_w, int full_h)
{
    if (espix_display_canvas() == NULL) {
        return NULL;            /* no display up: nothing to draw on */
    }

    if (s_gfx.events == NULL) {
        s_gfx.events = xQueueCreate(GFX_EVENTS, sizeof(espix_input_event_t));
        if (s_gfx.events == NULL) {
            return NULL;
        }
    }
    xQueueReset(s_gfx.events);

    s_gfx.screen = (espix_screen_t){
        .name    = "app",
        .input   = gfx_input,
        .repaint = gfx_repaint,
        .resized = gfx_resized,
        /* What the app asked for, applied while it owns the canvas. */
        .full_w  = full_w,
        .full_h  = full_h,
        .ctx     = &s_gfx,
    };

    if (espix_display_claim(&s_gfx.screen) != ESP_OK) {
        return NULL;
    }
    s_gfx.open = true;
    return &s_gfx;
}

/* The whole of the original: claim the screen, ask for no size in particular. */
espix_gfx_t *espix_gfx_open(void)
{
    return espix_gfx_open_mode(0, 0);
}

void espix_gfx_close(espix_gfx_t *g)
{
    if (g == NULL || !g->open) {
        return;
    }
    espix_display_release(&g->screen);
    g->open = false;
}

espix_gfx_fb_t espix_gfx_lock(espix_gfx_t *g)
{
    espix_gfx_fb_t fb = { 0, 0, 0, NULL };
    espix_canvas_t *c;

    if (g == NULL || !g->open) {
        return fb;
    }
    c = espix_display_canvas();
    if (c == NULL) {
        return fb;
    }

    gfx_canvas_take(c);
    fb.w      = espix_canvas_width(c);
    fb.h      = espix_canvas_height(c);
    fb.stride = fb.w;
    fb.pixels = espix_canvas_pixels(c);
    return fb;
}

void espix_gfx_present_rect(espix_gfx_t *g, int x, int y, int w, int h)
{
    espix_canvas_t *c;

    if (g == NULL || !g->open) {
        return;
    }
    c = espix_display_canvas();
    if (c == NULL) {
        return;
    }
    espix_canvas_damage(c, (espix_rect_t){ x, y, w, h });
    gfx_canvas_give(c);
}

void espix_gfx_present(espix_gfx_t *g)
{
    espix_canvas_t *c;

    if (g == NULL || !g->open) {
        return;
    }
    c = espix_display_canvas();
    if (c == NULL) {
        return;
    }
    espix_canvas_damage(c, (espix_rect_t){ 0, 0, espix_canvas_width(c),
                                           espix_canvas_height(c) });
    gfx_canvas_give(c);
}

void espix_gfx_unlock(espix_gfx_t *g)
{
    espix_canvas_t *c;

    if (g == NULL || !g->open) {
        return;
    }
    c = espix_display_canvas();
    if (c != NULL) {
        gfx_canvas_give(c);
    }
}

espix_gfx_surface_t *espix_gfx_surface_new(int w, int h)
{
    espix_gfx_surface_t *g;
    espix_surface_t     *s = espix_surface_new(w, h);

    if (s == NULL) {
        return NULL;
    }
    g = calloc(1, sizeof(*g));
    if (g == NULL) {
        espix_surface_free(s);
        return NULL;
    }
    g->s = s;
    return g;
}

void espix_gfx_surface_free(espix_gfx_surface_t *g)
{
    if (g == NULL) {
        return;
    }
    espix_surface_free(g->s);
    free(g);
}

espix_gfx_fb_t espix_gfx_surface_lock(espix_gfx_surface_t *g)
{
    espix_gfx_fb_t fb = { 0, 0, 0, NULL };

    if (g == NULL || g->s == NULL) {
        return fb;
    }
    espix_surface_lock(g->s);
    fb.w      = espix_surface_width(g->s);
    fb.h      = espix_surface_height(g->s);
    fb.stride = fb.w;
    fb.pixels = espix_surface_pixels(g->s);
    return fb;
}

void espix_gfx_surface_unlock(espix_gfx_surface_t *g)
{
    if (g != NULL && g->s != NULL) {
        espix_surface_unlock(g->s);
    }
}

void espix_gfx_present_surface(espix_gfx_t *g, const espix_gfx_surface_t *s)
{
    espix_canvas_t *c;

    if (g == NULL || !g->open || s == NULL || s->s == NULL) {
        return;
    }
    c = espix_display_canvas();
    if (c == NULL) {
        return;
    }

    gfx_canvas_take(c);
    espix_canvas_scale_surface(c, s->s);
    gfx_canvas_give(c);
}

bool espix_gfx_poll_event(espix_gfx_t *g, espix_input_event_t *ev)
{
    if (g == NULL || ev == NULL || g->events == NULL) {
        return false;
    }
    return xQueueReceive(g->events, ev, 0) == pdTRUE;
}

static esp_elf_symbol_table_t s_gfx_syms[] = {
    ESP_ELFSYM_EXPORT(espix_gfx_open),
    ESP_ELFSYM_EXPORT(espix_gfx_open_mode),
    ESP_ELFSYM_EXPORT(espix_gfx_close),
    ESP_ELFSYM_EXPORT(espix_gfx_lock),
    ESP_ELFSYM_EXPORT(espix_gfx_present),
    ESP_ELFSYM_EXPORT(espix_gfx_present_rect),
    ESP_ELFSYM_EXPORT(espix_gfx_unlock),
    ESP_ELFSYM_EXPORT(espix_gfx_surface_new),
    ESP_ELFSYM_EXPORT(espix_gfx_surface_free),
    ESP_ELFSYM_EXPORT(espix_gfx_surface_lock),
    ESP_ELFSYM_EXPORT(espix_gfx_surface_unlock),
    ESP_ELFSYM_EXPORT(espix_gfx_present_surface),
    ESP_ELFSYM_EXPORT(espix_gfx_poll_event),
    /* A key arrives as a keysym; this is how an app turns it into a character. */
    ESP_ELFSYM_EXPORT(espix_keysym_char),
    ESP_ELFSYM_END
};

void espix_proc_abi_gfx_register(void)
{
    if (esp_elf_register_symbol(s_gfx_syms) != 0) {
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "could not publish the graphics symbols to apps");
        return;
    }
    espix_klog(ESPIX_KLOG_INFO, TAG, "graphics and input published to apps");
}
