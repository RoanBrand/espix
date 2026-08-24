/*
 * curve25519-sha256 key exchange, and the key derivation that follows it.
 *
 * The delicate part is the exchange hash: it covers both version strings, both
 * complete KEXINIT payloads, the host key blob, both ephemeral public keys and
 * the shared secret, each in a specific encoding. Get one field's framing wrong
 * and the client rejects the signature with no hint as to which — so every
 * field here is written through the same ssh_put_* helpers the wire uses.
 */

#include <string.h>

#include "psa/crypto.h"

#include "espix_kernel.h"
#include "ssh_priv.h"

#define TAG "sshkex"

/* ------------------------------------------------------------------ */
/* Exchange hash                                                       */
/* ------------------------------------------------------------------ */

/*
 * H = HASH( V_C ‖ V_S ‖ I_C ‖ I_S ‖ K_S ‖ Q_C ‖ Q_S ‖ K )   (RFC 5656 §4)
 *
 * Everything is a string except K, which is an mpint. The version strings carry
 * no CR/LF. Hashed incrementally rather than assembled, because I_C alone can be
 * 1.3KB and buffering the lot would double the per-connection cost.
 */
static esp_err_t exchange_hash(ssh_conn_t *c,
                               const uint8_t *host_blob, size_t host_blob_len,
                               const uint8_t *qc, const uint8_t *qs,
                               const uint8_t *secret, size_t secret_len,
                               uint8_t *out)
{
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    esp_err_t            ret = ESP_FAIL;
    uint8_t              scratch[8];
    ssh_buf_t            sb;

    if (psa_hash_setup(&op, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        return ESP_FAIL;
    }

/* Each field is length-prefixed on the wire, so the hash sees the same. */
#define HASH_RAW(p, n)                                                        \
    do {                                                                      \
        if (psa_hash_update(&op, (const uint8_t *)(p), (n)) != PSA_SUCCESS) {  \
            goto out;                                                         \
        }                                                                     \
    } while (0)

#define HASH_U32(v)                                                           \
    do {                                                                      \
        ssh_buf_init(&sb, scratch, sizeof(scratch));                           \
        ssh_put_u32(&sb, (uint32_t)(v));                                       \
        HASH_RAW(scratch, sb.len);                                             \
    } while (0)

#define HASH_STRING(p, n)                                                     \
    do {                                                                      \
        HASH_U32(n);                                                           \
        HASH_RAW(p, n);                                                        \
    } while (0)

    HASH_STRING(c->client_version, strlen(c->client_version));
    HASH_STRING(ssh_server_version, strlen(ssh_server_version));
    HASH_STRING(c->kexinit_c, c->kexinit_c_len);
    HASH_STRING(c->kexinit_s, c->kexinit_s_len);
    HASH_STRING(host_blob, host_blob_len);
    HASH_STRING(qc, SSH_X25519_LEN);
    HASH_STRING(qs, SSH_X25519_LEN);

    /* K as an mpint, which for a 32-byte secret with the high bit set means a
     * leading zero byte — the single most common place to get this wrong. */
    {
        uint8_t   mp[SSH_X25519_LEN + 8];
        ssh_buf_t mb;
        ssh_buf_init(&mb, mp, sizeof(mp));
        ssh_put_mpint(&mb, secret, secret_len);
        if (mb.bad) {
            goto out;
        }
        HASH_RAW(mp, mb.len);
    }

    size_t out_len = 0;
    if (psa_hash_finish(&op, out, SSH_HASH_LEN, &out_len) != PSA_SUCCESS ||
        out_len != SSH_HASH_LEN) {
        goto out;
    }

    ret = ESP_OK;
out:
    psa_hash_abort(&op);
    return ret;

#undef HASH_STRING
#undef HASH_U32
#undef HASH_RAW
}

/* ------------------------------------------------------------------ */
/* Key derivation (RFC 4253 §7.2)                                      */
/* ------------------------------------------------------------------ */

/*
 * K(X) = HASH(K ‖ H ‖ X ‖ session_id), extended by
 * K = K1 ‖ HASH(K ‖ H ‖ K1) ‖ ... when more output is needed.
 *
 * Our longest requirement is 32 bytes, exactly one SHA-256 block, so the
 * extension path is never taken — but it is implemented rather than asserted
 * away, because a future cipher choice would silently need it.
 */
static esp_err_t derive_key(ssh_conn_t *c, const uint8_t *secret,
                           size_t secret_len, const uint8_t *h, char letter,
                           uint8_t *out, size_t want)
{
    uint8_t   mp[SSH_X25519_LEN + 8];
    ssh_buf_t mb;
    ssh_buf_init(&mb, mp, sizeof(mp));
    ssh_put_mpint(&mb, secret, secret_len);
    if (mb.bad) {
        return ESP_FAIL;
    }

    size_t produced = 0;
    uint8_t block[SSH_HASH_LEN];

    while (produced < want) {
        psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
        if (psa_hash_setup(&op, PSA_ALG_SHA_256) != PSA_SUCCESS) {
            return ESP_FAIL;
        }

        bool ok = psa_hash_update(&op, mp, mb.len) == PSA_SUCCESS &&
                  psa_hash_update(&op, h, SSH_HASH_LEN) == PSA_SUCCESS;

        if (ok && produced == 0) {
            ok = psa_hash_update(&op, (const uint8_t *)&letter, 1) == PSA_SUCCESS &&
                 psa_hash_update(&op, c->session_id,
                                 SSH_HASH_LEN) == PSA_SUCCESS;
        } else if (ok) {
            ok = psa_hash_update(&op, out, produced) == PSA_SUCCESS;
        }

        size_t n = 0;
        if (!ok || psa_hash_finish(&op, block, sizeof(block),
                                   &n) != PSA_SUCCESS) {
            psa_hash_abort(&op);
            return ESP_FAIL;
        }

        const size_t take = (want - produced < n) ? want - produced : n;
        memcpy(out + produced, block, take);
        produced += take;
    }

    return ESP_OK;
}

static esp_err_t import_sym(const uint8_t *key, size_t len,
                            psa_key_type_t type, psa_algorithm_t alg,
                            psa_key_usage_t usage,
                            mbedtls_svc_key_id_t *out)
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attr, usage);
    psa_set_key_algorithm(&attr, alg);
    psa_set_key_type(&attr, type);

    const psa_status_t st = psa_import_key(&attr, key, len, out);
    psa_reset_key_attributes(&attr);

    return (st == PSA_SUCCESS) ? ESP_OK : ESP_FAIL;
}

