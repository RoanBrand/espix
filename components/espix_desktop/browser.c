/*
 * The file browser: the desktop's window on the filesystem.
 *
 * An internal desktop app, in the sense the plan settled on: it is built into
 * the desktop image and draws into a window, so it cannot be launched on its
 * own and does not need the canvas handed to it. What it *does* hand out is the
 * other kind -- opening a file here is where an external app or another
 * internal window will eventually be started, and stage two is the one function
 * that will do it (browser_activate()).
 *
 * Stage one, which is this file, browses: a Places column, a path line, the
 * directory as a list, selection by mouse and keyboard, and navigation into and
 * out of directories. It reads only, it never mounts anything, and it opens
 * nothing yet. That is deliberate -- a browser that can walk the tree is
 * testable on its own, and the parts that hand a file to something else are
 * worth adding once there is something to hand it to.
 *
 * The shape is settings.c's, on purpose: one window held in one static, opened
 * through a three-function contract (open/forget/window), and one geometry
 * table that both the paint and the hit test walk. A widget drawn in one place
 * and pressed in another is the class of bug that arrangement does not have.
 */

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "espix_desktop.h"
#include "espix_display.h"
#include "espix_kernel.h"
#include "espix_places.h"
#include "espix_widgets.h"

#define TAG "browser"

#define NAV_W  120
#define ROW_H  WGT_ROW_H

/*
 * A directory is listed into one array, and the array is bounded.
 *
 * A USB stick can hold more entries than there is memory to describe -- and a
 * listing is a convenience, not a database -- so the walk stops at the cap and
 * says so rather than growing without limit or failing outright. The array is
 * PSRAM and lives exactly as long as the window: the browser is an internal
 * app, and stopping the desktop gives all of this back.
 */
#define BROWSER_MAX_ENTRIES 256
#define BROWSER_NAME_MAX    64

typedef struct {
    char     name[BROWSER_NAME_MAX];
    bool     is_dir;
    bool     truncated;     /* the name did not fit, so it cannot be opened */
    uint32_t size;
} browser_entry_t;

/*
 * The Places column: the fixed few, then one row per mounted volume. Rebuilt
 * whenever the directory is rescanned, so a stick that arrives while the window
 * is open is a row the next time anything navigates -- and the desktop's icons
 * read the same list, from the same place.
 */
static espix_place_t s_places[ESPIX_PLACES_MAX];
static int           s_nplaces;

static espix_window_t  *s_win;
static char             s_path[ESPIX_PATH_MAX];
static browser_entry_t *s_ents;
static int              s_nents;
static int              s_sel;          /* row 0 is "..", so 0..s_nents */
static int              s_top;          /* first listing row on the screen */
static bool             s_truncated;    /* the walk hit the cap */
static char             s_status[160];  /* an error, or empty */
static int64_t          s_click_us;
static int              s_click_row = -1;

/* ------------------------------------------------------------------ */
/* Geometry                                                            */
/* ------------------------------------------------------------------ */

static espix_rect_t nav_rect(const espix_window_t *w)
{
    const espix_rect_t c = espix_window_content(w);

    return (espix_rect_t){ c.x, c.y, NAV_W, c.h };
}

static espix_rect_t nav_row(const espix_window_t *w, int i)
{
    const espix_rect_t n = nav_rect(w);

    /* As tall as the spacing, so the text sits on the middle cell row of the
     * blue rectangle rather than at the top of it -- the same rule, and the
     * same reason, as the settings panel's. */
    return (espix_rect_t){ n.x + 8, n.y + 8 + i * ROW_H, NAV_W - 16, ROW_H };
}

static espix_rect_t pane_rect(const espix_window_t *w)
{
    const espix_rect_t c = espix_window_content(w);

    return (espix_rect_t){ c.x + NAV_W, c.y, c.w - NAV_W, c.h };
}

static espix_rect_t path_rect(const espix_window_t *w)
{
    const espix_rect_t p = pane_rect(w);

    return (espix_rect_t){ p.x + 8, p.y + 8, p.w - 16, ROW_H };
}

static int list_top(const espix_window_t *w)
{
    return path_rect(w).y + ROW_H + 6;
}

/* How many listing rows fit, with a row kept at the bottom for the status. */
static int list_vis(const espix_window_t *w)
{
    const espix_rect_t c = espix_window_content(w);
    const int          n = (c.y + c.h - ROW_H - 6 - list_top(w)) / ROW_H;

    return n > 0 ? n : 0;
}

