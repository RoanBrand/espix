/*
 * SSH host key: an ecdsa-sha2-nistp256 identity, generated once and kept.
 *
 * ecdsa rather than the more fashionable ed25519 because Mbed TLS has no
 * Ed25519 implementation — only a TLS 1.3 signature-algorithm constant. Every
 * OpenSSH client accepts ecdsa-sha2-nistp256.
 *
 * The private key lives in /etc/ssh/host_ecdsa_key as hex, mode 0600 and owned
 * by root. It is still plaintext -- PSA persistent key storage would be better
 * but is not configured in this build -- so the mode is the only thing standing
 * between another account on the device and the host's identity. It was 0644
 * until permissions grew teeth and nobody came back for it; see secure_key().
 */

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "mbedtls/base64.h"
#include "psa/crypto.h"

#include "espix_fs.h"
#include "espix_kernel.h"
#include "espix_ssh.h"
#include "ssh_priv.h"

#define TAG "sshkey"

#define KEY_DIR      "/etc/ssh"
#define KEY_PATH     KEY_DIR "/host_ecdsa_key"
#define PUB_PATH     KEY_DIR "/host_ecdsa_key.pub"

#define P256_SCALAR  32
#define KEY_TYPE_STR "ecdsa-sha2-nistp256"
#define CURVE_STR    "nistp256"

static mbedtls_svc_key_id_t s_key;
static bool                 s_have_key;

/*
 * The SSH-encoded public key, cached, because deriving it is not free.
 *
 * psa_export_public_key() on a key PSA holds as a private scalar has to
 * recompute the public point -- a base-point scalar multiplication, and on the
 * S3 there is no ECC accelerator to do it with. Measured at **141 ms**, paid on
 * every single connection, for a value that cannot change while the key is
 * loaded. It was a third of the key exchange.
 *
 * Invalidated wherever s_key is replaced, which is the only way it can go
 * stale.
 */
static uint8_t              s_blob[128];
static size_t               s_blob_len;
/* "SHA256:" plus 43 base64 chars for a 32-byte digest, plus slack. */
static char                 s_fingerprint[80];

static void to_hex(const uint8_t *in, size_t len, char *out)
{
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2]     = d[in[i] >> 4];
        out[i * 2 + 1] = d[in[i] & 0x0f];
    }
    out[len * 2] = '\0';
}

