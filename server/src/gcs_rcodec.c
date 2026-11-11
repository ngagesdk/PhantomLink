/** @file gcs_rcodec.c
 *
 *  Rifts stream codec: Blowfish, CRC-32, Adler-32, inflate and the
 *  stream packing, see gcs_rcodec.h.  Written from the algorithm
 *  descriptions (Blowfish: Schneier 1993; deflate: RFC 1951; zlib: RFC
 *  1950).
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#include "gcs_rcodec.h"
#include "gcs_blowfish_tab.h"

#include <stdlib.h>
#include <string.h>

/* ---- Blowfish ---- */

static gcs_u32 bf_f(const gcs_blowfish *bf, gcs_u32 x)
{
    gcs_u32 y = bf->s[0][x >> 24] + bf->s[1][(x >> 16) & 0xff];
    y ^= bf->s[2][(x >> 8) & 0xff];
    return y + bf->s[3][x & 0xff];
}

void gcs_bf_encrypt(const gcs_blowfish *bf, gcs_u32 *l, gcs_u32 *r)
{
    gcs_u32 xl = *l, xr = *r, t;
    int i;
    for (i = 0; i < 16; i++) {
        xl ^= bf->p[i];
        xr ^= bf_f(bf, xl);
        t = xl;
        xl = xr;
        xr = t;
    }
    t = xl;
    xl = xr;
    xr = t;
    xr ^= bf->p[16];
    xl ^= bf->p[17];
    *l = xl;
    *r = xr;
}

void gcs_bf_decrypt(const gcs_blowfish *bf, gcs_u32 *l, gcs_u32 *r)
{
    gcs_u32 xl = *l, xr = *r, t;
    int i;
    for (i = 17; i > 1; i--) {
        xl ^= bf->p[i];
        xr ^= bf_f(bf, xl);
        t = xl;
        xl = xr;
        xr = t;
    }
    t = xl;
    xl = xr;
    xr = t;
    xr ^= bf->p[1];
    xl ^= bf->p[0];
    *l = xl;
    *r = xr;
}

void gcs_bf_init(gcs_blowfish *bf, const unsigned char *key, int keylen)
{
    int i, j, k = 0;
    gcs_u32 l = 0, r = 0;
    for (i = 0; i < 18; i++) {
        bf->p[i] = GCS_BF_PI[i];
    }
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 256; j++) {
            bf->s[i][j] = GCS_BF_PI[18 + 256 * i + j];
        }
    }
    for (i = 0; i < 18; i++) {
        gcs_u32 w = 0;
        for (j = 0; j < 4; j++) {
            w = (w << 8) | key[k];
            k = (k + 1) % keylen;
        }
        bf->p[i] ^= w;
    }
    for (i = 0; i < 18; i += 2) {
        gcs_bf_encrypt(bf, &l, &r);
        bf->p[i] = l;
        bf->p[i + 1] = r;
    }
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 256; j += 2) {
            gcs_bf_encrypt(bf, &l, &r);
            bf->s[i][j] = l;
            bf->s[i][j + 1] = r;
        }
    }
}

static gcs_u32 rd32(const unsigned char *p)
{
    return (gcs_u32)p[0] | ((gcs_u32)p[1] << 8) | ((gcs_u32)p[2] << 16) | ((gcs_u32)p[3] << 24);
}