static espix_rect_t list_row(const espix_window_t *w, int vis)
{
    const espix_rect_t p = pane_rect(w);

    return (espix_rect_t){ p.x + 8, list_top(w) + vis * ROW_H, p.w - 16, ROW_H };
}

static espix_rect_t status_rect(const espix_window_t *w)
{
    const espix_rect_t c = espix_window_content(w);

    return (espix_rect_t){ c.x + NAV_W + 8, c.y + c.h - ROW_H - 6,
                           c.w - NAV_W - 16, ROW_H };
}

/* ------------------------------------------------------------------ */
/* Paths                                                               */
/* ------------------------------------------------------------------ */

/* The parent of an absolute path, with "/" as its own parent. */
static void path_parent(const char *in, char *out, size_t n)
{
    if (in[0] != '/' || in[1] == '\0') {
        strlcpy(out, "/", n);
        return;
    }

    strlcpy(out, in, n);

    char *slash = strrchr(out, '/');
    if (slash == NULL) {
        strlcpy(out, "/", n);
    } else if (slash == out) {
        out[1] = '\0';
    } else {
        *slash = '\0';
    }
}

/* A child of a directory path, without a doubled slash at the root. */
static void path_child(const char *base, const char *name, char *out, size_t n)
{
    /* strlcpy/strlcat rather than snprintf: they truncate to fit instead of
     * being analysable into a -Wformat-truncation error, which the compiler is
     * right to raise -- a path and a name together can exceed the buffer, and
     * what happens then is a failed opendir rather than a wrong path. */
    strlcpy(out, base, n);
    if (strcmp(base, "/") != 0) {
        strlcat(out, "/", n);
    }
    strlcat(out, name, n);
}

/*
 * Where the browser starts when it is not told: the account's home if there is
 * one, and the root otherwise. A release rootfs has no /home/esp, and starting
 * in a directory that does not exist would greet the user with an error.
 */
static const char *default_path(void)
{
    struct stat st;

    if (stat("/home/esp", &st) == 0 && S_ISDIR(st.st_mode)) {
        return "/home/esp";
    }
    return "/";
}

static bool place_selected(int i)
{
    const char  *p = s_places[i].path;
    const size_t n = strlen(p);

    if (strcmp(s_path, p) == 0) {
        return true;
    }
    if (strcmp(p, "/") == 0) {
        return false;                   /* everything is under the root */
    }
    return strncmp(s_path, p, n) == 0 && s_path[n] == '/';
}

/* ------------------------------------------------------------------ */
/* The listing                                                         */
/* ------------------------------------------------------------------ */

static int ent_cmp(const void *a, const void *b)
{
    const browser_entry_t *x = a;
    const browser_entry_t *y = b;

    /* Directories first, then by name without regard to case: the order a
     * browser is read in, and the order ls is not. */
    if (x->is_dir != y->is_dir) {
        return x->is_dir ? -1 : 1;
    }
    return strcasecmp(x->name, y->name);
}

/*
 * Walk the current directory into s_ents.
 *
 * The per-entry stat is what ls -l pays and it is paid here for the same
 * reason: the size and the file/directory distinction come from the metadata,
 * and readdir's d_type is DT_UNKNOWN on mounts that do not fill it in. The
 * alternative -- espix_fs_last_entry(), which reads the attributes the walk
 * already fetched -- exists only for FAT, so it is not the general answer.
 *
 * errno is cleared immediately before every readdir and read immediately after
 * a NULL, because readdir answers NULL both for "no more entries" and for a
 * failure and leaves errno alone in the first case. Getting this wrong reports
 * a truncated walk as a complete one, which is the failure mode ls documents.
 */
