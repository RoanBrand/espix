/*
 * The greeting every session opens with, and the `motd` command that reprints
 * it — logo on the left, system facts on the right, in the manner of fastfetch.
 *
 * It lives here, as a command, because this is the one component that already
 * sees everything it needs to report: the kernel, the filesystem, the network
 * and the heap. Putting it in espix_shell or espix_ssh would mean those
 * components acquiring dependencies purely to draw a banner, inverting the
 * layering. Sessions reach it through the ordinary dispatch, so the transports
 * gain nothing they did not already have.
 */

#include <stdio.h>
#include <string.h>

#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"

#include "espix_auth.h"
#include "espix_cmds_priv.h"
#include "espix_fs.h"
#include "espix_kernel.h"
#include "espix_net.h"

/*
 * Rendered as a fixed-width left column so the facts beside it stay aligned
 * without measuring anything. Trailing spaces are part of the art.
 */
static const char *const LOGO[] = {
    "  ___  ___ _ __ (_)_  __ ",
    " / _ \\/ __| '_ \\| \\ \\/ / ",
    "|  __/\\__ \\ |_) | |>  <  ",
    " \\___||___/ .__/|_/_/\\_\\ ",
    "          |_|            ",
};

#define LOGO_LINES  (sizeof(LOGO) / sizeof(LOGO[0]))
#define LOGO_WIDTH  25
#define GAP         "    "

/* Kept modest so the widest row still fits an 80-column terminal. */
#define VALUE_MAX   48

#define ANSI_LOGO   "\033[36m"      /* cyan */
#define ANSI_LABEL  "\033[1m"       /* bold */
#define ANSI_WARN   "\033[33m"      /* yellow */
#define ANSI_RESET  "\033[0m"

typedef struct {
    espix_session_t *s;
    size_t           line;          /* how many rows have been emitted */
} motd_ctx_t;

/*
 * Emit one row: the next line of the logo, then the given text. Once the logo
 * is exhausted the left column becomes blank padding, which is what lets the
 * facts run longer than the art.
 */
static void row(motd_ctx_t *ctx, const char *text)
{
    const bool  ansi = (ctx->s != NULL && ctx->s->ansi);
    const char *art  = (ctx->line < LOGO_LINES) ? LOGO[ctx->line] : NULL;

    if (art != NULL) {
        espix_printf(ctx->s, "%s%s%s" GAP "%s\n",
                     ansi ? ANSI_LOGO : "", art, ansi ? ANSI_RESET : "",
                     text != NULL ? text : "");
    } else if (text != NULL && text[0] != '\0') {
        espix_printf(ctx->s, "%*s" GAP "%s\n", LOGO_WIDTH, "", text);
    } else {
        espix_printf(ctx->s, "\n");
    }

    ctx->line++;
}

static void row_fact(motd_ctx_t *ctx, const char *label, const char *value)
{
    const bool ansi = (ctx->s != NULL && ctx->s->ansi);
    char       text[VALUE_MAX + 32];

    snprintf(text, sizeof(text), "%s%-9s%s %s",
             ansi ? ANSI_LABEL : "", label, ansi ? ANSI_RESET : "", value);
    row(ctx, text);
}

/* ------------------------------------------------------------------ */
/* The facts                                                           */
/* ------------------------------------------------------------------ */

static void fact_host(char *out, size_t len)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    snprintf(out, len, "%s rev%d.%d, %d core%s",
             espix_chip_model(), chip.revision / 100, chip.revision % 100,
             chip.cores, chip.cores == 1 ? "" : "s");
}

/*
 * One heap per row. Packing both onto a single line risked running past 80
 * columns once the numbers grew to five digits, and splitting them costs a row
 * the logo has to spare anyway.
 */
static void fact_heap(char *out, size_t len, uint32_t caps)
{
    multi_heap_info_t info;
    heap_caps_get_info(&info, caps);

    const size_t total = info.total_free_bytes + info.total_allocated_bytes;
    if (total == 0) {
        out[0] = '\0';          /* capability absent on this board */
        return;
    }

    snprintf(out, len, "%uK / %uK",
             (unsigned)(info.total_allocated_bytes / 1024),
             (unsigned)(total / 1024));
}

static void fact_storage(char *out, size_t len)
{
    espix_fs_info_t fs;

    if (!espix_fs_is_mounted() || espix_fs_stat_root(&fs) != ESP_OK) {
        strlcpy(out, "not mounted", len);
        return;
    }

    snprintf(out, len, "%uK / %uK on /",
             (unsigned)(fs.used_bytes / 1024),
             (unsigned)(fs.total_bytes / 1024));
}

