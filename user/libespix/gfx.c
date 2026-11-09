/*
 * espix_gfx.h as syscalls. A surface's pixels are mapped into the app; the
 * screen's own lock is a surface the screen's size, copied to it on present.
 */
#include <pthread.h>
#include <stdlib.h>

#include "espix_gfx.h"
#include "sys.h"

struct espix_gfx {
    bool                 open;
    espix_gfx_surface_t *screen;
};

struct espix_gfx_surface {
    long            handle;
    espix_gfx_fb_t  fb;
    pthread_mutex_t lock;
};

static struct espix_gfx s_gfx;

espix_gfx_t *espix_gfx_open_mode(int full_w, int full_h)
{
    if (espix_ret(espix_sys2(ESPIX_SYS_GFX_OPEN, full_w, full_h)) < 0) {
        return NULL;
    }
    s_gfx.open = true;
    return &s_gfx;
}

espix_gfx_t *espix_gfx_open(void)
{
    return espix_gfx_open_mode(0, 0);
}

void espix_gfx_close(espix_gfx_t *g)
{
    if (g == NULL || !g->open) {
        return;
    }
    espix_gfx_surface_free(g->screen);
    g->screen = NULL;
    espix_sys0(ESPIX_SYS_GFX_CLOSE);
    g->open = false;
}

static espix_gfx_surface_t *surface_new(int w, int h)
{
    espix_gfx_surface_t *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        return NULL;
    }
    struct espix_gfx_fb fb;
    s->handle = espix_ret(espix_sys3(ESPIX_SYS_GFX_SURFACE, w, h, &fb));
    if (s->handle < 0) {
        free(s);
        return NULL;
    }
    s->fb = (espix_gfx_fb_t){ fb.w, fb.h, fb.stride, (uint16_t *)fb.pixels };
    pthread_mutex_init(&s->lock, NULL);
    return s;
}

espix_gfx_fb_t espix_gfx_lock(espix_gfx_t *g)
{
    if (g == NULL || !g->open || (g->screen == NULL && (g->screen = surface_new(0, 0)) == NULL)) {
        return (espix_gfx_fb_t){ 0, 0, 0, NULL };
    }
    return espix_gfx_surface_lock(g->screen);
}

void espix_gfx_present_rect(espix_gfx_t *g, int x, int y, int w, int h)
{
    if (g == NULL || g->screen == NULL || w <= 0 || h <= 0) {
        return;
    }
    espix_sys5(ESPIX_SYS_GFX_PRESENT, g->screen->handle, x, y, w, h);
    pthread_mutex_unlock(&g->screen->lock);
}

void espix_gfx_present(espix_gfx_t *g)
{
    if (g != NULL && g->screen != NULL) {
        espix_gfx_present_rect(g, 0, 0, g->screen->fb.w, g->screen->fb.h);
    }
}

void espix_gfx_unlock(espix_gfx_t *g)
{
    if (g != NULL && g->screen != NULL) {
        pthread_mutex_unlock(&g->screen->lock);
    }
}

espix_gfx_surface_t *espix_gfx_surface_new(int w, int h)
{
    return (w > 0 && h > 0) ? surface_new(w, h) : NULL;
}

void espix_gfx_surface_free(espix_gfx_surface_t *s)
{
    if (s == NULL) {
        return;
    }
    espix_sys1(ESPIX_SYS_GFX_FREE, s->handle);
    pthread_mutex_destroy(&s->lock);
    free(s);
}

espix_gfx_fb_t espix_gfx_surface_lock(espix_gfx_surface_t *s)
{
    if (s == NULL) {
        return (espix_gfx_fb_t){ 0, 0, 0, NULL };
    }
    pthread_mutex_lock(&s->lock);
    return s->fb;
}

void espix_gfx_surface_unlock(espix_gfx_surface_t *s)
{
    if (s != NULL) {
        pthread_mutex_unlock(&s->lock);
    }
}

void espix_gfx_present_surface(espix_gfx_t *g, const espix_gfx_surface_t *s)
{
    if (g != NULL && g->open && s != NULL) {
        espix_sys5(ESPIX_SYS_GFX_PRESENT, s->handle, 0, 0, 0, 0);
    }
}

bool espix_gfx_poll_event(espix_gfx_t *g, espix_input_event_t *ev)
{
    struct espix_input u;
    if (g == NULL || ev == NULL || espix_sys1(ESPIX_SYS_GFX_POLL, &u) <= 0) {
        return false;
    }
    *ev = (espix_input_event_t){
        .kind = (espix_input_kind_t)u.kind, .x = u.x, .y = u.y,
        .buttons = (uint8_t)u.buttons, .keysym = u.keysym, .down = u.down != 0,
    };
    return true;
}

/* The kernel's own mapping (espix_display). */
char espix_keysym_char(uint32_t ks)
{
    if (ks >= 0x20 && ks <= 0x7E) {
        return (char)ks;
    }
    switch (ks) {
    case 0xFF0D:
    case 0xFF8D: return '\r';
    case 0xFF08: return '\b';
    default:     return 0;
    }
}
