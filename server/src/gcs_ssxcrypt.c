/** @file gcs_ssxcrypt.c
 *
 *  Hashes, triple DES and RSA for SSX Out of Bounds, see gcs_ssxcrypt.h.
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#include "gcs_ssxcrypt.h"

#include <stdint.h>
#include <string.h>

/* ======================================================================
 * MD5 (RFC 1321)
 * ====================================================================== */

typedef struct
{
    uint32_t h[5];
    uint64_t len;
    unsigned char buf[64];
    size_t n;
} hctx;

#define ROL(x, c) (((x) << (c)) | ((x) >> (32 - (c))))

static const uint32_t md5_k[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
};
static const unsigned char md5_r[64] = { 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                         5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                                         4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                         6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21 };

static void md5_block(hctx *c, const unsigned char *p)
{
    uint32_t w[16], a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3];
    int i;
    for (i = 0; i < 16; i++) {
        w[i] = (uint32_t)p[4 * i] | ((uint32_t)p[4 * i + 1] << 8) | ((uint32_t)p[4 * i + 2] << 16) |
               ((uint32_t)p[4 * i + 3] << 24);
    }
    for (i = 0; i < 64; i++) {
        uint32_t f, t;
        int g;
        if (i < 16) {
            f = (b & cc) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & cc);
            g = (5 * i + 1) & 15;
        } else if (i < 48) {
            f = b ^ cc ^ d;
            g = (3 * i + 5) & 15;
        } else {
            f = cc ^ (b | ~d);
            g = (7 * i) & 15;
        }
        t = d;
        d = cc;
        cc = b;
        f = a + f + md5_k[i] + w[g];
        b = b + ROL(f, md5_r[i]);
        a = t;
    }
    c->h[0] += a;
    c->h[1] += b;
    c->h[2] += cc;
    c->h[3] += d;
}

static void sha1_block(hctx *c, const unsigned char *p)
{
    uint32_t w[80], a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4];
    int i;
    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) |
               (uint32_t)p[4 * i + 3];
    }
    for (i = 16; i < 80; i++) {
        uint32_t x = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
        w[i] = ROL(x, 1);
    }
    for (i = 0; i < 80; i++) {
        uint32_t f, k, t;
        if (i < 20) {
            f = (b & cc) | (~b & d);
            k = 0x5a827999;
        } else if (i < 40) {
            f = b ^ cc ^ d;
            k = 0x6ed9eba1;
        } else if (i < 60) {
            f = (b & cc) | (b & d) | (cc & d);
            k = 0x8f1bbcdc;
        } else {
            f = b ^ cc ^ d;
            k = 0xca62c1d6;
        }
        t = ROL(a, 5) + f + e + k + w[i];
        e = d;
        d = cc;
        cc = ROL(b, 30);
        b = a;
        a = t;
    }
    c->h[0] += a;
    c->h[1] += b;
    c->h[2] += cc;
    c->h[3] += d;
    c->h[4] += e;
}

static void hash_run(hctx *c, const unsigned char *p, size_t len, int sha)
{
    unsigned char pad[72];
    size_t padlen, i;
    uint64_t bits;
    c->len = 0;
    c->n = 0;
    while (len >= 64) {
        if (sha) {
            sha1_block(c, p);
        } else {
            md5_block(c, p);
        }
        p += 64;
        len -= 64;
        c->len += 64;
    }
    if (len) {
        memcpy(c->buf, p, len);
    }
    c->n = len;
    c->len += len;
    bits = c->len * 8;
    padlen = (c->n < 56) ? 56 - c->n : 120 - c->n;
    memset(pad, 0, sizeof(pad));
    pad[0] = 0x80;
    for (i = 0; i < 8; i++) {
        pad[padlen + i] = sha ? (unsigned char)(bits >> (56 - 8 * i)) : (unsigned char)(bits >> (8 * i));
    }
    padlen += 8;
    for (i = 0; i < padlen; i++) {
        c->buf[c->n++] = pad[i];
        if (c->n == 64) {
            if (sha) {
                sha1_block(c, c->buf);
            } else {
                md5_block(c, c->buf);
            }
            c->n = 0;
        }
    }
}

