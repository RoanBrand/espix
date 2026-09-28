/*
 * Settings: the panel the desktop is configured from.
 *
 * One window, three sections, and the sections are the interesting part -- the
 * frame around them is deliberately thin. Wireframes are easy to write and
 * impossible to check, so the panel is *described* as rows rather than drawn row
 * by row: one table per section, and the same table is what the drawing walks and
 * what the pointer hit-tests. That is the lesson the window buttons taught, in
 * its general form -- a widget drawn in one place and pressed in another is a
 * whole class of bug, and the way not to have it is to have one list.
 *
 * What is real and what is not is marked in the panel itself. Network is real:
 * the interfaces, their addresses and the default route come from espix_net, and
 * DNS from the resolver. Audio is mostly real: the adapter state and the paired
 * devices come from espix_bt, and Unpair does what it says. Screen is a
 * placeholder and is drawn disabled, because resolution and colour depth are
 * both real work -- the canvas is 800x600 and 16 bits deep from end to end, and
 * neither changes without a restart at best.
 */

#include <stdio.h>
#include <string.h>

#include "espix_bt.h"
#include "espix_desktop.h"
#include "espix_kernel.h"
#include "espix_net.h"
#include "espix_widgets.h"

#define TAG "settings"

#define NAV_W  120
#define ROW_H  WGT_ROW_H
#define ROW_MAX 20

typedef enum {
    SEC_SCREEN = 0,
    SEC_NETWORK,
    SEC_AUDIO,
    SEC_COUNT,
} section_t;

static const char *const s_section_names[SEC_COUNT] = {
    "Screen", "Network", "Audio",
};

/* What pressing a row does. Rows that do nothing still say so, by ACT_NONE. */
typedef enum {
    ACT_NONE = 0,
    ACT_RESOLUTION,
    ACT_DEPTH,
    ACT_SCAN,
    ACT_UNPAIR,
    ACT_DEVICE,
} action_t;

typedef enum {
    ROW_HEAD,
    ROW_TEXT,       /* a line of text, no interaction */
    ROW_RADIO,
    ROW_FIELD,      /* a name on the left and a value on the right */
    ROW_BUTTON,
    ROW_DEVICE,     /* a device in a list, selectable */
    ROW_GAP,
} row_kind_t;

typedef struct {
    row_kind_t  kind;
    const char *text;
    const char *value;
    bool        on;         /* enabled: drawn grey and not acted on otherwise */
    bool        sel;        /* chosen: a radio, or the selected device */
    action_t    action;
    int         arg;        /* which device, for ROW_DEVICE */
} row_t;

static espix_window_t *s_win;
static section_t       s_sec = SEC_SCREEN;
static int             s_dev_sel = -1;

/*
 * Scratch for the right-hand column of the rows that compute theirs. File
 * scope because section_rows() is called twice per repaint -- once to draw and
 * once to hit-test -- and a strdup() per call would be two leaks per frame.
 */
static char            s_val[ROW_MAX][80];
static bool            s_dns_ok;

static espix_bt_dev_t  s_devs[8];
static int             s_ndev;
static espix_ifinfo_t  s_ifs[8];
static int             s_nif;
static uint32_t        s_gw;
static char            s_ifname[ESPIX_IF_NAME_MAX];
static char            s_dns[ESPIX_IP4STR_MAX * 4];

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

static espix_rect_t nav_rect(const espix_window_t *w)
{
    const espix_rect_t c = espix_window_content(w);

    return (espix_rect_t){ c.x, c.y, NAV_W, c.h };
}

static espix_rect_t nav_row(const espix_window_t *w, int i)
{
    const espix_rect_t n = nav_rect(w);

    return (espix_rect_t){ n.x + 8, n.y + 8 + i * 24, NAV_W - 16, ROW_H };
}

static espix_rect_t pane_rect(const espix_window_t *w)
{
    const espix_rect_t c = espix_window_content(w);

    return (espix_rect_t){ c.x + NAV_W, c.y, c.w - NAV_W, c.h };
}

/* Every row the same height, the heading included: a heading that is taller is a
 * second layout rule to get wrong, and the colour and the rule under it already
 * say what it is. */
static espix_rect_t row_rect(const espix_window_t *w, int i)
{
    const espix_rect_t p = pane_rect(w);

    return (espix_rect_t){ p.x + 8, p.y + 8 + i * ROW_H, p.w - 16, ROW_H };
}

