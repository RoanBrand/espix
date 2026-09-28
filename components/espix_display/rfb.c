/*
 * The RFB server -- what "VNC" actually is on the wire.
 *
 * VNC is the family name; RFB is the protocol, and every client that calls
 * itself a VNC client speaks it. That is the reason to implement this rather
 * than port a server: the standard is the wire format, not a codebase, so the
 * clients themselves are the conformance test and the only dependency is
 * sockets.
 *
 * What this speaks: RFB 3.3 and 3.7/3.8, security type None or VNC
 * authentication (type 2, on by default only when a password is set), and two
 * encodings -- Raw and Hextile. Hextile matters more than it looks: it encodes
 * a uniform 16x16 tile in five bytes instead of 512, so a flat desktop costs
 * about 10 KiB for its first frame rather than 1.9 MiB. It is also the lossless
 * path that will sit beside the JPEG encoder rather than being replaced by it
 * -- photographs want JPEG, text and UI edges want this.
 *
 * One connection at a time, like sshd: memory and failure isolation are worth
 * proving with a single session before multiplying them.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "espix_display.h"
#include "espix_kernel.h"
#include "espix_net.h"

#include "vnc_des.h"

#define TAG "vnc"

#define LISTEN_BACKLOG 1
#define POLL_US        (250 * 1000)   /* send and accept timeout */
#define STALL_LIMIT    40             /* * POLL_US before a peer is gone */

/*
 * How long a change made by *another task* may wait to be sent.
 *
 * Nothing wakes this task when the canvas is damaged, and it blocks in recv(),
 * so the only thing that can notice a local mouse is the receive timeout. At
 * POLL_US that is a quarter of a second, and it showed: the update rate for a
 * local mouse was 5-12 a second with a floor at 1/250ms, while a pointer moved
 * over VNC was sent the instant the client's own message was handled. The
 * asymmetry was never the input path -- it was this wait.
 *
 * A viewer keeps one FramebufferUpdateRequest outstanding, and update_send()
 * holds it rather than answering with an empty update, so polling fast is only
 * worth doing while an update is actually owed: the loop picks the timeout from
 * c->pending, and an idle connection still sleeps for POLL_US.
 */
#define TICK_US        (10 * 1000)

/* The same ten seconds as STALL_LIMIT, counted at the tick rather than at
 * POLL_US: read_full() is the path that waits on a peer part-way through a
 * message, and it uses whichever receive timeout is current. */
#define READ_STALL_LIMIT (STALL_LIMIT * POLL_US / TICK_US)
#define OUT_CAP        (64 * 1024)    /* encode buffer, flushed to the socket */

/* VNC uses the first eight characters of a password and ignores the rest. */
#define VNC_PW_MAX 8

/*
 * Listener state. Declared up here because the socket helpers below consult
 * s_run: a stop request has to be able to break a read or a write that is
 * already waiting on a peer.
 */
static volatile bool s_run;
static int           s_listen_fd = -1;
static TaskHandle_t  s_task;
static uint16_t      s_port;
static int           s_clients;
static char          s_peer[32];
static char          s_encodings[48];

/*
 * What the radio was doing before a viewer asked it to stay awake.
 *
 * A viewer wants the screen as soon as it changes, and a station that sleeps
 * between beacons cannot deliver that: measured on this board, the default
 * sleep costs 58 ms average and 28 ms of jitter on a round trip against 4 ms
 * and 1.7 ms on the cable. Nothing is wrong with the sleep -- it is the whole
 * point of a battery-powered radio -- but it is the wrong trade for the one
 * thing here whose entire job is latency, and a drag is a round trip per
 * motion.
 *
 * So the display asks rather than takes: the mode it found is the mode it puts
 * back, so a wifi-ps-off set by hand stays off and the default returns when the
 * last viewer goes. That is the shape ROADMAP.md asks for -- a component says
 * it wants a low-latency link, and the network decides what that means.
 */
static espix_wifi_ps_t s_ps_saved = ESPIX_WIFI_PS_UNKNOWN;

static void latency_want(bool on)
{
    if (on) {
        espix_wifi_status_t st;
        if (s_ps_saved == ESPIX_WIFI_PS_UNKNOWN &&
            espix_net_wifi_status(&st) == ESP_OK &&
            st.ps != ESPIX_WIFI_PS_UNKNOWN && st.ps != ESPIX_WIFI_PS_NONE) {
            s_ps_saved = st.ps;
            (void)espix_wifi_set_ps(ESPIX_WIFI_PS_NONE);
            espix_klog(ESPIX_KLOG_INFO, TAG,
                       "radio kept awake while a viewer is attached");
        }
    } else if (s_ps_saved != ESPIX_WIFI_PS_UNKNOWN) {
        (void)espix_wifi_set_ps(s_ps_saved);
        espix_klog(ESPIX_KLOG_INFO, TAG, "radio back to its own sleep setting");
        s_ps_saved = ESPIX_WIFI_PS_UNKNOWN;
    }
}

/*
 * The DES key VNC authentication checks against.
 *
 * Only the key is kept, not the password: RFB type 2 sends no username and
 * needs the key itself for the challenge-response, so a password *hash* would
 * be no use to it. `vnc password` writes it and `vnc start` reads it back.
 *
 * When there is no file there is still a key. macOS Screen Sharing refuses to
 * connect without one, and being made to run a command first to satisfy a
 * client's UI is a poor trade, so a built-in default is always in play unless a
 * command says otherwise. That default is *public* -- anyone who can reach port
 * 5900 knows it -- which is precisely why this is a compatibility shim and not
 * a security boundary. The same is already true of the shipped account
 * password, and espix_auth warns about that one; so does this.
 */
#define VNC_KEY_PATH "/etc/vnc.key"

static uint8_t s_vnc_key[8];
static bool    s_have_key;      /* a key to check against */
static bool    s_key_default;   /* ...and it is the built-in one */
static bool    s_auth_decided;  /* a command has already chosen */

