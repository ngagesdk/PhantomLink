/** @file test_ssx.c
 *
 *  SSX Out of Bounds (gcs_ssx) against a simulated AIRPLAYSERVER.EXE: the
 *  client side is rebuilt from the game's binary, not from the server:
 *
 *   - message header, acknowledgements (type 0xfa), duplicates
 *   - key exchange 18 -> 133 -> 19 -> 134: the client parses the decimal
 *     modulus, pads its 128-bit key with PKCS#1 v1.5 (block type 2) and
 *     encrypts it with the public exponent
 *   - encrypted messages exactly as the client builds and checks them
 *     (0x740c / 0x720c): IV string, plain length, triple DES CBC over
 *     plain + 8 bytes of MD5 or SHA-1 + zero padding + a block of 08;
 *     the receiver decrypts all but the last block and accepts plain
 *     lengths L+8 <= n < L+16 only
 *   - discovery with the billing DLL download (50, 150, 51, 121)
 *   - accounts, the menu tree, shadow and career uploads (chunks of 1255
 *     bytes like the game) and downloads, rankings, persistence
 *   - the same through gcs_native with datagrams arriving in UDP_PART
 *     pieces (gc_net.h), as a bridge sends SSX's large upload chunks.
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#include "gc_net.h"
#include "gc_test.h"
#include "gcs_accounts.h"
#include "gcs_native.h"
#include "gcs_ssx.h"
#include "gcs_ssxcrypt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const gc_u8 SRV[4] = { 10, 64, 0, 1 };
static gc_u32 g_now = 1000;
static int g_verbose;

/* ---- datagrams from the server ---- */

#define QMAX 512
typedef struct
{
    gc_u8 d[2048];
    size_t n;
} dgram;
static dgram g_q[QMAX];
static int g_qn;
static size_t g_max_dgram;

static void q_push(const gc_u8 *d, size_t n)
{
    if (g_qn < QMAX && n <= sizeof(g_q[0].d)) {
        memcpy(g_q[g_qn].d, d, n);
        g_q[g_qn].n = n;
        g_qn++;
    }
    if (n > g_max_dgram) {
        g_max_dgram = n;
    }
}

static void x_send(void *ctx, void *key, const gc_u8 addr[4], gc_u16 port, gc_u16 peer_port, const gc_u8 *d,
                   size_t len)
{
    (void)ctx;
    (void)key;
    (void)addr;
    (void)port;
    (void)peer_port;
    q_push(d, len);
}

static void x_log(void *ctx, int level, const char *line)
{
    (void)ctx;
    if (g_verbose || level <= 1) {
        printf("  [%d] %s\n", level, line);
    }
}