void gcs_md5(const void *data, size_t len, unsigned char out[16])
{
    hctx c;
    int i;
    c.h[0] = 0x67452301;
    c.h[1] = 0xefcdab89;
    c.h[2] = 0x98badcfe;
    c.h[3] = 0x10325476;
    hash_run(&c, (const unsigned char *)data, len, 0);
    for (i = 0; i < 16; i++) {
        out[i] = (unsigned char)(c.h[i / 4] >> (8 * (i % 4)));
    }
}

void gcs_sha1(const void *data, size_t len, unsigned char out[20])
{
    hctx c;
    int i;
    c.h[0] = 0x67452301;
    c.h[1] = 0xefcdab89;
    c.h[2] = 0x98badcfe;
    c.h[3] = 0x10325476;
    c.h[4] = 0xc3d2e1f0;
    hash_run(&c, (const unsigned char *)data, len, 1);
    for (i = 0; i < 20; i++) {
        out[i] = (unsigned char)(c.h[i / 4] >> (24 - 8 * (i % 4)));
    }
}

/* ======================================================================
 * DES (FIPS 46-3); tables are 1-based bit numbers, bit 1 = most significant
 * ====================================================================== */

static const unsigned char des_ip[64] = { 58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4,
                                          62, 54, 46, 38, 30, 22, 14, 6, 64, 56, 48, 40, 32, 24, 16, 8,
                                          57, 49, 41, 33, 25, 17, 9, 1, 59, 51, 43, 35, 27, 19, 11, 3,
                                          61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7 };
static const unsigned char des_fp[64] = { 40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31,
                                          38, 6, 46, 14, 54, 22, 62, 30, 37, 5, 45, 13, 53, 21, 61, 29,
                                          36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27,
                                          34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9, 49, 17, 57, 25 };
static const unsigned char des_e[48] = { 32, 1, 2, 3, 4, 5, 4, 5, 6, 7, 8, 9, 8, 9, 10, 11,
                                         12, 13, 12, 13, 14, 15, 16, 17, 16, 17, 18, 19, 20, 21, 20, 21,
                                         22, 23, 24, 25, 24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1 };
static const unsigned char des_p[32] = { 16, 7, 20, 21, 29, 12, 28, 17, 1, 15, 23, 26, 5, 18, 31, 10,
                                         2, 8, 24, 14, 32, 27, 3, 9, 19, 13, 30, 6, 22, 11, 4, 25 };
static const unsigned char des_pc1[56] = { 57, 49, 41, 33, 25, 17, 9, 1, 58, 50, 42, 34, 26, 18,
                                           10, 2, 59, 51, 43, 35, 27, 19, 11, 3, 60, 52, 44, 36,
                                           63, 55, 47, 39, 31, 23, 15, 7, 62, 54, 46, 38, 30, 22,
                                           14, 6, 61, 53, 45, 37, 29, 21, 13, 5, 28, 20, 12, 4 };
static const unsigned char des_pc2[48] = { 14, 17, 11, 24, 1, 5, 3, 28, 15, 6, 21, 10, 23, 19, 12, 4,
                                           26, 8, 16, 7, 27, 20, 13, 2, 41, 52, 31, 37, 47, 55, 30, 40,
                                           51, 45, 33, 48, 44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32 };
