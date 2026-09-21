/*
 * \`upgrade\`: get a kernel image into /boot and hand it to the loader.
 *
 * Nothing here writes flash. The image is archived as a file, checked, and
 * queued; the loader installs it on the next boot. See docs/OTA.md section 11.
 */
#include <dirent.h>
#include <stdio.h>
#include <string.h>

#include "esp_ota_ops.h"

#include "espix_cmds_priv.h"
#include "espix_kernel.h"
#include "espix_net.h"
#include "espix_ota.h"

static const char *state_name(int state)
{
    switch ((esp_ota_img_states_t)state) {
    case ESP_OTA_IMG_NEW:            return "NEW";
    case ESP_OTA_IMG_PENDING_VERIFY: return "PENDING_VERIFY";
    case ESP_OTA_IMG_VALID:          return "VALID";
    case ESP_OTA_IMG_INVALID:        return "INVALID";
    case ESP_OTA_IMG_ABORTED:        return "ABORTED";
    case ESP_OTA_IMG_UNDEFINED:      return "-";
    default:                         return "unknown";
    }
}

static int show_slots(espix_session_t *s)
{
    espix_ota_slot_t slots[4];
    const size_t n = espix_ota_slots(slots, sizeof(slots) / sizeof(slots[0]));

    char good[ESPIX_OTA_NAME_MAX] = {0};
    char prev[ESPIX_OTA_NAME_MAX] = {0};
    char pend[ESPIX_OTA_NAME_MAX] = {0};
    espix_ota_state(good, sizeof(good), prev, sizeof(prev), pend, sizeof(pend));

    if (n == 0) {
        espix_eprintf(s, "upgrade: no application partitions found\n");
        return 1;
    }

    espix_printf(s, "%-8s %-10s %-10s %-8s %-14s %-8s %s\n",
                 "SLOT", "OFFSET", "SIZE", "ROLE", "STATE", "VERSION", "BUILD");
    for (size_t i = 0; i < n; i++) {
        const espix_ota_slot_t *t = &slots[i];

        espix_printf(s, "%-8s 0x%06x   0x%06x   %-8s %-14s %-8s %s\n",
                     t->name, (unsigned)t->offset, (unsigned)t->size,
                     (t->role[0] != 0) ? t->role : "-",
                     state_name(t->state),
                     (t->version[0] != 0) ? t->version : "-",
                     (t->build[0] != 0) ? t->build : "-");
    }

    /* What the loader would install, which is the half a slot table cannot
     * show: images that are files until the loader writes one. */
    DIR *d = opendir(ESPIX_OTA_BOOT_DIR);
    if (d == NULL) {
        return 0;
    }

    espix_printf(s, "\n%-32s %s\n", "IMAGE", "ROLE");
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const size_t l = strlen(de->d_name);

        if (strncmp(de->d_name, "espix-", 6) != 0 || l < 5 ||
            strcmp(de->d_name + l - 4, ".bin") != 0) {
            continue;
        }
        const char *role = (strcmp(de->d_name, pend) == 0) ? "pending"
                         : (strcmp(de->d_name, good) == 0) ? "good"
                         : (strcmp(de->d_name, prev) == 0) ? "previous"
                         : "-";
        espix_printf(s, "%-32s %s\n", de->d_name, role);
    }
    closedir(d);
    return 0;
}

typedef struct {
    espix_session_t *s;
    int              last_decile;
} progress_ctx_t;

#define PROGRESS_STEP (256 * 1024)

static void report_progress(void *ctx, size_t done, size_t total)
{
    progress_ctx_t *p = ctx;

    if (total > 0) {
        const int pct = (int)((done * 100) / total);
        if (pct / 10 != p->last_decile) {
            p->last_decile = pct / 10;
            espix_printf(p->s, "\r  downloading %3d%%", pct);
        }
        if (done == total) {
            espix_printf(p->s, "\n");
        }
        return;
    }

    if (done / PROGRESS_STEP != 0) {
        espix_printf(p->s, "\r  %u KiB downloaded", (unsigned)(done / 1024));
    }
}