static gc_u32 g_rng = 0x1234567u;
static gc_u32 x_rand(void *ctx)
{
    (void)ctx;
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

/* ---- the transport: straight into gcs_ssx, or through gcs_native ---- */

static gcs_ssx *g_ssx;
static gcs_native *g_nat;
static size_t g_cap = 1094; /* tunnel message size for the gcs_native path */
static gc_u16 g_part_id;

static void n_send(void *ctx, void *key, const gc_u8 *data, size_t len)
{
    gcn_msg m;
    (void)ctx;
    (void)key;
    if (gcn_decode(data, len, &m) && m.type == GCN_MT_UDP) {
        CHECK_EQ(m.port, GCSX_PORT);
        q_push(m.data, m.len);
    }
}

static int emit_native(void *ctx, const gc_u8 *msg, size_t len)
{
    (void)ctx;
    gcs_native_input(g_nat, (void *)1, msg, len);
    return 1;
}

static void to_server(gc_u16 peer_port, const gc_u8 *d, size_t len)
{
    if (g_nat) {
        gcn_msg m;
        memset(&m, 0, sizeof(m));
        m.type = GCN_MT_UDP;
        memcpy(m.addr, SRV, 4);
        m.port = GCSX_PORT;
        m.peer_port = peer_port;
        m.data = d;
        m.len = len;
        CHECK(gcn_send_udp(&m, g_cap, ++g_part_id, emit_native, NULL));
    } else {
        gcs_ssx_input(g_ssx, (void *)1, SRV, GCSX_PORT, peer_port, d, len, g_now);
    }
}

/* ---- the client (AIRPLAYSERVER.EXE) ---- */

typedef struct
{
    gc_u16 port;
    gc_u32 seq;
    int have_key, sha;
    gcs_3des des;
    gc_u32 seen[1024];
    int nseen;
    int drop_acks; /* do not acknowledge (resend test) */
} client;

typedef struct
{
    int type;
    gc_u8 *d;
    size_t n, off;
} msg;

static void put32(gc_u8 *p, gc_u32 v)
{
    p[0] = (gc_u8)v;
    p[1] = (gc_u8)(v >> 8);
    p[2] = (gc_u8)(v >> 16);
    p[3] = (gc_u8)(v >> 24);
}

static gc_u32 get32(const gc_u8 *p)
{
    return (gc_u32)p[0] | ((gc_u32)p[1] << 8) | ((gc_u32)p[2] << 16) | ((gc_u32)p[3] << 24);
}

typedef struct
{
    gc_u8 d[70000];
    size_t n;
} buf;

static void b_u8(buf *b, gc_u32 v)
{
    b->d[b->n++] = (gc_u8)v;
}
static void b_u16(buf *b, gc_u32 v)
{
    b->d[b->n++] = (gc_u8)v;
    b->d[b->n++] = (gc_u8)(v >> 8);
}
static void b_u32(buf *b, gc_u32 v)
{
    put32(b->d + b->n, v);
    b->n += 4;
}
static void b_raw(buf *b, const void *p, size_t n)
{
    memcpy(b->d + b->n, p, n);
    b->n += n;
}
static void b_str(buf *b, const char *s)
{
    b_u32(b, (gc_u32)strlen(s));
    b_raw(b, s, strlen(s));
}

static void chk8(int sha, const gc_u8 *d, size_t n, gc_u8 out[8])
{
    gc_u8 h[20];
    if (sha) {
        gcs_sha1(d, n, h);
    } else {
        gcs_md5(d, n, h);
    }
    memcpy(out, h, 8);
}

/* f_740c */
static void c_send(client *c, int type, int noack, int enc, const buf *body)
{
    static gc_u8 out[80000];
    size_t n = 0;
    gc_u32 f = 0xff | (enc ? 0x8000u : 0);
    put32(out, c->seq++);
    out[4] = (gc_u8)type;
    out[5] = (gc_u8)noack;
    out[6] = (gc_u8)f;
    out[7] = (gc_u8)(f >> 8);
    n = 8;
    if (!enc) {
        memcpy(out + n, body->d, body->n);
        n += body->n;
    } else {
        size_t L = body->n, pad = (L % 8) ? L + 8 - L % 8 : L, i;
        static gc_u8 p[80000];
        gc_u8 iv[8];
        memcpy(p, body->d, L);
        chk8(c->sha, body->d, L, p + L);
        memset(p + L + 8, 0, pad - L);
        memset(p + pad + 8, 8, 8);
        for (i = 0; i < 8; i++) {
            iv[i] = (gc_u8)x_rand(NULL);
        }
        gcs_3des_cbc_encrypt(&c->des, iv, p, p, pad + 16);
        put32(out + n, 8);
        memcpy(out + n + 4, iv, 8);
        put32(out + n + 12, (gc_u32)L);
        memcpy(out + n + 16, p, pad + 16);
        n += 16 + pad + 16;
    }
    to_server(c->port, out, n);
}

static void c_req(client *c, int type, const buf *body)
{
    c_send(c, type, 0, c->have_key, body);
}

/* one datagram from the server: ack, duplicate check, decryption (f_720c) */
static int c_recv(client *c, const dgram *g, msg *m)
{
    gc_u32 seq;
    int i;
    memset(m, 0, sizeof(*m));
    CHECK(g->n >= 8);
    if (g->n < 8) {
        return 0;
    }
    seq = get32(g->d);
    m->type = g->d[4];
    if (!g->d[5] && !c->drop_acks) {
        buf a;
        a.n = 0;
        b_u32(&a, seq);
        c_send(c, 0xfa, 1, 0, &a);
    }
    for (i = 0; i < c->nseen; i++) {
        if (c->seen[i] == seq) {
            return 0; /* AIRPLAYSERVER.EXE 0x8144: every sequence once */
        }
    }
    if (c->nseen < 1024) {
        c->seen[c->nseen++] = seq;
    }
    if (m->type == 0xfa) {
        return 0;
    }
    if (g->d[7] & 0x80) {
        const gc_u8 *b = g->d + 8;
        size_t bn = g->n - 8, clen;
        gc_u32 ivlen, L;
        gc_u8 *plain, k[8];
        CHECK(c->have_key);
        CHECK(bn >= 16);
        ivlen = get32(b);
        CHECK_EQ(ivlen, 8);
        L = get32(b + 12);
        clen = bn - 16;
        CHECK(clen % 8 == 0);
        CHECK(clen >= 16);
        plain = (gc_u8 *)malloc(clen);
        gcs_3des_cbc_decrypt(&c->des, b + 4, b + 16, plain, clen - 8); /* all but the last block */
        CHECK(L + 8 <= clen - 8 && clen - 8 < L + 16);
        chk8(c->sha, plain, L, k);
        CHECK(!memcmp(k, plain + L, 8));
        m->d = plain;
        m->n = L;
    } else {
        m->d = (gc_u8 *)malloc(g->n - 8 + 1);
        memcpy(m->d, g->d + 8, g->n - 8);
        m->n = g->n - 8;
    }
    return 1;
}

/* the next message of type from the server (others are dropped) */
static int expect(client *c, int type, msg *out)
{
    int tries;
    for (tries = 0; tries < 4; tries++) {
        while (g_qn > 0) {
            dgram g = g_q[0];
            msg m;
            memmove(g_q, g_q + 1, (size_t)(g_qn - 1) * sizeof(dgram));
            g_qn--;
            if (!c_recv(c, &g, &m)) {
                continue;
            }
            if (m.type == type) {
                *out = m;
                return 1;
            }
            if (g_verbose) {
                printf("  skipped message %d\n", m.type);
            }
            free(m.d);
        }
        g_now += 100;
        if (g_nat) {
            gcs_native_poll(g_nat, g_now);
        } else {
            gcs_ssx_poll(g_ssx, g_now);
        }
    }
    fprintf(stderr, "message %d did not come\n", type);
    CHECK(0);
    return 0;
}

static gc_u32 m_u8(msg *m)
{
    CHECK(m->off + 1 <= m->n);
    return m->off < m->n ? m->d[m->off++] : 0;
}
static gc_u32 m_u16(msg *m)
{
    gc_u32 v;
    CHECK(m->off + 2 <= m->n);
    v = m->d[m->off] | (m->d[m->off + 1] << 8);
    m->off += 2;
    return v;
}
static gc_u32 m_u32(msg *m)
{
    gc_u32 v;
    CHECK(m->off + 4 <= m->n);
    v = get32(m->d + m->off);
    m->off += 4;
    return v;
}
static void m_str(msg *m, char *out, size_t cap)
{
    gc_u32 n = m_u32(m);
    CHECK(m->off + n <= m->n);
    if (n >= cap) {
        n = (gc_u32)cap - 1;
    }
    memcpy(out, m->d + m->off, n);
    out[n] = 0;
    m->off += get32(m->d + m->off - 4);
}

/* decimal string -> big endian bytes (the client's own parser, 0x8290) */
static void dec_to_bytes(const char *s, gc_u8 *out, size_t k)
{
    size_t i;
    memset(out, 0, k);
    for (; *s; s++) {
        unsigned carry = (unsigned)(*s - '0');
        for (i = k; i-- > 0;) {
            unsigned v = out[i] * 10u + carry;
            out[i] = (gc_u8)v;
            carry = v >> 8;
        }
    }
}

/* 18 -> 133 -> 19 -> 134 */
static void handshake(client *c)
{
    buf b;
    msg m;
    char n_dec[800], e_dec[32];
    gc_u8 nb[128], key[16], blk[128], enc[128];
    gcs_rsa pub;
    size_t i;
    b.n = 0;
    c->have_key = 0;
    c_send(c, 18, 0, 0, &b);
    if (!expect(c, 133, &m)) {
        return;
    }
    m_str(&m, n_dec, sizeof(n_dec));
    m_str(&m, e_dec, sizeof(e_dec));
    free(m.d);
    CHECK(strlen(n_dec) >= 300); /* 1024 bits */
    CHECK(!strcmp(e_dec, "65537"));
    dec_to_bytes(n_dec, nb, 128);
    memset(&pub, 0, sizeof(pub));
    pub.bits = 1024;
    pub.e = 65537;
    gcs_bn_from_bytes(&pub.n, nb, 128);
    for (i = 0; i < 16; i++) {
        key[i] = (gc_u8)x_rand(NULL);
    }
    key[0] |= 0x80; /* NewRandomLC: 16 bytes */
    blk[0] = 0;
    blk[1] = 2;
    for (i = 2; i < 128 - 17; i++) {
        blk[i] = (gc_u8)(x_rand(NULL) % 255 + 1);
    }
    blk[128 - 17] = 0;
    memcpy(blk + 128 - 16, key, 16);
    CHECK(gcs_rsa_public(&pub, blk, 128, enc) == 0);
    b.n = 0;
    b_u32(&b, 128);
    b_raw(&b, enc, 128);
    c_send(c, 19, 0, 0, &b);
    gcs_3des_setkey(&c->des, key);
    c->have_key = 1;
    if (expect(c, 134, &m)) {
        CHECK_EQ(m_u8(&m), 1);
        free(m.d);
    }
}

/* the common start of the account messages (0x47b0) */
static void acc_hdr(buf *b)
{
    int i;
    b_u16(b, 3);
    b_u16(b, 39);
    for (i = 0; i < 3; i++) {
        b_u16(b, 10);
    }
    b_str(b, "0123456789abcdef0123456789abcdef");
    b_u32(b, 30003);
}

static int logon(client *c, const char *user, const char *pw)
{
    buf b;
    msg m;
    int r = -1;
    b.n = 0;
    acc_hdr(&b);
    b_str(&b, user);
    b_str(&b, pw);
    b_str(&b, "351234567890123");
    b_str(&b, "NGAGE");
    b_u8(&b, 0);
    c_req(c, 0, &b);
    if (expect(c, 120, &m)) {
        char t[300];
        r = (int)m_u8(&m);
        m_u8(&m);
        m_u32(&m);
        m_u8(&m);
        m_str(&m, t, sizeof(t));
        free(m.d);
    }
    return r;
}

static int create(client *c, const char *user, const char *pw, const char *imei)
{
    buf b;
    msg m;
    int r = -1;
    b.n = 0;
    acc_hdr(&b);
    b_str(&b, user);
    b_str(&b, pw);
    b_str(&b, "First pet?");
    b_str(&b, "Rex");
    b_str(&b, imei);
    b_str(&b, "NGAGE");
    b_u8(&b, 0);
    b_str(&b, "1980");
    b_u8(&b, 1);
    c_req(c, 16, &b);
    if (expect(c, 120, &m)) {
        r = (int)m_u8(&m);
        free(m.d);
    }
    return r;
}

typedef struct
{
    gc_u32 id, type, v1, v2, v3;
    char name[32];
} ent;

static int objlist(client *c, gc_u32 parent, ent *e, int max)
{
    buf b;
    int n = 0, got = 0, total = 1;
    b.n = 0;
    b_u32(&b, parent);
    b_str(&b, "");
    c_req(c, 9, &b);
    while (got < total) {
        msg m;
        int cnt, i;
        if (!expect(c, 124, &m)) {
            break;
        }
        cnt = (int)m_u8(&m);
        m_u8(&m);
        total = (int)m_u8(&m);
        CHECK_EQ(m_u32(&m), parent); /* 0x3824 drops other lists */
        for (i = 0; i < cnt; i++) {
            char s2[32];
            ent x;
            x.id = m_u32(&m);
            x.type = m_u16(&m);
            CHECK_EQ(m_u16(&m), 0);
            x.v1 = m_u32(&m);
            x.v2 = m_u32(&m);
            x.v3 = m_u32(&m);
            m_u32(&m);
            m_str(&m, x.name, sizeof(x.name));
            CHECK(strlen(x.name) <= 20); /* TBuf8<20> */
            m_str(&m, s2, sizeof(s2));
            if (n < max) {
                e[n++] = x;
            }
        }
        CHECK_EQ(m.off, m.n);
        free(m.d);
        got++;
    }
    return n;
}

/* 121 chunks until the file is complete (0x337c); with_id: object download */
static size_t receive_file(client *c, gc_u8 *out, size_t cap, int with_id, gc_u32 id)
{
    size_t total = 0, got = 0;
    do {
        msg m;
        gc_u32 t, start, end, len;
        if (!expect(c, 121, &m)) {
            return 0;
        }
        t = m_u32(&m);
        start = m_u32(&m);
        end = m_u32(&m);
        len = m_u32(&m);
        CHECK_EQ(end - start + 1, len);
        CHECK(t <= cap && end < t);
        if (end < t && t <= cap) {
            memcpy(out + start, m.d + m.off, len);
        }
        m.off += len;
        if (with_id) {
            CHECK_EQ(m_u32(&m), id);
        }
        CHECK_EQ(m.off, m.n);
        free(m.d);
        total = t;
        got += len;
    } while (got < total);
    return total;
}

/* type 6 + chunks of 1255 bytes (0x36b8) */
static void upload(client *c, gc_u16 code, const gc_u8 *d, size_t n, gc_u32 a, gc_u32 bb)
{
    buf b;
    msg m;
    size_t off;
    b.n = 0;
    b_u16(&b, code);
    b_u32(&b, (gc_u32)n);
    b_str(&b, "");
    b_u32(&b, a);
    b_u32(&b, bb);
    c_req(c, 6, &b);
    if (!expect(c, 122, &m)) {
        return;
    }
    free(m.d);
    for (off = 0; off < n;) {
        size_t k = n - off > 1255 ? 1255 : n - off;
        b.n = 0;
        b_u32(&b, (gc_u32)off);
        b_u32(&b, (gc_u32)(off + k - 1));
        b_u32(&b, (gc_u32)k);
        b_raw(&b, d + off, k);
        c_req(c, 8, &b);
        off += k;
    }
    if (expect(c, 123, &m)) {
        free(m.d);
    }
}

static void make_shadow(gc_u8 *d, size_t n, int track, int seed)
{
    size_t i;
    for (i = 0; i < n; i++) {
        d[i] = (gc_u8)(i * 7 + (size_t)seed);
    }
    d[0] = 0xe9; /* 0x3e9 */
    d[1] = 0x03;
    d[2] = 0;
    d[3] = 0;
    d[4] = (gc_u8)track;
    d[5] = 0;
}

static void reset_server(const char *file, int via_native)
{
    gcs_ssx_io io;
    static gcs_accounts *acc;
    g_qn = 0;
    if (g_nat) {
        gcs_native_destroy(g_nat);
        g_nat = NULL;
    }
    gcs_ssx_destroy(g_ssx);
    g_ssx = NULL;
    if (via_native) {
        gcs_native_io nio;
        nio.send = n_send;
        nio.log = x_log;
        g_nat = gcs_native_create(&nio, NULL, NULL, NULL, NULL, NULL, NULL);
        CHECK(g_nat != NULL);
        gcs_native_set_random(g_nat, x_rand, NULL);
        CHECK(gcs_native_set_ssx(g_nat, file, NULL) == 0);
        g_ssx = NULL;
        return;
    }
    if (!acc) {
        acc = gcs_accounts_create(NULL, x_log, NULL);
    }
    memset(&io, 0, sizeof(io));
    io.send = x_send;
    io.log = x_log;
    io.random = x_rand;
    g_ssx = gcs_ssx_create(&io, NULL, acc, file);
    CHECK(g_ssx != NULL);
    gcs_ssx_set_time(g_ssx, 1100000000L);
}

static gcs_ssx *ssx(void)
{
    return g_nat ? gcs_native_ssx(g_nat) : g_ssx;
}

/* ---- tests ---- */

static int test_rand_bytes(void *ctx, void *b, size_t n)
{
    size_t i;
    (void)ctx;
    for (i = 0; i < n; i++) {
        ((gc_u8 *)b)[i] = (gc_u8)x_rand(NULL);
    }
    return 0;
}

static void test_crypto(void)
{
    static const gc_u8 k[8] = { 0x13, 0x34, 0x57, 0x79, 0x9b, 0xbc, 0xdf, 0xf1 };
    static const gc_u8 p[8] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef };
    static const gc_u8 want[8] = { 0x85, 0xe8, 0x13, 0x54, 0x0f, 0x0a, 0xb4, 0x05 };
    /* openssl enc -des-ede-cbc -K 0123456789abcdeffedcba9876543210 -iv 0102030405060708 -nopad */
    static const gc_u8 key2[16] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
                                    0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10 };
    static const gc_u8 iv[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    static const gc_u8 want3[16] = { 0x56, 0x24, 0xab, 0x70, 0x49, 0x81, 0xd5, 0x32,
                                     0x0a, 0xa0, 0xde, 0xd0, 0xef, 0xa9, 0x8d, 0xd4 };
    gc_u8 o[20], buf3[32];
    gcs_des d;
    gcs_3des t;
    gcs_rsa r;
    gc_u8 msgb[128], enc[128], dec[128];
    char hx[41];
    int i;
    gcs_md5("abc", 3, o);
    for (i = 0; i < 16; i++) {
        snprintf(hx + 2 * i, 3, "%02x", o[i]);
    }
    CHECK(!strcmp(hx, "900150983cd24fb0d6963f7d28e17f72"));
    gcs_sha1("abc", 3, o);
    for (i = 0; i < 20; i++) {
        snprintf(hx + 2 * i, 3, "%02x", o[i]);
    }
    CHECK(!strcmp(hx, "a9993e364706816aba3e25717850c26c9cd0d89d"));
    gcs_des_setkey(&d, k);
    gcs_des_block(&d, p, o, 0);
    CHECK(!memcmp(o, want, 8));
    gcs_des_block(&d, o, o + 8, 1);
    CHECK(!memcmp(o + 8, p, 8));
    gcs_3des_setkey(&t, key2);
    memcpy(buf3, "0123456789abcdefghijklmnopqrstu", 32);
    gcs_3des_cbc_encrypt(&t, iv, buf3, buf3, 32);
    CHECK(!memcmp(buf3, want3, 16));
    gcs_3des_cbc_decrypt(&t, iv, buf3, buf3, 32);
    CHECK(!memcmp(buf3, "0123456789abcdef", 16));
    CHECK(gcs_rsa_generate(&r, 1024, NULL, NULL) != 0);
    /* key pair, PKCS#1 block type 2 round trip */
    memset(msgb, 0, sizeof(msgb));
    msgb[1] = 2;
    for (i = 2; i < 100; i++) {
        msgb[i] = (gc_u8)(i | 1);
    }
    msgb[100] = 0;
    memcpy(msgb + 101, "twenty-seven bytes of text!", 27);
    CHECK(gcs_rsa_generate(&r, 1024, test_rand_bytes, NULL) == 0);
    CHECK(gcs_rsa_public(&r, msgb, 128, enc) == 0);
    CHECK_EQ(gcs_rsa_decrypt_pkcs1(&r, enc, 128, dec, sizeof(dec)), 27);
    CHECK(!memcmp(dec, "twenty-seven bytes of text!", 27));
}

