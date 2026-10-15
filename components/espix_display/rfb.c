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

#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_vfs_eventfd.h"

#include "espix_display.h"
#include "espix_kernel.h"
#include "espix_pm.h"
#include "espix_task.h"
#include "espix_net.h"

#include "vnc_des.h"

/*
 * SystemView user markers, for the profile build only.
 *
 * The per-hundred-update counters say what canvas/encode/wire cost in total;
 * these put the same three phases on the timeline, so a slow drag shows which
 * one it was waiting in and in what order, not just how long. Read them in
 * SystemView as User Start/Stop with these ids. Compiled out unless tracing is
 * on, so a release links none of it -- and then the id constants are unused,
 * which is why they only exist in this branch.
 */
#ifdef CONFIG_ESP_TRACE_ENABLE
#include "SEGGER_SYSVIEW.h"
#define RFB_MARK_CANVAS 0
#define RFB_MARK_ENCODE 1
#define RFB_MARK_WIRE   2
#define RFB_MARK_COPY   3
#define RFB_MARK_PIXELS 4
#define RFB_MARK_START(id) SEGGER_SYSVIEW_OnUserStart(id)
#define RFB_MARK_STOP(id)  SEGGER_SYSVIEW_OnUserStop(id)

/*
 * One line per update, so packet size and which path it took are in the trace
 * rather than only in the counters. Formatted on the target: over a plain USB
 * stream the SystemView application cannot read this image's memory to resolve
 * a host-side format string.
 */
#define RFB_TRACE_PACKET(copy, moves, full, cap, rects, bytes, flushes) \
    SEGGER_SYSVIEW_PrintfTarget( \
        "rfb copy=%d moves=%u full=%d cap=%d rects=%u bytes=%u flushes=%u", \
        (copy), (unsigned)(moves), (full), (cap), (unsigned)(rects), \
        (unsigned)(bytes), (unsigned)(flushes))
#else
#define RFB_MARK_START(id) ((void)0)
#define RFB_MARK_STOP(id)  ((void)0)
#define RFB_TRACE_PACKET(copy, moves, full, cap, rects, bytes, flushes) ((void)0)
#endif

#define TAG "vnc"

/* More than one: with a single pending connection and nobody accepting it
 * yet, the stack will not complete another handshake, so a second viewer sits
 * at "connecting" rather than queueing -- which looks like the network. */
#define LISTEN_BACKLOG 4
#define POLL_US        (250 * 1000)   /* send and accept timeout */
#define STALL_LIMIT    40             /* * POLL_US before a peer is gone */

/* Thirty seconds of a client asking for nothing, reported once. An idle viewer
 * is legitimately quiet, so this is a log line and not a disconnect: what it
 * answers is whether "unreachable board" is a client that never asks again. */
#define DEFER_LIMIT    40             /* quarter-seconds a frame may stay owed */

/* Answered and then silent for this long means the client is gone; see the
 * liveness test in the serve loop, which is only sound because an update is
 * always sent in reply. */
#define LIVENESS_US    (30 * 1000 * 1000)

/* Ten seconds of a peer stalled part-way through a message, at POLL_US a try. */
#define READ_STALL_LIMIT STALL_LIMIT
#define OUT_CAP        (64 * 1024)    /* encode buffer, flushed to the socket */

/* The PPA's cache-line requirement, for the staging buffers it may be asked to
 * write into. Same number as display.c's BUF_ALIGN, and for the same reason. */
#define RFB_ALIGN      128

/* VNC uses the first eight characters of a password and ignores the rest. */
#define VNC_PW_MAX 8

/*
 * Listener state. Declared up here because the socket helpers below consult
 * s_run: a stop request has to be able to break a read or a write that is
 * already waiting on a peer.
 */
static volatile bool s_run;
static int           s_listen_fd = -1;
static espix_task_exit_t s_exit;
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
/*
 * This used to hold the mode itself, save and restore it, and know about
 * espix_wifi_ps_t -- which worked exactly once, for exactly one holder. It is
 * now a request: the count is the facility's problem, the setting is the
 * network's, and this file no longer needs to know what either one does. A
 * viewer is one hold, and the last viewer to leave is what puts the radio back.
 */
static void latency_want(bool on)
{
    if (on) {
        (void)espix_pm_acquire(ESPIX_PM_LATENCY);
    } else {
        (void)espix_pm_release(ESPIX_PM_LATENCY);
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
    /*
     * An extension, and the *client* asking us to resize. RealVNC's viewer
     * sends it when its window changes size ("RemoteResize", on by default).
     * It is not one we implement, but it is one we must not choke on: the
     * default case below closes the connection, and a window drag is not a
     * reason to disconnect somebody.
     */
    RFB_SET_DESKTOP_SIZE     = 251,
};

/* Encodings. Raw is mandatory; Hextile is the one worth having. */
enum {
    RFB_ENC_RAW      = 0,
    RFB_ENC_COPYRECT = 1,
    RFB_ENC_HEXTILE  = 5,
    RFB_ENC_TIGHT    = 7,
};

/*
 * Tight is the only encoding in the base protocol that can carry a lossy
 * rectangle: zlib for the pixel data, a palette or a solid fill for the cheap
 * cases, and JPEG for a photograph. A client that offers it and also offers a
 * quality level is saying it will take a lossy update; one that offers Tight
 * and no level is asking for the lossless half only, and sending it JPEG anyway
 * is a protocol violation.
 *
 * The quality levels are pseudo-encodings -32 (level 0, lossless) to -23
 * (level 9), which is TightVNC's numbering and what TigerVNC and noVNC both
 * send. The subencoding bytes inside a Tight rectangle were taken from noVNC's
 * decoder rather than from memory: the control byte is (subencoding << 4) |
 * zlib-reset bits, 0x9 is JPEG, 0x8 is fill, and the JPEG payload is preceded
 * by a compact length written 7 bits at a time.
 */
#define RFB_ENC_TIGHT_Q_MAX ((int32_t)-23)   /* level 9: the loosest */
#define RFB_ENC_TIGHT_Q_MIN ((int32_t)-32)   /* level 0: lossless */

/*
 * ExtendedDesktopSize is a pseudo-encoding: a negative 32-bit number. It is
 * what lets a client be told the screen changed size without reconnecting, and
 * the only way a resize is live on the same connection.
 *
 * The value is TigerVNC's (common/rfb/encodings.h: -308). An earlier guess of
 * -16703 was self-consistent with the test client and wrong against every real
 * viewer, which is the failure this constant must not have.
 */
#define RFB_ENC_EXTENDED_DESKTOP_SIZE  ((uint32_t)-308)

/*
 * And the one it replaced. DesktopSize (-223) was TightVNC's, is still what
 * RealVNC offers, and -- measured on the viewer here -- RealVNC does *not*
 * offer ExtendedDesktopSize. So a RealVNC client is resized this way or not at
 * all: one rectangle whose width and height are the new size and which carries
 * no payload, against the screen list above.
 *
 * The payload is the part worth being careful about. This sends none, because
 * the size is in the rectangle header; if a client reads it as four bytes of
 * payload it will desync on the next message and that is visible immediately,
 * which is why the server also logs what a client offered.
 */
#define RFB_ENC_DESKTOP_SIZE           ((uint32_t)-223)

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

/* The serve loop sleeps on the socket and this eventfd; damage pokes it while an update is owed. */
static int           s_wake_fd = -1;
static volatile bool s_wake_armed;

static void rfb_damage_hook(void)
{
    if (!s_wake_armed || s_wake_fd < 0) {
        return;
    }
    s_wake_armed = false;

    const uint64_t one = 1;
    (void)write(s_wake_fd, &one, sizeof(one));
}

static void wake_open(void)
{
    if (s_wake_fd >= 0) {
        return;
    }
    s_wake_fd = eventfd(0, 0);
    if (s_wake_fd < 0) {
        const esp_vfs_eventfd_config_t ev = ESP_VFS_EVENTD_CONFIG_DEFAULT();
        if (esp_vfs_eventfd_register(&ev) == ESP_OK) {
            s_wake_fd = eventfd(0, 0);
        }
    }
    if (s_wake_fd < 0) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "no eventfd: updates owed are polled");
    }
}

static void wake_drain(void)
{
    uint64_t n;

    if (s_wake_fd >= 0) {
        (void)read(s_wake_fd, &n, sizeof(n));
    }
}

/* Wait for the client, the canvas, or room for a deferred frame; 1 when the socket is readable. */
#define WAIT_POLL_US (10 * 1000)        /* only without an eventfd */

