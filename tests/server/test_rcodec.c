/** @file test_rcodec.c
 *
 *  Rifts stream codec (server/src/gcs_rcodec.c): Blowfish, checksums,
 *  inflate and the stream packing.
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#include "gc_test.h"
#include "gcs_rcodec.h"

#include <stdlib.h>
#include <string.h>

/* zlib.compress(ZTEXT, 9) from Python: dynamic Huffman blocks */
static const unsigned char ZTEXT[] = "Rifts: Promise of Power - Coalition States, Tolkeen, the Federation of Magic. Rifts: Promise of Power - Coalition States, Tolkeen, the Federation of Magic. Rifts: Promise of Power - Coalition States, Tolkeen, the Federation of Magic. Rifts: Promise of Power - Coalition States, Tolkeen, the Federation of Magic. Rifts: Promise of Power - Coalition States, Tolkeen, the Federation of Magic. Rifts: Promise of Power - Coalition States, Tolkeen, the Federation of Magic. Mercenary, Psychic, Mage.";
static const unsigned char ZDATA[] = {
    0x78, 0xda, 0xed, 0xcc, 0xc1, 0x09, 0xc2, 0x40, 0x10, 0x85, 0xe1, 0x56,
    0x5e, 0x01, 0x6b, 0x0a, 0xc8, 0x55, 0xf0, 0x16, 0x58, 0x62, 0x1a, 0x18,
    0xd6, 0x17, 0x33, 0x18, 0x77, 0x60, 0x76, 0x40, 0xd2, 0xbd, 0x26, 0x55,
    0xe4, 0xe0, 0xf9, 0xe3, 0xff, 0x47, 0x9d, 0xa3, 0xf5, 0xc8, 0x6e, 0x6f,
    0x6d, 0x84, 0xcd, 0xc8, 0xf6, 0xa1, 0xe3, 0x82, 0xab, 0xc9, 0xaa, 0xa1,
    0x56, 0x71, 0x0f, 0x09, 0xb6, 0x84, 0xc9, 0xd6, 0x17, 0x59, 0x13, 0x62,
    0x21, 0x6e, 0x7c, 0xd0, 0xe5, 0xf0, 0x5f, 0x34, 0xc8, 0x53, 0x4b, 0x87,
    0xf1, 0x7f, 0x3b, 0xc1, 0x6d, 0xa0, 0x17, 0x56, 0xf1, 0x2d, 0x21, 0xb7,
    0xad, 0x2c, 0x5a, 0xd2, 0x4e, 0xec, 0xbe, 0x8f, 0x4d, 0xab, 0x3e
};

static const unsigned char KEY[8] = { 0x2c, 0xde, 0xcb, 0x4a, 0xa5, 0x6a, 0x35, 0x07 };

static void test_blowfish(void)
{
    gcs_blowfish bf;
    unsigned char k0[8], kf[8], k1[8] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef };
    gcs_u32 l, r;
    memset(k0, 0, 8);
    memset(kf, 0xff, 8);
    /* test vectors of the algorithm's author */
    gcs_bf_init(&bf, k0, 8);
    l = r = 0;
    gcs_bf_encrypt(&bf, &l, &r);
    CHECK(l == 0x4EF99745u && r == 0x6198DD78u);
    gcs_bf_init(&bf, kf, 8);
    l = r = 0xFFFFFFFFu;
    gcs_bf_encrypt(&bf, &l, &r);
    CHECK(l == 0x51866FD5u && r == 0xB85ECB8Au);
    gcs_bf_decrypt(&bf, &l, &r);
    CHECK(l == 0xFFFFFFFFu && r == 0xFFFFFFFFu);
    gcs_bf_init(&bf, k1, 8);
    l = 0x11111111u;
    r = 0x11111111u;
    gcs_bf_encrypt(&bf, &l, &r);
    CHECK(l == 0x61F9C380u && r == 0x2281B096u);
}

static void test_cbc(void)
{
    gcs_blowfish bf;
    unsigned char d[24], o[24];
    int i;
    for (i = 0; i < 24; i++) {
        d[i] = o[i] = (unsigned char)i;
    }
    gcs_bf_init(&bf, KEY, 8);
    gcs_bf_cbc_encrypt(&bf, d, 24);
    CHECK(memcmp(d, o, 24) != 0);
    /* chaining: equal plain blocks give different cipher blocks */
    memset(d, 0, 24);
    gcs_bf_cbc_encrypt(&bf, d, 24);
    CHECK(memcmp(d, d + 8, 8) != 0);
    gcs_bf_cbc_decrypt(&bf, d, 24);
    for (i = 0; i < 24; i++) {
        CHECK_EQ(d[i], 0);
    }
}