static const unsigned char des_shift[16] = { 1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1 };
static const unsigned char des_s[8][64] = {
    { 14, 4, 13, 1, 2, 15, 11, 8, 3, 10, 6, 12, 5, 9, 0, 7, 0, 15, 7, 4, 14, 2,
      13, 1, 10, 6, 12, 11, 9, 5, 3, 8, 4, 1, 14, 8, 13, 6, 2, 11, 15, 12, 9, 7,
      3, 10, 5, 0, 15, 12, 8, 2, 4, 9, 1, 7, 5, 11, 3, 14, 10, 0, 6, 13 },
    { 15, 1, 8, 14, 6, 11, 3, 4, 9, 7, 2, 13, 12, 0, 5, 10, 3, 13, 4, 7, 15, 2,
      8, 14, 12, 0, 1, 10, 6, 9, 11, 5, 0, 14, 7, 11, 10, 4, 13, 1, 5, 8, 12, 6,
      9, 3, 2, 15, 13, 8, 10, 1, 3, 15, 4, 2, 11, 6, 7, 12, 0, 5, 14, 9 },
    { 10, 0, 9, 14, 6, 3, 15, 5, 1, 13, 12, 7, 11, 4, 2, 8, 13, 7, 0, 9, 3, 4,
      6, 10, 2, 8, 5, 14, 12, 11, 15, 1, 13, 6, 4, 9, 8, 15, 3, 0, 11, 1, 2, 12,
      5, 10, 14, 7, 1, 10, 13, 0, 6, 9, 8, 7, 4, 15, 14, 3, 11, 5, 2, 12 },
    { 7, 13, 14, 3, 0, 6, 9, 10, 1, 2, 8, 5, 11, 12, 4, 15, 13, 8, 11, 5, 6, 15,
      0, 3, 4, 7, 2, 12, 1, 10, 14, 9, 10, 6, 9, 0, 12, 11, 7, 13, 15, 1, 3, 14,
      5, 2, 8, 4, 3, 15, 0, 6, 10, 1, 13, 8, 9, 4, 5, 11, 12, 7, 2, 14 },
    { 2, 12, 4, 1, 7, 10, 11, 6, 8, 5, 3, 15, 13, 0, 14, 9, 14, 11, 2, 12, 4, 7,
      13, 1, 5, 0, 15, 10, 3, 9, 8, 6, 4, 2, 1, 11, 10, 13, 7, 8, 15, 9, 12, 5,
      6, 3, 0, 14, 11, 8, 12, 7, 1, 14, 2, 13, 6, 15, 0, 9, 10, 4, 5, 3 },
    { 12, 1, 10, 15, 9, 2, 6, 8, 0, 13, 3, 4, 14, 7, 5, 11, 10, 15, 4, 2, 7, 12,
      9, 5, 6, 1, 13, 14, 0, 11, 3, 8, 9, 14, 15, 5, 2, 8, 12, 3, 7, 0, 4, 10,
      1, 13, 11, 6, 4, 3, 2, 12, 9, 5, 15, 10, 11, 14, 1, 7, 6, 0, 8, 13 },
    { 4, 11, 2, 14, 15, 0, 8, 13, 3, 12, 9, 7, 5, 10, 6, 1, 13, 0, 11, 7, 4, 9,
      1, 10, 14, 3, 5, 12, 2, 15, 8, 6, 1, 4, 11, 13, 12, 3, 7, 14, 10, 15, 6, 8,
      0, 5, 9, 2, 6, 11, 13, 8, 1, 4, 10, 7, 9, 5, 0, 15, 14, 2, 3, 12 },
    { 13, 2, 8, 4, 6, 15, 11, 1, 10, 9, 3, 14, 5, 0, 12, 7, 1, 15, 13, 8, 10, 3,
      7, 4, 12, 5, 6, 11, 0, 14, 9, 2, 7, 11, 4, 1, 9, 12, 14, 2, 0, 6, 10, 13,
      15, 3, 5, 8, 2, 1, 14, 7, 4, 10, 8, 13, 15, 12, 9, 0, 3, 5, 6, 11 }
};

/* output bit i (1-based, from the top of an nout-bit value) = input bit t[i] (of nin bits) */
static uint64_t permute(uint64_t in, const unsigned char *t, int nin, int nout)
{
    uint64_t r = 0;
    int i;
    for (i = 0; i < nout; i++) {
        r = (r << 1) | ((in >> (nin - t[i])) & 1u);
    }
    return r;
}

void gcs_des_setkey(gcs_des *d, const unsigned char key[8])
{
    uint64_t k = 0, cd;
    uint32_t c, dd;
    int i;
    for (i = 0; i < 8; i++) {
        k = (k << 8) | key[i];
    }
    cd = permute(k, des_pc1, 64, 56);
    c = (uint32_t)(cd >> 28) & 0x0fffffffu;
    dd = (uint32_t)cd & 0x0fffffffu;
    for (i = 0; i < 16; i++) {
        uint64_t sub;
        int s = des_shift[i];
        c = ((c << s) | (c >> (28 - s))) & 0x0fffffffu;
        dd = ((dd << s) | (dd >> (28 - s))) & 0x0fffffffu;
        sub = permute(((uint64_t)c << 28) | dd, des_pc2, 56, 48);
        d->k[i][0] = (unsigned int)(sub >> 24);
        d->k[i][1] = (unsigned int)(sub & 0xffffffu);
    }
}