static gc_u8 g_plugin[3333];

static void discovery(client *c)
{
    buf b;
    msg m;
    gc_u8 got[4000];
    char t[200];
    size_t n;
    handshake(c);
    b.n = 0;
    b_u16(&b, 2);
    b_str(&b, "d41d8cd98f00b204e9800998ecf8427e"); /* no abtesrv.dll yet */
    b_str(&b, "");
    b_u16(&b, 39);
    b_u16(&b, 10);
    b_str(&b, "351234567890123");
    b_str(&b, "NGAGE");
    c_req(c, 50, &b);
    if (!expect(c, 150, &m)) {
        return;
    }
    CHECK_EQ(m_u8(&m), 1);
    m_str(&m, t, sizeof(t));
    CHECK(t[0] != 0); /* the game recomputes the MD5 only then (0x100b6e74) */
    free(m.d);
    b.n = 0;
    c_req(c, 51, &b);
    n = receive_file(c, got, sizeof(got), 0, 0);
    CHECK_EQ(n, sizeof(g_plugin));
    CHECK(!memcmp(got, g_plugin, sizeof(g_plugin)));
}

static void test_game(void)
{
    client c, z;
    ent e[64];
    int n, i;
    gc_u8 sh1[5000], sh2[3000], career[0xe0], got[8000];
    buf b;
    msg m;
    char t[300];
    gc_u8 h[16];
    char md5[33];
    const char *file = "test_ssx.tmp";
    remove(file);
    reset_server(file, 0);
    for (i = 0; i < (int)sizeof(g_plugin); i++) {
        g_plugin[i] = (gc_u8)(i * 13 + 5);
    }
    CHECK(gcs_ssx_set_plugin_data(g_ssx, g_plugin, sizeof(g_plugin)) == 0);

    /* discovery, then the game connects again (new port, new numbering) */
    memset(&c, 0, sizeof(c));
    c.port = 3001;
    discovery(&c);
    memset(&c, 0, sizeof(c));
    c.port = 3002;
    handshake(&c);

    /* discovery with our own DLL: nothing to send */
    gcs_md5(g_plugin, sizeof(g_plugin), h);
    for (i = 0; i < 16; i++) {
        snprintf(md5 + 2 * i, 3, "%02x", h[i]);
    }
    b.n = 0;
    b_u16(&b, 2);
    b_str(&b, md5);
    b_str(&b, "");
    b_u16(&b, 39);
    b_u16(&b, 10);
    b_str(&b, "351234567890123");
    b_str(&b, "NGAGE");
    c_req(&c, 50, &b);
    if (expect(&c, 150, &m)) {
        CHECK_EQ(m_u8(&m), 0);
        free(m.d);
    }

    /* accounts */
    CHECK_EQ(logon(&c, "rider", "pw1"), 2); /* "User not found" */
    CHECK_EQ(create(&c, "rider", "pw1", "351234567890123"), 0);
    CHECK_EQ(create(&c, "Rider", "pw2", "351234567890999"), 3); /* "Username already exists" */
    CHECK_EQ(logon(&c, "rider", "wrong"), 1);
    CHECK_EQ(logon(&c, "RIDER", "pw1"), 0);

    /* the menus */
    n = objlist(&c, 2000100, e, 64);
    CHECK_EQ(n, 8);
    CHECK_EQ(e[0].id, 2000109);
    CHECK_EQ(e[5].id, 2000170); /* Fresh Gear */
    CHECK_EQ(e[7].id, 2000140);
    for (i = 0; i < n; i++) {
        CHECK(e[i].type < 2); /* folders: the game handles the special ids */
    }
    n = objlist(&c, 2000109, e, 64);
    CHECK_EQ(n, 3);
    CHECK_EQ(e[1].id, 2000120);
    n = objlist(&c, 2000120, e, 64);
    CHECK_EQ(n, 5);
    CHECK_EQ(e[1].id, 2000122);
    CHECK(!strcmp(e[1].name, "Intimidator"));
    n = objlist(&c, 2000130, e, 64);
    CHECK_EQ(n, 4);
    CHECK(!strcmp(e[3].name, "Throne Jam"));
    CHECK_EQ(objlist(&c, 2000122, e, 64), 0);

    /* shadows: Intimidator (track 7, a race: lower time is better) */
    make_shadow(sh1, sizeof(sh1), 7, 1);
    upload(&c, 10122, sh1, sizeof(sh1), 95000, 7);
    make_shadow(sh2, sizeof(sh2), 6, 2);
    upload(&c, 10122, sh2, sizeof(sh2), 1000, 7); /* file of another track: refused */

    memset(&z, 0, sizeof(z));
    z.port = 4001;
    z.sha = 1; /* the other hash */
    handshake(&z);
    CHECK_EQ(create(&z, "Zoe", "zz", "351234567890777"), 0);
    make_shadow(sh2, sizeof(sh2), 7, 3);
    upload(&z, 10122, sh2, sizeof(sh2), 90000, 7);

    n = objlist(&c, 2000122, e, 64);
    CHECK_EQ(n, 2);
    if (n == 2) {
        CHECK(!strcmp(e[0].name, "Zoe"));
        CHECK_EQ(e[0].v1, 90000);
        CHECK_EQ(e[0].v2, 1);
        CHECK_EQ(e[0].type, 10122 + 7);
        CHECK_EQ(e[0].v3, 1100000000L);
        CHECK(!strcmp(e[1].name, "rider"));
        CHECK_EQ(e[1].v1, 95000);
        /* download Zoe's */
        b.n = 0;
        b_u32(&b, e[0].id);
        b_str(&b, "");
        c_req(&c, 3, &b);
        CHECK_EQ(receive_file(&c, got, sizeof(got), 1, e[0].id), sizeof(sh2));
        CHECK(!memcmp(got, sh2, sizeof(sh2)));
    }

    /* a slopestyle track (2, R&B): higher score is better */
    make_shadow(sh2, sizeof(sh2), 2, 4);
    upload(&c, 10122, sh2, sizeof(sh2), 5000, 2);
    make_shadow(sh2, sizeof(sh2), 2, 5);
    upload(&z, 10122, sh2, sizeof(sh2), 7000, 2);
    n = objlist(&c, 2000113, e, 64);
    CHECK_EQ(n, 2);
    CHECK(n == 2 && !strcmp(e[0].name, "Zoe"));

    /* careers */
    for (i = 0; i < (int)sizeof(career); i++) {
        career[i] = (gc_u8)(255 - i);
    }
    CHECK_EQ(objlist(&c, 2000150, e, 64), 0);
    upload(&c, 10121, career, sizeof(career), 55, 123456);
    upload(&z, 10121, career, 100, 80, 99);
    n = objlist(&c, 2000150, e, 64);
    CHECK_EQ(n, 1);
    CHECK(n == 1 && !strcmp(e[0].name, "rider") && e[0].type == 1001 && e[0].v1 == 55 && e[0].v2 == 123456);
    n = objlist(&c, 2000160, e, 64);
    CHECK_EQ(n, 2);
    CHECK(n == 2 && !strcmp(e[0].name, "Zoe"));
    if (n == 2) {
        b.n = 0;
        b_u32(&b, e[1].id);
        b_str(&b, "");
        c_req(&c, 3, &b);
        CHECK_EQ(receive_file(&c, got, sizeof(got), 1, e[1].id), sizeof(career));
        CHECK(!memcmp(got, career, sizeof(career)));
    }

    /* forgotten name and password */
    b.n = 0;
    acc_hdr(&b);
    b_str(&b, "351234567890123");
    b_str(&b, "NGAGE");
    b_u8(&b, 0);
    c_req(&c, 17, &b);
    if (expect(&c, 132, &m)) {
        CHECK_EQ(m_u8(&m), 0);
        m_str(&m, t, sizeof(t));
        CHECK(!strcmp(t, "rider"));
        free(m.d);
    }
    b.n = 0;
    acc_hdr(&b);
    b_str(&b, "rider");
    b_str(&b, "351234567890123");
    b_str(&b, "NGAGE");
    b_u8(&b, 0);
    c_req(&c, 14, &b);
    if (expect(&c, 130, &m)) {
        CHECK_EQ(m_u8(&m), 0);
        m_str(&m, t, sizeof(t));
        CHECK(!strcmp(t, "First pet?"));
        free(m.d);
    }
    b.n = 0;
    acc_hdr(&b);
    b_str(&b, "rider");
    b_str(&b, "cat");
    b_str(&b, "351234567890123");
    b_str(&b, "NGAGE");
    b_u8(&b, 0);
    c_req(&c, 15, &b);
    if (expect(&c, 131, &m)) {
        CHECK_EQ(m_u8(&m), 5); /* wrong answer */
        free(m.d);
    }
    b.n = 0;
    acc_hdr(&b);
    b_str(&b, "rider");
    b_str(&b, "rEX");
    b_str(&b, "351234567890123");
    b_str(&b, "NGAGE");
    b_u8(&b, 0);
    c_req(&c, 15, &b);
    if (expect(&c, 131, &m)) {
        CHECK_EQ(m_u8(&m), 0);
        m_u8(&m);
        m_str(&m, t, sizeof(t));
        CHECK_EQ(strlen(t), 8);
        free(m.d);
        CHECK_EQ(logon(&c, "rider", t), 0);
        /* change it back */
        b.n = 0;
        acc_hdr(&b);
        b_str(&b, "rider");
        b_str(&b, t);
        b_str(&b, "pw1");
        b_str(&b, "");
        b_str(&b, "");
        b_str(&b, "351234567890123");
        b_str(&b, "NGAGE");
        b_u8(&b, 0);
        c_req(&c, 13, &b);
        if (expect(&c, 129, &m)) {
            CHECK_EQ(m_u8(&m), 0);
            free(m.d);
        }
        CHECK_EQ(logon(&c, "rider", "pw1"), 0);
    }

    /* time ping */
    b.n = 0;
    b_u32(&b, 0x01020304);
    b_u32(&b, 0x0a0b0c0d);
    c_req(&c, 12, &b);
    if (expect(&c, 127, &m)) {
        CHECK_EQ(m_u32(&m), 0x01020304);
        CHECK_EQ(m_u32(&m), 0x0a0b0c0d);
        free(m.d);
    }

    /* resends until acknowledged; a duplicate request is answered once */
    c.drop_acks = 1;
    objlist(&c, 2000109, e, 64);
    g_now += GCSX_RESEND_MS + 10;
    gcs_ssx_poll(g_ssx, g_now);
    CHECK_EQ(g_qn, 1);
    c.drop_acks = 0;
    if (g_qn == 1) {
        dgram g = g_q[0];
        g_qn = 0;
        CHECK(!c_recv(&c, &g, &m)); /* acknowledged, but a sequence the client has seen */
    }
    g_now += GCSX_RESEND_MS + 10;
    gcs_ssx_poll(g_ssx, g_now);
    CHECK_EQ(g_qn, 0); /* acknowledged by now */

    CHECK(g_max_dgram <= 1085); /* fits through a bridge (gc_link) */

    /* persistence */
    reset_server(file, 0);
    {
        gcs_ssx_stats st;
        gcs_ssx_get_stats(g_ssx, &st);
        CHECK_EQ(st.shadows, 4);
        CHECK_EQ(st.careers, 2);
    }
    memset(&c, 0, sizeof(c));
    c.port = 3003;
    handshake(&c);
    CHECK_EQ(logon(&c, "rider", "pw1"), 0);
    n = objlist(&c, 2000122, e, 64);
    CHECK_EQ(n, 2);
    if (n == 2) {
        b.n = 0;
        b_u32(&b, e[1].id);
        b_str(&b, "");
        c_req(&c, 3, &b);
        CHECK_EQ(receive_file(&c, got, sizeof(got), 1, e[1].id), sizeof(sh1));
        CHECK(!memcmp(got, sh1, sizeof(sh1)));
    }
    gcs_ssx_destroy(g_ssx);
    g_ssx = NULL;
    remove(file);
}

