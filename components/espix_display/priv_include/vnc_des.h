/*
 * DES, encryption only -- which is all VNC authentication needs.
 *
 * VNC authentication is one challenge: the server sends 16 random bytes, the
 * client encrypts them with DES in ECB mode under a key derived from the
 * password, and the server does the same and compares. There is no decryption,
 * no mode, and no padding anywhere in it, so this is a bare key schedule and a
 * bare block encrypt, kept separate from rfb.c so it can be tested on the host
 * against the FIPS vectors rather than debugged over the network.
 *
 * mbedtls is no help here: this tree is on mbedtls 4.1.1, where DES is gone --
 * the Kconfig symbol survives and mentions 3DES, but there is no des.c left.
 * Hence the tables.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One 8-byte block, ECB, encryption. key and in are not modified. */
void espix_vnc_des_encrypt(const uint8_t key[8], const uint8_t in[8],
                           uint8_t out[8]);

#ifdef __cplusplus
}
#endif
