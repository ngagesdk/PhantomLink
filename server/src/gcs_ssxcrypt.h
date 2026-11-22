/** @file gcs_ssxcrypt.h
 *
 *  The cryptography of SSX Out of Bounds' online service (gcs_ssx.h,
 *  docs/SSX.md): what AIRPLAYSERVER.EXE does with Symbian's cryptalg.dll,
 *  hash.dll and bigint.dll, rebuilt in portable C.
 *
 *   - MD5 and SHA-1 (the 8-byte check value of every encrypted message
 *     comes from one of them; the server finds out which on the first
 *     message of a session)
 *   - DES and two-key triple DES (EDE, K1 K2 K1) in CBC mode without
 *     padding (TCipher3DESCBC with an empty third key)
 *   - RSA with PKCS#1 v1.5 padding: the server makes a key pair, the
 *     game encrypts its session key with the public half
 *     (CRSAPublicKey::PublicEncryptL)
 *
 *  The big number code is small and plain (32-bit limbs, Montgomery
 *  multiplication); it is meant for one key pair per server start and one
 *  decryption per session, not for speed.
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#ifndef GCS_SSXCRYPT_H
#define GCS_SSXCRYPT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- hashes ---- */

void gcs_md5(const void *data, size_t len, unsigned char out[16]);
void gcs_sha1(const void *data, size_t len, unsigned char out[20]);

/* ---- DES ---- */

typedef struct
{
    unsigned int k[16][2]; /* round keys, 2 x 24 bits each */
} gcs_des;

void gcs_des_setkey(gcs_des *d, const unsigned char key[8]);
/** One block; decrypt: 0 = encrypt, 1 = decrypt. */
void gcs_des_block(const gcs_des *d, const unsigned char in[8], unsigned char out[8], int decrypt);

typedef struct
{
    gcs_des k1, k2;
} gcs_3des;

/** Two-key triple DES: key = K1 (8 bytes) followed by K2 (8 bytes). */
void gcs_3des_setkey(gcs_3des *t, const unsigned char key[16]);
/** CBC over len bytes (a multiple of 8), in place allowed. */
void gcs_3des_cbc_encrypt(const gcs_3des *t, const unsigned char iv[8], const unsigned char *in, unsigned char *out,
                          size_t len);
void gcs_3des_cbc_decrypt(const gcs_3des *t, const unsigned char iv[8], const unsigned char *in, unsigned char *out,
                          size_t len);

/* ---- RSA ---- */

#define GCS_BN_LIMBS 72 /* up to 2304 bits; products of two 1024-bit numbers fit */

typedef struct
{
    unsigned int v[GCS_BN_LIMBS]; /* little endian 32-bit limbs */
} gcs_bn;

typedef struct
{
    int bits;       /* modulus size */
    gcs_bn n, d;    /* modulus, private exponent */
    unsigned int e; /* public exponent (65537) */
} gcs_rsa;

/** random bytes; returns 0 on success */
typedef int (*gcs_rand_bytes_fn)(void *ctx, void *buf, size_t n);

/** New key pair with a modulus of bits (512..2048, multiple of 64).
 *  Returns 0 on success. */
int gcs_rsa_generate(gcs_rsa *k, int bits, gcs_rand_bytes_fn rnd, void *ctx);
/** Raw private operation on a big-endian number of up to bits/8 bytes;
 *  out gets exactly bits/8 bytes.  Returns 0 on success. */
int gcs_rsa_private(const gcs_rsa *k, const unsigned char *in, size_t len, unsigned char *out);
/** Raw public operation (for tests and for checking a key). */
int gcs_rsa_public(const gcs_rsa *k, const unsigned char *in, size_t len, unsigned char *out);
/** Decrypt and remove PKCS#1 v1.5 padding (block type 1 or 2).  Returns
 *  the message length (copied to out, at most cap bytes) or -1. */
int gcs_rsa_decrypt_pkcs1(const gcs_rsa *k, const unsigned char *in, size_t len, unsigned char *out, size_t cap);
/** Modulus or exponent as a decimal string (the form the game parses). */
void gcs_rsa_modulus_dec(const gcs_rsa *k, char *out, size_t cap);

/* big numbers, exposed for the tests */
void gcs_bn_from_bytes(gcs_bn *a, const unsigned char *in, size_t len);
void gcs_bn_to_bytes(const gcs_bn *a, unsigned char *out, size_t len);
void gcs_bn_to_dec(const gcs_bn *a, char *out, size_t cap);
/** r = b^e mod m (m odd, nonzero); returns 0 on success */
int gcs_bn_modexp(gcs_bn *r, const gcs_bn *b, const gcs_bn *e, const gcs_bn *m);

#ifdef __cplusplus
}
#endif

#endif /* GCS_SSXCRYPT_H */