/* Client -> server message types. */
enum {
    RFB_SET_PIXEL_FORMAT     = 0,
    RFB_SET_ENCODINGS        = 2,
    RFB_FB_UPDATE_REQUEST    = 3,
    RFB_KEY_EVENT            = 4,
    RFB_POINTER_EVENT        = 5,
    RFB_CLIENT_CUT_TEXT      = 6,
};

/* Encodings. Raw is mandatory; Hextile is the one worth having. */
enum {
    RFB_ENC_RAW      = 0,
    RFB_ENC_COPYRECT = 1,
    RFB_ENC_HEXTILE  = 5,
};

/* ------------------------------------------------------------------ */
/* Byte order helpers                                                  */
/* ------------------------------------------------------------------ */

static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* ------------------------------------------------------------------ */
/* Sockets                                                             */
/* ------------------------------------------------------------------ */

static int recv_byte(int fd, uint8_t *b)
{
    const ssize_t n = recv(fd, b, 1, 0);
    if (n == 1) {
        return 1;
    }
    if (n == 0) {
        return 0;                          /* orderly close */
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return -1;                         /* nothing right now */
    }
    return -2;
}

/* How long recv() may block for, which is the only clock this loop has. */
static void set_recv_timeout(int fd, int us)
{
    const struct timeval tv = { .tv_sec = 0, .tv_usec = us };
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

static bool read_full(int fd, void *buf, size_t n)
{
    uint8_t *p = buf;
    int      stalls = 0;

    while (n > 0) {
        const ssize_t r = recv(fd, p, n, 0);
        if (r > 0) {
            p += r;
            n -= (size_t)r;
            stalls = 0;
            continue;
        }
        if (r == 0) {
            return false;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* Part-way through a message: wait, but not forever. */
            if (!s_run || ++stalls > READ_STALL_LIMIT) {
                return false;
            }
            continue;
        }
        return false;
    }
    return true;
}

static bool write_full(int fd, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    int            stalls = 0;

    while (n > 0) {
        const ssize_t w = send(fd, p, n, 0);
        if (w > 0) {
            p += w;
            n -= (size_t)w;
            stalls = 0;
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            /* The peer stopped draining. Retry while it is merely slow. */
            if (!s_run || ++stalls > STALL_LIMIT) {
                return false;
            }
            continue;
        }
        return false;
    }
    return true;
}

/* An encode buffer that flushes itself, so a rectangle is a handful of sends
 * rather than one per row. */
typedef struct {
    int      fd;
    uint8_t *buf;
    size_t   cap, len;
    bool     bad;
} sink_t;

static void sink_flush(sink_t *s)
{
    if (s->len > 0) {
        if (!write_full(s->fd, s->buf, s->len)) {
            s->bad = true;
        }
        s->len = 0;
    }
}

static void sink_write(sink_t *s, const void *data, size_t n)
{
    const uint8_t *p = data;

    while (n > 0 && !s->bad) {
        if (s->len == s->cap) {
            sink_flush(s);
        }
        const size_t room = s->cap - s->len;
        const size_t take = n < room ? n : room;
        memcpy(s->buf + s->len, p, take);
        s->len += take;
        p += take;
        n -= take;
    }
}

/* ------------------------------------------------------------------ */
/* Pixel format                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t  bpp, depth, big_endian, true_color;
    uint16_t rmax, gmax, bmax;
    uint8_t  rshift, gshift, bshift;
} rfb_pf_t;

/* What the canvas is, and what a client gets if it never asks. */
static rfb_pf_t pf_default(void)
{
    return (rfb_pf_t){
        .bpp = 16, .depth = 16, .big_endian = 0, .true_color = 1,
        .rmax = 31, .gmax = 63, .bmax = 31,
        .rshift = 11, .gshift = 5, .bshift = 0,
    };
}

static void pf_read(rfb_pf_t *pf, const uint8_t *b)
{
    pf->bpp        = b[0];
    pf->depth      = b[1];
    pf->big_endian = b[2];
    pf->true_color = b[3];
    pf->rmax       = rd16(b + 4);
    pf->gmax       = rd16(b + 6);
    pf->bmax       = rd16(b + 8);
    pf->rshift     = b[10];
    pf->gshift     = b[11];
    pf->bshift     = b[12];
}

static void pf_write(uint8_t *b, const rfb_pf_t *pf)
{
    b[0]  = pf->bpp;
    b[1]  = pf->depth;
    b[2]  = pf->big_endian;
    b[3]  = pf->true_color;
    wr16(b + 4, pf->rmax);
    wr16(b + 6, pf->gmax);
    wr16(b + 8, pf->bmax);
    b[10] = pf->rshift;
    b[11] = pf->gshift;
    b[12] = pf->bshift;
    b[13] = b[14] = b[15] = 0;
}

/*
 * One row of RGB565 into the client's format.
 *
 * The 32-bit true-colour case is the one every desktop client asks for, so it
 * gets a straight 5-6-5 to 8-8-8 expansion with no division; everything else
 * goes through the general scale. This loop is the CPU work PPA SRM is meant
 * to take over, and the reason to keep it obvious.
 */
static void row_to_pf(const espix_px_t *src, uint8_t *dst, int n, const rfb_pf_t *pf)
{
    const int bytes = pf->bpp / 8;

    /*
     * Already in the canvas format: copy instead of converting. This only pays
     * for a client that accepts the format ServerInit advertised and never
     * sends SetPixelFormat. Both viewers tested here override it -- TigerVNC
     * asks for 32bpp and RealVNC on Android for 8bpp -- so the win today is a
     * client that does not, not the ones we have. It is here because it is the
     * cheapest correct thing to do before the hardware path replaces this.
     */
    if (pf->bpp == 16 && pf->true_color && !pf->big_endian &&
        pf->rmax == 31 && pf->gmax == 63 && pf->bmax == 31 &&
        pf->rshift == 11 && pf->gshift == 5 && pf->bshift == 0) {
        memcpy(dst, src, (size_t)n * sizeof(espix_px_t));
        return;
    }

    const bool full = (pf->rmax == 255 && pf->gmax == 255 && pf->bmax == 255);

    for (int i = 0; i < n; i++) {
        const uint32_t r5 = (uint32_t)((src[i] >> 11) & 0x1F);
        const uint32_t g6 = (uint32_t)((src[i] >> 5) & 0x3F);
        const uint32_t b5 = (uint32_t)(src[i] & 0x1F);

        uint32_t r, g, b;
        if (full) {
            r = (r5 << 3) | (r5 >> 2);
            g = (g6 << 2) | (g6 >> 4);
            b = (b5 << 3) | (b5 >> 2);
        } else {
            const uint32_t r8 = (r5 << 3) | (r5 >> 2);
            const uint32_t g8 = (g6 << 2) | (g6 >> 4);
            const uint32_t b8 = (b5 << 3) | (b5 >> 2);
            r = pf->rmax ? (r8 * pf->rmax + 127) / 255 : 0;
            g = pf->gmax ? (g8 * pf->gmax + 127) / 255 : 0;
            b = pf->bmax ? (b8 * pf->bmax + 127) / 255 : 0;
        }

        const uint32_t v = (r << pf->rshift) | (g << pf->gshift) | (b << pf->bshift);
        uint8_t       *d = dst + (size_t)i * bytes;

        switch (bytes) {
        case 1:
            d[0] = (uint8_t)v;
            break;
        case 2:
            if (pf->big_endian) { d[0] = (uint8_t)(v >> 8); d[1] = (uint8_t)v; }
            else                { d[0] = (uint8_t)v; d[1] = (uint8_t)(v >> 8); }
            break;
        default:
            if (pf->big_endian) {
                d[0] = (uint8_t)(v >> 24); d[1] = (uint8_t)(v >> 16);
                d[2] = (uint8_t)(v >> 8);  d[3] = (uint8_t)v;
            } else {
                d[0] = (uint8_t)v;         d[1] = (uint8_t)(v >> 8);
                d[2] = (uint8_t)(v >> 16); d[3] = (uint8_t)(v >> 24);
            }
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Encoders                                                            */
/* ------------------------------------------------------------------ */

static void enc_raw(sink_t *s, const espix_px_t *px, int stride, espix_rect_t r,
                    const rfb_pf_t *pf, uint8_t *row)
{
    const size_t rb = (size_t)r.w * (pf->bpp / 8);

    for (int y = 0; y < r.h && !s->bad; y++) {
        row_to_pf(px + (size_t)(r.y + y) * stride + r.x, row, r.w, pf);
        sink_write(s, row, rb);
    }
}

/*
 * Hextile, in its simplest correct form: a tile is either one colour or it is
 * raw. The subrect machinery -- foreground colours, per-subrect runs -- would
 * shrink a window full of text a little; a uniform tile, which is what a
 * desktop mostly is, is already down from 512 bytes to five.
 *
 * The background is specified on every uniform tile rather than remembered
 * across tiles. One byte and one pixel per tile buys not having to reason
 * about what a client believes the previous tile left behind.
 */
/*
 * `r` without the part of it inside `hole`, as at most four rectangles.
 *
 * This is what a copy needs: the pixels a client is told to move are *not* also
 * sent, so every damaged rectangle that ran under the moved one has to come back
 * as the parts that did not.
 */
static size_t rect_subtract(espix_rect_t r, espix_rect_t hole, espix_rect_t *out)
{
    if (hole.w <= 0 || hole.h <= 0 ||
        hole.x >= r.x + r.w || hole.x + hole.w <= r.x ||
        hole.y >= r.y + r.h || hole.y + hole.h <= r.y) {
        out[0] = r;
        return 1;
    }

    size_t n = 0;

    if (hole.y > r.y) {                     /* above */
        out[n++] = (espix_rect_t){ r.x, r.y, r.w, hole.y - r.y };
    }
    if (hole.y + hole.h < r.y + r.h) {       /* below */
        out[n++] = (espix_rect_t){ r.x, hole.y + hole.h, r.w,
                                   r.y + r.h - (hole.y + hole.h) };
    }

    const int y0 = hole.y > r.y ? hole.y : r.y;
    const int y1 = (hole.y + hole.h) < (r.y + r.h) ? (hole.y + hole.h)
                                                   : (r.y + r.h);

    if (hole.x > r.x) {                     /* left */
        out[n++] = (espix_rect_t){ r.x, y0, hole.x - r.x, y1 - y0 };
    }
    if (hole.x + hole.w < r.x + r.w) {       /* right */
        out[n++] = (espix_rect_t){ hole.x + hole.w, y0,
                                   r.x + r.w - (hole.x + hole.w), y1 - y0 };
    }
    return n;
}

static void enc_hextile(sink_t *s, const espix_px_t *px, int stride, espix_rect_t r,
                        const rfb_pf_t *pf, uint8_t *row)
{
    const int bytes = pf->bpp / 8;

    for (int ty = 0; ty < r.h && !s->bad; ty += 16) {
        const int th = (r.h - ty) < 16 ? (r.h - ty) : 16;

        for (int tx = 0; tx < r.w && !s->bad; tx += 16) {
            const int tw = (r.w - tx) < 16 ? (r.w - tx) : 16;
            const espix_px_t *base = px + (size_t)(r.y + ty) * stride + (r.x + tx);

            const espix_px_t first = base[0];
            bool uniform = true;
            for (int y = 0; y < th && uniform; y++) {
                const espix_px_t *p = base + (size_t)y * stride;
                for (int x = 0; x < tw; x++) {
                    if (p[x] != first) {
                        uniform = false;
                        break;
                    }
                }
            }

            if (uniform) {
                uint8_t mask = 0x02;            /* BackgroundSpecified */
                uint8_t bg[4];
                sink_write(s, &mask, 1);
                row_to_pf(&first, bg, 1, pf);
                sink_write(s, bg, (size_t)bytes);
                continue;
            }

            uint8_t mask = 0x01;                /* Raw */
            sink_write(s, &mask, 1);
            for (int y = 0; y < th; y++) {
                row_to_pf(base + (size_t)y * stride, row, tw, pf);
                sink_write(s, row, (size_t)tw * bytes);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Connection state                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    int       fd;
    rfb_pf_t  pf;
    bool      hextile;
    bool      pending;        /* an update is owed to the client */
    bool      pending_full;   /* ...and it asked for a whole frame */
    bool      copyrect;       /* ...and it can be told to move pixels itself */
    bool      logged_req;     /* the first request is worth one line */
    bool      logged_send;    /* and so is the first update */
    espix_px_t *stage;        /* canvas copy, so encoding is off the lock */
    uint8_t    *out;
    uint8_t    *row;
} rfb_conn_t;

/*
 * What a frame costs to put on the wire, reported every hundred.
 *
 * The twin of the desktop's input counter, and the two together answer the
 * question a slow drag asks: is it the drawing or the encoding and sending.
 */
static struct {
    uint32_t n;
    uint64_t us;
    uint64_t rects;
    int64_t  worst;
} s_send_stat;

static void send_stat(int64_t us, size_t rects)
{
    s_send_stat.n++;
    s_send_stat.us += (uint64_t)us;
    s_send_stat.rects += rects;
    if (us > s_send_stat.worst) {
        s_send_stat.worst = us;
    }
    if (s_send_stat.n < 100) {
        return;
    }
    espix_klog(ESPIX_KLOG_INFO, TAG,
               "send: 100 updates, mean %lld us, worst %lld us, mean %lld rects",
               (long long)(s_send_stat.us / 100), (long long)s_send_stat.worst,
               (long long)((s_send_stat.rects + 50) / 100));
    s_send_stat.n     = 0;
    s_send_stat.us    = 0;
    s_send_stat.rects = 0;
    s_send_stat.worst = 0;
}

static bool update_send(rfb_conn_t *c)
{
    const int64_t  t0 = esp_timer_get_time();
    espix_canvas_t *cv = espix_display_canvas();
    if (cv == NULL || c->stage == NULL) {
        return false;
    }

    const int cw = espix_canvas_width(cv);
    const int ch = espix_canvas_height(cv);

    espix_rect_t rects[ESPIX_DISPLAY_DAMAGE_MAX];
    espix_rect_t keep[ESPIX_DISPLAY_DAMAGE_MAX];
    espix_move_t moves[ESPIX_DISPLAY_MOVE_MAX];
    const bool   full = c->pending_full;
    size_t       nmove = 0;
    size_t       n;

    espix_canvas_lock(cv);
    if (full) {
        rects[0] = (espix_rect_t){ 0, 0, cw, ch };
        n = 1;
        espix_canvas_damage_clear(cv);
        espix_canvas_move_clear(cv);        /* a whole frame needs no copies */
    } else {
        nmove = espix_canvas_move_take(cv, moves, ESPIX_DISPLAY_MOVE_MAX);
        n     = espix_canvas_damage_take(cv, rects, ESPIX_DISPLAY_DAMAGE_MAX);
    }

    if (n == 0 && nmove == 0) {
        espix_canvas_unlock(cv);
        return true;                        /* nothing changed; stay owed */
    }

    /*
     * One move, and only one.
     *
     * A copy is an instruction about the client's framebuffer as it is *now*, so
     * a second one in the same update would depend on the first having been
     * applied -- which it has, in order, but the pixel updates in between would
     * too, and reasoning about that is how a protocol bug gets written. A drag
     * sends a frame per motion anyway, so one is all a drag needs; anything else
     * goes as pixels, which is always right.
     */
    bool copy = c->copyrect && !full && nmove == 1;

    /*
     * And the moved pixels come out of the damage, because sending both would
     * send the same rectangle twice -- once as a copy and once as itself.
     */
    if (copy) {
        size_t k = 0;

        for (size_t i = 0; i < n; i++) {
            espix_rect_t parts[4];
            const size_t np = rect_subtract(rects[i], moves[0].r, parts);

            for (size_t j = 0; j < np; j++) {
                if (k >= ESPIX_DISPLAY_DAMAGE_MAX) {
                    copy = false;           /* no room: send everything */
                    break;
                }
                keep[k++] = parts[j];
            }
            if (!copy) {
                break;
            }
        }
        if (copy) {
            memcpy(rects, keep, k * sizeof(keep[0]));
            n = k;
        }
    }

    /*
     * Copy out under the lock, encode outside it. Holding it across the send
     * would freeze the cursor for as long as the client takes to read a frame,
     * which on a slow link is the difference between a desktop and a slideshow.
     */
    const espix_px_t *px = espix_canvas_pixels(cv);
    for (size_t i = 0; i < n; i++) {
        const espix_rect_t r = rects[i];
        for (int y = 0; y < r.h; y++) {
            memcpy(c->stage + (size_t)(r.y + y) * cw + r.x,
                   px + (size_t)(r.y + y) * cw + r.x,
                   (size_t)r.w * sizeof(espix_px_t));
        }
    }
    espix_canvas_unlock(cv);

    if (!c->logged_send) {
        c->logged_send = true;
        espix_klog(ESPIX_KLOG_INFO, TAG, "sending %dx%d at %u bpp, %s",
                   cw, ch, c->pf.bpp, c->hextile ? "hextile" : "raw");
    }

    const size_t total = n + (copy ? 1 : 0);

    uint8_t hdr[4];
    hdr[0] = 0;                             /* FramebufferUpdate */
    hdr[1] = 0;
    wr16(hdr + 2, (uint16_t)total);
    if (!write_full(c->fd, hdr, 4)) {
        return false;
    }

    sink_t s = { .fd = c->fd, .buf = c->out, .cap = OUT_CAP };

    if (copy) {
        /* Twelve bytes of header and four of payload, against a hundred
         * kilobytes of pixels: this is the whole of why a drag is affordable. */
        uint8_t ch[16];
        wr16(ch + 0, (uint16_t)moves[0].r.x);
        wr16(ch + 2, (uint16_t)moves[0].r.y);
        wr16(ch + 4, (uint16_t)moves[0].r.w);
        wr16(ch + 6, (uint16_t)moves[0].r.h);
        wr32(ch + 8, RFB_ENC_COPYRECT);
        wr16(ch + 12, (uint16_t)moves[0].sx);
        wr16(ch + 14, (uint16_t)moves[0].sy);
        sink_write(&s, ch, sizeof(ch));
    }

    for (size_t i = 0; i < n; i++) {
        const espix_rect_t r = rects[i];
        uint8_t            rh[12];
        wr16(rh + 0, (uint16_t)r.x);
        wr16(rh + 2, (uint16_t)r.y);
        wr16(rh + 4, (uint16_t)r.w);
        wr16(rh + 6, (uint16_t)r.h);
        wr32(rh + 8, c->hextile ? RFB_ENC_HEXTILE : RFB_ENC_RAW);
        sink_write(&s, rh, sizeof(rh));

        if (c->hextile) {
            enc_hextile(&s, c->stage, cw, r, &c->pf, c->row);
        } else {
            enc_raw(&s, c->stage, cw, r, &c->pf, c->row);
        }
        if (s.bad) {
            return false;
        }
    }
    sink_flush(&s);
    if (s.bad) {
        return false;
    }

    c->pending = false;
    c->pending_full = false;
    send_stat(esp_timer_get_time() - t0, n + (copy ? 1 : 0));
    return true;
}

static bool rfb_handle(rfb_conn_t *c, uint8_t type)
{
    switch (type) {
    case RFB_SET_PIXEL_FORMAT: {
        uint8_t buf[19];                    /* 3 pad + 16 */
        if (!read_full(c->fd, buf, sizeof(buf))) {
            return false;
        }
        rfb_pf_t pf;
        pf_read(&pf, buf + 3);

        if (pf.bpp != 8 && pf.bpp != 16 && pf.bpp != 32) {
            espix_klog(ESPIX_KLOG_WARN, TAG, "ignoring %u bpp pixel format", pf.bpp);
            return true;
        }
        if (!pf.true_color) {
            espix_klog(ESPIX_KLOG_WARN, TAG,
                       "colour-map client refused; staying true colour");
            return true;
        }
        c->pf = pf;
        espix_klog(ESPIX_KLOG_DEBUG, TAG,
                   "pixel format %u bpp depth %u max %u/%u/%u shift %u/%u/%u",
                   pf.bpp, pf.depth, pf.rmax, pf.gmax, pf.bmax,
                   pf.rshift, pf.gshift, pf.bshift);
        return true;
    }

    case RFB_SET_ENCODINGS: {
        uint8_t buf[3];                     /* 1 pad + u16 count */
        if (!read_full(c->fd, buf, sizeof(buf))) {
            return false;
        }
        const uint16_t n = rd16(buf + 1);
        bool           hextile  = false;
        bool           copyrect = false;

        for (uint16_t i = 0; i < n; i++) {
            uint8_t e[4];
            if (!read_full(c->fd, e, sizeof(e))) {
                return false;
            }
            if (rd32(e) == RFB_ENC_HEXTILE) {
                hextile = true;
            } else if (rd32(e) == RFB_ENC_COPYRECT) {
                copyrect = true;
            }
        }
        c->hextile  = hextile;
        c->copyrect = copyrect;
        snprintf(s_encodings, sizeof(s_encodings), "%u offered: hextile %s, "
                 "copyrect %s", n, hextile ? "yes" : "no",
                 copyrect ? "yes" : "no");
        /*
         * At INFO rather than DEBUG, and it is the line that decides whether a
         * drag is 8 KB or 300 KB: CopyRect is a *client* capability, and a
         * client that does not ask for it gets pixels and no way to tell from
         * the outside which it is.
         */
        espix_klog(ESPIX_KLOG_INFO, TAG, "client offered %u encodings: hextile "
                   "%s, copyrect %s (%s)", n, hextile ? "yes" : "no",
                   copyrect ? "yes" : "no",
                   copyrect ? "drags will be copies" : "drags will be pixels");
        return true;
    }

    case RFB_FB_UPDATE_REQUEST: {
        uint8_t buf[9];                     /* incremental + x,y,w,h */
        if (!read_full(c->fd, buf, sizeof(buf))) {
            return false;
        }
        /*
         * Logged once, because a viewer asks again after every update and this
         * would otherwise be the loudest line in dmesg. It is the one fact that
         * settles an argument about who got the framebuffer size wrong.
         */
        if (!c->logged_req) {
            c->logged_req = true;
            espix_klog(ESPIX_KLOG_INFO, TAG,
                       "client asks for %ux%u at (%u,%u)%s",
                       (unsigned)rd16(buf + 5), (unsigned)rd16(buf + 7),
                       (unsigned)rd16(buf + 1), (unsigned)rd16(buf + 3),
                       buf[0] ? " incremental" : " full");
        }
        c->pending_full = (buf[0] == 0);
        c->pending = true;
        return update_send(c);
    }

    case RFB_KEY_EVENT: {
        uint8_t buf[7];                     /* down + 2 pad + u32 keysym */
        if (!read_full(c->fd, buf, sizeof(buf))) {
            return false;
        }
        const espix_input_event_t ev = {
            .kind = ESPIX_INPUT_KEY,
            .down = buf[0] != 0,
            .keysym = rd32(buf + 3),
        };
        espix_display_input(&ev);
        /* The desktop task outranks this one, so the damage it just made is
         * already there -- no need to wait for the poll to notice. */
        return c->pending ? update_send(c) : true;
    }

    case RFB_POINTER_EVENT: {
        uint8_t buf[5];                     /* button mask + x + y */
        if (!read_full(c->fd, buf, sizeof(buf))) {
            return false;
        }
        /*
         * RFB coordinates are unsigned and the event's are signed, because a
         * local mouse reports a delta. A peer that names a point past the
         * canvas is clamped by the desktop, which is where that belongs.
         */
        const espix_input_event_t ev = {
            .kind    = ESPIX_INPUT_POINTER,
            .buttons = buf[0],
            .x       = (int16_t)rd16(buf + 1),
            .y       = (int16_t)rd16(buf + 3),
        };
        espix_display_input(&ev);
        return c->pending ? update_send(c) : true;
    }

    case RFB_CLIENT_CUT_TEXT: {
        uint8_t buf[7];                     /* 3 pad + u32 length */
        if (!read_full(c->fd, buf, sizeof(buf))) {
            return false;
        }
        const uint32_t len = rd32(buf + 3);
        for (uint32_t i = 0; i < len; i++) {
            uint8_t b;
            if (!read_full(c->fd, &b, 1)) {
                return false;
            }
        }
        return true;                        /* no clipboard yet */
    }

    default:
        espix_klog(ESPIX_KLOG_WARN, TAG, "unknown client message %u; closing", type);
        return false;
    }
}

/*
 * VNC authentication (RFB security type 2).
 *
 * The whole scheme is one DES challenge: sixteen random bytes out, the client's
 * encryption of them back, and the server performs the same encryption with the
 * password it already knows and compares. Nothing after it is protected and DES
 * is weak by any modern measure -- it is a password check and nothing more.
 *
 * It is here because it is the only thing macOS Screen Sharing will accept. It
 * insists on a password, and with security type None it never proceeds to
 * ClientInit: it sits waiting for a challenge until it gives up, which is
 * exactly the "handshake failed at reading ClientInit ... after 10247 ms" that
 * sent us looking.
 */

/* The key is the password's first eight bytes, each byte's *bits reversed*. The
 * reversal is a VNC quirk older than any of this and is not optional. */
static void vnc_key(const char *pw, uint8_t key[8])
{
    memset(key, 0, 8);

    for (int i = 0; i < 8 && pw[i] != '\0'; i++) {
        uint8_t b = (uint8_t)pw[i];
        b = (uint8_t)(((b & 0xF0u) >> 4) | ((b & 0x0Fu) << 4));
        b = (uint8_t)(((b & 0xCCu) >> 2) | ((b & 0x33u) << 2));
        b = (uint8_t)(((b & 0xAAu) >> 1) | ((b & 0x55u) << 1));
        key[i] = b;
    }
}

/* The key is eight bytes and that is the whole secret; 0600 because it is a
 * password equivalent, weak though it is. */
static bool vnc_key_save(const uint8_t key[8])
{
    const int fd = open(VNC_KEY_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        return false;
    }
    const ssize_t n = write(fd, key, 8);
    close(fd);
    return n == 8;
}

static bool vnc_key_load(uint8_t key[8])
{
    const int fd = open(VNC_KEY_PATH, O_RDONLY);
    if (fd < 0) {
        return false;
    }
    const ssize_t n = read(fd, key, 8);
    close(fd);
    return n == 8;
}

static bool vnc_auth(int fd, const uint8_t key[8])
{
    uint8_t challenge[16], response[16], expect[16];

    esp_fill_random(challenge, sizeof(challenge));
    if (!write_full(fd, challenge, sizeof(challenge))) {
        return false;
    }
    if (!read_full(fd, response, sizeof(response))) {
        return false;
    }

    espix_vnc_des_encrypt(key, challenge, expect);
    espix_vnc_des_encrypt(key, challenge + 8, expect + 8);

    /* Accumulated rather than memcmp: stopping at the first difference would
     * leak how much of a guess was right. */
    uint8_t diff = 0;
    for (size_t i = 0; i < sizeof(response); i++) {
        diff |= (uint8_t)(response[i] ^ expect[i]);
    }
    return diff == 0;
}

static bool rfb_handshake(rfb_conn_t *c, const char **why)
{
#define FAIL(w) do { *why = (w); return false; } while (0)

    static const char version[] = "RFB 003.008\n";
    const int         fd = c->fd;

    if (!write_full(fd, version, 12)) {
        FAIL("writing our version");
    }

    char v[12];
    if (!read_full(fd, v, 12)) {
        FAIL("reading the client's version");
    }
    if (memcmp(v, "RFB ", 4) != 0) {
        FAIL("the client's version string");
    }

    const int major = (v[4] - '0') * 100 + (v[5] - '0') * 10 + (v[6] - '0');
    const int minor = (v[8] - '0') * 100 + (v[9] - '0') * 10 + (v[10] - '0');
    if (major != 3 || (minor != 3 && minor < 7)) {
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "RFB %d.%d unsupported (need 3.3, or 3.7 and up)", major, minor);
        FAIL("the version it offered");
    }

    /*
     * 3.3 and 3.7 differ in the security handshake and nowhere that matters
     * here. In 3.3 the server *dictates* the type as one u32; from 3.7 the
     * server offers a list, the client chooses, and the choice is confirmed
     * with a SecurityResult.
     *
     * 3.3 is worth the branch because macOS's built-in Screen Sharing still
     * opens with it -- refusing it is what produced "RFB 3.3 unsupported".
     * Hextile and CopyRect exist in 3.3, so nothing is lost by supporting it.
     *
     * Which type: None unless a password has been set, in which case VNC
     * authentication is the *only* type offered. Offering both would let a
     * client sidestep the password, which is not what setting one means.
     */
    const bool want_auth = s_have_key;

    if (minor == 3) {
        /*
         * 3.3 gives the server one choice and no negotiation, so a client that
         * cannot manage without a password gets one and a client that can gets
         * None. The 3.3 client here in practice is macOS Screen Sharing, which
         * requires the password.
         */
        uint8_t sec[4];
        wr32(sec, want_auth ? 2u : 1u);
        if (!write_full(fd, sec, sizeof(sec))) {
            FAIL("writing the 3.3 security type");
        }
        espix_klog(ESPIX_KLOG_INFO, TAG, "client speaks RFB 3.3, %s",
                   want_auth ? "VNC authentication" : "no authentication");

        if (want_auth) {
            if (!vnc_auth(fd, s_vnc_key)) {
                uint8_t bad[4];
                wr32(bad, 1);
                (void)write_full(fd, bad, sizeof(bad));
                FAIL("VNC authentication");
            }

            /*
             * Authentication is confirmed even in 3.3. Appendix A.1 carves out
             * only the None case -- "if the security-type is 1 ... the server
             * does not send the SecurityResult message" -- which implies the
             * other types do, and the main body has SecurityResult follow the
             * security handshake by default.
             *
             * Reading that carve-out as "3.3 sends nothing on success" cost a
             * round trip: with the exchange correct but unconfirmed, macOS
             * Screen Sharing waited exactly where it had before, for a message
             * that was never coming, and gave up after ten seconds with
             * "handshake failed at reading ClientInit".
             */
            const uint32_t ok = 0;          /* SecurityResult: OK */
            if (!write_full(fd, &ok, 4)) {
                FAIL("writing SecurityResult");
            }
        }
    } else {
        /*
         * From 3.7 there is a list, and both types are offered when a key
         * exists -- None first. A viewer happy with no password (TigerVNC,
         * RealVNC) keeps working without one, and one that insists on
         * authenticating can take the second entry.
         *
         * The consequence is worth being blunt about: while both are offered
         * the password is a compatibility mechanism, not a security boundary.
         * A client that can choose will choose None.
         */
        const uint8_t list[2] = { 1, 2 };
        const uint8_t n = want_auth ? 2 : 1;
        if (!write_full(fd, &n, 1) || !write_full(fd, list, n)) {
            FAIL("offering security types");
        }

        uint8_t chosen;
        if (!read_full(fd, &chosen, 1)) {
            FAIL("reading the chosen security type");
        }
        if (chosen != 1 && chosen != 2) {
            uint32_t len;                   /* the client's reason length */
            (void)read_full(fd, &len, 4);
            espix_klog(ESPIX_KLOG_WARN, TAG, "client chose security type %u",
                       chosen);
            FAIL("the client's security choice");
        }
        if (chosen == 2) {
            if (!want_auth || !vnc_auth(fd, s_vnc_key)) {
                const uint32_t bad = 1;
                (void)write_full(fd, &bad, 4);
                FAIL("VNC authentication");
            }
        }

        /* From 3.7 the handshake is confirmed either way. */
        const uint32_t ok = 0;              /* SecurityResult: OK */
        if (!write_full(fd, &ok, 4)) {
            FAIL("writing SecurityResult");
        }
        espix_klog(ESPIX_KLOG_INFO, TAG, "client speaks RFB 3.%d, security %u",
                   minor, chosen);
    }

    uint8_t shared;                         /* ClientInit */
    if (!read_full(fd, &shared, 1)) {
        FAIL("reading ClientInit");
    }

    espix_canvas_t *cv = espix_display_canvas();
    if (cv == NULL) {
        FAIL("no canvas");
    }

    static const char name[] = "espix";
    uint8_t           si[24];
    wr16(si + 0, (uint16_t)espix_canvas_width(cv));
    wr16(si + 2, (uint16_t)espix_canvas_height(cv));
    pf_write(si + 4, &c->pf);
    wr32(si + 20, (uint32_t)(sizeof(name) - 1));

    if (!write_full(fd, si, sizeof(si))) {
        FAIL("writing ServerInit");
    }
    if (!write_full(fd, name, sizeof(name) - 1)) {
        FAIL("writing the desktop name");
    }
    return true;

#undef FAIL
}

static void rfb_serve(int fd)
{
    espix_canvas_t *cv = espix_display_canvas();
    if (cv == NULL) {
        return;
    }

    const int cw = espix_canvas_width(cv);
    const int ch = espix_canvas_height(cv);

    rfb_conn_t c;
    memset(&c, 0, sizeof(c));
    c.fd = fd;
    c.pf = pf_default();

    c.stage = heap_caps_malloc((size_t)cw * ch * sizeof(espix_px_t),
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    c.out = heap_caps_malloc(OUT_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    c.row = heap_caps_malloc((size_t)cw * 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (c.out == NULL) {
        c.out = heap_caps_malloc(OUT_CAP, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (c.row == NULL) {
        c.row = heap_caps_malloc((size_t)cw * 4, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }

    if (c.stage == NULL || c.out == NULL || c.row == NULL) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "no memory for a connection");
        heap_caps_free(c.stage);
        heap_caps_free(c.out);
        heap_caps_free(c.row);
        return;
    }

    /*
     * Timed, because it is what tells the two failures apart: a client that
     * hung up fails in no time at all, while one that is still waiting for a
     * message we did not send sits here until the stall limit expires. Same
     * line, opposite diagnoses.
     */
    const char   *why  = "unknown";
    const int64_t t_hs = esp_timer_get_time();
    if (!rfb_handshake(&c, &why)) {
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "handshake failed at %s with %s after %lld ms",
                   why, s_peer, (long long)((esp_timer_get_time() - t_hs) / 1000));
        goto done;
    }

    /* A client that has seen nothing is owed a whole frame; whatever damage
     * predates it would be a confusing partial first paint. */
    espix_canvas_lock(cv);
    espix_canvas_damage_clear(cv);
    espix_canvas_unlock(cv);

    s_clients = 1;
    espix_klog(ESPIX_KLOG_INFO, TAG, "client connected from %s", s_peer);
    latency_want(true);

    /*
     * A viewer with nothing on the screen gets the console, and only then: a
     * desktop that is already running is what it should see. Created here and
     * torn down below, so a board with nobody watching allocates none of it.
     */
    espix_display_viewer_attached();

    while (s_run) {
        uint8_t type;

        /*
         * Owed an update, poll at the tick so a change another task made goes
         * out promptly. Not owed one, sleep on the socket: there is nothing to
         * send until the client asks, so waking early would only burn cycles.
         */
        set_recv_timeout(fd, c.pending ? TICK_US : POLL_US);

        const int rc = recv_byte(fd, &type);
        if (rc <= 0) {
            if (rc == -1) {
                /* Nothing arrived: the tick that lets a late update go out. */
                if (c.pending && !update_send(&c)) {
                    break;
                }
                continue;
            }
            break;
        }
        if (!rfb_handle(&c, type)) {
            break;
        }
    }

    s_clients = 0;
    s_peer[0] = '\0';
    espix_display_viewer_detached();
    espix_klog(ESPIX_KLOG_INFO, TAG, "client disconnected");
    latency_want(false);

done:
    heap_caps_free(c.stage);
    heap_caps_free(c.out);
    heap_caps_free(c.row);
}

/* ------------------------------------------------------------------ */
/* Listener                                                            */
/* ------------------------------------------------------------------ */

static void rfb_task(void *arg)
{
    (void)arg;
    /* DEBUG, not INFO: 'vnc start' already reports this on the console, and the
     * kernel log echoes, so INFO here printed the same line twice. */
    espix_klog(ESPIX_KLOG_DEBUG, TAG, "listening on port %u", s_port);

    while (s_run) {
        struct sockaddr_in peer;
        socklen_t          plen = sizeof(peer);

        const int fd = accept(s_listen_fd, (struct sockaddr *)&peer, &plen);
        if (fd < 0) {
            /* SO_RCVTIMEO makes this the normal idle path, not an error. */
            continue;
        }

        /* Interactive: coalescing pointer moves makes the cursor feel broken. */
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        const struct timeval io = { .tv_sec = 0, .tv_usec = POLL_US };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &io, sizeof(io));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &io, sizeof(io));

        snprintf(s_peer, sizeof(s_peer), "%s:%u",
                 inet_ntoa(peer.sin_addr), (unsigned)ntohs(peer.sin_port));

        rfb_serve(fd);
        close(fd);
    }

    close(s_listen_fd);
    s_listen_fd = -1;
    s_task = NULL;
    vTaskDeleteWithCaps(NULL);              /* frees the PSRAM stack it was given */
}

/*
 * Settle on a key, once per boot: one an earlier `vnc password` saved, or the
 * built-in default. A command that has already chosen wins -- otherwise
 * `vnc nopassword` would be undone by the next `vnc start`.
 */
static void vnc_auth_begin(void)
{
    if (s_auth_decided) {
        return;
    }
    s_auth_decided = true;

    if (vnc_key_load(s_vnc_key)) {
        s_have_key = true;
        s_key_default = false;
        espix_klog(ESPIX_KLOG_INFO, TAG, "VNC key loaded from %s", VNC_KEY_PATH);
        return;
    }

    vnc_key(ESPIX_DISPLAY_VNC_DEFAULT_PASSWORD, s_vnc_key);
    s_have_key = true;
    s_key_default = true;
    espix_klog(ESPIX_KLOG_WARN, TAG,
               "no %s, so the built-in default VNC password is in use; "
               "change it with 'vnc password <pw>'",
               VNC_KEY_PATH);
}

esp_err_t espix_display_rfb_listen(uint16_t port)
{
    if (s_run) {
        return s_port == port ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

    vnc_auth_begin();

    /* The socket is opened here rather than in the task so that "port already
     * in use" is an answer the command can report, not a log line later. */
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return ESP_FAIL;
    }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "bind port %u failed: errno %d", port, errno);
        close(fd);
        return ESP_FAIL;
    }
    if (listen(fd, LISTEN_BACKLOG) < 0) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "listen failed: errno %d", errno);
        close(fd);
        return ESP_FAIL;
    }

    const struct timeval tv = { .tv_sec = 0, .tv_usec = 500 * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    s_listen_fd = fd;
    s_port = port;
    s_run = true;

    if (xTaskCreateWithCaps(rfb_task, "espix:vnc", 8192, NULL, 4, &s_task,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        (void)xTaskCreateWithCaps(rfb_task, "espix:vnc", 8192, NULL, 4, &s_task,
                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (s_task == NULL) {
        s_run = false;
        close(fd);
        s_listen_fd = -1;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void espix_display_rfb_stop(void)
{
    if (!s_run) {
        return;
    }

    s_run = false;

    /*
     * Wait for the task to clear its own handle. It notices within one accept
     * or recv timeout, and deletes itself with the matching vTaskDeleteWithCaps
     * -- deleting it from here would leak the PSRAM stack it was given.
     */
    for (int i = 0; i < 300 && s_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    s_port = 0;
}

void espix_display_vnc_set_password(const char *pw)
{
    /* Chosen by hand: vnc_auth_begin() must not second-guess it. */
    s_auth_decided = true;

    if (pw == NULL) {
        s_have_key = false;
        s_key_default = false;
        memset(s_vnc_key, 0, sizeof(s_vnc_key));
        (void)remove(VNC_KEY_PATH);
        return;
    }

    vnc_key(pw, s_vnc_key);
    s_have_key = true;
    s_key_default = false;

    if (!vnc_key_save(s_vnc_key)) {
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "cannot write %s (errno %d); the password will be forgotten "
                   "at the next boot", VNC_KEY_PATH, errno);
    }
}

bool espix_display_vnc_auth_required(void) { return s_have_key; }

bool espix_display_vnc_auth_is_default(void) { return s_have_key && s_key_default; }

bool     espix_display_vnc_running(void) { return s_run; }
uint16_t espix_display_vnc_port(void)    { return s_port; }
int      espix_display_vnc_clients(void) { return s_clients; }
const char *espix_display_vnc_peer(void) { return s_peer; }

/*
 * What the last client said it can decode, as a short list.
 *
 * Worth a command rather than only a log line, because it is the answer to
 * "why is dragging still slow": CopyRect is the client's capability to declare,
 * and a server that is not allowed to use it can only fall back to pixels.
 */
const char *espix_display_vnc_encodings(void) { return s_encodings; }