static void wr32(unsigned char *p, gcs_u32 v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

void gcs_bf_cbc_encrypt(const gcs_blowfish *bf, unsigned char *d, size_t len)
{
    gcs_u32 iv0 = 0, iv1 = 0, l, r;
    size_t i;
    for (i = 0; i + 8 <= len; i += 8) {
        l = rd32(d + i) ^ iv0;
        r = rd32(d + i + 4) ^ iv1;
        gcs_bf_encrypt(bf, &l, &r);
        wr32(d + i, l);
        wr32(d + i + 4, r);
        iv0 = l;
        iv1 = r;
    }
}

void gcs_bf_cbc_decrypt(const gcs_blowfish *bf, unsigned char *d, size_t len)
{
    gcs_u32 iv0 = 0, iv1 = 0, l, r, c0, c1;
    size_t i;
    for (i = 0; i + 8 <= len; i += 8) {
        c0 = l = rd32(d + i);
        c1 = r = rd32(d + i + 4);
        gcs_bf_decrypt(bf, &l, &r);
        wr32(d + i, l ^ iv0);
        wr32(d + i + 4, r ^ iv1);
        iv0 = c0;
        iv1 = c1;
    }
}

/* ---- checksums ---- */

gcs_u32 gcs_crc32(gcs_u32 crc, const unsigned char *d, size_t len)
{
    static gcs_u32 table[256];
    static int ready;
    size_t i;
    if (!ready) {
        gcs_u32 n, c;
        int k;
        for (n = 0; n < 256; n++) {
            c = n;
            for (k = 0; k < 8; k++) {
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            table[n] = c;
        }
        ready = 1;
    }
    crc ^= 0xFFFFFFFFu;
    for (i = 0; i < len; i++) {
        crc = table[(crc ^ d[i]) & 0xff] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

gcs_u32 gcs_adler32(gcs_u32 adler, const unsigned char *d, size_t len)
{
    gcs_u32 a = adler & 0xffff, b = adler >> 16;
    size_t i;
    for (i = 0; i < len; i++) {
        a = (a + d[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

/* ---- inflate (RFC 1951) ---- */

typedef struct
{
    const unsigned char *in;
    size_t inlen, inpos;
    unsigned char *out;
    size_t outcap, outpos;
    gcs_u32 bitbuf;
    int bitcnt;
} inf_state;

#define MAXBITS 15

typedef struct
{
    short count[MAXBITS + 1];
    short symbol[288];
} huffman;

static int bits(inf_state *s, int need)
{
    gcs_u32 val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->inpos >= s->inlen) {
            return -1;
        }
        val |= (gcs_u32)s->in[s->inpos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= need;
    return (int)(val & ((1u << need) - 1));
}

static int stored(inf_state *s)
{
    unsigned len, nlen;
    s->bitbuf = 0;
    s->bitcnt = 0;
    if (s->inpos + 4 > s->inlen) {
        return -1;
    }
    len = s->in[s->inpos] | (s->in[s->inpos + 1] << 8);
    nlen = s->in[s->inpos + 2] | (s->in[s->inpos + 3] << 8);
    s->inpos += 4;
    if (len != (~nlen & 0xffffu) || s->inpos + len > s->inlen || s->outpos + len > s->outcap) {
        return -1;
    }
    memcpy(s->out + s->outpos, s->in + s->inpos, len);
    s->inpos += len;
    s->outpos += len;
    return 0;
}

static int decode(inf_state *s, const huffman *h)
{
    int code = 0, first = 0, index = 0, len, count, b;
    for (len = 1; len <= MAXBITS; len++) {
        b = bits(s, 1);
        if (b < 0) {
            return -1;
        }
        code |= b;
        count = h->count[len];
        if (code - count < first) {
            return h->symbol[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

/* 0 ok, <0 over-subscribed or bad */
static int construct(huffman *h, const short *length, int n)
{
    short offs[MAXBITS + 1];
    int sym, len, left;
    for (len = 0; len <= MAXBITS; len++) {
        h->count[len] = 0;
    }
    for (sym = 0; sym < n; sym++) {
        h->count[length[sym]]++;
    }
    if (h->count[0] == n) {
        return 0;
    }
    left = 1;
    for (len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) {
            return -1;
        }
    }
    offs[1] = 0;
    for (len = 1; len < MAXBITS; len++) {
        offs[len + 1] = (short)(offs[len] + h->count[len]);
    }
    for (sym = 0; sym < n; sym++) {
        if (length[sym] != 0) {
            h->symbol[offs[length[sym]]++] = (short)sym;
        }
    }
    return 0;
}

static int codes(inf_state *s, const huffman *lencode, const huffman *distcode)
{
    static const short lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
                                     31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
    static const short lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
    static const short dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                     193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
    static const short dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
    int symbol, e;
    size_t len, dist;
    for (;;) {
        symbol = decode(s, lencode);
        if (symbol < 0) {
            return -1;
        }
        if (symbol < 256) {
            if (s->outpos >= s->outcap) {
                return -1;
            }
            s->out[s->outpos++] = (unsigned char)symbol;
        } else if (symbol == 256) {
            return 0;
        } else {
            symbol -= 257;
            if (symbol >= 29) {
                return -1;
            }
            e = bits(s, lext[symbol]);
            if (e < 0) {
                return -1;
            }
            len = (size_t)(lbase[symbol] + e);
            symbol = decode(s, distcode);
            if (symbol < 0 || symbol >= 30) {
                return -1;
            }
            e = bits(s, dext[symbol]);
            if (e < 0) {
                return -1;
            }
            dist = (size_t)(dbase[symbol] + e);
            if (dist > s->outpos || s->outpos + len > s->outcap) {
                return -1;
            }
            while (len--) {
                s->out[s->outpos] = s->out[s->outpos - dist];
                s->outpos++;
            }
        }
    }
}

static int fixed(inf_state *s)
{
    huffman lencode, distcode;
    short lengths[288];
    int sym;
    for (sym = 0; sym < 144; sym++) {
        lengths[sym] = 8;
    }
    for (; sym < 256; sym++) {
        lengths[sym] = 9;
    }
    for (; sym < 280; sym++) {
        lengths[sym] = 7;
    }
    for (; sym < 288; sym++) {
        lengths[sym] = 8;
    }
    construct(&lencode, lengths, 288);
    for (sym = 0; sym < 30; sym++) {
        lengths[sym] = 5;
    }
    construct(&distcode, lengths, 30);
    return codes(s, &lencode, &distcode);
}

static int dynamic(inf_state *s)
{
    static const short order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    short lengths[320];
    huffman lencode, distcode;
    int nlen, ndist, ncode, index, symbol, len, rep, b;

    nlen = bits(s, 5);
    ndist = bits(s, 5);
    ncode = bits(s, 4);
    if (nlen < 0 || ndist < 0 || ncode < 0) {
        return -1;
    }
    nlen += 257;
    ndist += 1;
    ncode += 4;
    if (nlen > 286 || ndist > 30) {
        return -1;
    }
    for (index = 0; index < ncode; index++) {
        b = bits(s, 3);
        if (b < 0) {
            return -1;
        }
        lengths[order[index]] = (short)b;
    }
    for (; index < 19; index++) {
        lengths[order[index]] = 0;
    }
    if (construct(&lencode, lengths, 19) != 0) {
        return -1;
    }
    index = 0;
    while (index < nlen + ndist) {
        symbol = decode(s, &lencode);
        if (symbol < 0) {
            return -1;
        }
        if (symbol < 16) {
            lengths[index++] = (short)symbol;
            continue;
        }
        len = 0;
        if (symbol == 16) {
            if (index == 0) {
                return -1;
            }
            len = lengths[index - 1];
            rep = bits(s, 2);
            rep = rep < 0 ? -1 : 3 + rep;
        } else if (symbol == 17) {
            rep = bits(s, 3);
            rep = rep < 0 ? -1 : 3 + rep;
        } else {
            rep = bits(s, 7);
            rep = rep < 0 ? -1 : 11 + rep;
        }
        if (rep < 0 || index + rep > nlen + ndist) {
            return -1;
        }
        while (rep--) {
            lengths[index++] = (short)len;
        }
    }
    if (lengths[256] == 0) {
        return -1;
    }
    if (construct(&lencode, lengths, nlen) != 0 || construct(&distcode, lengths + nlen, ndist) != 0) {
        return -1;
    }
    return codes(s, &lencode, &distcode);
}

long gcs_zlib_inflate(const unsigned char *in, size_t inlen, unsigned char *out, size_t outcap)
{
    inf_state s;
    int last, type, err;
    if (inlen < 6 || (in[0] & 0x0f) != 8 || ((in[0] << 8) | in[1]) % 31 != 0 || (in[1] & 0x20)) {
        return -1;
    }
    memset(&s, 0, sizeof(s));
    s.in = in + 2;
    s.inlen = inlen - 2;
    s.out = out;
    s.outcap = outcap;
    do {
        last = bits(&s, 1);
        type = bits(&s, 2);
        if (last < 0 || type < 0) {
            return -1;
        }
        if (type == 0) {
            err = stored(&s);
        } else if (type == 1) {
            err = fixed(&s);
        } else if (type == 2) {
            err = dynamic(&s);
        } else {
            err = -1;
        }
        if (err) {
            return -1;
        }
    } while (!last);
    /* Adler-32 of the output follows, big endian, on a byte boundary */
    if (s.inpos + 4 > s.inlen) {
        return -1;
    }
    {
        const unsigned char *a = s.in + s.inpos;
        gcs_u32 want = ((gcs_u32)a[0] << 24) | ((gcs_u32)a[1] << 16) | ((gcs_u32)a[2] << 8) | a[3];
        if (want != gcs_adler32(1, out, s.outpos)) {
            return -1;
        }
    }
    return (long)s.outpos;
}

size_t gcs_zlib_stored(const unsigned char *in, size_t len, unsigned char *out)
{
    size_t o = 0, done = 0;
    gcs_u32 ad = gcs_adler32(1, in, len);
    out[o++] = 0x78; /* deflate, 32K window */
    out[o++] = 0x01; /* no dictionary, fastest; (0x7801 % 31 == 0) */
    do {
        size_t k = len - done > 65535 ? 65535 : len - done;
        out[o++] = (unsigned char)(done + k == len ? 1 : 0); /* BFINAL, BTYPE 00 */
        out[o++] = (unsigned char)k;
        out[o++] = (unsigned char)(k >> 8);
        out[o++] = (unsigned char)~k;
        out[o++] = (unsigned char)(~k >> 8);
        memcpy(out + o, in + done, k);
        o += k;
        done += k;
    } while (done < len);
    out[o++] = (unsigned char)(ad >> 24);
    out[o++] = (unsigned char)(ad >> 16);
    out[o++] = (unsigned char)(ad >> 8);
    out[o++] = (unsigned char)ad;
    return o;
}

/* ---- Rifts streams ---- */

#define RC_MAX_PLAIN (1024u * 1024u)

unsigned char *gcs_rc_pack(const unsigned char *key, const unsigned char *in, size_t len, size_t *outlen)
{
    gcs_blowfish *bf;
    unsigned char *buf;
    size_t zl, pad;
    if (len > RC_MAX_PLAIN) {
        return NULL;
    }
    buf = (unsigned char *)calloc(1, 8 + gcs_zlib_stored_bound(len) + 8);
    bf = (gcs_blowfish *)malloc(sizeof(*bf));
    if (!buf || !bf) {
        free(buf);
        free(bf);
        return NULL;
    }
    zl = gcs_zlib_stored(in, len, buf + 8);
    pad = 8 - zl % 8;
    buf[8 + zl + pad - 1] = (unsigned char)pad;
    zl += pad;
    wr32(buf, (gcs_u32)len);
    wr32(buf + 4, gcs_crc32(0, buf + 8, zl));
    gcs_bf_init(bf, key, GCS_RC_KEYLEN);
    gcs_bf_cbc_encrypt(bf, buf, zl + 8);
    free(bf);
    *outlen = zl + 8;
    return buf;
}

unsigned char *gcs_rc_unpack(const unsigned char *key, const unsigned char *in, size_t len, size_t *outlen)
{
    gcs_blowfish *bf;
    unsigned char *tmp, *out;
    gcs_u32 size;
    size_t pad;
    long got;
    if (len < 16 || len % 8 != 0) {
        return NULL;
    }
    tmp = (unsigned char *)malloc(len);
    bf = (gcs_blowfish *)malloc(sizeof(*bf));
    if (!tmp || !bf) {
        free(tmp);
        free(bf);
        return NULL;
    }
    memcpy(tmp, in, len);
    gcs_bf_init(bf, key, GCS_RC_KEYLEN);
    gcs_bf_cbc_decrypt(bf, tmp, len);
    free(bf);
    size = rd32(tmp);
    pad = tmp[len - 1] % 8;
    if (size > RC_MAX_PLAIN || rd32(tmp + 4) != gcs_crc32(0, tmp + 8, len - 8) || len - 8 < pad) {
        free(tmp);
        return NULL;
    }
    out = (unsigned char *)malloc(size ? size : 1);
    if (!out) {
        free(tmp);
        return NULL;
    }
    got = gcs_zlib_inflate(tmp + 8, len - 8 - pad, out, size);
    free(tmp);
    if (got != (long)size) {
        free(out);
        return NULL;
    }
    *outlen = size;
    return out;
}