static bool from_hex(const char *in, uint8_t *out, size_t out_len)
{
    for (size_t i = 0; i < out_len; i++) {
        unsigned hi = 0, lo = 0;
        const char a = in[i * 2], b = in[i * 2 + 1];
        if (!isxdigit((unsigned char)a) || !isxdigit((unsigned char)b)) {
            return false;
        }
        hi = (a <= '9') ? (unsigned)(a - '0') : (unsigned)((a | 0x20) - 'a' + 10);
        lo = (b <= '9') ? (unsigned)(b - '0') : (unsigned)((b | 0x20) - 'a' + 10);
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static void key_attributes(psa_key_attributes_t *attr)
{
    /* SIGN_MESSAGE, not SIGN_HASH — see ssh_hostkey_sign() for why the
     * exchange hash is signed as a *message*. */
    psa_set_key_usage_flags(attr, PSA_KEY_USAGE_SIGN_MESSAGE |
                                  PSA_KEY_USAGE_VERIFY_MESSAGE |
                                  PSA_KEY_USAGE_EXPORT);
    psa_set_key_algorithm(attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    psa_set_key_type(attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(attr, 256);
}

/* ------------------------------------------------------------------ */

esp_err_t ssh_hostkey_blob(uint8_t *out, size_t cap, size_t *out_len)
{
    if (!s_have_key) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_blob_len == 0) {
        uint8_t point[SSH_P256_POINT];
        size_t  point_len = 0;
        if (psa_export_public_key(s_key, point, sizeof(point),
                                  &point_len) != PSA_SUCCESS) {
            return ESP_FAIL;
        }

        ssh_buf_t b;
        ssh_buf_init(&b, s_blob, sizeof(s_blob));
        ssh_put_cstr(&b, KEY_TYPE_STR);
        ssh_put_cstr(&b, CURVE_STR);
        ssh_put_string(&b, point, point_len);

        if (b.bad) {
            return ESP_ERR_INVALID_SIZE;
        }
        s_blob_len = b.len;
    }

    if (cap < s_blob_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(out, s_blob, s_blob_len);
    *out_len = s_blob_len;
    return ESP_OK;
}

esp_err_t ssh_hostkey_sign(const uint8_t *hash, size_t hash_len,
                           uint8_t *out, size_t cap, size_t *out_len)
{
    if (!s_have_key) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * psa_sign_MESSAGE, not psa_sign_hash — and this is the subtle part.
     *
     * The exchange hash H is the *message* here, not the digest. OpenSSH's
     * ssh_ecdsa_verify() takes H and runs it through SHA-256 again before
     * calling ECDSA_do_verify, so for ecdsa-sha2-nistp256 the signature is over
     * SHA256(H). psa_sign_hash() would treat H as the digest and sign it
     * directly, producing a signature that is perfectly valid and that no SSH
     * client will accept. OpenSSH reports it as "error in libcrypto", because
     * ECDSA_do_verify returns -1 rather than 0.
     *
     * PSA returns ECDSA as raw r‖s, each the curve size, which is exactly what
     * SSH wants — the legacy mbedtls API would have needed DER unwrapping.
     */
    uint8_t raw[64];
    size_t  raw_len = 0;
    if (psa_sign_message(s_key, PSA_ALG_ECDSA(PSA_ALG_SHA_256), hash, hash_len,
                         raw, sizeof(raw), &raw_len) != PSA_SUCCESS) {
        return ESP_FAIL;
    }
    if (raw_len != 64) {
        return ESP_FAIL;
    }

    /* No self-check here. It tests a property of the key, and the key is fixed
     * when it is loaded, so it runs once there -- hostkey_self_check(). */

    /* Inner blob: mpint r, mpint s. Then wrapped as a string inside the outer
     * (key-type, signature) pair — RFC 5656 §3.1.2. */
    uint8_t   inner[80];
    ssh_buf_t ib;
    ssh_buf_init(&ib, inner, sizeof(inner));
    ssh_put_mpint(&ib, raw, 32);
    ssh_put_mpint(&ib, raw + 32, 32);
    if (ib.bad) {
        return ESP_ERR_INVALID_SIZE;
    }

    ssh_buf_t b;
    ssh_buf_init(&b, out, cap);
    ssh_put_cstr(&b, KEY_TYPE_STR);
    ssh_put_string(&b, inner, ib.len);

    if (b.bad) {
        return ESP_ERR_INVALID_SIZE;
    }
    *out_len = b.len;

    /* The mpint lengths are the interesting part: 33 rather than 32 means a
     * leading zero was prepended because the top bit was set. That happens for
     * roughly half of all signatures, so a bug in that path fails
     * intermittently across reconnects — worth being able to see. */
    espix_klog(ESPIX_KLOG_DEBUG, TAG,
               "sig: blob %u, r %02x.. s %02x.., inner %u",
               (unsigned)b.len, raw[0], raw[32], (unsigned)ib.len);

    return ESP_OK;
}

/* ------------------------------------------------------------------ */

static void compute_fingerprint(void)
{
    uint8_t blob[128];
    size_t  blob_len = 0;
    if (ssh_hostkey_blob(blob, sizeof(blob), &blob_len) != ESP_OK) {
        return;
    }

    uint8_t digest[SSH_HASH_LEN];
    size_t  digest_len = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, blob, blob_len, digest,
                         sizeof(digest), &digest_len) != PSA_SUCCESS) {
        return;
    }

    unsigned char b64[64];
    size_t        b64_len = 0;
    if (mbedtls_base64_encode(b64, sizeof(b64), &b64_len, digest,
                              digest_len) != 0) {
        return;
    }

    /* OpenSSH prints these without base64 padding. */
    while (b64_len > 0 && b64[b64_len - 1] == '=') {
        b64_len--;
    }
    b64[b64_len] = '\0';

    snprintf(s_fingerprint, sizeof(s_fingerprint), "SHA256:%s", (char *)b64);
}

/*
 * Root's alone to read, and re-asserted on load as well as on save.
 *
 * A device flashed before this existed carries no stored mode, so the rule
 * answers 0644 and the key is world-readable; nothing would ever correct that
 * if only save_key() set it, because the key is generated once and then only
 * ever loaded. espix_fs_ensure_mode() is what keeps the load-path call from
 * costing a LittleFS metadata write on every boot.
 */
static void secure_key(void)
{
    (void)espix_fs_ensure_mode(KEY_PATH, 0600);
}

static esp_err_t save_key(void)
{
    uint8_t scalar[P256_SCALAR];
    size_t  scalar_len = 0;
    if (psa_export_key(s_key, scalar, sizeof(scalar),
                       &scalar_len) != PSA_SUCCESS ||
        scalar_len != P256_SCALAR) {
        return ESP_FAIL;
    }

    mkdir(KEY_DIR, 0755);   /* EEXIST is fine */

    FILE *f = fopen(KEY_PATH, "w");
    if (f == NULL) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "cannot write %s: %s", KEY_PATH,
                   strerror(errno));
        return ESP_FAIL;
    }
    char hex[P256_SCALAR * 2 + 1];
    to_hex(scalar, scalar_len, hex);
    fprintf(f, "%s\n", hex);
    fclose(f);

    secure_key();

    /* Public half alongside it, so the identity is inspectable with cat even
     * though the private key is not meant to be read. */
    uint8_t blob[128];
    size_t  blob_len = 0;
    if (ssh_hostkey_blob(blob, sizeof(blob), &blob_len) == ESP_OK) {
        unsigned char b64[256];
        size_t        b64_len = 0;
        if (mbedtls_base64_encode(b64, sizeof(b64), &b64_len, blob,
                                  blob_len) == 0) {
            FILE *p = fopen(PUB_PATH, "w");
            if (p != NULL) {
                fprintf(p, "%s %.*s espix\n", KEY_TYPE_STR, (int)b64_len,
                        (char *)b64);
                fclose(p);
            }
        }
    }

    return ESP_OK;
}

