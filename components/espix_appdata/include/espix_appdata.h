/*
 * App data the program does not ship.
 *
 * A program's data is not the kernel's business, and espix should not grow a
 * per-game exception to prove it. The shape is the one real systems use: the
 * program declares what it needs and whatever owns installation resolves it
 * before the program runs. Here that is a manifest beside the app's config
 *
 *     data  <url>  <path>  sha256:<hex>
 *
 * in /etc/apps/<name>.conf, read by the desktop's launcher and by the shell's
 * run path, so both agree about what "run it" means. The download itself is
 * espix_net_fetch(), which shares esp_http_client, mbedtls and the certificate
 * bundle with `upgrade` -- there is no second HTTP or TLS stack, and nothing
 * new is exposed to apps, so a program still cannot reach the network except
 * through what its manifest declares.
 *
 * A manifest is optional, and an unknown app is not an error: no file means the
 * program ships everything it needs, which is the normal case.
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bytes arriving for `path`, which is the destination the manifest named.
 * `total` is 0 when the server sent no Content-Length, so a caller showing a
 * percentage has to cope with not knowing the end.
 */
typedef void (*espix_appdata_progress_fn)(void *ctx, const char *path,
                                          size_t done, size_t total);

typedef enum {
    ESPIX_APPDATA_OK = 0,   /* present, fetched, or nothing declared */
    ESPIX_APPDATA_NO_ROOM,  /* it will not fit; free some space and retry */
    ESPIX_APPDATA_NO_NET,   /* could not reach the host, or it refused */
    ESPIX_APPDATA_BAD_HASH, /* what arrived is not what the manifest named */
    ESPIX_APPDATA_IO,       /* could not write it */
} espix_appdata_status_t;

/* Enough for a caller to say what happened without asking the manifest again. */
typedef struct {
    char   path[256];       /* the file that was missing */
    size_t need;            /* bytes it takes, as the server said; 0 unknown */
    size_t free_now;        /* room on / when the transfer started */
    size_t written;         /* bytes actually fetched */
    int    err;             /* errno behind ESPIX_APPDATA_IO, 0 otherwise */
} espix_appdata_info_t;

/*
 * Make sure everything `app` declares is present, fetching what is not.
 *
 * `app` is a program's basename, as the launcher has it: "doom", not
 * "/bin/doom". Anything that is not a plain name is answered ESPIX_APPDATA_OK
 * without reading a file, so a path cannot be used to reach an arbitrary
 * manifest.
 *
 * Called before a spawn, never after: a program that starts and then finds its
 * data missing has already made its own decisions about what to do about it.
 */
espix_appdata_status_t espix_appdata_ensure(const char *app,
                                            espix_appdata_progress_fn progress,
                                            void *ctx,
                                            espix_appdata_info_t *info);

#ifdef __cplusplus
}
#endif
