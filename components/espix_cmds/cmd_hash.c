/*
 * sha256sum: one SHA-256 per file, in coreutils' format.
 *
 * The same primitive the OTA path wants -- an image's digest, computed on the
 * device, checked against what a manifest or a release page published.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psa/crypto.h"

#include "espix_cmds_priv.h"

#define HASH_CHUNK 4096

static int hash_open_file(espix_session_t *s, const char *label, FILE *f)
{
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;

    psa_status_t st = psa_hash_setup(&op, PSA_ALG_SHA_256);
    if (st != PSA_SUCCESS) {
        espix_eprintf(s, "sha256sum: %s: cannot start a hash (%d)\n",
                      label, (int)st);
        return 1;
    }

    unsigned char *buf = malloc(HASH_CHUNK);
    if (buf == NULL) {
        psa_hash_abort(&op);
        espix_eprintf(s, "sha256sum: %s: out of memory for the read buffer\n", label);
        return 1;
    }

    size_t n;
    while ((n = fread(buf, 1, HASH_CHUNK, f)) > 0) {
        st = psa_hash_update(&op, buf, n);
        if (st != PSA_SUCCESS) {
            free(buf);
            psa_hash_abort(&op);
            espix_eprintf(s, "sha256sum: %s: hash update failed (%d)\n",
                          label, (int)st);
            return 1;
        }
    }
    free(buf);

    unsigned char digest[32];
    size_t len = 0;
    st = psa_hash_finish(&op, digest, sizeof(digest), &len);
    if (st != PSA_SUCCESS || len != sizeof(digest)) {
        psa_hash_abort(&op);
        espix_eprintf(s, "sha256sum: %s: hash finalise failed (%d)\n",
                      label, (int)st);
        return 1;
    }

    for (size_t i = 0; i < len; i++) {
        espix_printf(s, "%02x", digest[i]);
    }
    espix_printf(s, "  %s\n", label);
    return 0;
}

static int cmd_sha256sum(espix_session_t *s, int argc, char **argv)
{
    if (argc < 2) {
        espix_eprintf(s, "usage: sha256sum <file>...\n");
        return 1;
    }

    /* Idempotent; the SSH server does the same before it needs PSA. */
    (void)psa_crypto_init();

    int rc = 0;
    for (int i = 1; i < argc; i++) {
        char path[ESPIX_PATH_MAX];
        if (!espix_cmd_path(s, argv[i], path, sizeof(path))) {
            rc = 1;
            continue;
        }

        FILE *f = fopen(path, "rb");
        if (f == NULL) {
            espix_eprintf(s, "sha256sum: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
            continue;
        }
        rc |= hash_open_file(s, argv[i], f);
        fclose(f);
    }
    return rc;
}

static espix_cmd_t s_hash_cmds[] = {
    { .name = "sha256sum", .fn = cmd_sha256sum,
      .help = "print SHA-256 checksums", .usage = "sha256sum <file>..." },
};

void espix_cmds_register_hash(void)
{
    espix_cmds_register_table(s_hash_cmds,
                              sizeof(s_hash_cmds) / sizeof(s_hash_cmds[0]));
}