static uint32_t des_f(uint32_t r, const unsigned int *k)
{
    uint64_t x = permute(r, des_e, 32, 48) ^ (((uint64_t)k[0] << 24) | k[1]);
    uint32_t out = 0;
    int i;
    for (i = 0; i < 8; i++) {
        unsigned b = (unsigned)(x >> (42 - 6 * i)) & 0x3f;
        unsigned row = ((b >> 4) & 2) | (b & 1), col = (b >> 1) & 15;
        out = (out << 4) | des_s[i][row * 16 + col];
    }
    return (uint32_t)permute(out, des_p, 32, 32);
}

void gcs_des_block(const gcs_des *d, const unsigned char in[8], unsigned char out[8], int decrypt)
{
    uint64_t b = 0;
    uint32_t l, r;
    int i;
    for (i = 0; i < 8; i++) {
        b = (b << 8) | in[i];
    }
    b = permute(b, des_ip, 64, 64);
    l = (uint32_t)(b >> 32);
    r = (uint32_t)b;
    for (i = 0; i < 16; i++) {
        uint32_t t = r;
        r = l ^ des_f(r, d->k[decrypt ? 15 - i : i]);
        l = t;
    }
    b = permute(((uint64_t)r << 32) | l, des_fp, 64, 64);
    for (i = 7; i >= 0; i--) {
        out[i] = (unsigned char)b;
        b >>= 8;
    }
}

void gcs_3des_setkey(gcs_3des *t, const unsigned char key[16])
{
    gcs_des_setkey(&t->k1, key);
    gcs_des_setkey(&t->k2, key + 8);
}

static void ede(const gcs_3des *t, const unsigned char in[8], unsigned char out[8], int decrypt)
{
    unsigned char a[8], b[8];
    if (!decrypt) {
        gcs_des_block(&t->k1, in, a, 0);
        gcs_des_block(&t->k2, a, b, 1);
        gcs_des_block(&t->k1, b, out, 0);
    } else {
        gcs_des_block(&t->k1, in, a, 1);
        gcs_des_block(&t->k2, a, b, 0);
        gcs_des_block(&t->k1, b, out, 1);
    }
}

void gcs_3des_cbc_encrypt(const gcs_3des *t, const unsigned char iv[8], const unsigned char *in, unsigned char *out,
                          size_t len)
{
    unsigned char chain[8], x[8];
    size_t i, j;
    memcpy(chain, iv, 8);
    for (i = 0; i + 8 <= len; i += 8) {
        for (j = 0; j < 8; j++) {
            x[j] = (unsigned char)(in[i + j] ^ chain[j]);
        }
        ede(t, x, chain, 0);
        memcpy(out + i, chain, 8);
    }
}

void gcs_3des_cbc_decrypt(const gcs_3des *t, const unsigned char iv[8], const unsigned char *in, unsigned char *out,
                          size_t len)
{
    unsigned char chain[8], c[8], x[8];
    size_t i, j;
    memcpy(chain, iv, 8);
    for (i = 0; i + 8 <= len; i += 8) {
        memcpy(c, in + i, 8);
        ede(t, c, x, 1);
        for (j = 0; j < 8; j++) {
            out[i + j] = (unsigned char)(x[j] ^ chain[j]);
        }
        memcpy(chain, c, 8);
    }
}

/* ======================================================================
 * Big numbers
 * ====================================================================== */

#define NL GCS_BN_LIMBS

static void bn_zero(gcs_bn *a)
{
    memset(a, 0, sizeof(*a));
}

static void bn_set(gcs_bn *a, uint32_t v)
{
    bn_zero(a);
    a->v[0] = v;
}

static int bn_limbs(const gcs_bn *a)
{
    int i;
    for (i = NL - 1; i >= 0 && !a->v[i]; i--) {
    }
    return i + 1;
}

static int bn_bits(const gcs_bn *a)
{
    int n = bn_limbs(a), b = 0;
    uint32_t top;
    if (!n) {
        return 0;
    }
    top = a->v[n - 1];
    while (top) {
        b++;
        top >>= 1;
    }
    return (n - 1) * 32 + b;
}