/* First interface carrying an address. `lo` always has one and never says
 * anything useful, so it is skipped. */
static void fact_network(char *out, size_t len)
{
    espix_ifinfo_t ifs[4];
    const size_t   n = espix_net_iflist(ifs, sizeof(ifs) / sizeof(ifs[0]));

    for (size_t i = 0; i < n; i++) {
        if (ifs[i].kind == ESPIX_IF_LO || !ifs[i].has_addr || !ifs[i].up) {
            continue;
        }
        char ip[ESPIX_IP4STR_MAX];
        snprintf(out, len, "%s %s/%d", ifs[i].name,
                 espix_net_ip4str(ifs[i].ip, ip, sizeof(ip)),
                 espix_net_prefix_len(ifs[i].netmask));
        return;
    }

    strlcpy(out, "not connected", len);
}

/* ------------------------------------------------------------------ */

void espix_cmds_print_greeting(espix_session_t *s)
{
    motd_ctx_t ctx  = { .s = s };
    const bool ansi = (s != NULL && s->ansi);
    char       value[VALUE_MAX];
    char       head[VALUE_MAX + 32];

    espix_printf(s, "\n");

    /* Identity line, then a rule the width of it. The console has no login
     * step, so there is no user to name there. */
    /* Empty if networking never came up; the target name is a better answer
     * than a blank. */
    const char *host = espix_net_hostname();
    if (host == NULL || host[0] == '\0') {
        host = espix_target();
    }
    const bool  named = (s != NULL && s->user[0] != '\0');

    const int width = named ? snprintf(value, sizeof(value), "%s@%s", s->user, host)
                            : snprintf(value, sizeof(value), "%s", host);

    snprintf(head, sizeof(head), "%s%s%s",
             ansi ? ANSI_LABEL : "", value, ansi ? ANSI_RESET : "");
    row(&ctx, head);

    char rule[VALUE_MAX + 1];
    int  i = 0;
    for (; i < width && i < (int)sizeof(rule) - 1; i++) {
        rule[i] = '-';
    }
    rule[i] = '\0';
    row(&ctx, rule);

    snprintf(value, sizeof(value), "espix %s", espix_version());
    row_fact(&ctx, "OS", value);

    fact_host(value, sizeof(value));
    row_fact(&ctx, "Host", value);

    snprintf(value, sizeof(value), "ESP-IDF %s", esp_get_idf_version());
    row_fact(&ctx, "Kernel", value);

    espix_uptime_str(value, sizeof(value));
    row_fact(&ctx, "Uptime", value);

    snprintf(value, sizeof(value), "espix sh (%s)",
             (s != NULL && s->name != NULL) ? s->name : "?");
    row_fact(&ctx, "Shell", value);

    fact_heap(value, sizeof(value), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    row_fact(&ctx, "Memory", value);

    fact_heap(value, sizeof(value), MALLOC_CAP_SPIRAM);
    if (value[0] != '\0') {
        row_fact(&ctx, "PSRAM", value);
    }

    fact_storage(value, sizeof(value));
    row_fact(&ctx, "Storage", value);

    fact_network(value, sizeof(value));
    row_fact(&ctx, "Network", value);

    /* Any logo lines not yet consumed still have to be drawn. */
    while (ctx.line < LOGO_LINES) {
        row(&ctx, NULL);
    }

    espix_printf(s, "\n");

    /*
     * Warn on both transports, not just SSH. Serial access is the *less*
     * authenticated of the two, so telling only remote users was backwards.
     */
    if (espix_auth_is_default()) {
        espix_printf(s, "%swarning: '%s' still has the default password; "
                        "run 'passwd'%s\n",
                     ansi ? ANSI_WARN : "", ESPIX_AUTH_DEFAULT_USER,
                     ansi ? ANSI_RESET : "");
    }

    espix_printf(s, "Type 'help' for the command list. "
                    "TAB completes, UP/DOWN walks history.\n\n");
}

static int cmd_motd(espix_session_t *s, int argc, char **argv)
{
    (void)argc;
    (void)argv;

    espix_cmds_print_greeting(s);
    return 0;
}

static espix_cmd_t s_motd_cmds[] = {
    { .name = "motd", .fn = cmd_motd,
      .help = "print the login greeting", .usage = "motd" },
};

void espix_cmds_register_motd(void)
{
    espix_cmds_register_table(s_motd_cmds,
                              sizeof(s_motd_cmds) / sizeof(s_motd_cmds[0]));
}