/* Last path component of a URL, without any query or fragment. */
static void url_basename(const char *url, char *out, size_t len)
{
    const char *p = strrchr(url, '/');
    p = (p != NULL) ? p + 1 : url;

    size_t i = 0;
    while (p[i] != 0 && p[i] != '?' && p[i] != '#' && i + 1 < len) {
        out[i] = p[i];
        i++;
    }
    out[i] = 0;
    if (i == 0) {
        strlcpy(out, "kernel.bin", len);
    }
}

/* Download, verify, queue: the tail every install path shares. */
static int fetch_and_queue(espix_session_t *s, const char *url, const char *name,
                           const char *sha256)
{
    char err[192];
    progress_ctx_t p = { .s = s, .last_decile = -1 };

    espix_printf(s, "upgrade: fetching %s\n", url);
    const esp_err_t e = espix_ota_download(url, name, sha256,
                                           report_progress, &p, err, sizeof(err));
    if (e != ESP_OK) {
        espix_eprintf(s, "upgrade: %s\n", (err[0] != 0) ? err : esp_err_to_name(e));
        return 1;
    }
    espix_printf(s, "upgrade: %s\n", err);

    if (espix_ota_queue(name, err, sizeof(err)) != ESP_OK) {
        espix_eprintf(s, "upgrade: %s\n", (err[0] != 0) ? err : "cannot queue it");
        return 1;
    }
    espix_printf(s, "upgrade: %s; run 'reboot' to install it\n", err);
    return 0;
}

/* The repo half: check the manifest, then download what it points at. */
static int run_manifest(espix_session_t *s, bool check_only, bool assume_yes)
{
    const char *src = espix_ota_source();
    char err[192];

    espix_printf(s, "upgrade: checking %s\n", src);

    /* Say why before trying, rather than after a long timeout: with no default
     * route there is nothing to reach and the reason is knowable here. */
    char ifname[ESPIX_IF_NAME_MAX];
    uint32_t gw;
    if (!espix_net_default_route(ifname, sizeof(ifname), &gw)) {
        espix_eprintf(s, "upgrade: no route to the network\n");
        return 1;
    }

    espix_ota_manifest_t m;
    const esp_err_t e = espix_ota_check(src, &m, err, sizeof(err));
    if (e != ESP_OK) {
        espix_eprintf(s, "upgrade: %s\n", (err[0] != '\0') ? err : esp_err_to_name(e));
        return 1;
    }

    switch (espix_ota_compare(&m)) {
    case ESPIX_OTA_UPDATE_UP_TO_DATE:
        espix_printf(s, "espix %s is up to date (build %s)\n",
                     espix_version(), espix_build_id());
        return 0;
    case ESPIX_OTA_UPDATE_UNKNOWN:
        espix_eprintf(s, "upgrade: cannot tell whether %s is newer\n", m.version);
        return 1;
    default:
        break;
    }

    espix_printf(s, "update available: espix %s", m.version);
    if (m.build[0] != '\0') {
        espix_printf(s, " (build %.9s)", m.build);
    }
    espix_printf(s, "\n");

    if (check_only) {
        return 1;           /* 1 means "there is one", for a script */
    }

    if (!espix_ota_meets_min(&m)) {
        espix_eprintf(s, "upgrade: this update needs espix %s or newer; this is %s\n",
                      m.min_version, espix_version());
        return 1;
    }

    if (!assume_yes) {
        char ans[8];
        if (s == NULL || s->read_line == NULL ||
            s->read_line(s, "Proceed? [y/N] ", ans, sizeof(ans)) < 0) {
            espix_eprintf(s, "upgrade: no answer\n");
            return 1;
        }
        if (ans[0] != 'y' && ans[0] != 'Y') {
            espix_printf(s, "upgrade: cancelled\n");
            return 1;
        }
    }

    char name[ESPIX_OTA_NAME_MAX];
    if (m.build[0] != '\0') {
        snprintf(name, sizeof(name), "espix-%s-%.9s.bin", m.version, m.build);
    } else {
        snprintf(name, sizeof(name), "espix-%s.bin", m.version);
    }
    return fetch_and_queue(s, m.url, name, m.sha256);
}