static int bn_cmp(const gcs_bn *a, const gcs_bn *b)
{
    int i;
    for (i = NL - 1; i >= 0; i--) {
        if (a->v[i] != b->v[i]) {
            return a->v[i] < b->v[i] ? -1 : 1;
        }
    }
    return 0;
}

/* a -= b (a >= b) */
static void bn_sub(gcs_bn *a, const gcs_bn *b)
{
    uint64_t borrow = 0;
    int i;
    for (i = 0; i < NL; i++) {
        uint64_t t = (uint64_t)a->v[i] - b->v[i] - borrow;
        a->v[i] = (uint32_t)t;
        borrow = (t >> 63) & 1;
    }
}

static void bn_add_small(gcs_bn *a, uint32_t v)
{
    uint64_t c = v;
    int i;
    for (i = 0; i < NL && c; i++) {
        c += a->v[i];
        a->v[i] = (uint32_t)c;
        c >>= 32;
    }
}

static void bn_sub_small(gcs_bn *a, uint32_t v)
{
    int i;
    for (i = 0; i < NL && v; i++) {
        uint32_t old = a->v[i];
        a->v[i] = old - v;
        v = old < v ? 1 : 0;
    }
}

static void bn_mul_small(gcs_bn *a, uint32_t v)
{
    uint64_t c = 0;
    int i;
    for (i = 0; i < NL; i++) {
        c += (uint64_t)a->v[i] * v;
        a->v[i] = (uint32_t)c;
        c >>= 32;
    }
}

/* a /= v, returns the remainder */
static uint32_t bn_div_small(gcs_bn *a, uint32_t v)
{
    uint64_t r = 0;
    int i;
    for (i = NL - 1; i >= 0; i--) {
        r = (r << 32) | a->v[i];
        a->v[i] = (uint32_t)(r / v);
        r %= v;
    }
    return (uint32_t)r;
}

static uint32_t bn_mod_small(const gcs_bn *a, uint32_t v)
{
    uint64_t r = 0;
    int i;
    for (i = bn_limbs(a) - 1; i >= 0; i--) {
        r = ((r << 32) | a->v[i]) % v;
    }
    return (uint32_t)r;
}

/* r = a * b; returns -1 when the product does not fit */
static int bn_mul(gcs_bn *r, const gcs_bn *a, const gcs_bn *b)
{
    uint32_t t[2 * NL];
    int na = bn_limbs(a), nb = bn_limbs(b), i, j;
    if (na + nb > NL) {
        return -1;
    }
    memset(t, 0, sizeof(t));
    for (i = 0; i < na; i++) {
        uint64_t c = 0;
        for (j = 0; j < nb; j++) {
            c += (uint64_t)a->v[i] * b->v[j] + t[i + j];
            t[i + j] = (uint32_t)c;
            c >>= 32;
        }
        t[i + nb] = (uint32_t)c;
    }
    memcpy(r->v, t, sizeof(r->v));
    return 0;
}

static void bn_shr1(gcs_bn *a)
{
    int i;
    for (i = 0; i < NL - 1; i++) {
        a->v[i] = (a->v[i] >> 1) | (a->v[i + 1] << 31);
    }
    a->v[NL - 1] >>= 1;
}

void gcs_bn_from_bytes(gcs_bn *a, const unsigned char *in, size_t len)
{
    size_t i;
    bn_zero(a);
    for (i = 0; i < len && i < 4 * NL; i++) {
        size_t bit = 8 * i; /* byte i from the end */
        a->v[bit / 32] |= (uint32_t)in[len - 1 - i] << (bit % 32);
    }
}

void gcs_bn_to_bytes(const gcs_bn *a, unsigned char *out, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        size_t bit = 8 * i;
        out[len - 1 - i] = bit / 32 < NL ? (unsigned char)(a->v[bit / 32] >> (bit % 32)) : 0;
    }
}

void gcs_bn_to_dec(const gcs_bn *a, char *out, size_t cap)
{
    gcs_bn t = *a;
    char tmp[800];
    size_t n = 0, i;
    if (!cap) {
        return;
    }
    do {
        uint32_t r = bn_div_small(&t, 1000000000u);
        int k;
        for (k = 0; k < 9 && n < sizeof(tmp); k++) {
            tmp[n++] = (char)('0' + r % 10);
            r /= 10;
        }
    } while (bn_limbs(&t));
    while (n > 1 && tmp[n - 1] == '0') {
        n--;
    }
    for (i = 0; i < n && i + 1 < cap; i++) {
        out[i] = tmp[n - 1 - i];
    }
    out[i] = 0;
}