static void browser_scan(void)
{
    s_nplaces   = (int)espix_places(s_places, ESPIX_PLACES_MAX);
    s_nents     = 0;
    s_truncated = false;
    s_status[0] = '\0';
    s_sel       = 0;
    s_top       = 0;
    s_click_row = -1;

    DIR *d = opendir(s_path);
    if (d == NULL) {
        snprintf(s_status, sizeof(s_status), "%.120s: %.30s", s_path,
                 strerror(errno));
        espix_klog(ESPIX_KLOG_WARN, TAG, "cannot list %s: %s", s_path,
                   strerror(errno));
        return;
    }

    int read_errno = 0;

    while (1) {
        errno = 0;
        const struct dirent *e = readdir(d);
        if (e == NULL) {
            read_errno = errno;
            break;
        }
        if (e->d_name[0] == '\0') {
            continue;
        }
        if (s_nents >= BROWSER_MAX_ENTRIES) {
            s_truncated = true;
            break;
        }

        browser_entry_t *ent = &s_ents[s_nents];
        memset(ent, 0, sizeof(*ent));

        if (strlcpy(ent->name, e->d_name, sizeof(ent->name))
                >= sizeof(ent->name)) {
            ent->truncated = true;
        }
        ent->is_dir = (e->d_type == DT_DIR);

        char        child[ESPIX_PATH_MAX];
        struct stat st;

        /* The names go in with strlcpy/strlcat, so the worst case is a path
         * that was cut short -- which fails to stat rather than describing the
         * wrong file. `fits` is what says whether the stat is worth doing. */
        const bool fits = strlen(s_path) + 1 + strlen(e->d_name)
                              < sizeof(child);

        strlcpy(child, s_path, sizeof(child));
        if (strcmp(s_path, "/") != 0) {
            strlcat(child, "/", sizeof(child));
        }
        strlcat(child, e->d_name, sizeof(child));

        if (fits && stat(child, &st) == 0) {
            ent->is_dir = S_ISDIR(st.st_mode);
            ent->size   = (uint32_t)st.st_size;
        }
        s_nents++;
    }

    closedir(d);

    if (read_errno != 0) {
        snprintf(s_status, sizeof(s_status), "listing stopped: %.120s",
                 strerror(read_errno));
    }

    qsort(s_ents, (size_t)s_nents, sizeof(*s_ents), ent_cmp);
}

/* Keep the selected row on the screen, without moving the list when it is. */
static void sel_visible(const espix_window_t *w)
{
    const int vis = list_vis(w);

    if (vis <= 0) {
        s_top = 0;
        return;
    }
    if (s_sel < s_top) {
        s_top = s_sel;
    }
    if (s_sel >= s_top + vis) {
        s_top = s_sel - vis + 1;
    }
    if (s_top < 0) {
        s_top = 0;
    }
}

static void size_text(uint32_t n, char *out, size_t len)
{
    if (n < 1024u) {
        snprintf(out, len, "%u B", (unsigned)n);
    } else if (n < 1024u * 1024u) {
        snprintf(out, len, "%u K", (unsigned)(n / 1024u));
    } else {
        snprintf(out, len, "%u.%u M",
                 (unsigned)(n / (1024u * 1024u)),
                 (unsigned)(((n % (1024u * 1024u)) * 10u) / (1024u * 1024u)));
    }
}

/*
 * What opens where. An extension whitelist rather than a guess: nothing here
 * reads the file to decide what it is, so a file with no extension or an
 * unknown one is reported as having no handler rather than opened as something
 * it is not. These tables are where the audio and video viewers will register.
 */
static const char *const s_text_ext[] = {
    "txt", "md", "conf", "cfg", "ini", "log", "json", "csv",
    "c", "h", "sh", "py", "js",
};
#define TEXT_EXT_N ((int)(sizeof(s_text_ext) / sizeof(s_text_ext[0])))

static const char *const s_image_ext[] = { "jpg", "jpeg" };
#define IMAGE_EXT_N ((int)(sizeof(s_image_ext) / sizeof(s_image_ext[0])))

