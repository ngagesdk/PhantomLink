/** @file gcs_rcodec.h
 *
 *  Codec for the data streams of Rifts: Promise of Power's Arena game
 *  server (gcs_rifts.c), and the building blocks it needs.
 *
 *  A stream is packed like this before it is cut into ServerMessage
 *  chunks (findings in docs/ARENA.md, "Rifts"):
 *
 *     plain = u32 LE uncompressed size | u32 LE CRC-32 | zlib data | pad
 *     wire  = Blowfish-CBC(plain), zero IV, 8 byte game key
 *
 *  The padding makes the zlib data a multiple of 8 bytes; its last byte
 *  holds the number of pad bytes (8 when nothing was missing, stored as
 *  0).  The CRC covers the zlib data including the padding.  Blowfish
 *  works on pairs of little endian 32 bit words.
 *
 *  Plain C89, no allocation in the primitives; gcs_rc_pack/unpack
 *  allocate the result with malloc.
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#ifndef GCS_RCODEC_H
#define GCS_RCODEC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int gcs_u32;

/* ---- Blowfish ---- */

typedef struct
{
    gcs_u32 p[18];
    gcs_u32 s[4][256];
} gcs_blowfish;

void gcs_bf_init(gcs_blowfish *bf, const unsigned char *key, int keylen);
void gcs_bf_encrypt(const gcs_blowfish *bf, gcs_u32 *l, gcs_u32 *r);
void gcs_bf_decrypt(const gcs_blowfish *bf, gcs_u32 *l, gcs_u32 *r);
/** CBC with a zero IV over len bytes (a multiple of 8), words little endian. */
void gcs_bf_cbc_encrypt(const gcs_blowfish *bf, unsigned char *d, size_t len);
void gcs_bf_cbc_decrypt(const gcs_blowfish *bf, unsigned char *d, size_t len);

/* ---- checksums ---- */

gcs_u32 gcs_crc32(gcs_u32 crc, const unsigned char *d, size_t len); /* zlib's crc32() */
gcs_u32 gcs_adler32(gcs_u32 adler, const unsigned char *d, size_t len);

/* ---- zlib (RFC 1950/1951) ---- */

/** Inflate a zlib stream into out; returns the output length or -1. */
long gcs_zlib_inflate(const unsigned char *in, size_t inlen, unsigned char *out, size_t outcap);
/** zlib stream with stored (uncompressed) deflate blocks; out needs
 *  gcs_zlib_stored_bound(len) bytes.  Returns the length. */
size_t gcs_zlib_stored(const unsigned char *in, size_t len, unsigned char *out);
#define gcs_zlib_stored_bound(len) ((len) + 6 + 5 * ((len) / 65535 + 1))

/* ---- Rifts stream packing ---- */

#define GCS_RC_KEYLEN 8

/** Pack a stream for sending; *outlen gets the length.  NULL on error. */
unsigned char *gcs_rc_pack(const unsigned char *key, const unsigned char *in, size_t len, size_t *outlen);
/** Unpack a received stream (decrypt, check, inflate).  NULL if the data
 *  is corrupt; *outlen gets the length. */
unsigned char *gcs_rc_unpack(const unsigned char *key, const unsigned char *in, size_t len, size_t *outlen);

#ifdef __cplusplus
}
#endif

#endif /* GCS_RCODEC_H */