static int conn_wait(int fd, bool owed, bool want_write)
{
    fd_set r, w;
    int    maxfd = fd;

    FD_ZERO(&r);
    FD_ZERO(&w);
    FD_SET(fd, &r);
    if (want_write) {
        FD_SET(fd, &w);                 /* a deferred frame waits for room */
    }
    if (owed && s_wake_fd >= 0) {
        FD_SET(s_wake_fd, &r);
        if (s_wake_fd > maxfd) {
            maxfd = s_wake_fd;
        }
    }

    const int      us = (owed && s_wake_fd < 0) ? WAIT_POLL_US : POLL_US;
    struct timeval tv = { .tv_sec = 0, .tv_usec = us };

    if (select(maxfd + 1, &r, want_write ? &w : NULL, NULL, &tv) <= 0) {
        return 0;
    }
    if (s_wake_fd >= 0 && FD_ISSET(s_wake_fd, &r)) {
        wake_drain();
    }
    return FD_ISSET(fd, &r) ? 1 : 0;
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

/*
 * The queue, not the cost.
 *
 * A drag that is smooth and then chugs, and finishes seconds after the mouse
 * stops, is a backlog -- events arriving faster than updates leave -- and cost
 * per motion does not say whether one is growing. These do: input events in,
 * updates out, and how often the socket made us wait. If in outruns out for as
 * long as the drag lasts, the backlog is ours and the fix is to make a motion
 * cheap; if they keep pace and it still feels slow, the queue is somewhere else
 * and no amount of server-side work will find it.
 *
 * The socket is non-blocking, so "made us wait" is a spin on EAGAIN rather than
 * a sleep: it burns the input path's time either way, and counting it is the
 * difference between knowing and guessing.
 */
static struct {
    uint32_t updates;
    uint32_t stalls;
    uint64_t waited_us;
    int64_t  since;
} s_q;

/* Where one update's time goes, averaged over a hundred of them. */
static struct {
    uint32_t n;
    uint64_t canvas_us;
    uint64_t encode_us;
    uint64_t write_us;
    /* The canvas phase, split: waiting for it, taking the damage, deciding and
     * packing a JPEG, and staging everything else. Asked because it is the
     * largest phase and the split says which part of it to aim at. */
    uint64_t lock_us;
    uint64_t pick_us;
    uint64_t cls_us;
    uint64_t copy_us;
    /* ...and what the staging copy was carrying: the copy is the largest item
     * in the phase, and its size is the question. */
    uint32_t stage_rects;
    uint64_t stage_px;
    uint32_t stage_big_px;
    uint16_t stage_big_w;
    uint16_t stage_big_h;
} s_phase;

/*
 * What the encoder does with those microseconds, tile by tile.
 *
 * The phase split above says "encode" and stops there, which is enough to know
 * the encoder is the ceiling and not enough to know what to do about it. This
 * says how much of it is the RRE analysis -- the part that can be abandoned
 * early -- against the pixel conversion that every tile pays whatever encoding
 * wins, and how many tiles each outcome took.
 *
 * Counted in CPU cycles, not read from the timer: this is inside the loop it
 * measures, and two esp_timer_get_time() calls a tile were themselves a
 * measurable part of the number they reported. The cycle count is a register
 * read, and the division into microseconds happens once, when the line prints.
 *
 * "runs" is the raw material the subrect list is made from, against the
 * "subrects" that survived the vertical extension -- the one number that says
 * whether that extension is earning its keep on this content.
 */
static struct {
    uint64_t analyse_cyc;   /* colour runs and subrect matching */
    uint64_t emit_cyc;      /* converting pixels and writing them out */
    uint32_t tiles;
    uint32_t flat;          /* one colour: a background byte, done */
    uint32_t raw;           /* sent as pixels */
    uint32_t sub;           /* sent as subrects */
    uint32_t runs;          /* colour runs the tiles were made of */
    uint32_t subrects;      /* ...and the rectangles those became */
} s_enc;

/* Cycles to microseconds, for the line below and nowhere else. */
static uint64_t cyc_to_us(uint64_t cycles)
{
    const uint32_t per_us = esp_rom_get_cpu_ticks_per_us();

    return per_us ? cycles / per_us : 0;
}

/*
 * Start a frame, or do not start it at all.
 *
 * This is the whole of the decoupling, and it is four bytes' worth. Input and
 * output are one task, so a send that sits in the socket stops the server
 * reading the client's events -- and a drag then arrives as a queue that plays
 * out seconds after the mouse stops. Measured: 152 ms mean and 1.9 s worst
 * inside the send, against 150 motions a second coming in.
 *
 * The first write of a frame is the gate, so it is attempted without waiting. If
 * it cannot go out, the frame is abandoned *before any of it has been written*
 * and left owed; the next attempt carries whatever is true then. That is the
 * right answer for a screen as well as for the socket: an intermediate position
 * nobody will ever see is worth less than the input that waiting for it would
 * have blocked, so a slow link should get fewer, newer frames rather than a
 * queue of old ones.
 *
 * Once it has started, though, the frame has to be finished -- a client handed
 * half a message is a client that has lost the protocol -- so the rest blocks as
 * it always did.
 */
static bool write_full(int fd, const void *buf, size_t n);

/*
 * Start a frame: 1 written, 0 would block (nothing written), -1 gone.
 *
 * The distinction is the whole of it. This used to answer true or false, and
 * the caller treated false as "not now, still owed" -- right for a peer that is
 * merely slow, and catastrophic for one that has gone away, because the frame
 * is then deferred and retried for ever: an update every quarter of a second
 * against a dead socket, this loop never reading and never accepting, and no
 * log line, because each attempt looks exactly like the last. Measured: a
 * viewer that stopped asking after its own sixty-second timeout, a server that
 * said nothing at all, and a board that could not be reached until it was
 * restarted.
 */
static int write_start(int fd, const void *buf, size_t n)
{
    const ssize_t w = send(fd, buf, n, MSG_DONTWAIT);

    if (w == (ssize_t)n) {
        return 1;                       /* done, and nothing was waited for */
    }
    if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return 0;                       /* nothing written: try again later */
    }
    if (w < 0) {
        return -1;                      /* the connection is not usable */
    }
    return write_full(fd, (const uint8_t *)buf + w, n - (size_t)w) ? 1 : -1;
}

/*
 * Wait until the socket will take at least one byte, or the deadline passes.
 *
 * Why not the socket's own send timeout: it is set -- SO_SNDTIMEO, a quarter of
 * a second, and the setsockopt is checked now -- and it is not honoured on this
 * path. Measured: a client that asked for a full frame and then stopped reading
 * left send() inside the stack for thirty-two seconds and counting, so the retry
 * limit below never saw the EAGAIN that it counts. select() asks the question
 * the timeout was supposed to answer, and it sleeps rather than spins: a healthy
 * peer is writable immediately, so this is one extra syscall per send.
 */
static bool writable_within(int fd, int us)
{
    fd_set         w;
    struct timeval tv = { .tv_sec = 0, .tv_usec = us };

    FD_ZERO(&w);
    FD_SET(fd, &w);

    return select(fd + 1, NULL, &w, NULL, &tv) > 0;
}

static bool write_full(int fd, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    int            stalls = 0;
    const int64_t  t0 = esp_timer_get_time();

    while (n > 0) {
        const ssize_t w = send(fd, p, n, 0);
        if (w > 0) {
            p += w;
            n -= (size_t)w;
            if (stalls != 0) {
                s_q.stalls += (uint32_t)stalls;
                stalls = 0;
            }
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            /*
             * The peer stopped draining. Wait for it rather than spin -- but
             * only here, on the refusal, and never before a send that is going
             * to work: asking first cost four times the throughput of this
             * whole path when it was tried, because a healthy socket pays the
             * question and does not need it. Retry while the peer is merely
             * slow, and not for longer than the write may take: this task also
             * accepts the next client.
             */
            if (!writable_within(fd, POLL_US) || !s_run
                || ++stalls > STALL_LIMIT) {
                s_q.stalls += (uint32_t)stalls;
                return false;
            }
            continue;
        }
        return false;
    }
    if (stalls != 0) {
        s_q.stalls += (uint32_t)stalls;
    }
    if (s_q.stalls != 0) {
        s_q.waited_us += (uint64_t)(esp_timer_get_time() - t0);
    }
    return true;
}

/*
 * What the serving task is doing, so that a task which stops making progress
 * says where it stopped.
 *
 * Written by that task and read by a timer of its own, because the one thing a
 * wedged task cannot do is report on itself. This exists because "the client
 * cannot reconnect until the board is restarted" reproduced five times and left
 * no trace whatsoever: the last line in the log was always whatever happened
 * before the stall, and the stall is invisible by definition. It reports only
 * the states that are meant to be brief -- a handshake, handling a request,
 * sending an update -- so a quiet client sitting on an idle connection (which
 * is normal) says nothing.
 */
enum {
    VNC_IDLE, VNC_ACCEPT, VNC_HANDSHAKE, VNC_WAIT, VNC_HANDLING,
    VNC_CANVAS, VNC_SENDING,
};

static volatile int       s_state = VNC_IDLE;
static volatile int64_t   s_state_at;
static esp_timer_handle_t s_state_timer;

static const char *const s_state_name[] = {
    "idle", "waiting to accept", "in the handshake", "waiting for a request",
    "handling a request", "waiting for the canvas", "sending an update",
};

static void vnc_state(int st)
{
    s_state    = st;
    s_state_at = esp_timer_get_time();
}

/* Every five seconds, if the task has been in a state that should be brief for
 * more than two, say so and how long. */
static void vnc_state_tick(void *arg)
{
    (void)arg;

    const int64_t stuck = esp_timer_get_time() - s_state_at;

    if (s_state != VNC_HANDSHAKE && s_state != VNC_HANDLING
        && s_state != VNC_CANVAS && s_state != VNC_SENDING) {
        return;
    }
    if (stuck < 2000000) {
        return;
    }
    if (s_state == VNC_CANVAS) {
        espix_canvas_t *cv = espix_display_canvas();
        void           *h  = (cv != NULL) ? espix_canvas_holder(cv) : NULL;

        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "stuck: %s for %lld ms, held by %s (client %s)",
                   s_state_name[s_state], (long long)(stuck / 1000),
                   h != NULL ? pcTaskGetName((TaskHandle_t)h) : "nobody",
                   s_peer[0] != '\0' ? s_peer : "gone");
        return;
    }

    espix_klog(ESPIX_KLOG_WARN, TAG, "stuck: %s for %lld ms (client %s)",
               s_state_name[s_state], (long long)(stuck / 1000),
               s_peer[0] != '\0' ? s_peer : "gone");
}

/* An encode buffer that flushes itself, so a rectangle is a handful of sends
 * rather than one per row. */
typedef struct {
    int      fd;
    uint8_t *buf;
    size_t   cap, len;
    bool     bad;
#ifdef CONFIG_ESP_TRACE_ENABLE
    uint32_t bytes;                 /* trace only: what the update put on the wire */
    uint32_t flushes;
#endif
} sink_t;

/* Which part of an update costs: the canvas copy, the encoding, or the wire.
 * Accumulated rather than reasoned about, because three rounds of reasoning
 * about it produced three different answers. */
static uint64_t s_write_us;