/* through gcs_native: the upload chunks come in UDP_PART pieces */
static void test_native_parts(void)
{
    client c;
    ent e[8];
    gc_u8 sh[4000], got[5000];
    buf b;
    reset_server(NULL, 1);
    g_cap = 300; /* small tunnel messages: everything above comes in parts */
    memset(&c, 0, sizeof(c));
    c.port = 5001;
    handshake(&c);
    CHECK_EQ(create(&c, "parts", "pp", "351234567890555"), 0);
    make_shadow(sh, sizeof(sh), 11, 9);
    upload(&c, 10122, sh, sizeof(sh), 61000, 11);
    CHECK_EQ(objlist(&c, 2000131, e, 8), 1);
    b.n = 0;
    b_u32(&b, e[0].id);
    b_str(&b, "");
    c_req(&c, 3, &b);
    CHECK_EQ(receive_file(&c, got, sizeof(got), 1, e[0].id), sizeof(sh));
    CHECK(!memcmp(got, sh, sizeof(sh)));
    CHECK(ssx() != NULL);
    g_cap = 1094;
    gcs_native_destroy(g_nat);
    g_nat = NULL;
}

/* gc_net: UDP_PART encoding and splitting */
static gc_u8 g_parts[16][400];
static size_t g_partn[16];
static int g_np;
static int collect(void *ctx, const gc_u8 *msg, size_t len)
{
    (void)ctx;
    if (g_np < 16) {
        memcpy(g_parts[g_np], msg, len);
        g_partn[g_np++] = len;
    }
    return 1;
}