static void test_checksums(void)
{
    CHECK(gcs_crc32(0, (const unsigned char *)"123456789", 9) == 0xCBF43926u);
    CHECK(gcs_crc32(gcs_crc32(0, (const unsigned char *)"1234", 4), (const unsigned char *)"56789", 5) == 0xCBF43926u);
    CHECK(gcs_adler32(1, (const unsigned char *)"Wikipedia", 9) == 0x11E60398u);
}

static void test_inflate(void)
{
    unsigned char out[1024], z[1100];
    long n = gcs_zlib_inflate(ZDATA, sizeof(ZDATA), out, sizeof(out));
    size_t zl;
    CHECK_EQ(n, (long)strlen((const char *)ZTEXT));
    CHECK(n > 0 && !memcmp(out, ZTEXT, (size_t)n));
    /* too small, truncated, damaged */
    CHECK_EQ(gcs_zlib_inflate(ZDATA, sizeof(ZDATA), out, 100), -1);
    CHECK_EQ(gcs_zlib_inflate(ZDATA, sizeof(ZDATA) - 5, out, sizeof(out)), -1);
    memcpy(z, ZDATA, sizeof(ZDATA));
    z[sizeof(ZDATA) - 1] ^= 1; /* Adler-32 */
    CHECK_EQ(gcs_zlib_inflate(z, sizeof(ZDATA), out, sizeof(out)), -1);
    /* stored blocks round trip */
    zl = gcs_zlib_stored(ZTEXT, sizeof(ZTEXT) - 1, z);
    CHECK(zl <= gcs_zlib_stored_bound(sizeof(ZTEXT) - 1));
    n = gcs_zlib_inflate(z, zl, out, sizeof(out));
    CHECK(n == (long)sizeof(ZTEXT) - 1 && !memcmp(out, ZTEXT, (size_t)n));
    zl = gcs_zlib_stored(ZTEXT, 0, z);
    CHECK_EQ(gcs_zlib_inflate(z, zl, out, sizeof(out)), 0);
}

static void test_pack(void)
{
    unsigned char big[70000], *p, *u;
    size_t pl, ul, len;
    int i;
    for (i = 0; i < (int)sizeof(big); i++) {
        big[i] = (unsigned char)(i * 7 + (i >> 8));
    }
    /* every padding length, and more than one stored block */
    for (len = 0; len < 40; len++) {
        p = gcs_rc_pack(KEY, big, len, &pl);
        CHECK(p != NULL && pl % 8 == 0);
        u = p ? gcs_rc_unpack(KEY, p, pl, &ul) : NULL;
        CHECK(u != NULL && ul == len && !memcmp(u, big, len));
        free(p);
        free(u);
    }
    p = gcs_rc_pack(KEY, big, sizeof(big), &pl);
    u = gcs_rc_unpack(KEY, p, pl, &ul);
    CHECK(u != NULL && ul == sizeof(big) && !memcmp(u, big, sizeof(big)));
    free(u);
    /* damaged data or wrong key */
    p[pl / 2] ^= 0x10;
    CHECK(gcs_rc_unpack(KEY, p, pl, &ul) == NULL);
    p[pl / 2] ^= 0x10;
    {
        unsigned char k2[8];
        memcpy(k2, KEY, 8);
        k2[0] ^= 1;
        CHECK(gcs_rc_unpack(k2, p, pl, &ul) == NULL);
    }
    CHECK(gcs_rc_unpack(KEY, p, pl - 3, &ul) == NULL);
    free(p);
    /* a stream packed with real deflate (as the game sends it) */
    {
        gcs_blowfish bf;
        unsigned char buf[8 + sizeof(ZDATA) + 8];
        size_t zl = sizeof(ZDATA), pad = 8 - zl % 8;
        memset(buf, 0, sizeof(buf));
        memcpy(buf + 8, ZDATA, zl);
        buf[8 + zl + pad - 1] = (unsigned char)pad;
        zl += pad;
        buf[0] = (unsigned char)(sizeof(ZTEXT) - 1);
        buf[1] = (unsigned char)((sizeof(ZTEXT) - 1) >> 8);
        {
            gcs_u32 c = gcs_crc32(0, buf + 8, zl);
            buf[4] = (unsigned char)c;
            buf[5] = (unsigned char)(c >> 8);
            buf[6] = (unsigned char)(c >> 16);
            buf[7] = (unsigned char)(c >> 24);
        }
        gcs_bf_init(&bf, KEY, 8);
        gcs_bf_cbc_encrypt(&bf, buf, zl + 8);
        u = gcs_rc_unpack(KEY, buf, zl + 8, &ul);
        CHECK(u != NULL && ul == sizeof(ZTEXT) - 1 && !memcmp(u, ZTEXT, ul));
        free(u);
    }
}

int main(void)
{
    RUN(test_blowfish);
    RUN(test_cbc);
    RUN(test_checksums);
    RUN(test_inflate);
    RUN(test_pack);
    TEST_MAIN_END();
}
