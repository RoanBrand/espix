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
 * The logo is two marks in one column: the wordmark, and the antenna beneath
 * it. Each is drawn in a single colour, which is why they are separate arrays
 * rather than one array of individually-coloured rows -- nothing here wants a
 * row to differ from its neighbours.
 *
 * Every row of both is exactly LOGO_WIDTH *display columns*, so the facts
 * beside them stay aligned without anything measuring. Columns, not bytes: the
 * antenna's box-drawing characters are three bytes each, so those rows run
 * 50-66 bytes wide. Count glyphs when editing, never strlen(). Trailing spaces
 * are part of the art.
 *
 * The antenna is also the first non-ASCII espix puts on the wire -- the rest of
 * the tree's non-ASCII is em-dashes in comments -- so the greeting now assumes
 * a UTF-8 terminal where before it assumed nothing. There is no ASCII fallback
 * because there is nothing to switch on: `ansi` describes colour support and
 * says nothing about the character set.
 */
static const char *const WORDMARK[] = {
    "  ___  ___ _ __ (_)_  __ ",
    " / _ \\/ __| '_ \\| \\ \\/ / ",
    "|  __/\\__ \\ |_) | |>  <  ",
    " \\___||___/ .__/|_/_/\\_\\ ",
    "          |_|            ",
};

/*
 * A PCB antenna, as etched on the module this runs on.
 *
 * The trace forks at the T and two verticals leave the bottom edge, which is
 * drawn that way on purpose: the ESP32 module's antenna is an inverted-F, so
 * alongside the meander there really is a feed line and a shorting stub. It is
 * not a plain serpentine that lost its way, and should not be "corrected" into
 * one.
 *
 * It sits directly under the wordmark with no separating row: the wordmark's
 * last line is nearly all whitespace already, so a blank row between them opens
 * a gap wide enough to read as two unrelated pictures.
 */
static const char *const ANTENNA[] = {
    "   ┏━━┓  ┏━━┓  ┏━━┳━━┓   ",
    "   ┃  ┃  ┃  ┃  ┃  ┃  ┃   ",
    "   ┃  ┗━━┛  ┗━━┛  ┃  ┃   ",
};

#define WORDMARK_LINES  (sizeof(WORDMARK) / sizeof(WORDMARK[0]))
#define ANTENNA_LINES   (sizeof(ANTENNA) / sizeof(ANTENNA[0]))
#define LOGO_LINES      (WORDMARK_LINES + ANTENNA_LINES)
#define LOGO_WIDTH  25
#define GAP         "    "

/* Kept modest so the widest row still fits an 80-column terminal. */
#define VALUE_MAX   48

#define ANSI_LOGO   "\033[36m"          /* cyan */
#define ANSI_LABEL  "\033[1m"           /* bold */
#define ANSI_WARN   "\033[33m"          /* yellow */
#define ANSI_RESET  "\033[0m"

/*
 * Gold, for the antenna: the ENIG plating a module's antenna is actually
 * finished in, which is a pale bright yellow against the black solder mask
 * rather than the red-brown of bare copper. 222 is (255,215,135); the tan 179
 * (215,175,95) that reads as "copper" on paper looks like mud on a terminal.
 *
 * Still deliberately clear of the yellow ANSI_WARN uses a few rows below, so
 * bare trace and "your password is still the default" do not read as the same
 * colour.
 *
 * Bold, and deliberately so. The glyphs are already the HEAVY box-drawing
 * variants (U+250F, U+2501 and friends); SGR 1 renders those in the font's bold
 * weight on top of that, which is what gives the trace its thickness. Drop the
 * 1 and it thins out to a hairline. The wordmark beside it stays unbolded --
 * ASCII art built from underscores and slashes gains nothing from weight, and
 * the etched trace reading heavier than the letters is the point.
 *
 * The one 256-colour sequence espix emits. A terminal that does not know it
 * drops the sequence rather than printing it, so the cost of being wrong is a
 * monochrome antenna.
 */
#define ANSI_ANTENNA "\033[1;38;5;222m"

/*
 * The nth row of the logo and the colour its block is drawn in, or NULL once
 * the art runs out -- which is what tells row() to switch to blank padding.
 */
static const char *logo_row(size_t n, const char **color)
{
    if (n < WORDMARK_LINES) {
        *color = ANSI_LOGO;
        return WORDMARK[n];
    }
    n -= WORDMARK_LINES;

    if (n < ANTENNA_LINES) {
        *color = ANSI_ANTENNA;
        return ANTENNA[n];
    }

    return NULL;
}

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
    const bool  ansi   = (ctx->s != NULL && ctx->s->ansi);
    const char *color  = NULL;
    const char *art    = logo_row(ctx->line, &color);

    if (art != NULL) {
        espix_printf(ctx->s, "%s%s%s" GAP "%s\n",
                     ansi ? color : "", art, ansi ? ANSI_RESET : "",
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