static void test_gcn_parts(void)
{
    gc_u8 d[1000], whole[1000];
    gcn_msg m, x;
    int i;
    size_t got = 0;
    for (i = 0; i < 1000; i++) {
        d[i] = (gc_u8)i;
    }
    memset(&m, 0, sizeof(m));
    m.type = GCN_MT_UDP;
    memcpy(m.addr, SRV, 4);
    m.port = GCSX_PORT;
    m.peer_port = 777;
    m.data = d;
    m.len = 100;
    g_np = 0;
    CHECK(gcn_send_udp(&m, 300, 1, collect, NULL));
    CHECK_EQ(g_np, 1);
    CHECK(gcn_decode(g_parts[0], g_partn[0], &x) && x.type == GCN_MT_UDP && x.len == 100);
    m.len = 1000;
    g_np = 0;
    CHECK(gcn_send_udp(&m, 300, 42, collect, NULL));
    CHECK_EQ(g_np, 4);
    for (i = 0; i < g_np; i++) {
        CHECK(g_partn[i] <= 300);
        CHECK(gcn_decode(g_parts[i], g_partn[i], &x));
        CHECK_EQ(x.type, GCN_MT_UDP_PART);
        CHECK_EQ(x.part_id, 42);
        CHECK_EQ(x.total, 1000);
        CHECK_EQ(x.peer_port, 777);
        memcpy(whole + x.offset, x.data, x.len);
        got += x.len;
    }
    CHECK_EQ(got, 1000);
    CHECK(!memcmp(whole, d, 1000));
    m.len = GCN_UDP_MAX + 1;
    CHECK(!gcn_send_udp(&m, 300, 2, collect, NULL));
}

int main(int argc, char **argv)
{
    g_verbose = argc > 1 && !strcmp(argv[1], "-v");
    RUN(test_crypto);
    RUN(test_gcn_parts);
    RUN(test_game);
    RUN(test_native_parts);
    TEST_MAIN_END();
}