/* ------------------------------------------------------------------ */
/* What is in each section                                             */
/* ------------------------------------------------------------------ */

static void refresh_network(void)
{
    s_nif = (int)espix_net_iflist(s_ifs, sizeof(s_ifs) / sizeof(s_ifs[0]));

    uint32_t gw = 0;
    s_gw = espix_net_default_route(s_ifname, sizeof(s_ifname), &gw) ? gw : 0;

    s_dns[0] = '\0';
    uint32_t dns[3];
    const size_t n = espix_net_dns(dns, 3);
    char         one[ESPIX_IP4STR_MAX];

    for (size_t i = 0; i < n; i++) {
        if (i != 0) {
            strlcat(s_dns, ", ", sizeof(s_dns));
        }
        strlcat(s_dns, espix_net_ip4str(dns[i], one, sizeof(one)), sizeof(s_dns));
    }
    s_dns_ok = (s_dns[0] != '\0');
    if (!s_dns_ok) {
        strlcpy(s_dns, "none", sizeof(s_dns));
    }
}

static void refresh_audio(void)
{
    s_ndev = espix_bt_ready()
           ? (int)espix_bt_devices(s_devs, sizeof(s_devs) / sizeof(s_devs[0]))
           : 0;

    if (s_dev_sel >= s_ndev) {
        s_dev_sel = -1;
    }
}

/*
 * The whole of a section, as a list. The drawing and the hit-testing both walk
 * this, which is the point: there is no second place for a row to be.
 */
