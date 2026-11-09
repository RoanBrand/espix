/*
 * A U-mode app's screen, surfaces and audio streams (docs/USER-MODE.md): what an
 * M-mode app calls through espix_gfx.h and espix_audio.h, as syscalls on handles
 * its process holds. A surface's pixels are lent to the app, so it draws where
 * the PPA reads.
 */
#include "sdkconfig.h"

#if CONFIG_ESPIX_UMODE

#include <errno.h>
#include <stdint.h>
#include <sys/param.h>

#include "esp_heap_caps.h"
#include "riscv/encoding.h"

#include "espix_audio.h"
#include "espix_display.h"
#include "espix_gfx.h"
#include "espix_umode.h"
#include "umode_sys.h"

#define PG 4096u

/* A write's wait for room in the ring between looks for a signal, and its whole wait, as an M-mode write's. */
#define AUDIO_WAIT_MS  50
#define AUDIO_GIVE_UP_MS 1000

/* ------------------------------------------------------------------ */
/* Handles                                                             */
/* ------------------------------------------------------------------ */

static void surface_release(void *s)
{
    espix_surface_free(s);
}

/* The object behind a handle whose last reference has gone. */
static void obj_end(espix_um_proc_t *p, const espix_um_obj_t *o)
{
    if (o->kind == ESPIX_UM_OBJ_SURFACE) {
        /* Freed once no kernel call still holds a pointer into its pixels. */
        espix_um_unmap_lent(p->as, o->va, surface_release, o->obj);
    } else if (o->kind == ESPIX_UM_OBJ_AUDIO) {
        (void)espix_audio_stream_close(o->obj);
    }
}

/* A handle for obj, or -EMFILE. */
static int32_t obj_add(espix_um_proc_t *p, uint8_t kind, void *obj, uint32_t va)
{
    int32_t h = -EMFILE;
    xSemaphoreTake(p->lock, portMAX_DELAY);
    for (int i = 0; i < ESPIX_UM_OBJS; i++) {
        if (p->objs[i].kind == ESPIX_UM_OBJ_FREE) {
            p->objs[i] = (espix_um_obj_t){ .kind = kind, .refs = 1, .obj = obj, .va = va };
            h = i;
            break;
        }
    }
    xSemaphoreGive(p->lock);
    return h;
}

/* Handle h's object if it is of kind, with a reference for the caller to put back. */
static espix_um_obj_t *obj_get(espix_um_proc_t *p, uint32_t h, uint8_t kind)
{
    espix_um_obj_t *o = NULL;
    xSemaphoreTake(p->lock, portMAX_DELAY);
    if (h < ESPIX_UM_OBJS && p->objs[h].kind == kind && !p->objs[h].closed) {
        o = &p->objs[h];
        o->refs++;
    }
    xSemaphoreGive(p->lock);
    return o;
}

static void obj_put(espix_um_proc_t *p, espix_um_obj_t *o)
{
    espix_um_obj_t last = { 0 };
    xSemaphoreTake(p->lock, portMAX_DELAY);
    if (--o->refs == 0) {
        last = *o;
        *o = (espix_um_obj_t){ 0 };
    }
    xSemaphoreGive(p->lock);
    if (last.kind != ESPIX_UM_OBJ_FREE) {
        obj_end(p, &last);
    }
}

/* The handle goes now; its object when the calls using it have returned. */
static int32_t obj_close(espix_um_proc_t *p, uint32_t h, uint8_t kind)
{
    espix_um_obj_t *o = obj_get(p, h, kind);
    if (o == NULL) {
        return -EBADF;
    }
    xSemaphoreTake(p->lock, portMAX_DELAY);
    const bool first = !o->closed;
    o->closed = true;
    if (first) {
        o->refs--;      /* the handle's own */
    }
    xSemaphoreGive(p->lock);
    obj_put(p, o);
    return first ? 0 : -EBADF;
}

void espix_um_objs_release(espix_um_proc_t *p)
{
    for (int i = 0; i < ESPIX_UM_OBJS; i++) {
        const espix_um_obj_t o = p->objs[i];
        p->objs[i] = (espix_um_obj_t){ 0 };
        if (o.kind != ESPIX_UM_OBJ_FREE) {
            obj_end(p, &o);
        }
    }
}

/* ------------------------------------------------------------------ */
/* The screen                                                          */
/* ------------------------------------------------------------------ */

int32_t um_sys_gfx_open(um_call_t *c)
{
    espix_gfx_t *g = espix_gfx_open_mode((int)c->a[0], (int)c->a[1]);
    if (g == NULL) {
        return (espix_display_canvas() == NULL) ? -ENODEV : -EBUSY;
    }
    c->p->gfx = g;
    return 0;
}

int32_t um_sys_gfx_close(um_call_t *c)
{
    espix_gfx_t *g = c->p->gfx;
    c->p->gfx = NULL;
    espix_gfx_close(g);
    return 0;
}