static esp_err_t install_dir(ssh_dir_t *d, const uint8_t *iv,
                             const uint8_t *ckey, const uint8_t *mkey,
                             bool encrypt)
{
    if (import_sym(ckey, SSH_AES_KEY_LEN, PSA_KEY_TYPE_AES, PSA_ALG_CTR,
                   encrypt ? PSA_KEY_USAGE_ENCRYPT : PSA_KEY_USAGE_DECRYPT,
                   &d->cipher_key) != ESP_OK) {
        return ESP_FAIL;
    }
    if (import_sym(mkey, SSH_MAC_KEY_LEN, PSA_KEY_TYPE_HMAC,
                   PSA_ALG_HMAC(PSA_ALG_SHA_256),
                   PSA_KEY_USAGE_SIGN_MESSAGE | PSA_KEY_USAGE_VERIFY_MESSAGE,
                   &d->mac_key) != ESP_OK) {
        return ESP_FAIL;
    }

    /*
     * CTR is a stream cipher, so one operation runs for the life of the
     * connection and the counter advances with it. Setting up per packet would
     * restart the keystream and produce garbage after the first.
     */
    d->cipher = psa_cipher_operation_init();
    const psa_status_t st = encrypt
        ? psa_cipher_encrypt_setup(&d->cipher, d->cipher_key, PSA_ALG_CTR)
        : psa_cipher_decrypt_setup(&d->cipher, d->cipher_key, PSA_ALG_CTR);
    if (st != PSA_SUCCESS) {
        return ESP_FAIL;
    }
    if (psa_cipher_set_iv(&d->cipher, iv, SSH_AES_IV_LEN) != PSA_SUCCESS) {
        return ESP_FAIL;
    }

    d->active = true;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */

esp_err_t ssh_kex_run(ssh_conn_t *c)
{
    /* KEX_ECDH_INIT already read by the caller: string Q_C. */
    ssh_buf_t in;
    ssh_buf_read_from(&in, c->in_payload, c->in_len);
    ssh_skip(&in, 1);

    size_t         qc_len = 0;
    const uint8_t *qc = ssh_get_string(&in, &qc_len);
    if (in.bad || qc == NULL || qc_len != SSH_X25519_LEN) {
        espix_klog(ESPIX_KLOG_WARN, TAG, "bad KEX_ECDH_INIT");
        return ESP_ERR_INVALID_SIZE;
    }

    /* Copy: the reply we build reuses in_buf's neighbourhood. */
    uint8_t qc_copy[SSH_X25519_LEN];
    memcpy(qc_copy, qc, sizeof(qc_copy));

    /* Ephemeral X25519 keypair. */
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DERIVE);
    psa_set_key_algorithm(&attr, PSA_ALG_ECDH);
    psa_set_key_type(&attr,
                     PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY));
    psa_set_key_bits(&attr, 255);

    mbedtls_svc_key_id_t eph = MBEDTLS_SVC_KEY_ID_INIT;
    const psa_status_t gen = psa_generate_key(&attr, &eph);
    psa_reset_key_attributes(&attr);
    if (gen != PSA_SUCCESS) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "ephemeral keygen failed (%d)",
                   (int)gen);
        return ESP_FAIL;
    }

    esp_err_t ret = ESP_FAIL;
    uint8_t   qs[SSH_X25519_LEN];
    uint8_t   secret[SSH_X25519_LEN];
    uint8_t   h[SSH_HASH_LEN];
    size_t    n = 0;

    if (psa_export_public_key(eph, qs, sizeof(qs), &n) != PSA_SUCCESS ||
        n != SSH_X25519_LEN) {
        goto out;
    }
    if (psa_raw_key_agreement(PSA_ALG_ECDH, eph, qc_copy, sizeof(qc_copy),
                              secret, sizeof(secret), &n) != PSA_SUCCESS ||
        n != SSH_X25519_LEN) {
        espix_klog(ESPIX_KLOG_ERROR, TAG, "X25519 agreement failed");
        goto out;
    }

    uint8_t host_blob[128];
    size_t  host_blob_len = 0;
    if (ssh_hostkey_blob(host_blob, sizeof(host_blob),
                         &host_blob_len) != ESP_OK) {
        goto out;
    }

    if (exchange_hash(c, host_blob, host_blob_len, qc_copy, qs,
                      secret, sizeof(secret), h) != ESP_OK) {
        goto out;
    }

    /* First KEX fixes the session id for the connection's lifetime. */
    if (!c->have_session_id) {
        memcpy(c->session_id, h, SSH_HASH_LEN);
        c->have_session_id = true;
    }

    /* H identifies the whole exchange. Logging it makes a mismatch visible
     * across runs and is not secret — it is sent in the clear as part of the
     * signature verification the client performs. */
    espix_klog(ESPIX_KLOG_DEBUG, TAG,
               "H %02x%02x%02x%02x%02x%02x%02x%02x, K_S %u, I_C %u, I_S %u",
               h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7],
               (unsigned)host_blob_len, (unsigned)c->kexinit_c_len,
               (unsigned)c->kexinit_s_len);

    uint8_t sig[128];
    size_t  sig_len = 0;
    if (ssh_hostkey_sign(h, SSH_HASH_LEN, sig, sizeof(sig),
                         &sig_len) != ESP_OK) {
        goto out;
    }

    {
        ssh_buf_t b;
        ssh_buf_init(&b, c->out_buf, sizeof(c->out_buf));
        ssh_put_u8(&b, SSH_MSG_KEX_ECDH_REPLY);
        ssh_put_string(&b, host_blob, host_blob_len);
        ssh_put_string(&b, qs, sizeof(qs));
        ssh_put_string(&b, sig, sig_len);
        if (ssh_packet_write(c, &b) != ESP_OK) {
            goto out;
        }
    }

    {
        ssh_buf_t b;
        ssh_buf_init(&b, c->out_buf, sizeof(c->out_buf));
        ssh_put_u8(&b, SSH_MSG_NEWKEYS);
        if (ssh_packet_write(c, &b) != ESP_OK) {
            goto out;
        }
    }

    /*
     * Derive before reading the client's NEWKEYS: our own outbound direction
     * becomes encrypted immediately after we sent NEWKEYS, and the client's
     * NEWKEYS is still in the clear. Order matters and the two are not
     * symmetric.
     */
    uint8_t iv_c2s[SSH_HASH_LEN], iv_s2c[SSH_HASH_LEN];
    uint8_t key_c2s[SSH_HASH_LEN], key_s2c[SSH_HASH_LEN];
    uint8_t mac_c2s[SSH_HASH_LEN], mac_s2c[SSH_HASH_LEN];

    if (derive_key(c, secret, sizeof(secret), h, 'A', iv_c2s, SSH_AES_IV_LEN)
            != ESP_OK ||
        derive_key(c, secret, sizeof(secret), h, 'B', iv_s2c, SSH_AES_IV_LEN)
            != ESP_OK ||
        derive_key(c, secret, sizeof(secret), h, 'C', key_c2s, SSH_AES_KEY_LEN)
            != ESP_OK ||
        derive_key(c, secret, sizeof(secret), h, 'D', key_s2c, SSH_AES_KEY_LEN)
            != ESP_OK ||
        derive_key(c, secret, sizeof(secret), h, 'E', mac_c2s, SSH_MAC_KEY_LEN)
            != ESP_OK ||
        derive_key(c, secret, sizeof(secret), h, 'F', mac_s2c, SSH_MAC_KEY_LEN)
            != ESP_OK) {
        goto out;
    }

    /* Outbound first: everything we send from here is encrypted. */
    if (install_dir(&c->tx, iv_s2c, key_s2c, mac_s2c, true) != ESP_OK) {
        goto out;
    }

    /*
     * Distinguish "peer hung up" from "peer sent something else". The first
     * means the client rejected something we sent and the fault is upstream of
     * here; the second is a real protocol disagreement. Reporting both as
     * "expected NEWKEYS" hides which.
     */
    {
        const esp_err_t rd = ssh_packet_read(c);
        if (rd != ESP_OK) {
            espix_klog(ESPIX_KLOG_WARN, TAG,
                       "client closed before NEWKEYS (%s) — it rejected our "
                       "KEX_ECDH_REPLY", esp_err_to_name(rd));
            goto out;
        }
        if (c->in_payload[0] != SSH_MSG_NEWKEYS) {
            espix_klog(ESPIX_KLOG_WARN, TAG, "expected NEWKEYS, got msg %u",
                       c->in_payload[0]);
            goto out;
        }
    }

    if (install_dir(&c->rx, iv_c2s, key_c2s, mac_c2s, false) != ESP_OK) {
        goto out;
    }

    if (c->strict_kex) {
        /* Terrapin mitigation: both sequence numbers restart at zero after
         * NEWKEYS, so an attacker cannot silently drop or insert packets
         * during the unauthenticated phase. */
        c->seq_in  = 0;
        c->seq_out = 0;
    }

    espix_klog(ESPIX_KLOG_INFO, TAG, "key exchange complete%s",
               c->strict_kex ? " (strict)" : "");
    ret = ESP_OK;

out:
    /* The ephemeral key has served its purpose either way; forward secrecy
     * depends on it not outliving the exchange. */
    psa_destroy_key(eph);
    mbedtls_platform_zeroize(secret, sizeof(secret));
    return ret;
}
