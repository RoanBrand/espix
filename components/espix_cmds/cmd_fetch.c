/*
 * fetch: pull a file from an HTTP(S) URL into the filesystem.
 *
 * This exists because a game's data is not the kernel's business. espix ships
 * the program; the data a program needs is fetched, and the same downloader
 * installs a kernel -- espix_net_fetch(), which shares esp_http_client, mbedtls
 * and the certificate bundle with 'upgrade'. So this file is only the part that
 * talks to a person: it names the URL, reports the progress, and turns the
 * downloader's answer into something a user can act on.
 *
 *   fetch <url> <path>
 *
 * Room is checked before a byte is written and again against the response's
 * Content-Length, because a 4 MB download into a 3 MB hole otherwise fails at
 * the worst possible moment. When there is not room, the message says where to
 * get some back: on this system the usual answer is an old kernel still
 * sitting in /boot.
 */
#include <stdio.h>
#include <string.h>

#include "espix_cmds.h"
#include "espix_cmds_priv.h"
#include "espix_net.h"

/*
 * A line every tenth of the way, not every chunk. A 4 MB transfer arrives in
 * thousands of pieces and one line each would be a wall of text that scrolls the
 * useful part off the screen.
 *
 * An unknown length is mentioned once instead: the server may not have said how
 * big the file is, and that must not look like a hang.
 */
void espix_cmds_fetch_progress(void *ctx, const char *path,
                               size_t done, size_t total)
{
    espix_fetch_progress_t *p = ctx;

    (void)path;         /* the command named the destination already */

    if (total == 0) {
        /*
         * Only once a real body is arriving. A host that answers with a
         * redirect sends a few hundred bytes with no length first, and saying
         * "length not given" for those would be technically true and useless.
         */
        if (!p->noted && done >= 64 * 1024) {
            p->noted = true;
            espix_printf(p->s, "fetch: receiving (length not given)\n");
        }
        return;
    }

    const unsigned decile = (unsigned)((done * 10) / total);
    if (decile <= p->decile && done < total) {
        return;
    }
    p->decile = decile;
    espix_printf(p->s, "fetch: %3u%%  %u of %u KiB\n",
                 (unsigned)((done * 100) / total),
                 (unsigned)(done / 1024), (unsigned)(total / 1024));
}

int cmd_fetch(espix_session_t *s, int argc, char **argv)
{
    if (argc < 3) {
        espix_eprintf(s, "usage: fetch <url> <path>\n");
        return 1;
    }

    {
    /* The URL can carry a password, and this line goes to the console: log
     * through the redactor, as the klog does. */
    char safe[256];
    espix_net_redact_url(argv[1], safe, sizeof(safe));
    espix_printf(s, "fetch: %s\n", safe);
}

    espix_fetch_progress_t progress = { .s = s };
    espix_fetch_info_t     info     = { 0 };

    const espix_fetch_status_t st =
        espix_net_fetch(argv[1], argv[2], NULL, espix_cmds_fetch_progress,
                        &progress, &info);

    switch (st) {
    case ESPIX_FETCH_OK:
        espix_printf(s, "fetch: %u bytes -> %s\n", (unsigned)info.written,
                     argv[2]);
        return 0;

    case ESPIX_FETCH_NO_ROOM:
        if (info.need > 0) {
            espix_eprintf(s, "fetch: %u bytes needed, %u free on /\n",
                          (unsigned)info.need, (unsigned)info.free_now);
        } else {
            espix_eprintf(s, "fetch: larger than the %u bytes free on /\n",
                          (unsigned)info.free_now);
        }
        espix_eprintf(s, "fetch: free some space first. An old kernel in /boot is\n"
                         "       the usual candidate: 'ls /boot', then rm one you are\n"
                         "       not running.\n");
        return 1;

    case ESPIX_FETCH_NO_NET:
        if (info.http_status > 0) {
            espix_eprintf(s, "fetch: the server answered HTTP %d\n",
                          info.http_status);
        } else {
            espix_eprintf(s, "fetch: cannot reach %s\n", argv[1]);
        }
        return 1;

    case ESPIX_FETCH_BAD_HASH:
        espix_eprintf(s, "fetch: %s is not what was expected\n", argv[1]);
        return 1;

    case ESPIX_FETCH_IO:
        espix_eprintf(s, "fetch: cannot write %s: %s\n", argv[2],
                      strerror(info.err));
        return 1;

    default:
        espix_eprintf(s, "fetch: bad request\n");
        return 1;
    }
}