static int cmd_upgrade(espix_session_t *s, int argc, char **argv)
{
    if (!espix_ota_enabled()) {
        espix_eprintf(s, "upgrade: this build has no firmware update support\n");
        return 1;
    }

    if (argc == 2 && strcmp(argv[1], "--slots") == 0) {
        return show_slots(s);
    }
    if (argc == 2 && strcmp(argv[1], "--check") == 0) {
        return run_manifest(s, true, false);
    }

    if (s == NULL || s->uid != 0) {
        espix_eprintf(s, "upgrade: only root can install a firmware image\n");
        return 1;
    }

    if (!espix_ota_available()) {
        espix_eprintf(s, "upgrade: this image has no loader slot (ota1) to install from\n");
        return 1;
    }

    if (argc == 1) {
        return run_manifest(s, false, false);
    }
    if (argc == 2 && (strcmp(argv[1], "-y") == 0 || strcmp(argv[1], "--yes") == 0)) {
        return run_manifest(s, false, true);
    }

    if (argc == 3 && strcmp(argv[1], "--file") == 0) {
        char name[ESPIX_OTA_NAME_MAX];
        char err[192];

        espix_printf(s, "upgrade: adopting %s\n", argv[2]);
        if (espix_ota_adopt(argv[2], name, sizeof(name), err, sizeof(err)) != ESP_OK) {
            espix_eprintf(s, "upgrade: %s\n", (err[0] != 0) ? err : "cannot read it");
            return 1;
        }
        espix_printf(s, "upgrade: %s\n", err);

        if (espix_ota_queue(name, err, sizeof(err)) != ESP_OK) {
            espix_eprintf(s, "upgrade: %s\n", (err[0] != 0) ? err : "cannot queue it");
            return 1;
        }
        espix_printf(s, "upgrade: %s; run 'reboot' to install it\n", err);
        return 0;
    }

    if (argc == 2 && strcmp(argv[1], "--rollback") == 0) {
        char name[ESPIX_OTA_NAME_MAX];
        char err[192];

        if (!espix_ota_previous(name, sizeof(name))) {
            espix_eprintf(s, "upgrade: there is no previous image to roll back to\n");
            return 1;
        }
        espix_printf(s, "upgrade: rolling back to %s\n", name);
        if (espix_ota_queue(name, err, sizeof(err)) != ESP_OK) {
            espix_eprintf(s, "upgrade: %s\n", (err[0] != 0) ? err : "cannot queue it");
            return 1;
        }
        espix_printf(s, "upgrade: %s; run 'reboot' to install it\n", err);
        return 0;
    }

    if (argc == 2 && argv[1][0] != '-') {
        char name[ESPIX_OTA_NAME_MAX];
        url_basename(argv[1], name, sizeof(name));
        return fetch_and_queue(s, argv[1], name, NULL);
    }

    espix_eprintf(s, "usage: upgrade [-y|--check] | --slots | --file <path> | "
                     "--rollback | <url>\n");
    return 1;
}

static espix_cmd_t s_ota_cmds[] = {
    { .name = "upgrade", .fn = cmd_upgrade,
      .help = "fetch a kernel image, queue it for the loader",
      .usage = "upgrade [-y|--check] | --slots | --file <path> | --rollback | <url>",
      .stack = 8192 },
};

void espix_cmds_register_ota(void)
{
    espix_cmds_register_table(s_ota_cmds,
                              sizeof(s_ota_cmds) / sizeof(s_ota_cmds[0]));
}