/* ---- Montgomery arithmetic modulo an odd m of k limbs ---- */

typedef struct
{
    gcs_bn m;
    int k;
    uint32_t minv; /* -m^-1 mod 2^32 */
    gcs_bn r2;     /* R^2 mod m, R = 2^(32k) */
} mont;

static void mont_mul(const mont *M, gcs_bn *r, const gcs_bn *a, const gcs_bn *b)
{
    uint32_t t[NL + 2];
    int k = M->k, i, j;
    memset(t, 0, sizeof(t));
    for (i = 0; i < k; i++) {
        uint64_t c = 0, s;
        uint32_t q;
        for (j = 0; j < k; j++) {
            c += (uint64_t)a->v[j] * b->v[i] + t[j];
            t[j] = (uint32_t)c;
            c >>= 32;
        }
        s = (uint64_t)t[k] + c;
        t[k] = (uint32_t)s;
        t[k + 1] = (uint32_t)(s >> 32);
        q = t[0] * M->minv;
        c = ((uint64_t)q * M->m.v[0] + t[0]) >> 32;
        for (j = 1; j < k; j++) {
            c += (uint64_t)q * M->m.v[j] + t[j];
            t[j - 1] = (uint32_t)c;
            c >>= 32;
        }
        s = (uint64_t)t[k] + c;
        t[k - 1] = (uint32_t)s;
        t[k] = t[k + 1] + (uint32_t)(s >> 32);
    }
    bn_zero(r);
    memcpy(r->v, t, (size_t)(k + 1) * sizeof(uint32_t));
    if (bn_cmp(r, &M->m) >= 0) {
        bn_sub(r, &M->m);
    }
}

static int mont_init(mont *M, const gcs_bn *m)
{
    uint32_t inv;
    int i;
    M->m = *m;
    M->k = bn_limbs(m);
    if (!M->k || !(m->v[0] & 1) || M->k > NL - 2) {
        return -1;
    }
    inv = m->v[0]; /* Newton: inv = m0^-1 mod 2^32 */
    for (i = 0; i < 5; i++) {
        inv *= 2 - m->v[0] * inv;
    }
    M->minv = (uint32_t)(0u - inv);
    /* R^2 mod m by doubling 1, 64k times */
    bn_set(&M->r2, 1);
    for (i = 0; i < 64 * M->k; i++) {
        int j;
        uint32_t carry = 0;
        for (j = 0; j < NL; j++) {
            uint32_t nc = M->r2.v[j] >> 31;
            M->r2.v[j] = (M->r2.v[j] << 1) | carry;
            carry = nc;
        }
        if (bn_cmp(&M->r2, m) >= 0) {
            bn_sub(&M->r2, m);
        }
    }
    return 0;
}

/* r = b^e mod M (b < m) */
static void mont_exp(const mont *M, gcs_bn *r, const gcs_bn *b, const gcs_bn *e)
{
    gcs_bn x, bm, one;
    int i;
    bn_set(&one, 1);
    mont_mul(M, &bm, b, &M->r2);
    mont_mul(M, &x, &one, &M->r2); /* R mod m */
    for (i = bn_bits(e) - 1; i >= 0; i--) {
        mont_mul(M, &x, &x, &x);
        if ((e->v[i / 32] >> (i % 32)) & 1) {
            mont_mul(M, &x, &x, &bm);
        }
    }
    mont_mul(M, r, &x, &one);
}

int gcs_bn_modexp(gcs_bn *r, const gcs_bn *b, const gcs_bn *e, const gcs_bn *m)
{
    mont M;
    gcs_bn bb = *b;
    if (mont_init(&M, m) != 0) {
        return -1;
    }
    while (bn_cmp(&bb, m) >= 0) { /* only ever used with b < m; keep it total */
        bn_sub(&bb, m);
    }
    mont_exp(&M, r, &bb, e);
    return 0;
}

/* ---- primes and keys ---- */