static void sink_flush(sink_t *s)
{
    if (s->len > 0) {
        const int64_t t = esp_timer_get_time();

        RFB_MARK_START(RFB_MARK_WIRE);
        if (!write_full(s->fd, s->buf, s->len)) {
            s->bad = true;
        }
        RFB_MARK_STOP(RFB_MARK_WIRE);
        s_write_us += (uint64_t)(esp_timer_get_time() - t);
#ifdef CONFIG_ESP_TRACE_ENABLE
        s->bytes += (uint32_t)s->len;
        s->flushes++;
#endif
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

/*
 * A contiguous run of the output buffer, reserved rather than appended to a few
 * bytes at a time. The caller writes exactly n bytes and commits them.
 *
 * NULL when the sink is dead or the run cannot fit in the buffer at all, which
 * is the caller's cue to encode the rectangle some other way.
 */
static uint8_t *sink_gap(sink_t *s, size_t n)
{
    if (s->bad || n > s->cap) {
        return NULL;
    }
    if (s->cap - s->len < n) {
        sink_flush(s);
        if (s->bad) {
            return NULL;
        }
    }
    return s->buf + s->len;
}

static void sink_commit(sink_t *s, uint8_t *end)
{
    s->len = (size_t)(end - s->buf);
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
/*
 * One pixel in the client's format, returning how many bytes it took.
 *
 * Split out of the loop below because the hextile emitter converts a colour per
 * subrect -- thousands of times an update -- and a call into row_to_pf() for a
 * single pixel is worth avoiding. One implementation, so the two cannot drift.
 */
static int pix_to_pf(espix_px_t px, uint8_t *dst, const rfb_pf_t *pf)
{
    const int      bytes = pf->bpp / 8;
    const uint32_t r5 = (uint32_t)((px >> 11) & 0x1F);
    const uint32_t g6 = (uint32_t)((px >> 5) & 0x3F);
    const uint32_t b5 = (uint32_t)(px & 0x1F);

    uint32_t r, g, b;
    if (pf->rmax == 255 && pf->gmax == 255 && pf->bmax == 255) {
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

    if (bytes == 1) {
        dst[0] = (uint8_t)v;
    } else if (bytes == 2) {
        if (pf->big_endian) {
            dst[0] = (uint8_t)(v >> 8); dst[1] = (uint8_t)v;
        } else {
            dst[0] = (uint8_t)v;        dst[1] = (uint8_t)(v >> 8);
        }
    } else if (pf->big_endian) {
        dst[0] = (uint8_t)(v >> 24); dst[1] = (uint8_t)(v >> 16);
        dst[2] = (uint8_t)(v >> 8);  dst[3] = (uint8_t)v;
    } else {
        dst[0] = (uint8_t)v;         dst[1] = (uint8_t)(v >> 8);
        dst[2] = (uint8_t)(v >> 16); dst[3] = (uint8_t)(v >> 24);
    }
    return bytes;
}

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

    /*
     * 32bpp, 8-8-8, red at bit 16: what every desktop client asks for, and the
     * one loop where the general path below is measurably slow. It reloads the
     * format out of *pf on every pixel, because nothing tells the compiler the
     * format and the output cannot overlap. Hoisted here, with the 5-6-5
     * expansion written out, and the four bytes stored from one word.
     */
    if (bytes == 4 && pf->true_color && !pf->big_endian &&
        pf->rmax == 255 && pf->gmax == 255 && pf->bmax == 255 &&
        pf->rshift == 16 && pf->gshift == 8 && pf->bshift == 0) {
        for (int i = 0; i < n; i++) {
            const uint32_t px = src[i];
            const uint32_t r5 = (px >> 11) & 0x1F;
            const uint32_t g6 = (px >> 5) & 0x3F;
            const uint32_t b5 = px & 0x1F;
            const uint32_t v  = (((r5 << 3) | (r5 >> 2)) << 16) |
                                (((g6 << 2) | (g6 >> 4)) << 8) |
                                ((b5 << 3) | (b5 >> 2));
            uint8_t *d = dst + (size_t)i * 4;
            d[0] = (uint8_t)v;
            d[1] = (uint8_t)(v >> 8);
            d[2] = (uint8_t)(v >> 16);
            d[3] = 0;
        }
        return;
    }

    for (int i = 0; i < n; i++) {
        (void)pix_to_pf(src[i], dst + (size_t)i * bytes, pf);
    }
}

/* ------------------------------------------------------------------ */
/* Encoders                                                            */
/* ------------------------------------------------------------------ */

static void enc_raw(sink_t *s, const espix_px_t *px, int stride, espix_rect_t r,
                    const rfb_pf_t *pf)
{
    const size_t rb = (size_t)r.w * (pf->bpp / 8);

    /*
     * The row is converted straight into the buffer it is sent from. It used to
     * convert into the connection's row scratch and then memcpy it here, and on
     * a whole-canvas raw frame that was 300 KiB of PSRAM traffic a frame spent
     * moving bytes the encoder had just written.
     */
    for (int y = 0; y < r.h && !s->bad; y++) {
        uint8_t *o = sink_gap(s, rb);
        if (o == NULL) {
            break;
        }
        row_to_pf(px + (size_t)(r.y + y) * stride + r.x, o, r.w, pf);
        sink_commit(s, o + rb);
    }
}

/* The overlap of two rectangles; empty when they do not meet. */
static espix_rect_t rect_meet(espix_rect_t a, espix_rect_t b)
{
    const int x0 = a.x > b.x ? a.x : b.x;
    const int y0 = a.y > b.y ? a.y : b.y;
    const int x1 = (a.x + a.w) < (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    const int y1 = (a.y + a.h) < (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);

    if (x1 <= x0 || y1 <= y0) {
        return (espix_rect_t){ 0, 0, 0, 0 };
    }
    return (espix_rect_t){ x0, y0, x1 - x0, y1 - y0 };
}

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

/*
 * Hextile, with the subrect form.
 *
 * A tile that is not one colour is offered as a background plus a list of
 * subrectangles (RFC 6143 7.7.4) and falls back to Raw when that would be
 * larger. The background is the tile's top-left pixel: for the flat chrome and
 * text a desktop is made of, that is the majority colour, and where it is not
 * -- a photograph -- the subrects come out larger than Raw and Raw is what is
 * sent.
 *
 * Background is specified on every tile, as it always was: one pixel per tile
 * buys not having to reason about what the client believes the previous tile
 * left behind. The foreground is specified only for the monochrome form.
 */

/* One subrect, packed as it goes on the wire. */
typedef struct {
    uint8_t    xy;      /* x << 4 | y */
    uint8_t    wh;      /* (w - 1) << 4 | (h - 1) */
    espix_px_t px;
} subrect_t;

#define SUBRECT_MAX 255

static void enc_hextile_tile(sink_t *s, const espix_px_t *base, int stride,
                             int tw, int th, const rfb_pf_t *pf, uint8_t *row)
{
    const int bytes = pf->bpp / 8;
    const espix_px_t bg = base[0];

    const uint32_t t_analyse = esp_cpu_get_cycle_count();

    bool uniform = true;
    for (int y = 0; y < th && uniform; y++) {
        const espix_px_t *p = base + (size_t)y * stride;
        for (int x = 0; x < tw; x++) {
            if (p[x] != bg) {
                uniform = false;
                break;
            }
        }
    }

    if (uniform) {
        const uint8_t mask = 0x02;              /* BackgroundSpecified */
        uint8_t       pix[4];
        sink_write(s, &mask, 1);
        row_to_pf(&bg, pix, 1, pf);
        sink_write(s, pix, (size_t)bytes);

        s_enc.analyse_cyc += (uint32_t)(esp_cpu_get_cycle_count() - t_analyse);
        s_enc.tiles++;
        s_enc.flat++;
        return;
    }

    /*
     * Split each row into maximal runs of one colour, then extend a rectangle
     * from the row above when a run shares its x, width and colour. That is
     * the RRE decomposition Hextile is a variation on, and it turns a text row
     * into a handful of subrects instead of a whole tile of pixels.
     *
     * Every rectangle in the next row corresponds to one run in that row
     * (either an extended one or a new one), so nnext <= nspan <= 16 and the
     * scratch arrays are one tile wide at most.
     */
    subrect_t  closed[SUBRECT_MAX];
    subrect_t  active[16];
    size_t     nclosed = 0;
    size_t     nactive = 0;
    bool       overflow = false;

    for (int y = 0; y < th; y++) {
        const espix_px_t *p = base + (size_t)y * stride;

        uint8_t    span_xy[16], span_wh[16];
        espix_px_t span_px[16];
        int        nspan = 0;

        /*
         * Which span starts at each x.
         *
         * A run starts where the last one ended, so at most one span per x, and
         * a subrect can only continue into a span that starts at the subrect's
         * own x. That makes the matching below an index lookup rather than a
         * search -- it used to compare every active subrect against all sixteen
         * spans on every row, 256 comparisons of three conditions for a tile
         * whose rows have nothing in common.
         *
         * "span_have" is a bit per x rather than a sentinel in the array: the
         * array only ever gets written where a span starts, and clearing
         * sixteen entries on every row was the same work as scanning the row
         * that filled them.
         */
        int8_t   span_at[16];
        uint16_t span_have = 0;

        for (int x = 0; x < tw; ) {
            const espix_px_t c = p[x];
            int w = 1;
            while (x + w < tw && p[x + w] == c) {
                w++;
            }
            if (c != bg && nspan < 16) {
                span_xy[nspan]   = (uint8_t)((x << 4) | y);
                span_wh[nspan]   = (uint8_t)((w - 1) << 4);
                span_px[nspan]   = c;
                span_at[x]       = (int8_t)nspan;
                span_have       |= (uint16_t)1 << x;
                nspan++;
            }
            x += w;
        }
        s_enc.runs += (uint32_t)nspan;

        /* The same trick for the spans this row's subrects consumed. */
        uint16_t  used = 0;
        subrect_t next[16];
        size_t    nnext = 0;

        for (size_t i = 0; i < nactive; i++) {
            subrect_t a    = active[i];
            bool      grew = false;

            const int sx = (a.xy & 0xF0) >> 4;
            const int j  = (span_have & ((uint16_t)1 << sx)) ? span_at[sx] : -1;

            /* The span that starts where this subrect does, of the same width
             * (the high nibble of wh is the height) and the same colour. */
            if (j >= 0 && !(used & ((uint16_t)1 << j)) &&
                (span_wh[j] & 0xF0) == (a.wh & 0xF0) && span_px[j] == a.px) {
                used         |= (uint16_t)1 << j;
                a.wh          = (uint8_t)(a.wh + 1);    /* h += 1 */
                next[nnext++] = a;
                grew          = true;
            }

            if (!grew) {
                if (nclosed >= SUBRECT_MAX) {
                    overflow = true;
                    break;
                }
                closed[nclosed++] = a;
            }
        }
        if (overflow) {
            break;
        }

        for (int j = 0; j < nspan; j++) {
            if (!(used & ((uint16_t)1 << j))) {
                next[nnext++] = (subrect_t){ span_xy[j], span_wh[j], span_px[j] };
            }
        }

        for (size_t i = 0; i < nnext; i++) {
            active[i] = next[i];
        }
        nactive = nnext;
    }

    if (!overflow) {
        for (size_t i = 0; i < nactive; i++) {
            if (nclosed >= SUBRECT_MAX) {
                overflow = true;
                break;
            }
            closed[nclosed++] = active[i];
        }
    }

    /* Analysis over; from here it is pixels or subrects. */
    s_enc.analyse_cyc += (uint32_t)(esp_cpu_get_cycle_count() - t_analyse);
    const uint32_t t_emit = esp_cpu_get_cycle_count();

    const size_t raw_size = 1 + (size_t)tw * th * bytes;

    if (!overflow && nclosed > 0) {
        bool mono = true;
        for (size_t i = 1; i < nclosed; i++) {
            if (closed[i].px != closed[0].px) {
                mono = false;
                break;
            }
        }

        /* 1 mask + background + [foreground] + 1 count + the subrects. */
        const size_t sub_size = mono
            ? 1 + (size_t)2 * bytes + 1 + 2 * nclosed
            : 1 + (size_t)bytes + 1 + (size_t)(bytes + 2) * nclosed;

        if (sub_size < raw_size) {
            const uint8_t mask = mono ? (uint8_t)(0x02 | 0x04 | 0x08)
                                      : (uint8_t)(0x02 | 0x10 | 0x08);

            /*
             * Reserved once, then stored.
             *
             * This was three sink_write() calls per subrect, and each of those
             * ended in a memcpy() whose length is the caller's -- a variable
             * length, so a real call rather than an inlined move. Measured, the
             * emission cost more than the analysis that produced it. The size is
             * known here, so the buffer is taken once and the bytes are put
             * where they go.
             */
            uint8_t *o = sink_gap(s, sub_size);
            if (o != NULL) {
                *o++ = mask;
                o += pix_to_pf(bg, o, pf);
                if (mono) {
                    o += pix_to_pf(closed[0].px, o, pf);
                }
                *o++ = (uint8_t)nclosed;

                for (size_t i = 0; i < nclosed; i++) {
                    if (!mono) {
                        o += pix_to_pf(closed[i].px, o, pf);
                    }
                    *o++ = closed[i].xy;
                    *o++ = closed[i].wh;
                }
                sink_commit(s, o);

                s_enc.subrects += (uint32_t)nclosed;
                s_enc.emit_cyc += (uint32_t)(esp_cpu_get_cycle_count() - t_emit);
                s_enc.tiles++;
                s_enc.sub++;
                return;
            }
            /* No room for the subrect encoding: send it as pixels instead. */
        }
    }

    const uint8_t mask = 0x01;                  /* Raw */
    sink_write(s, &mask, 1);
    for (int y = 0; y < th; y++) {
        row_to_pf(base + (size_t)y * stride, row, tw, pf);
        sink_write(s, row, (size_t)tw * bytes);
    }

    s_enc.emit_cyc += (uint32_t)(esp_cpu_get_cycle_count() - t_emit);
    s_enc.tiles++;
    s_enc.raw++;
}

static void enc_hextile(sink_t *s, const espix_px_t *px, int stride, espix_rect_t r,
                        const rfb_pf_t *pf, uint8_t *row)
{
    for (int ty = 0; ty < r.h && !s->bad; ty += 16) {
        const int th = (r.h - ty) < 16 ? (r.h - ty) : 16;

        for (int tx = 0; tx < r.w && !s->bad; tx += 16) {
            const int tw = (r.w - tx) < 16 ? (r.w - tx) : 16;
            enc_hextile_tile(s, px + (size_t)(r.y + ty) * stride + (r.x + tx),
                             stride, tw, th, pf, row);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Tight, with JPEG                                                    */
/* ------------------------------------------------------------------ */

/*
 * The JPEG quality each of Tight's ten levels asks for.
 *
 * Level 0 means lossless and never reaches here; 1-9 is the client saying how
 * much quality it will give up for bytes. TightVNC's own mapping, so a client
 * that picks a level gets what it asked for.
 */
static int tight_quality(int level)
{
    static const int q[10] = { 5, 10, 15, 25, 37, 50, 60, 70, 80, 90 };

    if (level < 1) { level = 1; }
    if (level > 9) { level = 9; }
    return q[level];
}

/* Where a JPEG goes between the codec and the socket buffer. */
static uint8_t *s_tight_out;
static size_t   s_tight_cap;

/* Room for a JPEG of a w x h rectangle; false when there is none. */
static bool tight_room(espix_rect_t r)
{
    const size_t want = (size_t)r.w * (size_t)r.h * sizeof(espix_px_t) + 4096;

    if (s_tight_out == NULL || s_tight_cap < want) {
        uint8_t *n = heap_caps_malloc(want, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

        if (n == NULL) {
            return false;
        }
        heap_caps_free(s_tight_out);
        s_tight_out = n;
        s_tight_cap = want;
    }
    return true;
}

/* A JPEG of whatever the encoder's input holds, or 0 when it will not encode. */
static size_t tight_jpeg(espix_rect_t r, int level)
{
    if (!tight_room(r)) {
        return 0;
    }
    return espix_display_jpeg_encode(r.w, r.h, tight_quality(level),
                                     s_tight_out, s_tight_cap);
}

/*
 * The control byte and the length that precede a Tight rectangle.
 *
 * The control byte carries the subencoding in its high nibble and, in the low
 * one, which of the four zlib streams to reset -- none, for a JPEG. The length
 * follows it seven bits at a time, low group first, a set high bit meaning
 * "another byte follows". Both were taken from noVNC's decoder rather than from
 * memory, and 0x90 for a JPEG is what a real client expects to read.
 */
static void tight_head(sink_t *s, uint8_t ctl, size_t n)
{
    uint8_t hdr[4];
    int     k = 0;

    /*
     * The control byte is the control byte and nothing else. The length is not
     * folded into it -- an earlier version here did exactly that, and a 19 KB
     * JPEG then arrived at the client as subencoding 0xB rather than 0x9
     * because the low bits of the length landed in the nibble that says what
     * the payload is. The length is its own field, and the client read that
     * correctly before this was fixed.
     */
    hdr[k++] = ctl;

    hdr[k++] = (uint8_t)(n & 0x7F);
    if (n > 0x7F) {
        hdr[k - 1] |= 0x80;
        hdr[k++] = (uint8_t)((n >> 7) & 0x7F);
        if (n > 0x3FFF) {
            hdr[k - 1] |= 0x80;
            hdr[k++] = (uint8_t)((n >> 14) & 0xFF);
        }
    }
    sink_write(s, hdr, (size_t)k);
}

/*
 * The rectangle the encoder was handed, put back on the stage at canvas stride.
 *
 * Only for the update that has to fall back: nothing has announced the
 * rectangle as Tight yet, but its pixels are in the encoder's input rather than
 * in the stage, and Hextile reads the stage.
 */
static void tight_restage(espix_rect_t r, espix_px_t *stage, int cw)
{
    const espix_px_t *in = espix_display_jpeg_input((size_t)r.w * (size_t)r.h);

    if (in == NULL) {
        return;
    }
    for (int y = 0; y < r.h; y++) {
        memcpy(stage + (size_t)(r.y + y) * cw + r.x,
               in + (size_t)y * r.w, (size_t)r.w * sizeof(espix_px_t));
    }
}

/* ------------------------------------------------------------------ */
/* Connection state                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    int       fd;
    rfb_pf_t  pf;
    bool      hextile;
    bool      tight;          /* ...and it speaks Tight, lossless or JPEG */
    bool      lossy;          /* ...and it allowed lossy: a quality level >= 1 */
    int       qlevel;         /* the level to use, 0-9; -1 when none was offered */
    bool      pending;        /* an update is owed to the client */
uint32_t  deferred;       /* frames abandoned because the socket was full */
int64_t   last_empty_us;  /* when the client was last answered with nothing */
int64_t   last_rx_us;     /* when anything at all was last heard from it */
    bool      pending_full;   /* ...and it asked for a whole frame */
    bool      copyrect;       /* ...and it can be told to move pixels itself */
    bool      eds;            /* ...and it can be resized without reconnecting */
    bool      desksize;       /* ...or it takes the older DesktopSize instead */
    bool      logged_req;     /* the first request is worth one line */
    bool      logged_send;    /* and so is the first update */
    int       stage_w, stage_h; /* the size stage and row were allocated for */
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

/*
 * How long spent waiting for the socket, in one second, is worth a line. When
 * the encoder is keeping up this is zero; a millisecond is under a frame at any
 * rate this runs at, and above it something is holding the send up.
 */
#define QUEUE_REPORT_US 1000

/* ------------------------------------------------------------------ */
/* Which rectangles are photographs                                    */
/* ------------------------------------------------------------------ */

/*
 * Would this rectangle be better off as a JPEG?
 *
 * Observer only, for now: it decides, counts and says so in the log, and
 * nothing goes on the wire differently. Deciding is cheap; acting on it changes
 * what every client sees, and the thresholds to do that with should come from
 * measurements of the content this desktop actually shows rather than from an
 * opinion about it.
 *
 * Two numbers, because one is not enough. The distinct colour count separates a
 * photograph from a user interface -- a text row, a window frame and a flat
 * panel are a handful of colours, a photograph is thousands. The edge count
 * separates them the other way: palette art and text are few colours *and* hard
 * edges, which is what a JPEG rings on and compresses badly, so the rectangle
 * worth a JPEG is the one that is both colourful and smooth.
 *
 * Sampled on a step that lands on about CLS_SAMPLES points whatever the area,
 * so looking costs the same for a window as for a whole canvas -- well under a
 * millisecond beside the 28 ms an encode takes.
 */
#define CLS_SAMPLES       4096
#define CLS_COLOURS       256
/*
 * "More colours than a desktop uses" -- and the number is 32 because 64 was
 * above the thing it was meant to catch.
 *
 * Measured over RFB: a desktop canvas classifies at 12-15 colours, the browser
 * at 4-8 (text and chrome are a handful of colours), and a *dark* 640x480 video
 * frame at 46-62 -- the video window's own pixels, where a bright frame
 * saturates the count at 256 and a near-black one does not. Sixty-four sent
 * every dark frame to Hextile, which was the largest cost left on the board:
 * the whole 660x530 window staged and copied (350,000 px, 14.5 ms of the 30 ms
 * canvas phase, measured) and Hextile-encoded into roughly six times the bytes,
 * on about two updates in five.
 *
 * Thirty-two sits between the two, with the desktop's 15 and the video's 46
 * as the margin. Palette art -- the case a colour count cannot separate from a
 * photograph, because there is nothing in the count to separate -- is the edge
 * test's job, and stays the edge test's job: a dithered picture has many
 * differing neighbours, and a photograph has few.
 */
#define CLS_JPEG_COLOURS  32        /* "more colours than a desktop uses" */
#define CLS_EDGE_DIFF     32        /* "differs by four or more 5-bit steps" */
#define CLS_RUN           8         /* pixels examined from each sampled point */
#define CLS_JPEG_MIN_PX   4096      /* a JPEG header is not worth a small rect */
#define CLS_JPEG_MAX_EDGE 40        /* per cent of neighbours that differ */

static espix_px_t s_cls_seen[CLS_COLOURS];
static bool       s_cls_used[CLS_COLOURS];

static int cls_diff(espix_px_t a, espix_px_t b)
{
    int dr = (int)((a >> 11) & 0x1F) - (int)((b >> 11) & 0x1F);
    int dg = (int)((a >>  5) & 0x3F) - (int)((b >>  5) & 0x3F);
    int db = (int)( a        & 0x1F) - (int)( b        & 0x1F);

    if (dr < 0) { dr = -dr; }
    if (dg < 0) { dg = -dg; }
    if (db < 0) { db = -db; }

    return dr * 8 + dg * 4 + db * 8;
}

void espix_display_classify(const espix_px_t *px, int stride, int rx, int ry,
                            int rw, int rh, espix_display_class_t *out)
{
    if (out == NULL) {
        return;
    }
    out->colours = 0;
    out->edges   = 0;
    out->pairs   = 0;
    out->jpeg    = false;

    if (px == NULL || rw <= 0 || rh <= 0) {
        return;
    }

    const espix_rect_t r = { rx, ry, rw, rh };
    uint32_t step = 1;

    while (step < 32
           && (uint32_t)(r.w / (int)step) * (uint32_t)(r.h / (int)step) > CLS_SAMPLES) {
        step++;
    }

    memset(s_cls_used, 0, sizeof(s_cls_used));

    uint32_t nseen = 0, edges = 0, pairs = 0;

    /*
     * A sampled point is the start of a short run, not a lone pixel.
     *
     * Comparing a sample against the *next* sample measures content, not detail
     * -- nine pixels away, even a photograph has changed -- and comparing it
     * against the pixel beside it measures detail but only where the sample
     * happens to land on an edge, which on a sparse grid is almost never.
     * Measured: a desktop full of text read as 0% edges that way.
     *
     * So each seed is walked for CLS_RUN pixels: the colours come from all of
     * them, and the edges are the neighbouring pairs among them. Smooth
     * photographic tone changes slowly enough that neighbours usually land on
     * the same RGB565 value; dithering and text do not.
     */
    for (int y = r.y; y < r.y + r.h && nseen < CLS_COLOURS; y += (int)step) {
        const espix_px_t *row = px + (size_t)y * stride;

        for (int x = r.x; x < r.x + r.w; x += (int)step) {
            espix_px_t prev = row[x];

            for (int k = 0; k < CLS_RUN && x + k < r.x + r.w; k++) {
                const espix_px_t p = row[x + k];

                if (nseen < CLS_COLOURS) {
                    uint32_t i = ((uint32_t)p * 2654435761u >> 12) % CLS_COLOURS;

                    for (uint32_t probe = 0; probe < CLS_COLOURS; probe++) {
                        if (!s_cls_used[i]) {
                            s_cls_used[i] = true;
                            s_cls_seen[i] = p;
                            nseen++;
                            break;
                        }
                        if (s_cls_seen[i] == p) {
                            break;
                        }
                        i = (i + 1) % CLS_COLOURS;
                    }
                }

                if (k > 0) {
                    pairs++;
                    if (cls_diff(prev, p) > CLS_EDGE_DIFF) { edges++; }
                }
                prev = p;
            }
        }
    }

    out->colours = nseen;
    out->edges   = edges;
    out->pairs   = pairs;

    const uint32_t area = (uint32_t)r.w * (uint32_t)r.h;

    out->jpeg = area >= CLS_JPEG_MIN_PX
        && nseen >= CLS_JPEG_COLOURS
        && (pairs == 0 || (uint64_t)edges * 100 / pairs <= CLS_JPEG_MAX_EDGE);
}

/* What the classifier has seen since the last line, and the client's answer. */
static struct {
    uint32_t rects;
    uint32_t jpeg;
    uint32_t px_all;
    uint32_t px_jpeg;
    uint64_t colours;
    uint64_t edges;
    uint64_t pairs;
    bool     lossy;
    uint32_t sent_jpeg;     /* rectangles actually put on the wire as Tight */
    uint32_t passed;        /* ...of which the picture's own JPEG, not encoded */
    /* And the largest rectangle, whole, because an average over a hundred
     * rectangles hides the one the question is about. */
    uint32_t big_px;
    uint32_t big_colours;
    uint32_t big_edges;
    uint32_t big_pairs;
    bool     big_jpeg;
} s_cls;

static void classify_note(const rfb_conn_t *c, const espix_px_t *px, int stride,
                          espix_rect_t r)
{
    espix_display_class_t cl;
    espix_display_classify(px, stride, r.x, r.y, r.w, r.h, &cl);

    const uint32_t colours = cl.colours;
    const uint32_t edges   = cl.edges;
    const uint32_t pairs   = cl.pairs;
    const bool     jpeg    = cl.jpeg;
    const uint32_t area    = (uint32_t)r.w * (uint32_t)r.h;

    s_cls.rects++;
    s_cls.px_all  += area;
    s_cls.colours += colours;
    s_cls.edges   += edges;
    s_cls.pairs   += pairs;
    s_cls.lossy    = c->lossy;

    if (jpeg) {
        s_cls.jpeg++;
        s_cls.px_jpeg += area;
    }

    if (area > s_cls.big_px) {
        s_cls.big_px      = area;
        s_cls.big_colours = colours;
        s_cls.big_edges   = edges;
        s_cls.big_pairs   = pairs;
        s_cls.big_jpeg    = jpeg;
    }
}

static void send_stat(int64_t us, size_t rects)
{
    const int64_t now = esp_timer_get_time();

    s_q.updates++;
    if (s_q.since == 0) {
        s_q.since = now;
    }
    if (now - s_q.since >= 1000000) {
        /*
         * Only when the second was not clean.
         *
         * A stall, or time spent waiting for the socket, is something to fix;
         * "17 updates out, 0 stalls, 0 ms waiting" is not. Printed every second
         * it makes the reader work out which numbers are the boring ones, and it
         * takes ring space from the line that does matter. The window is reset
         * either way, so a line that does appear reports one second rather than
         * a total that keeps growing.
         */
        if (s_q.stalls > 0 || s_q.waited_us >= QUEUE_REPORT_US) {
            espix_klog(ESPIX_KLOG_INFO, TAG,
                       "queue: %u updates out, %u socket stalls, %lld ms waiting",
                       (unsigned)s_q.updates, (unsigned)s_q.stalls,
                       (long long)(s_q.waited_us / 1000));
        }
        s_q.updates   = 0;
        s_q.stalls    = 0;
        s_q.waited_us = 0;
        s_q.since     = now;
    }

    s_send_stat.n++;
    s_send_stat.us += (uint64_t)us;
    s_send_stat.rects += rects;
    if (us > s_send_stat.worst) {
        s_send_stat.worst = us;
    }
    if (s_send_stat.n < 100) {
        return;
    }
    const uint32_t u = s_phase.n ? s_phase.n : 1;

    espix_klog(ESPIX_KLOG_INFO, TAG,
               "send: 100 updates, mean %lld us (canvas %lld, encode %lld, "
               "wire %lld), worst %lld, mean %lld rects",
               (long long)(s_send_stat.us / 100),
               (long long)(s_phase.canvas_us / u),
               (long long)(s_phase.encode_us / u),
               (long long)(s_phase.write_us / u),
               (long long)s_send_stat.worst,
               (long long)((s_send_stat.rects + 50) / 100));

    espix_klog(ESPIX_KLOG_INFO, TAG,
               "canvas: of %lld, wait %lld + take %lld + decide-pack %lld "
               "+ stage %lld (of %u rects, %llu px, largest %ux%u)",
               (long long)(s_phase.canvas_us / u),
               (long long)(s_phase.lock_us / u),
               (long long)(s_phase.pick_us / u),
               (long long)(s_phase.cls_us / u),
               (long long)(s_phase.copy_us / u),
               (unsigned)s_phase.stage_rects,
               (unsigned long long)s_phase.stage_px,
               (unsigned)s_phase.stage_big_w, (unsigned)s_phase.stage_big_h);

    const uint32_t tiles = s_enc.tiles ? s_enc.tiles : 1;
    /*
     * The emit counter covers the socket flush it triggered as well, and the
     * phase split above already calls that wire. Taking it out here is what
     * makes the two lines add up: without it, "emit" was reporting a third of
     * the network as encoder time.
     */
    const uint64_t analyse_us = cyc_to_us(s_enc.analyse_cyc);
    const uint64_t emit_all   = cyc_to_us(s_enc.emit_cyc);
    const uint64_t emit_us    = emit_all > s_phase.write_us
                                ? emit_all - s_phase.write_us : 0;

    espix_klog(ESPIX_KLOG_INFO, TAG,
               "enc: %u tiles an update (%u flat, %u raw, %u sub, "
               "%u runs -> %u subrects), %lld us a tile (analyse %lld, emit %lld)",
               (unsigned)(s_enc.tiles / u),
               (unsigned)(s_enc.flat / u), (unsigned)(s_enc.raw / u),
               (unsigned)(s_enc.sub / u),
               (unsigned)(s_enc.runs / u), (unsigned)(s_enc.subrects / u),
               (long long)((analyse_us + emit_us) / tiles),
               (long long)(analyse_us / tiles),
               (long long)(emit_us / tiles));

    if (s_cls.rects > 0) {
        espix_klog(ESPIX_KLOG_INFO, TAG,
                   "classify: %u rects, %u sent as Tight, %u passed through "
                   "(client takes lossy: %s)",
                   (unsigned)s_cls.rects, (unsigned)s_cls.sent_jpeg,
                   (unsigned)s_cls.passed, s_cls.lossy ? "yes" : "no");
        if (s_cls.big_px > 0) {
            const uint32_t big_edge = s_cls.big_pairs
                ? (uint32_t)((uint64_t)s_cls.big_edges * 100 / s_cls.big_pairs) : 0;

            espix_klog(ESPIX_KLOG_INFO, TAG,
                       "classify: largest was %u px: %u colours, %u%% edges -> %s",
                       (unsigned)s_cls.big_px, (unsigned)s_cls.big_colours,
                       (unsigned)big_edge, s_cls.big_jpeg ? "JPEG" : "Hextile");
        }
        memset(&s_cls, 0, sizeof(s_cls));
    }

    s_enc.analyse_cyc = 0;
    s_enc.emit_cyc    = 0;
    s_enc.tiles       = 0;
    s_enc.flat        = 0;
    s_enc.raw         = 0;
    s_enc.sub         = 0;
    s_enc.runs        = 0;
    s_enc.subrects    = 0;

    s_phase.n = 0;
    s_phase.canvas_us = 0;
    s_phase.encode_us = 0;
    s_phase.write_us = 0;
    s_phase.lock_us  = 0;
    s_phase.pick_us  = 0;
    s_phase.cls_us   = 0;
    s_phase.copy_us  = 0;
    s_phase.stage_rects = 0;
    s_phase.stage_px    = 0;
    s_phase.stage_big_px = 0;
    s_phase.stage_big_w  = 0;
    s_phase.stage_big_h  = 0;
    s_send_stat.n     = 0;
    s_send_stat.us    = 0;
    s_send_stat.rects = 0;
    s_send_stat.worst = 0;
}

/*
 * Tell the client the screen changed size (ExtendedDesktopSize).
 *
 * One FramebufferUpdate carrying one rectangle whose "encoding" is the
 * pseudo-encoding and whose payload is a single screen at the origin. The
 * client resizes its framebuffer and asks again -- which is what makes a resize
 * live on the same connection rather than a reconnect.
 */
static bool send_desktop_size_eds(rfb_conn_t *c, int w, int h)
{
    uint8_t b[4 + 12 + 4 + 16];

    b[0] = 0;                       /* FramebufferUpdate */
    b[1] = 0;
    wr16(b + 2, 1);                 /* one rectangle */

    uint8_t *r = b + 4;
    wr16(r + 0, 0);
    wr16(r + 2, 0);
    wr16(r + 4, (uint16_t)w);
    wr16(r + 6, (uint16_t)h);
    wr32(r + 8, RFB_ENC_EXTENDED_DESKTOP_SIZE);

    /* One screen, then three bytes of padding before the screen entries --
     * which the spec has and this first did not, so the client read the id out
     * of the padding and every field after it was three bytes early. */
    r[12] = 1;                      /* number of screens */
    r[13] = r[14] = r[15] = 0;      /* padding */

    wr32(r + 16, 0);                /* screen id */
    wr16(r + 20, 0);                /* screen x */
    wr16(r + 22, 0);                /* screen y */
    wr16(r + 24, (uint16_t)w);
    wr16(r + 26, (uint16_t)h);
    wr32(r + 28, 0);                /* flags */

    return write_full(c->fd, b, sizeof(b));
}

/*
 * The older resize, for a client that never offered ExtendedDesktopSize:
 * one rectangle whose width and height are the new framebuffer size and which
 * carries no payload at all -- twelve bytes of header and a four of message.
 */
static bool send_desktop_size_old(rfb_conn_t *c, int w, int h)
{
    uint8_t b[4 + 12];

    b[0] = 0;                       /* FramebufferUpdate */
    b[1] = 0;
    wr16(b + 2, 1);                 /* one rectangle */

    wr16(b + 4, 0);                 /* x */
    wr16(b + 6, 0);                 /* y */
    wr16(b + 8, (uint16_t)w);       /* ...the new size */
    wr16(b + 10, (uint16_t)h);
    wr32(b + 12, RFB_ENC_DESKTOP_SIZE);

    return write_full(c->fd, b, sizeof(b));
}

/*
 * The canvas changed size under a live connection: give the staging copy and
 * the row scratch the new dimensions, then tell the client. A client that never
 * offered ExtendedDesktopSize cannot be resized in place, so it is dropped and
 * reconnects at the new size -- ServerInit is only ever sent once.
 */
/* Cache-line aligned and sized, so the PPA can write it. */
static void *stage_alloc(size_t bytes)
{
    bytes = (bytes + RFB_ALIGN - 1) & ~(size_t)(RFB_ALIGN - 1);

    void *p = heap_caps_aligned_alloc(RFB_ALIGN, bytes,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == NULL) {
        p = heap_caps_aligned_alloc(RFB_ALIGN, bytes,
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return p;
}

static bool connection_resize(rfb_conn_t *c, int w, int h)
{
    espix_px_t *stage = stage_alloc((size_t)w * h * sizeof(espix_px_t));
    uint8_t    *row   = stage_alloc((size_t)w * 4);

    if (stage == NULL || row == NULL) {
        heap_caps_free(stage);
        heap_caps_free(row);
        return false;
    }

    heap_caps_free(c->stage);
    heap_caps_free(c->row);
    c->stage   = stage;
    c->row     = row;
    c->stage_w = w;
    c->stage_h = h;

    if (!c->eds && !c->desksize) {
        espix_klog(ESPIX_KLOG_WARN, TAG,
                   "client cannot resize; dropping it to reconnect at %dx%d", w, h);
        return false;
    }

    espix_klog(ESPIX_KLOG_INFO, TAG, "client told the screen is now %dx%d", w, h);
    return c->eds ? send_desktop_size_eds(c, w, h)
                  : send_desktop_size_old(c, w, h);
}

/*
 * Answer a request for something that has not changed, with an update that
 * carries no rectangles.
 *
 * This is what makes a client's silence mean something. A viewer sends a
 * request and waits; with nothing to send this server used to say nothing, so
 * a quiet client and a client that had gone were the same thing from in here:
 * there was no write that could fail and no read that would return, and the
 * viewer's own patience ran out first (measured: sixty seconds, then it
 * disconnects). Four zero bytes say "nothing changed" -- and, twice a second at
 * worst, they are also the write that fails when the client is no longer
 * there, which is the liveness signal this loop never had.
 *
 * A *full* request never reaches here: that asks for the whole screen and is
 * answered with the whole screen.
 */
static bool update_send_empty(rfb_conn_t *c)
{
    const int64_t now = esp_timer_get_time();

    if (now - c->last_empty_us < 500000) {
        return true;                    /* owed, and answered recently */
    }
    c->last_empty_us = now;

    const uint8_t hdr[4] = { 0, 0, 0, 0 };   /* FramebufferUpdate, no rects */

    return write_full(c->fd, hdr, sizeof(hdr));
}

static bool update_send(rfb_conn_t *c)
{
    vnc_state(VNC_CANVAS);              /* the lock and the copy out */

    const int64_t  t0 = esp_timer_get_time();
    int64_t        t_canvas;
    int64_t        t_sent;
    int64_t        t_lock = 0, t_pick = 0, t_cls = 0;
    espix_canvas_t *cv = espix_display_canvas();
    if (cv == NULL || c->stage == NULL) {
        return false;
    }

    const int cw = espix_canvas_width(cv);
    const int ch = espix_canvas_height(cv);

    /*
     * The screen can change size under a live connection. Reallocate to match
     * before anything reads the stage, then tell the client; the frame itself
     * follows on the request the client sends once it has resized.
     */
    if (cw != c->stage_w || ch != c->stage_h) {
        if (!connection_resize(c, cw, ch)) {
            return false;
        }
        c->pending = false;
        return true;
    }

    espix_rect_t rects[ESPIX_DISPLAY_DAMAGE_MAX];
    espix_rect_t keep[ESPIX_DISPLAY_DAMAGE_MAX];
    espix_move_t moves[ESPIX_DISPLAY_MOVE_MAX];
    const bool   full = c->pending_full;
    size_t       nmove = 0;
    size_t       n;

    espix_canvas_lock(cv);
    t_lock = esp_timer_get_time();
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
        return update_send_empty(c);
    }

    RFB_MARK_START(RFB_MARK_CANVAS);

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
     * And when there is no copy -- more than one move since the last update, or
     * a client that never asked for CopyRect -- the moved pixels have to be
     * sent as pixels. Nothing else will say they moved.
     *
     * Which is the whole of a bug that warped every dragged window: the desktop
     * moves its own canvas and repairs only the strips it uncovers, so with no
     * copy in the update the client was told about the strips and not about the
     * rectangle between them. The window sat where it had been, torn at the
     * edges, until something repainted it -- and moving *another* window
     * repainted it, which is why the damage appeared to chase the drag around.
     */
    if (!copy) {
        for (size_t i = 0; i < nmove && n < ESPIX_DISPLAY_DAMAGE_MAX; i++) {
            rects[n++] = moves[i].r;
        }
    }

    /*
     * Copy out under the lock, encode outside it. Holding it across the send
     * would freeze the cursor for as long as the client takes to read a frame,
     * which on a slow link is the difference between a desktop and a slideshow.
     */
    const espix_px_t *px = espix_canvas_pixels(cv);

    /*
     * One rectangle in an update may go as JPEG, and it is chosen before
     * anything is copied.
     *
     * The encoder owns one input buffer, so a second photographic rectangle in
     * the same update would need a second 600 KiB copy and a second 28 ms
     * encode -- and the largest rectangle is the one worth it anyway. Its
     * pixels are copied out *packed*, straight from the canvas, because the
     * codec takes a picture and not a pitch: staging it at canvas stride and
     * gathering it afterwards is two copies of the same 600 KiB, which is more
     * CPU than the Hextile it replaces rather than less.
     *
     * Off unless the client offered Tight *and* allowed lossy, so a client that
     * speaks only Hextile never reaches any of this.
     */
    int    jpeg_i   = -1;
    size_t pass_len = 0;               /* the picture's own JPEG, already in s_tight_out */

    t_pick = esp_timer_get_time();

    if (c->tight && c->lossy && n > 0) {
        size_t best = 0;

        for (size_t i = 1; i < n; i++) {
            if ((uint32_t)rects[i].w * (uint32_t)rects[i].h
                > (uint32_t)rects[best].w * (uint32_t)rects[best].h) {
                best = i;
            }
        }

        /* Where the owner says a picture is, that overlap is the JPEG, unsampled. */
        const bool         enc   = espix_display_jpeg_can_encode();
        const espix_rect_t photo = espix_canvas_photo(cv);
        const espix_rect_t ov    = rect_meet(rects[best], photo);
        const bool         whole = ov.w == photo.w && ov.h == photo.h && ov.w > 0;
        bool               hint  = false;

        /* The picture's own bytes first: no copy and no encode, on any target. */
        if (whole && tight_room(photo)) {
            pass_len = espix_canvas_photo_jpeg(cv, s_tight_out, s_tight_cap);
        }

        if ((pass_len > 0 || enc) && ov.w > 0 && ov.h > 0
            && (uint32_t)ov.w * ov.h * 2 >= (uint32_t)photo.w * photo.h) {
            espix_rect_t rest[4];
            const size_t nr = rect_subtract(rects[best], ov, rest);

            if (n - 1 + nr <= ESPIX_DISPLAY_DAMAGE_MAX) {
                rects[best] = ov;
                for (size_t k = 0; k < nr; k++) {
                    rects[n++] = rest[k];
                }
                hint = true;
            }
        }
        if (pass_len > 0 && !hint) {
            pass_len = 0;               /* no room to split it out */
        }

        const espix_rect_t    r = rects[best];
        espix_display_class_t cl = { .jpeg = hint };

        if (pass_len > 0) {
            jpeg_i  = (int)best;
            cl.jpeg = false;            /* nothing to copy or encode */
        } else if (!hint && enc) {
            espix_display_classify(px, cw, r.x, r.y, r.w, r.h, &cl);
        }
        t_cls = esp_timer_get_time();

        if (cl.jpeg && enc) {
            espix_px_t *in = espix_display_jpeg_input((size_t)r.w * (size_t)r.h);

            if (in != NULL) {
                /* Packed, so the picture is the rectangle itself. */
                espix_canvas_copy_rect(cv, r, in, r.w, r.h, r.w, 0, 0);
                jpeg_i = (int)best;
            }
        }
    }

    for (size_t i = 0; i < n; i++) {
        const espix_rect_t r = rects[i];

        if ((int)i == jpeg_i) {
            continue;                   /* in the encoder's input already */
        }

        s_phase.stage_rects++;
        s_phase.stage_px += (uint32_t)r.w * (uint32_t)r.h;
        if ((uint32_t)r.w * (uint32_t)r.h > s_phase.stage_big_px) {
            s_phase.stage_big_px = (uint32_t)r.w * (uint32_t)r.h;
            s_phase.stage_big_w  = (uint16_t)r.w;
            s_phase.stage_big_h  = (uint16_t)r.h;
        }

        /*
         * A full-width rectangle is one contiguous run of pixels, so the common
         * case -- a game that damages the whole canvas every frame -- is one
         * memcpy rather than one per row. Same bytes, a quarter of the calls.
         */
        /* The one copy that is not for a JPEG: hardware where the PPA takes
         * it, the same rows in software where it does not. */
        espix_canvas_copy_rect(cv, r, c->stage, cw, ch, cw, r.x, r.y);
    }
    espix_canvas_unlock(cv);
    vnc_state(VNC_SENDING);             /* now the encoding and the wire */
    t_canvas = esp_timer_get_time();
    if (t_cls == 0) {
        t_cls = t_pick;                 /* nothing was classified */
    }
    s_phase.lock_us += (uint64_t)(t_lock - t0);
    s_phase.pick_us += (uint64_t)(t_pick - t_lock);
    s_phase.cls_us  += (uint64_t)(t_cls - t_pick);
    s_phase.copy_us += (uint64_t)(t_canvas - t_cls);
    RFB_MARK_STOP(RFB_MARK_CANVAS);
    RFB_MARK_START(RFB_MARK_ENCODE);

    /*
     * The JPEG next, and before any header is written: the encoder is the half
     * that can fail, and a rectangle announced as Tight cannot then be sent as
     * Hextile. On failure the pixels go back on the stage -- they are still in
     * the encoder's input -- and the rectangle goes losslessly instead.
     */
    size_t jpeg_len = 0;

    if (jpeg_i >= 0 && pass_len > 0) {
        jpeg_len = pass_len;
        s_cls.passed++;
    } else if (jpeg_i >= 0) {
        jpeg_len = tight_jpeg(rects[jpeg_i], c->qlevel);

        if (jpeg_len == 0) {
            tight_restage(rects[jpeg_i], c->stage, cw);
            jpeg_i = -1;
        }
    }

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
    const int ws = write_start(c->fd, hdr, 4);

    if (ws < 0) {
        RFB_MARK_STOP(RFB_MARK_ENCODE);
        return false;                   /* gone: close rather than retry */
    }
    if (ws == 0) {
        /*
         * Not now. Still owed, so the next attempt describes the screen as it
         * is by then rather than as it was -- but only for so long: a peer that
         * never takes the frame is not a peer, and this is bounded so that the
         * deferral cannot become the silent loop it used to be.
         */
        if (++c->deferred > DEFER_LIMIT) {
            espix_klog(ESPIX_KLOG_WARN, TAG,
                       "client has not taken an update in %d tries; dropping it",
                       DEFER_LIMIT);
            c->deferred = 0;
            RFB_MARK_STOP(RFB_MARK_ENCODE);
            return false;
        }
        RFB_MARK_STOP(RFB_MARK_ENCODE);
        return true;
    }

    c->deferred = 0;

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
        RFB_MARK_START(RFB_MARK_COPY);
        sink_write(&s, ch, sizeof(ch));
        RFB_MARK_STOP(RFB_MARK_COPY);
    }

    RFB_MARK_START(RFB_MARK_PIXELS);
    for (size_t i = 0; i < n; i++) {
        const espix_rect_t r     = rects[i];
        const bool         tight = ((int)i == jpeg_i);
        uint8_t            rh[12];

        /* The observer counts what went out, and only for the rectangles it
         * can still see: a JPEG rectangle's pixels are in the encoder's input,
         * not in the stage, so classifying the stage for it would be reading
         * whatever the last frame left there. */
        if (!tight) {
            classify_note(c, c->stage, cw, r);
        } else {
            s_cls.rects++;
            s_cls.px_all += (uint32_t)r.w * (uint32_t)r.h;
            s_cls.sent_jpeg++;
        }

        wr16(rh + 0, (uint16_t)r.x);
        wr16(rh + 2, (uint16_t)r.y);
        wr16(rh + 4, (uint16_t)r.w);
        wr16(rh + 6, (uint16_t)r.h);
        wr32(rh + 8, tight ? RFB_ENC_TIGHT
                           : (c->hextile ? RFB_ENC_HEXTILE : RFB_ENC_RAW));
        sink_write(&s, rh, sizeof(rh));

        if (tight) {
            tight_head(&s, 0x90, jpeg_len);
            sink_write(&s, s_tight_out, jpeg_len);
        } else if (c->hextile) {
            enc_hextile(&s, c->stage, cw, r, &c->pf, c->row);
        } else {
            enc_raw(&s, c->stage, cw, r, &c->pf);
        }
        if (s.bad) {
            RFB_MARK_STOP(RFB_MARK_PIXELS);
            RFB_MARK_STOP(RFB_MARK_ENCODE);
            return false;
        }
    }
    RFB_MARK_STOP(RFB_MARK_PIXELS);
    sink_flush(&s);
    t_sent = esp_timer_get_time();
    RFB_MARK_STOP(RFB_MARK_ENCODE);
    if (s.bad) {
        return false;
    }

    RFB_TRACE_PACKET(copy, nmove, full, c->copyrect, total, s.bytes, s.flushes);

    s_phase.n++;
    s_phase.canvas_us += (uint64_t)(t_canvas - t0);
    s_phase.encode_us += (uint64_t)(t_sent - t_canvas) - s_write_us;
    s_phase.write_us  += s_write_us;
    s_write_us = 0;

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
        bool           eds      = false;
        bool           desksize = false;
        bool           tight    = false;
        int            qmin     = -1;       /* quality levels offered, 0-9 */
        int            qmax     = -1;
        char           offered[192];
        size_t         off = 0;

        offered[0] = '\0';

        for (uint16_t i = 0; i < n; i++) {
            uint8_t e[4];
            if (!read_full(c->fd, e, sizeof(e))) {
                return false;
            }
            const uint32_t enc  = rd32(e);
            const int32_t  senc = (int32_t)enc;

            if (enc == RFB_ENC_HEXTILE) {
                hextile = true;
            } else if (enc == RFB_ENC_TIGHT) {
                tight = true;
            } else if (enc == RFB_ENC_COPYRECT) {
                copyrect = true;
            } else if (enc == RFB_ENC_EXTENDED_DESKTOP_SIZE) {
                eds = true;
            } else if (enc == RFB_ENC_DESKTOP_SIZE) {
                desksize = true;
            } else if (senc >= RFB_ENC_TIGHT_Q_MIN && senc <= RFB_ENC_TIGHT_Q_MAX) {
                /*
                 * A quality level: -32 is level 0 and means lossless, -23 is
                 * level 9. Offering any of them is offering to take a JPEG;
                 * which one is a question of how much quality the client will
                 * give up, and only it can answer.
                 */
                const int lvl = (int)(senc - RFB_ENC_TIGHT_Q_MIN);

                if (qmin < 0 || lvl < qmin) { qmin = lvl; }
                if (lvl > qmax)             { qmax = lvl; }
            }
            /*
             * And the numbers themselves, because which pseudo-encoding a
             * viewer offers is the whole question when a resize lands on a
             * client that cannot take one -- and not something to guess at
             * twice.
             */
            if (off + 12 < sizeof(offered)) {
                const int w = snprintf(offered + off, sizeof(offered) - off,
                                       "%s%ld", off ? " " : "", (long)(int32_t)enc);
                if (w > 0) {
                    off += (size_t)w;
                }
            }
        }
        c->hextile  = hextile;
        c->copyrect = copyrect;
        c->eds      = eds;
        c->desksize = desksize;
        c->tight    = tight;
        c->lossy    = (qmax >= 1);          /* level 0 is lossless, so 1 is the flag */
        /*
         * Which level to use is the client's bounds and our choice between
         * quality and bytes. Six is TightVNC's own default and a starting point
         * to measure from rather than a decision.
         */
        c->qlevel   = c->lossy ? ((qmax >= 6) ? 6 : qmax) : 0;

        snprintf(s_encodings, sizeof(s_encodings), "%u offered: hextile %s, "
                 "copyrect %s", n, hextile ? "yes" : "no",
                 copyrect ? "yes" : "no");
        /*
         * At INFO rather than DEBUG, and it is the line that decides whether a
         * drag is 8 KB or 300 KB and whether a resize is a blink or a
         * reconnect: both are *client* capabilities, and a client that offers
         * neither gives no other way to tell from the outside.
         */
        espix_klog(ESPIX_KLOG_INFO, TAG, "client offered %u encodings: hextile "
                   "%s, copyrect %s, tight %s, lossy %s (levels %d-%d)", n,
                   hextile ? "yes" : "no", copyrect ? "yes" : "no",
                   tight ? "yes" : "no", c->lossy ? "yes" : "no", qmin, qmax);
        /* The list separately, so neither line is long enough to be cut. */
        espix_klog(ESPIX_KLOG_INFO, TAG, "resize %s (%s); all of them [%s]",
                   eds ? "extended" : (desksize ? "desktopsize" : "no"),
                   copyrect ? "drags will be copies" : "drags will be pixels",
                   offered);
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
        return true;                        /* its damage wakes the loop */
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
        return true;                        /* its damage wakes the loop */
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

    case RFB_SET_DESKTOP_SIZE: {
        /*
         * The client asking us to resize -- RealVNC's viewer does this when
         * its window changes size. Not implemented: a server that accepts one
         * is meant to answer with a screen layout, and we have one screen and
         * no interest in following a viewer's window. But it has to be read
         * and dropped rather than closed on, because a window drag is not a
         * reason to disconnect somebody.
         */
        uint8_t buf[7];                     /* pad, w, h, screens, pad */
        if (!read_full(c->fd, buf, sizeof(buf))) {
            return false;
        }
        const uint16_t screens = buf[5];
        for (uint16_t i = 0; i < screens; i++) {
            uint8_t s[16];
            if (!read_full(c->fd, s, sizeof(s))) {
                return false;
            }
        }
        espix_klog(ESPIX_KLOG_INFO, TAG,
                   "client asked for a %ux%u desktop; not implemented, ignoring",
                   (unsigned)rd16(buf + 1), (unsigned)rd16(buf + 3));
        return true;
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
    c.fd     = fd;
    c.qlevel = -1;              /* nothing offered yet, and Tight is not assumed */
    c.pf = pf_default();

    c.stage = stage_alloc((size_t)cw * ch * sizeof(espix_px_t));
    c.row   = stage_alloc((size_t)cw * 4);
    c.out   = heap_caps_malloc(OUT_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (c.out == NULL) {
        c.out = heap_caps_malloc(OUT_CAP, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }

    if (c.stage == NULL || c.out == NULL || c.row == NULL) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "no memory for a connection");
        heap_caps_free(c.stage);
        heap_caps_free(c.out);
        heap_caps_free(c.row);
        return;
    }

    /* What stage and row were sized for; a change here is a live resize. */
    c.stage_w = cw;
    c.stage_h = ch;

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
    c.last_rx_us = esp_timer_get_time();
    espix_klog(ESPIX_KLOG_INFO, TAG, "client connected from %s", s_peer);
    latency_want(true);

    /*
     * A viewer with nothing on the screen gets the console, and only then: a
     * desktop that is already running is what it should see. Created here and
     * torn down below, so a board with nobody watching allocates none of it.
     */
    espix_display_viewer_attached();

    espix_display_set_damage_hook(rfb_damage_hook);

    while (s_run) {
        uint8_t type;

        vnc_state(VNC_WAIT);

        /* Armed before looking, so damage between the look and the sleep still wakes it. */
        s_wake_armed = c.pending;
        if (c.pending && !update_send(&c)) {
            break;
        }

        if (conn_wait(fd, c.pending, c.pending && c.deferred > 0) == 0) {
            /* Idle viewers are answered twice a second, so this much silence means gone. */
            if (esp_timer_get_time() - c.last_rx_us > LIVENESS_US) {
                espix_klog(ESPIX_KLOG_WARN, TAG,
                           "client silent for %d s after an answer; dropping it",
                           (int)(LIVENESS_US / 1000000));
                break;
            }
            continue;
        }

        const int rc = recv_byte(fd, &type);
        if (rc == -1) {
            continue;                   /* readable, then not: a spurious wake */
        }
        if (rc <= 0) {
            break;
        }

        vnc_state(VNC_HANDLING);
        c.last_rx_us = esp_timer_get_time();
        if (!rfb_handle(&c, type)) {
            break;
        }
    }

    s_wake_armed = false;
    espix_display_set_damage_hook(NULL);

    s_clients = 0;
    s_peer[0] = '\0';
    vnc_state(VNC_HANDSHAKE);           /* tearing this client down */
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

    if (s_state_timer == NULL) {
        const esp_timer_create_args_t ta = { .callback = vnc_state_tick,
                                             .name     = "vnc-state" };

        if (esp_timer_create(&ta, &s_state_timer) == ESP_OK) {
            (void)esp_timer_start_periodic(s_state_timer, 5 * 1000 * 1000);
        }
    }

    while (s_run) {
        struct sockaddr_in peer;
        socklen_t          plen = sizeof(peer);

        vnc_state(VNC_ACCEPT);
        wake_open();
        const int fd = accept(s_listen_fd, (struct sockaddr *)&peer, &plen);
        if (fd < 0) {
            /* SO_RCVTIMEO makes this the normal idle path, not an error. */
            continue;
        }

        /*
         * Keepalive, because "quiet" and "gone" look identical from here and
         * only one of them should hold this task -- which is also the task that
         * accepts the next client. An idle viewer is legitimately silent for
         * minutes (the server only answers when something changed), so a
         * silence timeout would drop real clients; the stack probing the peer
         * is the only thing that tells the two apart.
         */
        const int one_on = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one_on, sizeof(one_on));
#ifdef TCP_KEEPIDLE
        {
            const int idle = 15, intvl = 5, cnt = 3;
            (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
            (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
            (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
        }
#endif

        vnc_state(VNC_HANDSHAKE);
        if (espix_shutdown_started()) {
            /* RFB has no "the server is leaving" message -- the protocol has no
             * such notion -- so a close is the whole goodbye. */
            close(fd);
            continue;
        }

        /* Interactive: coalescing pointer moves makes the cursor feel broken. */
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    /*
     * Room to hand a whole update over without stopping to wait for the wire.
     *
     * lwIP has no per-socket send buffer: SO_SNDBUF is defined and
     * "Unimplemented: send buffer size" in its own header, and TCP_SNDBUF is
     * not a socket option either. What a socket may queue is the compile-time
     * TCP_SND_BUF (CONFIG_LWIP_TCP_SND_BUF_DEFAULT), which
     * sdkconfig.defaults.esp32s31 raises to 114688 -- without it, a frame that
     * should cost microseconds took several round trips on the wire.
     *
     * Which is fine, because it is a ceiling on *queued* bytes and not a
     * reservation: nothing is charged per connection, and each byte is
     * allocated only as it is queued. With MEM_LIBC_MALLOC and MEMP_MEM_MALLOC
     * set, those allocations come from the heap, and
     * CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP makes lwIP ask PSRAM first -- so a
     * busy VNC session's queue does not eat the internal RAM an SSH session
     * needs. It is TCP_WND that holds memory, by advertising how much a peer
     * may have in flight, which is why the receive window stays at its modest
     * default: the only thing this connection receives is pointer events.
     */

        /*
         * The socket's timeouts -- and the answers to them.
         *
         * These are what the stall handling below rests on: a blocking send()
         * that never returns EAGAIN cannot be counted, so a client that stops
         * reading wedges the serving task and the accept loop with it. lwIP
         * only implements these two when CONFIG_LWIP_SO_RCVTIMEO and
         * CONFIG_LWIP_SO_SNDTIMEO are set; otherwise it returns an error that
         * used to go in the bin, and the failure stays silent until a client
         * dies.
         */
        const struct timeval io = { .tv_sec = 0, .tv_usec = POLL_US };

        if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &io, sizeof(io)) != 0
            || setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &io, sizeof(io)) != 0) {
            espix_klog(ESPIX_KLOG_WARN, TAG,
                       "socket timeouts refused: a dead client will not be "
                       "noticed (CONFIG_LWIP_SO_*TIMEO)");
        }

        snprintf(s_peer, sizeof(s_peer), "%s:%u",
                 inet_ntoa(peer.sin_addr), (unsigned)ntohs(peer.sin_port));

        rfb_serve(fd);
        close(fd);
    }

    close(s_listen_fd);
    s_listen_fd = -1;
    espix_task_exited(&s_exit);
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

    if (!espix_task_exit_init(&s_exit)) {
        s_run = false;
        close(fd);
        s_listen_fd = -1;
        return ESP_ERR_NO_MEM;
    }

    /*
     * Core 1.
     *
     * Watched on the board while dragging: core 0 at 64% and core 1 at 1%. This
     * task does the desktop's drawing as well as the connection's reading and
     * encoding -- input callbacks are dispatched in the context of whoever
     * posted them -- so it *is* that 64%, and it was sharing a core with the
     * network stack that feeds it.
     *
     * Which it now does not, and it is felt: with the drawing, the reading and
     * the encoding all off core 0, a drag runs smooth where it used to stutter.
     * Worth saying that the first attempt at this measured *nothing*, and the
     * measurement is why -- the test client waits for a frame after every
     * motion, so the server was never saturated and there was no contention to
     * remove. A hand on a mouse keeps both busy at once, and that is the case
     * this is for.
     *
     * The cost is a cross-core wakeup per event, which at these rates is noise
     * next to a core that is a third idle.
     */
    (void)espix_kthread_start(&(const espix_kthread_t){
        .name = "espix:vnc", .fn = rfb_task, .stack = 8192, .priority = 4,
        .core = 1 }, &s_exit.task);
    if (s_exit.task == NULL) {
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

    /* Wait for the task to clear its own handle. It notices within one accept
     * or recv timeout and returns; deleting it from here would leave its
     * process half torn down. */
    (void)espix_task_exit_wait(&s_exit, 3000);
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