static bool name_has_ext(const char *name, const char *const *exts, int n)
{
    const char *dot = strrchr(name, '.');

    if (dot == NULL || dot[1] == '\0') {
        return false;
    }
    for (int i = 0; i < n; i++) {
        if (strcasecmp(dot + 1, exts[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool name_is_text(const char *name)
{
    return name_has_ext(name, s_text_ext, TEXT_EXT_N);
}

static bool name_is_image(const char *name)
{
    return name_has_ext(name, s_image_ext, IMAGE_EXT_N);
}

/* ------------------------------------------------------------------ */
/* Navigation                                                          */
/* ------------------------------------------------------------------ */

static void browser_go(const char *path);

/*
 * Open whatever row is under the pointer. Row 0 is the parent, the rest are the
 * listing.
 *
 * A directory is entered. A file is where stage two goes: this is the one place
 * that decides what opens it, and the extension table, the terminal window and
 * the media windows all land here rather than in the browser's input path.
 */
static void browser_activate(int row)
{
    if (row == 0) {
        char up[ESPIX_PATH_MAX];

        path_parent(s_path, up, sizeof(up));
        browser_go(up);
        return;
    }

    const int i = row - 1;
    if (i < 0 || i >= s_nents) {
        return;
    }

    const browser_entry_t *ent = &s_ents[i];

    if (ent->truncated) {
        snprintf(s_status, sizeof(s_status), "%.64s: name is too long to open",
                 ent->name);
        espix_window_repaint(s_win);
        return;
    }

    char child[ESPIX_PATH_MAX];
    path_child(s_path, ent->name, child, sizeof(child));

    if (ent->is_dir) {
        browser_go(child);
        return;
    }

    if (name_is_text(ent->name)) {
        /* The editor, in a terminal window of its own. Quoted, because this is
         * the one place a path is handed to a shell -- a name with a space in
         * it survives that much. */
        char cmd[ESPIX_PATH_MAX + 8];

        snprintf(cmd, sizeof(cmd), "nano '%.120s'", child);
        espix_desktop_open_terminal(cmd);
        return;
    }

    if (name_is_image(ent->name)) {
        espix_desktop_open_image(child);
        return;
    }

    /* An audio file or a video gets its player here, once there is one. Until
     * then, a file that does nothing at all is worse than one that says so. */
    snprintf(s_status, sizeof(s_status), "%.64s: no handler yet", ent->name);
    espix_window_repaint(s_win);
}

static void browser_go(const char *path)
{
    if (path == NULL || path[0] != '/') {
        return;
    }
    if (strcmp(path, s_path) != 0) {
        strlcpy(s_path, path, sizeof(s_path));
    }

    browser_scan();

    if (s_win != NULL) {
        espix_window_repaint(s_win);
    }
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

static void browser_draw(espix_window_t *w, espix_surface_t *s, espix_rect_t r,
                         void *ctx)
{
    (void)ctx;
    (void)r;

    const espix_rect_t c = espix_window_content(w);
    const espix_rect_t n = nav_rect(w);

    /* The whole panel every time, as settings does: a frame this small is not
     * worth a damage-tracking path, and idempotent drawing is what makes that
     * safe. */
    espix_surface_fill(s, c, WGT_PANE);
    espix_surface_fill(s, n, WGT_NAV);

    for (int i = 0; i < s_nplaces; i++) {
        espix_wgt_navrow(s, nav_row(w, i), s_places[i].label,
                         place_selected(i), false);
    }

    /* The path, as a heading so the rule under it does the separating. A path
     * too long for the pane shows its tail, which is the half that says where
     * you are. */
    char      shown[80];
    const int room = (path_rect(w).w - WGT_ROW_H) / WGT_TEXT_H;
    int       keep = room - 3;

    if (keep < 1) {
        keep = 1;
    }
    if (keep > 74) {
        keep = 74;                  /* what the buffer holds beside "..." */
    }
    if ((int)strlen(s_path) <= room) {
        strlcpy(shown, s_path, sizeof(shown));
    } else {
        snprintf(shown, sizeof(shown), "...%.74s",
                 s_path + strlen(s_path) - (size_t)keep);
    }
    espix_wgt_heading(s, path_rect(w), shown);

    const int vis = list_vis(w);

    for (int k = 0; k < vis; k++) {
        const int row = s_top + k;

        if (row > s_nents) {
            break;
        }

        char        right[16] = "";
        const char *label;

        if (row == 0) {
            label = "..";
        } else {
            const browser_entry_t *ent = &s_ents[row - 1];

            label = ent->name;
            if (!ent->is_dir) {
                size_text(ent->size, right, sizeof(right));
            }
        }
        espix_wgt_listrow(s, list_row(w, k), label,
                          right[0] != '\0' ? right : NULL,
                          row == s_sel, true, false);
    }

    if (s_status[0] != '\0') {
        espix_wgt_label(s, status_rect(w), s_status, false);
    } else {
        char line[48];

        if (s_truncated) {
            snprintf(line, sizeof(line), "%d shown, more not listed", s_nents);
        } else {
            snprintf(line, sizeof(line), "%d item%s", s_nents,
                     s_nents == 1 ? "" : "s");
        }
        espix_wgt_label(s, status_rect(w), line, true);
    }
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

#define DBLCLICK_US (400 * 1000)

static void browser_pointer(espix_window_t *w, int x, int y, uint8_t buttons,
                            void *ctx)
{
    (void)ctx;

    if (buttons == 0) {
        return;                         /* act on the press, not the release */
    }

    for (int i = 0; i < s_nplaces; i++) {
        if (espix_wgt_hit(nav_row(w, i), x, y)) {
            browser_go(s_places[i].path);
            return;
        }
    }

    const int vis = list_vis(w);

    for (int k = 0; k < vis; k++) {
        const int row = s_top + k;

        if (row > s_nents || !espix_wgt_hit(list_row(w, k), x, y)) {
            continue;
        }

        /* Double-click to open, and only on the same row: a second click on a
         * different file moves the selection instead of running something. */
        const int64_t now = esp_timer_get_time();
        const bool    dbl = (row == s_click_row)
                         && (now - s_click_us < DBLCLICK_US);

        s_click_row = dbl ? -1 : row;
        s_click_us  = now;

        s_sel = row;

        if (dbl) {
            browser_activate(row);
            return;
        }
        sel_visible(w);
        espix_window_repaint(w);
        return;
    }
}

static void browser_key(espix_window_t *w, uint32_t keysym, bool down, void *ctx)
{
    (void)ctx;

    if (!down) {
        return;
    }

    const int vis   = list_vis(w);
    const int last  = s_nents;
    const int step  = (vis > 0) ? vis : 1;
    bool      moved = false;

    switch (keysym) {
    case 0xFF52:                                        /* Up */
        if (s_sel > 0)    { s_sel--; moved = true; }
        break;
    case 0xFF54:                                        /* Down */
        if (s_sel < last) { s_sel++; moved = true; }
        break;
    case 0xFF55:                                        /* Page Up */
        s_sel -= step;
        if (s_sel < 0) { s_sel = 0; }
        moved = true;
        break;
    case 0xFF56:                                        /* Page Down */
        s_sel += step;
        if (s_sel > last) { s_sel = last; }
        moved = true;
        break;
    case 0xFF50:                                        /* Home */
        s_sel = 0;
        moved = true;
        break;
    case 0xFF57:                                        /* End */
        s_sel = last;
        moved = true;
        break;
    case 0xFF0D:                                        /* Return */
        browser_activate(s_sel);
        return;
    case 0xFF08: {                                      /* Backspace: up */
        char up[ESPIX_PATH_MAX];

        path_parent(s_path, up, sizeof(up));
        browser_go(up);
        return;
    }
    case 0xFFC2:                                        /* F5: refresh */
        browser_go(s_path);
        return;
    default:
        return;
    }

    if (moved) {
        sel_visible(w);
        espix_window_repaint(w);
    }
}

/* ------------------------------------------------------------------ */
/* The window                                                          */
/* ------------------------------------------------------------------ */

espix_window_t *espix_browser_open(const char *path)
{
    if (s_win == NULL) {
        if (s_ents == NULL) {
            s_ents = heap_caps_malloc(BROWSER_MAX_ENTRIES * sizeof(*s_ents),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (s_ents == NULL) {
                s_ents = malloc(BROWSER_MAX_ENTRIES * sizeof(*s_ents));
            }
            if (s_ents == NULL) {
                espix_klog(ESPIX_KLOG_WARN, TAG, "no memory for a listing");
                return NULL;
            }
        }

        strlcpy(s_path, (path != NULL) ? path : default_path(), sizeof(s_path));

        s_win = espix_window_new(100, 60, 620, 440, "files");
        if (s_win == NULL) {
            free(s_ents);
            s_ents = NULL;
            return NULL;
        }
        espix_window_set_draw(s_win, browser_draw);
        espix_window_set_key(s_win, browser_key);
        espix_window_set_pointer(s_win, browser_pointer);

        browser_scan();
        espix_window_repaint(s_win);
    } else if (path != NULL && strcmp(path, s_path) != 0) {
        browser_go(path);
    }
    return s_win;
}

void espix_browser_forget(void)
{
    s_win = NULL;                       /* its pixels went with the window */

    /* The listing goes with it too: reopening rescans, and holding it for a
     * window that no longer exists is the memory the next app wants. */
    free(s_ents);
    s_ents      = NULL;
    s_nents     = 0;
    s_sel       = 0;
    s_top       = 0;
    s_truncated = false;
    s_status[0] = '\0';
    s_click_row = -1;
}

espix_window_t *espix_browser_window(void)
{
    return s_win;
}