static int section_rows(const espix_window_t *w, row_t *rows)
{
    (void)w;
    int n = 0;
    char buf[64];

    switch (s_sec) {
    case SEC_SCREEN:
        rows[n++] = (row_t){ ROW_HEAD, "Display", NULL, true, false, ACT_NONE, 0 };
        rows[n++] = (row_t){ ROW_TEXT, "Resolution", NULL, true, false, ACT_NONE, 0 };
        rows[n++] = (row_t){ ROW_RADIO, "800 x 600  (current)", NULL, true, true,
                             ACT_RESOLUTION, 0 };
        rows[n++] = (row_t){ ROW_RADIO, "1024 x 768", NULL, false, false,
                             ACT_RESOLUTION, 1 };
        rows[n++] = (row_t){ ROW_RADIO, "1280 x 800", NULL, false, false,
                             ACT_RESOLUTION, 2 };
        rows[n++] = (row_t){ ROW_GAP, NULL, NULL, true, false, ACT_NONE, 0 };
        rows[n++] = (row_t){ ROW_TEXT, "Colour depth", NULL, true, false, ACT_NONE, 0 };
        rows[n++] = (row_t){ ROW_RADIO, "High colour  (RGB565, 16-bit, current)",
                             NULL, true, true, ACT_DEPTH, 0 };
        rows[n++] = (row_t){ ROW_RADIO, "Full colour  (RGB888, 24-bit)",
                             NULL, false, false, ACT_DEPTH, 1 };
        rows[n++] = (row_t){ ROW_GAP, NULL, NULL, true, false, ACT_NONE, 0 };
        rows[n++] = (row_t){ ROW_TEXT,
                             "Neither changes without a restart, so both",
                             NULL, false, false, ACT_NONE, 0 };
        rows[n++] = (row_t){ ROW_TEXT, "are shown and not offered.", NULL,
                             false, false, ACT_NONE, 0 };
        break;

    case SEC_NETWORK:
        rows[n++] = (row_t){ ROW_HEAD, "Network", NULL, true, false, ACT_NONE, 0 };
        rows[n++] = (row_t){ ROW_TEXT, "Interfaces", NULL, true, false, ACT_NONE, 0 };

        for (int i = 0; i < s_nif && n < ROW_MAX - 6; i++) {
            char v[48];
            char ip[ESPIX_IP4STR_MAX];

            if (!s_ifs[i].has_addr) {
                snprintf(v, sizeof(v), "%s", s_ifs[i].up ? "no address" : "down");
            } else {
                snprintf(v, sizeof(v), "%s/%d",
                         espix_net_ip4str(s_ifs[i].ip, ip, sizeof(ip)),
                         espix_net_prefix_len(s_ifs[i].netmask));
            }
            snprintf(s_val[n], sizeof(s_val[n]), "%s", v);
            rows[n++] = (row_t){ ROW_FIELD, s_ifs[i].name, s_val[n], true, false,
                                 ACT_NONE, 0 };
        }

        rows[n++] = (row_t){ ROW_GAP, NULL, NULL, true, false, ACT_NONE, 0 };
        rows[n++] = (row_t){ ROW_TEXT, "Routing and name resolution", NULL, true,
                             false, ACT_NONE, 0 };

        char gw[ESPIX_IP4STR_MAX];
        snprintf(buf, sizeof(buf), "%s via %s",
                 s_gw != 0 ? espix_net_ip4str(s_gw, gw, sizeof(gw)) : "none",
                 s_gw != 0 ? s_ifname : "-");
        snprintf(s_val[n], sizeof(s_val[n]), "%s", buf);
        rows[n++] = (row_t){ ROW_FIELD, "Default route", s_val[n],
                             s_gw != 0, false, ACT_NONE, 0 };
        snprintf(s_val[n], sizeof(s_val[n]), "%s", s_dns);
        rows[n++] = (row_t){ ROW_FIELD, "Nameservers", s_val[n],
                             s_dns_ok, false, ACT_NONE, 0 };
        rows[n++] = (row_t){ ROW_FIELD, "Hostname", espix_net_hostname(), true,
                             false, ACT_NONE, 0 };
        break;

    case SEC_AUDIO: {
        const bool ready = espix_bt_ready();

        rows[n++] = (row_t){ ROW_HEAD, "Audio", NULL, true, false, ACT_NONE, 0 };
        rows[n++] = (row_t){ ROW_FIELD, "Adapter", ready ? "ready" : "not started",
                             ready, false, ACT_NONE, 0 };
        rows[n++] = (row_t){ ROW_FIELD, "Scanning",
                             espix_bt_scanning() ? "yes" : "no", ready, false,
                             ACT_NONE, 0 };
        rows[n++] = (row_t){ ROW_BUTTON,
                             espix_bt_scanning() ? "Stop scanning" : "Scan for devices",
                             NULL, ready, false, ACT_SCAN, 0 };
        rows[n++] = (row_t){ ROW_GAP, NULL, NULL, true, false, ACT_NONE, 0 };
        rows[n++] = (row_t){ ROW_TEXT, "Paired devices", NULL, true, false,
                             ACT_NONE, 0 };

        if (s_ndev == 0) {
            rows[n++] = (row_t){ ROW_TEXT,
                                 ready ? "  none paired" : "  the adapter is not up",
                                 NULL, false, false, ACT_NONE, 0 };
        }
        for (int i = 0; i < s_ndev && n < ROW_MAX - 2; i++) {
            const char *state = s_devs[i].connected ? "A2DP"
                              : s_devs[i].bonded    ? "paired" : "seen";
            rows[n++] = (row_t){ ROW_DEVICE, s_devs[i].name[0] != '\0'
                                             ? s_devs[i].name
                                             : "(unnamed)",
                                 state, true, i == s_dev_sel, ACT_DEVICE, i };
        }

        rows[n++] = (row_t){ ROW_GAP, NULL, NULL, true, false, ACT_NONE, 0 };
        rows[n++] = (row_t){ ROW_BUTTON, "Unpair", NULL, s_dev_sel >= 0, false,
                             ACT_UNPAIR, 0 };
        rows[n++] = (row_t){ ROW_TEXT, "Turning the adapter on and off, and the",
                             NULL, false, false, ACT_NONE, 0 };
        rows[n++] = (row_t){ ROW_TEXT, "codec and volume, come next.",
                             NULL, false, false, ACT_NONE, 0 };
        break;
    }

    default:
        break;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

static void settings_draw(espix_window_t *w, espix_surface_t *s, espix_rect_t r,
                          void *ctx)
{
    (void)ctx;

    const espix_rect_t c = espix_window_content(w);
    const espix_rect_t n = nav_rect(w);

    /* The frame is redrawn with the window, so both sides are painted whole
     * rather than only where `r` reaches: a panel this small is not worth a
     * damage-tracking path. */
    espix_surface_fill(s, c, WGT_PANE);
    espix_surface_fill(s, n, WGT_NAV);

    for (int i = 0; i < SEC_COUNT; i++) {
        espix_wgt_navrow(s, nav_row(w, i), s_section_names[i],
                         (int)s_sec == i, false);
    }

    (void)r;

    row_t rows[ROW_MAX];
    const int cnt = section_rows(w, rows);

    for (int i = 0; i < cnt; i++) {
        const espix_rect_t rr = row_rect(w, i);

        if (rr.y + rr.h > c.y + c.h) {
            break;
        }
        switch (rows[i].kind) {
        case ROW_HEAD:   espix_wgt_heading(s, rr, rows[i].text); break;
        case ROW_TEXT:   espix_wgt_label(s, rr, rows[i].text, rows[i].on); break;
        case ROW_RADIO:  espix_wgt_radio(s, rr, rows[i].text, rows[i].sel,
                                         rows[i].on, false); break;
        case ROW_FIELD:  espix_wgt_field(s, rr, rows[i].text, rows[i].value,
                                         rows[i].on); break;
        case ROW_BUTTON: espix_wgt_button(s, rr, rows[i].text, rows[i].on, false);
                         break;
        case ROW_DEVICE: espix_wgt_listrow(s, rr, rows[i].text, rows[i].value,
                                           rows[i].sel, rows[i].on, false); break;
        case ROW_GAP:    break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

static void settings_pointer(espix_window_t *w, int x, int y, uint8_t buttons,
                             void *ctx)
{
    (void)ctx;

    if (buttons == 0) {
        return;                         /* act on the press, not the release */
    }

    /* The navigation first, then the rows, and both through the same tables the
     * drawing used. */
    for (int i = 0; i < SEC_COUNT; i++) {
        if (espix_wgt_hit(nav_row(w, i), x, y)) {
            s_sec = (section_t)i;
            if (s_sec == SEC_NETWORK) {
                refresh_network();
            } else if (s_sec == SEC_AUDIO) {
                refresh_audio();
            }
            espix_window_repaint(w);
            return;
        }
    }

    row_t     rows[ROW_MAX];
    const int cnt = section_rows(w, rows);

    for (int i = 0; i < cnt; i++) {
        const espix_rect_t rr = row_rect(w, i);

        if (!espix_wgt_hit(rr, x, y) || !rows[i].on) {
            continue;
        }
        switch (rows[i].action) {
        case ACT_DEVICE:
            s_dev_sel = (s_dev_sel == rows[i].arg) ? -1 : rows[i].arg;
            break;
        case ACT_SCAN:
            (void)espix_bt_scan(!espix_bt_scanning());
            refresh_audio();
            break;
        case ACT_UNPAIR:
            if (s_dev_sel >= 0 && s_dev_sel < s_ndev) {
                (void)espix_bt_remove(s_devs[s_dev_sel].bda);
                s_dev_sel = -1;
                refresh_audio();
            }
            break;
        default:
            return;                     /* a disabled row: nothing happened */
        }
        espix_window_repaint(w);
        return;
    }
}

static void settings_key(espix_window_t *w, uint32_t keysym, bool down, void *ctx)
{
    (void)ctx;

    if (!down) {
        return;
    }
    if (keysym == 0xFF54) {                          /* Down */
        s_sec = (section_t)((s_sec + 1) % SEC_COUNT);
    } else if (keysym == 0xFF52) {                    /* Up */
        s_sec = (section_t)((s_sec + SEC_COUNT - 1) % SEC_COUNT);
    } else {
        return;
    }
    if (s_sec == SEC_NETWORK) {
        refresh_network();
    } else if (s_sec == SEC_AUDIO) {
        refresh_audio();
    }
    espix_window_repaint(w);
}

/* ------------------------------------------------------------------ */
/* The window                                                          */
/* ------------------------------------------------------------------ */

espix_window_t *espix_settings_open(void)
{
    if (s_win == NULL) {
        /* Placed so the content rectangle lands on the 8-pixel grid: the window
         * is at 4 off a cell boundary because the frame takes 4, and the whole
         * panel is easier to draw and to read back when its text is too. */
        s_win = espix_window_new(116, 90, 560, 420, "settings");
        if (s_win == NULL) {
            return NULL;
        }
        espix_window_set_draw(s_win, settings_draw);
        espix_window_set_key(s_win, settings_key);
        espix_window_set_pointer(s_win, settings_pointer);

        refresh_network();
        refresh_audio();
        espix_window_repaint(s_win);
    }
    return s_win;
}

void espix_settings_forget(void)
{
    s_win = NULL;                       /* its pixels went with the window */
}

espix_window_t *espix_settings_window(void)
{
    return s_win;
}