static esp_err_t load_key(void)
{
    FILE *f = fopen(KEY_PATH, "r");
    if (f == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    char line[P256_SCALAR * 2 + 4] = {0};
    const bool got = fgets(line, sizeof(line), f) != NULL;
    fclose(f);

    /* After the close, not before: a stored attribute written against an open
     * entry is the littlefs#1076 case, and there is no reason to take it here. */
    secure_key();

    uint8_t scalar[P256_SCALAR];
    if (!got || strlen(line) < P256_SCALAR * 2 ||
        !from_hex(line, scalar, sizeof(scalar))) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "%s is malformed; regenerating",
                   KEY_PATH);
        return ESP_ERR_INVALID_STATE;
    }

    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    key_attributes(&attr);

    const psa_status_t st = psa_import_key(&attr, scalar, sizeof(scalar),
                                           &s_key);
    psa_reset_key_attributes(&attr);

    if (st != PSA_SUCCESS) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "cannot import stored key (%d)",
                   (int)st);
        return ESP_ERR_INVALID_STATE;
    }

    s_have_key = true;
    s_blob_len = 0;      /* a new key means a new blob */
    return ESP_OK;
}

/*
 * Sign and verify a fixed message, once, when the key is loaded.
 *
 * This is the check that used to run on every connection. It belongs here
 * because the value it tests cannot change between logins: the key is fixed
 * when it is loaded, and ECDSA is deterministic in this build
 * (CONFIG_MBEDTLS_ECDSA_DETERMINISTIC), so the signature over a given message
 * is a pure function of the key. Verifying per login recomputed an answer that
 * was already known, at the cost of a second scalar multiplication -- and the
 * larger part of what the "ecdsa sign" timer showed: 258 ms of its 318 ms,
 * against 60 ms for the signature itself.
 *
 * What it can and cannot catch is unchanged. It uses the same convention as the
 * signing call, so it would have passed happily while that convention was
 * wrong; it rules out a broken key or a malformed PSA call, not a disagreement
 * with the peer. A key that fails here is refused at startup rather than
 * accepted and then failing every login.
 *
 * The probe is the SHA-256 of the empty string: a constant with an independent
 * definition, so it cannot be edited by accident and quietly sign something
 * else.
 */
static esp_err_t hostkey_self_check(void)
{
    static const uint8_t probe[P256_SCALAR] = {
        0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14,
        0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
        0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c,
        0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55,
    };
    uint8_t sig[64];
    size_t  sig_len = 0;

    if (psa_sign_message(s_key, PSA_ALG_ECDSA(PSA_ALG_SHA_256),
                         probe, sizeof(probe),
                         sig, sizeof(sig), &sig_len) != PSA_SUCCESS ||
        sig_len != sizeof(sig)) {
        espix_klog(ESPIX_KLOG_ERROR, TAG,
                   "cannot sign a test message with the host key");
        return ESP_FAIL;
    }
    if (psa_verify_message(s_key, PSA_ALG_ECDSA(PSA_ALG_SHA_256),
                           probe, sizeof(probe),
                           sig, sig_len) != PSA_SUCCESS) {
        espix_klog(ESPIX_KLOG_ERROR, TAG,
                   "our own signature does not verify");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t ssh_hostkey_init(void)
{
    if (s_have_key) {
        return ESP_OK;
    }

    if (psa_crypto_init() != PSA_SUCCESS) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "psa_crypto_init failed");
        return ESP_FAIL;
    }

    if (load_key() == ESP_OK) {
        if (hostkey_self_check() != ESP_OK) {
            return ESP_FAIL;
        }
        compute_fingerprint();
        espix_klog(ESPIX_KLOG_INFO, TAG, "host key %s", s_fingerprint);
        return ESP_OK;
    }

    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    key_attributes(&attr);
    const psa_status_t st = psa_generate_key(&attr, &s_key);
    psa_reset_key_attributes(&attr);

    if (st != PSA_SUCCESS) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "key generation failed (%d)",
                   (int)st);
        return ESP_FAIL;
    }
    s_have_key = true;
    s_blob_len = 0;      /* a new key means a new blob */

    if (save_key() != ESP_OK) {
        /* Usable this boot, but the client will warn about a changed key next
         * time — worth being loud about rather than silently ephemeral. */
        espix_klog(ESPIX_KLOG_ERROR, TAG,
                   "host key not persisted; it will change on reboot");
    }

    if (hostkey_self_check() != ESP_OK) {
        return ESP_FAIL;
    }
    compute_fingerprint();
    espix_klog(ESPIX_KLOG_WARN, TAG, "generated host key %s", s_fingerprint);
    return ESP_OK;
}

const char *espix_ssh_fingerprint(void)
{
    return s_fingerprint;
}