static const unsigned short small_primes[] = {
    3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47, 53, 59, 61, 67, 71, 73, 79, 83,
    89, 97, 101, 103, 107, 109, 113, 127, 131, 137, 139, 149, 151, 157, 163, 167, 173, 179, 181, 191, 193, 197,
    199, 211, 223, 227, 229, 233, 239, 241, 251, 257, 263, 269, 271, 277, 281, 283, 293, 307, 311, 313, 317, 331,
    337, 347, 349, 353, 359, 367, 373, 379, 383, 389, 397, 401, 409, 419, 421, 431, 433, 439, 443, 449, 457, 461,
    463, 467, 479, 487, 491, 499, 503, 509, 521, 523, 541, 547, 557, 563, 569, 571, 577, 587, 593, 599, 601, 607,
    613, 617, 619, 631, 641, 643, 647, 653, 659, 661, 673, 677, 683, 691, 701, 709, 719, 727, 733, 739, 743, 751,
    757, 761, 769, 773, 787, 797, 809, 811, 821, 823, 827, 829, 839, 853, 857, 859, 863, 877, 881, 883, 887, 907,
    911, 919, 929, 937, 941, 947, 953, 967, 971, 977, 983, 991, 997
};
#define NSMALL (sizeof(small_primes) / sizeof(small_primes[0]))

/* Miller-Rabin with rounds random bases */
static int probable_prime(const gcs_bn *p, int rounds, gcs_rand_bytes_fn rnd, void *ctx)
{
    mont M;
    gcs_bn d, pm1, a, x, one;
    int s = 0, i, r;
    size_t bytes = (size_t)(bn_bits(p) + 7) / 8;
    if (mont_init(&M, p) != 0) {
        return 0;
    }
    pm1 = *p;
    bn_sub_small(&pm1, 1);
    d = pm1;
    while (!(d.v[0] & 1)) {
        bn_shr1(&d);
        s++;
    }
    bn_set(&one, 1);
    for (r = 0; r < rounds; r++) {
        unsigned char buf[4 * NL];
        if (rnd(ctx, buf, bytes - 1) != 0) {
            return 0;
        }
        gcs_bn_from_bytes(&a, buf, bytes - 1); /* below p */
        if (bn_bits(&a) < 2) {
            bn_set(&a, 2 + (uint32_t)r);
        }
        mont_exp(&M, &x, &a, &d);
        if (!bn_cmp(&x, &one) || !bn_cmp(&x, &pm1)) {
            continue;
        }
        for (i = 1; i < s; i++) {
            gcs_bn x2;
            mont_mul(&M, &x2, &x, &M.r2); /* to Montgomery form */
            mont_mul(&M, &x2, &x2, &x2);
            mont_mul(&M, &x, &x2, &one); /* back */
            if (!bn_cmp(&x, &pm1)) {
                break;
            }
        }
        if (i >= s) {
            return 0;
        }
    }
    return 1;
}

static int gen_prime(gcs_bn *p, int bits, uint32_t e, gcs_rand_bytes_fn rnd, void *ctx)
{
    unsigned char buf[4 * NL];
    unsigned short res[NSMALL];
    size_t bytes = (size_t)bits / 8, i;
    int tries;
    if (rnd(ctx, buf, bytes) != 0) {
        return -1;
    }
    buf[0] |= 0xc0; /* two top bits: the product has exactly 2*bits bits */
    buf[bytes - 1] |= 1;
    gcs_bn_from_bytes(p, buf, bytes);
    for (i = 0; i < NSMALL; i++) {
        res[i] = (unsigned short)bn_mod_small(p, small_primes[i]);
    }
    for (tries = 0; tries < 100000; tries++) {
        int ok = 1;
        for (i = 0; i < NSMALL; i++) {
            if (!res[i]) {
                ok = 0;
                break;
            }
        }
        if (ok && bn_mod_small(p, e) != 1 && probable_prime(p, 32, rnd, ctx)) {
            return bn_bits(p) == bits ? 0 : -1;
        }
        bn_add_small(p, 2);
        for (i = 0; i < NSMALL; i++) {
            res[i] = (unsigned short)((res[i] + 2) % small_primes[i]);
        }
    }
    return -1;
}