int32_t um_sys_gfx_surface(um_call_t *c)
{
    int w = (int)c->a[0], h = (int)c->a[1];
    if (w == 0 && h == 0) {
        espix_canvas_t *cv = espix_display_canvas();
        if (cv == NULL) {
            return -ENODEV;
        }
        w = espix_canvas_width(cv);
        h = espix_canvas_height(cv);
    }
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
        return -EINVAL;
    }
    espix_surface_t *s = espix_surface_new_aligned(w, h, PG);
    if (s == NULL) {
        return -ENOMEM;
    }
    const uint32_t va = espix_um_map_lent(c->p->as, espix_surface_pixels(s),
                                          (size_t)w * h * sizeof(espix_px_t), PTE_R | PTE_W);
    if (va == 0) {
        espix_surface_free(s);
        return -ENOMEM;
    }
    const int32_t hd = obj_add(c->p, ESPIX_UM_OBJ_SURFACE, s, va);
    if (hd < 0) {
        espix_um_unmap_lent(c->p->as, va, surface_release, s);
        return hd;
    }
    const struct espix_gfx_fb fb = { .w = w, .h = h, .stride = w, .pixels = va };
    const int32_t r = espix_um_copyout(c->p->as, c->a[2], &fb, sizeof(fb));
    if (r < 0) {
        obj_close(c->p, (uint32_t)hd, ESPIX_UM_OBJ_SURFACE);
        return r;
    }
    return hd;
}

int32_t um_sys_gfx_free(um_call_t *c)
{
    return obj_close(c->p, c->a[0], ESPIX_UM_OBJ_SURFACE);
}

int32_t um_sys_gfx_present(um_call_t *c)
{
    if (c->p->gfx == NULL) {
        return -ENODEV;
    }
    espix_um_obj_t *o = obj_get(c->p, c->a[0], ESPIX_UM_OBJ_SURFACE);
    if (o == NULL) {
        return -EBADF;
    }
    const bool ok = espix_gfx_present_rect_of(o->obj, (int)c->a[1], (int)c->a[2], (int)c->a[3], (int)c->a[4]);
    obj_put(c->p, o);
    return ok ? 0 : -ENODEV;
}

int32_t um_sys_gfx_poll(um_call_t *c)
{
    espix_input_event_t ev;
    if (c->p->gfx == NULL || !espix_gfx_poll_event(c->p->gfx, &ev)) {
        return 0;
    }
    const struct espix_input u = {
        .kind = ev.kind, .x = ev.x, .y = ev.y, .buttons = ev.buttons, .keysym = ev.keysym, .down = ev.down,
    };
    const int32_t r = espix_um_copyout(c->p->as, c->a[0], &u, sizeof(u));
    return (r < 0) ? r : 1;
}

/* ------------------------------------------------------------------ */
/* Audio                                                               */
/* ------------------------------------------------------------------ */

int32_t um_sys_audio_open(um_call_t *c)
{
    struct espix_audio_fmt f;
    const int32_t r = espix_um_copyin(c->p->as, &f, c->a[0], sizeof(f));
    if (r < 0) {
        return r;
    }
    if (f.channels == 0 || f.channels > 2 || f.bits != 16) {
        return -EINVAL;
    }
    const espix_audio_format_t fmt = { .rate = f.rate, .channels = (uint8_t)f.channels, .bits = (uint8_t)f.bits };
    espix_audio_stream_t *st = espix_audio_stream_open(&fmt);
    if (st == NULL) {
        return -ENODEV;
    }
    espix_audio_stream_disown(st);
    const int32_t h = obj_add(c->p, ESPIX_UM_OBJ_AUDIO, st, 0);
    if (h < 0) {
        (void)espix_audio_stream_close(st);
    }
    return h;
}

/* What the ring takes, waiting while it takes nothing; a short count as an M-mode write's, which the app loops on. */
int32_t um_sys_audio_write(um_call_t *c)
{
    espix_um_obj_t *o = obj_get(c->p, c->a[0], ESPIX_UM_OBJ_AUDIO);
    if (o == NULL) {
        return -EBADF;
    }
    const uint32_t uva = c->a[1], len = c->a[2];
    uint32_t done = 0, waited = 0;
    int32_t r = 0;
    while (done < len) {
        const uint32_t va = uva + done;
        const void *k = espix_um_user_pin(c->p->as, va, PTE_R);
        if (k == NULL) {
            r = -EFAULT;
            break;
        }
        const uint32_t want = MIN(len - done, PG - (va & (PG - 1)));
        const size_t put = espix_audio_stream_write_ms(o->obj, k, want, AUDIO_WAIT_MS);
        espix_um_user_unpin(c->p->as);
        done += put;
        if (put == want) {
            continue;
        }
        if (done > 0) {
            break;
        }
        if (espix_sigcheck_mask() != 0) {
            r = -EINTR;
            break;
        }
        if ((waited += AUDIO_WAIT_MS) >= AUDIO_GIVE_UP_MS) {
            break;
        }
    }
    obj_put(c->p, o);
    return (done > 0) ? (int32_t)done : r;
}

int32_t um_sys_audio_ctl(um_call_t *c)
{
    espix_um_obj_t *o = obj_get(c->p, c->a[0], ESPIX_UM_OBJ_AUDIO);
    if (o == NULL) {
        return -EBADF;
    }
    int32_t r;
    switch (c->a[1]) {
    case ESPIX_AUDIO_VOLUME:
        r = (espix_audio_stream_volume(o->obj, (int)c->a[2]) == ESP_OK) ? 0 : -EINVAL;
        break;
    case ESPIX_AUDIO_MUTE:
        r = (espix_audio_stream_mute(o->obj, c->a[2] != 0) == ESP_OK) ? 0 : -EINVAL;
        break;
    case ESPIX_AUDIO_QUEUED:
        r = (int32_t)MIN(espix_audio_stream_queued_us(o->obj), (uint32_t)INT32_MAX);
        break;
    default:
        r = -EINVAL;
        break;
    }
    obj_put(c->p, o);
    return r;
}

int32_t um_sys_audio_close(um_call_t *c)
{
    return obj_close(c->p, c->a[0], ESPIX_UM_OBJ_AUDIO);
}

#endif /* CONFIG_ESPIX_UMODE */
