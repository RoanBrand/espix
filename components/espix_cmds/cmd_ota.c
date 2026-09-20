/*
 * `upgrade`: write a firmware image into the passive application slot and
 * point the bootloader at it.
 *
 * Network sources (a manifest over HTTPS, and pulling from the dev machine)
 * come next; --file is the local half, and the one that proves slot selection
 * and rollback without TLS in the picture.
 */
#include <stdio.h>
#include <string.h>

#include "esp_ota_ops.h"

#include "espix_cmds_priv.h"
#include "espix_ota.h"

static const char *state_name(int state)
{
    switch ((esp_ota_img_states_t)state) {
    case ESP_OTA_IMG_NEW:            return "NEW";
    case ESP_OTA_IMG_PENDING_VERIFY: return "PENDING_VERIFY";
    case ESP_OTA_IMG_VALID:          return "VALID";
    case ESP_OTA_IMG_INVALID:        return "INVALID";
    case ESP_OTA_IMG_ABORTED:        return "ABORTED";
    case ESP_OTA_IMG_UNDEFINED:      return "UNDEFINED";
    default:                         return "unknown";
    }
}

static int show_slots(espix_session_t *s)
{
    espix_ota_slot_t slots[4];
    const size_t n = espix_ota_slots(slots, sizeof(slots) / sizeof(slots[0]));

    if (n == 0) {
        espix_eprintf(s, "upgrade: no application partitions found\n");
        return 1;
    }

    espix_printf(s, "%-8s %-10s %-10s %-14s %s\n",
                 "SLOT", "OFFSET", "SIZE", "STATE", "BOOT");
    for (size_t i = 0; i < n; i++) {
        const espix_ota_slot_t *t = &slots[i];
        char boot[20] = "";

        if (t->active) {
            strlcpy(boot, "running", sizeof(boot));
        }
        if (t->next) {
            strlcat(boot, t->active ? " + next" : "next", sizeof(boot));
        }

        espix_printf(s, "%-8s 0x%06x   0x%06x   %-14s %s\n",
                     t->name, (unsigned)t->offset, (unsigned)t->size,
                     state_name(t->state), boot);
    }
    return 0;
}

typedef struct {
    espix_session_t *s;
    int              last_decile;
    size_t           last_step;
} progress_ctx_t;

#define PROGRESS_STEP (256 * 1024)

static void report_progress(void *ctx, size_t done, size_t total)
{
    progress_ctx_t *p = ctx;

    if (total > 0) {
        const int pct = (int)((done * 100) / total);
        if (pct / 10 != p->last_decile) {
            p->last_decile = pct / 10;
            espix_printf(p->s, "\r  writing %3d%%", pct);
        }
        if (done == total) {
            espix_printf(p->s, "\n");
        }
        return;
    }

    /* Unknown length (stdin): count in steps rather than invent a percentage. */
    if (done / PROGRESS_STEP != p->last_step) {
        p->last_step = done / PROGRESS_STEP;
        espix_printf(p->s, "\r  %u KiB written", (unsigned)(done / 1024));
    }
}

/* The repo half: check the manifest, then install what it points at. */
static int run_manifest(espix_session_t *s, bool check_only, bool assume_yes)
{
    const char *src = espix_ota_source();
    char err[160];

    espix_printf(s, "upgrade: checking %s\n", src);

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

    /* A release can refuse to be installed on an espix too old to run it. */
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

    char ierr[160];
    progress_ctx_t p = { .s = s, .last_decile = -1, .last_step = (size_t)-1 };

    espix_printf(s, "upgrade: fetching %s\n", m.url);
    const esp_err_t ie = espix_ota_install_url(m.url, report_progress, &p,
                                               ierr, sizeof(ierr));
    if (ie != ESP_OK) {
        espix_eprintf(s, "upgrade: %s\n", (ierr[0] != '\0') ? ierr : esp_err_to_name(ie));
        return 1;
    }
    espix_printf(s, "upgrade: %s\n", ierr);
    espix_printf(s, "run 'reboot' to start it\n");
    return 0;
}

static int cmd_upgrade(espix_session_t *s, int argc, char **argv)
{
    if (!espix_ota_enabled()) {
        espix_eprintf(s, "upgrade: this build has no firmware update support\n");
        return 1;
    }
    /* Reading the slot table is harmless, so it is not root-only; writing one
     * is, and that check sits below. */
    if (argc == 2 && strcmp(argv[1], "--slots") == 0) {
        return show_slots(s);
    }

    /* --check only reads, so it is not root-only either. */
    if (argc == 2 && strcmp(argv[1], "--check") == 0) {
        return run_manifest(s, true, false);
    }

    if (s == NULL || s->uid != 0) {
        espix_eprintf(s, "upgrade: only root can write a firmware slot\n");
        return 1;
    }

    /* No argument (or -y) is the repo path: check, then ask, then install. */
    if (argc == 1) {
        return run_manifest(s, false, false);
    }
    if (argc == 2 && (strcmp(argv[1], "-y") == 0 || strcmp(argv[1], "--yes") == 0)) {
        return run_manifest(s, false, true);
    }

    if (argc == 3 && strcmp(argv[1], "--file") == 0) {
        char err[128];
        progress_ctx_t p = { .s = s, .last_decile = -1, .last_step = (size_t)-1 };

        espix_printf(s, "upgrade: writing %s\n", argv[2]);
        const esp_err_t e = espix_ota_install_file(argv[2], report_progress,
                                                   &p, err, sizeof(err));
        if (e != ESP_OK) {
            espix_eprintf(s, "upgrade: %s\n",
                          (err[0] != '\0') ? err : esp_err_to_name(e));
            return 1;
        }
        espix_printf(s, "upgrade: %s\n", err);
        espix_printf(s, "run 'reboot' to start it\n");
        return 0;
    }

    if (argc == 2 && argv[1][0] != '-') {
        char err[160];
        progress_ctx_t p = { .s = s, .last_decile = -1, .last_step = (size_t)-1 };

        espix_printf(s, "upgrade: fetching %s\n", argv[1]);
        const esp_err_t e = espix_ota_install_url(argv[1], report_progress,
                                                  &p, err, sizeof(err));
        if (e != ESP_OK) {
            espix_eprintf(s, "upgrade: %s\n",
                          (err[0] != '\0') ? err : esp_err_to_name(e));
            return 1;
        }
        espix_printf(s, "upgrade: %s\n", err);
        espix_printf(s, "run 'reboot' to start it\n");
        return 0;
    }

    espix_eprintf(s, "usage: upgrade [-y|--check] | --slots | --file <path> | <url>\n");
    return 1;
}

static espix_cmd_t s_ota_cmds[] = {
    { .name = "upgrade", .fn = cmd_upgrade,
      .help = "write a firmware image to the passive slot",
      .usage = "upgrade [-y|--check] | --slots | --file <path> | <url>",
      .stack = 8192 },
};

void espix_cmds_register_ota(void)
{
    espix_cmds_register_table(s_ota_cmds,
                              sizeof(s_ota_cmds) / sizeof(s_ota_cmds[0]));
}