/* x^-1 mod m for small m (gcd 1 assumed) */
static uint32_t inv_small(uint32_t x, uint32_t m)
{
    int64_t t = 0, nt = 1, r = m, nr = x % m;
    while (nr) {
        int64_t q = r / nr, tmp;
        tmp = t - q * nt;
        t = nt;
        nt = tmp;
        tmp = r - q * nr;
        r = nr;
        nr = tmp;
    }
    if (t < 0) {
        t += m;
    }
    return (uint32_t)t;
}

int gcs_rsa_generate(gcs_rsa *k, int bits, gcs_rand_bytes_fn rnd, void *ctx)
{
    int attempt;
    if (bits < 512 || bits > 2048 || bits % 64 || !rnd) {
        return -1;
    }
    for (attempt = 0; attempt < 8; attempt++) {
        gcs_bn p, q, pm1, qm1, phi, t;
        unsigned char probe[256], enc[256], dec[256];
        uint32_t rem, kk;
        size_t nbytes = (size_t)bits / 8;
        memset(k, 0, sizeof(*k));
        k->bits = bits;
        k->e = 65537;
        if (gen_prime(&p, bits / 2, k->e, rnd, ctx) != 0 || gen_prime(&q, bits / 2, k->e, rnd, ctx) != 0 ||
            !bn_cmp(&p, &q)) {
            continue;
        }
        if (bn_mul(&k->n, &p, &q) != 0 || bn_bits(&k->n) != bits) {
            continue;
        }
        pm1 = p;
        qm1 = q;
        bn_sub_small(&pm1, 1);
        bn_sub_small(&qm1, 1);
        if (bn_mul(&phi, &pm1, &qm1) != 0) {
            continue;
        }
        /* d = (kk * phi + 1) / e with kk = (-phi)^-1 mod e */
        rem = bn_mod_small(&phi, k->e);
        if (!rem) {
            continue;
        }
        kk = inv_small(k->e - rem, k->e);
        t = phi;
        bn_mul_small(&t, kk);
        bn_add_small(&t, 1);
        if (bn_div_small(&t, k->e) != 0) {
            continue;
        }
        k->d = t;
        /* check the pair on a random message */
        if (rnd(ctx, probe, nbytes) != 0) {
            return -1;
        }
        probe[0] = 0;
        if (gcs_rsa_public(k, probe, nbytes, enc) != 0 || gcs_rsa_private(k, enc, nbytes, dec) != 0 ||
            memcmp(probe, dec, nbytes)) {
            continue;
        }
        return 0;
    }
    return -1;
}

static int rsa_op(const gcs_rsa *k, const gcs_bn *exp, const unsigned char *in, size_t len, unsigned char *out)
{
    gcs_bn c, m;
    size_t nbytes = (size_t)k->bits / 8;
    if (len > nbytes) {
        return -1;
    }
    gcs_bn_from_bytes(&c, in, len);
    if (bn_cmp(&c, &k->n) >= 0) {
        return -1;
    }
    if (gcs_bn_modexp(&m, &c, exp, &k->n) != 0) {
        return -1;
    }
    gcs_bn_to_bytes(&m, out, nbytes);
    return 0;
}

int gcs_rsa_private(const gcs_rsa *k, const unsigned char *in, size_t len, unsigned char *out)
{
    return rsa_op(k, &k->d, in, len, out);
}

int gcs_rsa_public(const gcs_rsa *k, const unsigned char *in, size_t len, unsigned char *out)
{
    gcs_bn e;
    bn_set(&e, k->e);
    return rsa_op(k, &e, in, len, out);
}

int gcs_rsa_decrypt_pkcs1(const gcs_rsa *k, const unsigned char *in, size_t len, unsigned char *out, size_t cap)
{
    unsigned char m[256];
    size_t nbytes = (size_t)k->bits / 8, i;
    if (nbytes > sizeof(m) || gcs_rsa_private(k, in, len, m) != 0) {
        return -1;
    }
    if (m[0] != 0 || (m[1] != 1 && m[1] != 2)) {
        return -1;
    }
    for (i = 2; i < nbytes && m[i]; i++) {
    }
    if (i >= nbytes || i < 3) {
        return -1;
    }
    i++;
    if (nbytes - i > cap) {
        return -1;
    }
    memcpy(out, m + i, nbytes - i);
    return (int)(nbytes - i);
}

void gcs_rsa_modulus_dec(const gcs_rsa *k, char *out, size_t cap)
{
    gcs_bn_to_dec(&k->n, out, cap);
}
