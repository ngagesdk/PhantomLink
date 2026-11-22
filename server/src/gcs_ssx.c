/** @file gcs_ssx.c
 *
 *  SSX Out of Bounds online service, see gcs_ssx.h and docs/SSX.md.
 *
 *  Reference: AIRPLAYSERVER.EXE and 6R65.APP of the retail game
 *  (version 041211), static analysis only.
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#include "gcs_ssx.h"

#include "gcs_accounts.h"
#include "gcs_ssxcrypt.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NAMESZ (GCSA_MAX_NAME + 1)

/* message types, game -> server */
#define MT_LOGON     0
#define MT_LOGOUT    1
#define MT_DL_OBJECT 3
#define MT_DL_FILE   4
#define MT_UP_CODE   6
#define MT_UP_NAME   7
#define MT_UP_CHUNK  8
#define MT_OBJLIST   9
#define MT_FILELIST  10
#define MT_MSGLIST   11
#define MT_PING      12
#define MT_CHANGE_PW 13
#define MT_QUESTION  14
#define MT_PASSWORD  15
#define MT_CREATE    16
#define MT_USERNAME  17
#define MT_KEY_REQ   18
#define MT_KEY       19
#define MT_DISCOVER  50
#define MT_DISC_FILE 51
#define MT_ACK       0xfa
/* server -> game */
#define ST_LOGON     120
#define ST_CHUNK     121
#define ST_UP_READY  122
#define ST_UP_DONE   123
#define ST_OBJLIST   124
#define ST_FILELIST  125
#define ST_MSGLIST   126
#define ST_PONG      127
#define ST_CHANGE_PW 129
#define ST_QUESTION  130
#define ST_PASSWORD  131
#define ST_USERNAME  132
#define ST_KEY       133
#define ST_KEY_OK    134
#define ST_DISCOVER  150

/* logon results (AIRPLAYSERVER.EXE 0x1d38 maps them to the game's codes) */
#define LR_OK         0
#define LR_AUTH       1 /* "Authentication failed." */
#define LR_NO_USER    2 /* "User not found" */
#define LR_NAME_TAKEN 3 /* "Username already exists" */

/* the game's menu ids (6R65.APP 0x10d4cc) */
#define ID_ROOT       2000100
#define ID_SHADOWS    2000109
#define ID_PEAK1      2000110
#define ID_PEAK2      2000120
#define ID_PEAK3      2000130
#define ID_DISCONNECT 2000140
#define ID_MY_CAREER  2000150
#define ID_CAREERS    2000160
#define ID_FRESH_GEAR 2000170
#define ID_UP_SHADOWS 2000180
#define ID_UP_CAREER  2000190
#define ID_TIPS       2000195
#define TYPE_CAREER   1001
#define TYPE_SHADOW0  10122 /* + track */
#define UP_CAREER     10121
#define UP_SHADOW     10122
#define FIRST_OBJECT  10000000u

#define HDR       8
#define CHUNK     900 /* download bytes per message: datagrams stay below a bridge's 1085 */
#define PER_LIST  10  /* list entries per message */
#define SEEN      256
#define QUEUE_MAX 2048

typedef struct
{
    gc_u32 id;
    int track;
    char user[NAMESZ];
    gc_u32 value; /* time in ms (races) or score */
    long time;
    gc_u8 *data;
    size_t len;
} shadow;

typedef struct
{
    gc_u32 id;
    char user[NAMESZ];
    gc_u32 pct, cash;
    long time;
    gc_u8 *data;
    size_t len;
} career;

typedef struct
{
    gc_u32 seq;
    gc_u8 *d;
    size_t len;
    gc_u32 t;
    int tries;
} outmsg;

typedef struct
{
    int used;
    void *key;
    gc_u8 addr[4];
    gc_u16 port, peer_port;
    gc_u32 t_last;
    gc_u32 out_seq;
    /* crypto */
    int have_key;
    gcs_3des des;
    int hash;        /* 0 not known yet, 1 MD5, 2 SHA-1 */
    int key_req_seq; /* sequence of the last key request, -1 none */
    /* duplicates */
    gc_u32 seen[SEEN];
    int nseen, seen_pos;
    /* reliable messages in flight and waiting */
    outmsg fly[GCSX_WINDOW];
    int nfly;
    outmsg *q;
    int nq;
    /* login */
    char user[NAMESZ];
    char imei[32];
    /* upload */
    int up;
    gc_u16 up_code;
    gc_u32 up_size, up_got, up_a, up_b;
    gc_u8 *up_buf, *up_map;
} session;

struct gcs_ssx
{
    gcs_ssx_io io;
    void *ctx;
    gcs_accounts *acc;
    char file[260];
    long fixed_time;
    gc_u32 rng;
    gcs_rsa rsa;
    int have_rsa;
    char rsa_n[700], rsa_e[16];
    gc_u8 *plugin;
    size_t plugin_len;
    char plugin_md5[33];
    session ses[GCSX_SESSIONS];
    shadow *sh;
    int nsh, capsh;
    career *ca;
    int nca, capca;
    gc_u32 next_id;
    gc_u32 datagrams, bad;
};

static const char *const k_tracks[GCSX_TRACKS] = { "Snow Jam", "Metro City", "R&B", "The Junction",
                                                   "Happiness", "Happiness Jam", "Ruthless Ridge", "Intimidator",
                                                   "Style Mile", "Ruthless", "Ruthless Jam", "Gravitude",
                                                   "Kick Doubt", "The Throne", "Throne Jam" };
/* tracks scored by points (slopestyle, super pipe): 6R65.APP 0x10024d54 */
static const gc_u8 k_scored[GCSX_TRACKS] = { 0, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0, 1 };

static void slog(gcs_ssx *s, int level, const char *fmt, ...)
{
    char line[400];
    va_list ap;
    if (!s->io.log) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    s->io.log(s->ctx, level, line);
}

#define LOG_E(...) slog(s, 0, "ssx: " __VA_ARGS__)
#define LOG_W(...) slog(s, 1, "ssx: " __VA_ARGS__)
#define LOG_I(...) slog(s, 2, "ssx: " __VA_ARGS__)
#define LOG_D(...) slog(s, 3, "ssx: " __VA_ARGS__)

static long now_s(const gcs_ssx *s)
{
    return s->fixed_time ? s->fixed_time : (long)time(NULL);
}

static void copy_str(char *dst, const char *src, size_t cap)
{
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}

static gc_u32 rnd32(gcs_ssx *s)
{
    gc_u32 x;
    if (s->io.random) {
        return s->io.random(s->ctx);
    }
    x = s->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s->rng = x;
    return x;
}

static int rnd_bytes(void *ctx, void *buf, size_t n)
{
    gcs_ssx *s = (gcs_ssx *)ctx;
    gc_u8 *p = (gc_u8 *)buf;
    size_t i;
    for (i = 0; i < n; i += 4) {
        gc_u32 v = rnd32(s);
        size_t k;
        for (k = 0; k < 4 && i + k < n; k++) {
            p[i + k] = (gc_u8)(v >> (8 * k));
        }
    }
    return 0;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static int ieq(const char *a, const char *b)
{
    while (*a && *b) {
        int x = (unsigned char)*a++, y = (unsigned char)*b++;
        if (x >= 'A' && x <= 'Z') {
            x += 32;
        }
        if (y >= 'A' && y <= 'Z') {
            y += 32;
        }
        if (x != y) {
            return 0;
        }
    }
    return *a == *b;
}

/* ---- reading and writing message bodies ---- */

typedef struct
{
    const gc_u8 *p;
    size_t n, off;
    int bad;
} rd;

static gc_u32 r_u16(rd *r)
{
    gc_u32 v;
    if (r->off + 2 > r->n) {
        r->bad = 1;
        return 0;
    }
    v = (gc_u32)r->p[r->off] | ((gc_u32)r->p[r->off + 1] << 8);
    r->off += 2;
    return v;
}

static gc_u32 r_u32(rd *r)
{
    gc_u32 v;
    if (r->off + 4 > r->n) {
        r->bad = 1;
        return 0;
    }
    v = (gc_u32)r->p[r->off] | ((gc_u32)r->p[r->off + 1] << 8) | ((gc_u32)r->p[r->off + 2] << 16) |
        ((gc_u32)r->p[r->off + 3] << 24);
    r->off += 4;
    return v;
}

/* string: u32 length + bytes; copied as text (cut at cap - 1, no control characters) */
static void r_str(rd *r, char *out, size_t cap)
{
    gc_u32 len = r_u32(r), i;
    size_t k = 0;
    if (r->bad || len > r->n - r->off) {
        r->bad = 1;
        out[0] = 0;
        return;
    }
    for (i = 0; i < len; i++) {
        gc_u8 c = r->p[r->off + i];
        if (k + 1 < cap && c >= 0x20 && c != 0x7f) {
            out[k++] = (char)c;
        }
    }
    out[k] = 0;
    r->off += len;
}

/* raw bytes of a string field (the RSA block) */
static const gc_u8 *r_blob(rd *r, size_t *len)
{
    gc_u32 n = r_u32(r);
    const gc_u8 *p;
    if (r->bad || n > r->n - r->off) {
        r->bad = 1;
        *len = 0;
        return NULL;
    }
    p = r->p + r->off;
    r->off += n;
    *len = n;
    return p;
}

typedef struct
{
    gc_u8 *d;
    size_t n, cap;
    int oom;
} wr;

static void w_raw(wr *w, const void *p, size_t len)
{
    if (w->oom) {
        return;
    }
    if (w->n + len > w->cap) {
        size_t cap = w->cap ? w->cap : 256;
        gc_u8 *d;
        while (cap < w->n + len) {
            cap *= 2;
        }
        d = (gc_u8 *)realloc(w->d, cap);
        if (!d) {
            w->oom = 1;
            return;
        }
        w->d = d;
        w->cap = cap;
    }
    if (len) {
        memcpy(w->d + w->n, p, len);
    }
    w->n += len;
}

static void w_u8(wr *w, gc_u32 v)
{
    gc_u8 b = (gc_u8)v;
    w_raw(w, &b, 1);
}

static void w_u16(wr *w, gc_u32 v)
{
    gc_u8 b[2];
    b[0] = (gc_u8)v;
    b[1] = (gc_u8)(v >> 8);
    w_raw(w, b, 2);
}

static void w_u32(wr *w, gc_u32 v)
{
    gc_u8 b[4];
    b[0] = (gc_u8)v;
    b[1] = (gc_u8)(v >> 8);
    b[2] = (gc_u8)(v >> 16);
    b[3] = (gc_u8)(v >> 24);
    w_raw(w, b, 4);
}

static void w_strn(wr *w, const char *str, size_t max)
{
    size_t n = strlen(str);
    if (n > max) {
        n = max;
    }
    w_u32(w, (gc_u32)n);
    w_raw(w, str, n);
}

static void w_str(wr *w, const char *str)
{
    w_strn(w, str, 1024);
}

/* ---- sessions ---- */

static void out_free(outmsg *m)
{
    free(m->d);
    memset(m, 0, sizeof(*m));
}

static void upload_end(session *se)
{
    free(se->up_buf);
    free(se->up_map);
    se->up_buf = NULL;
    se->up_map = NULL;
    se->up = 0;
}

static void ses_free(session *se)
{
    int i;
    for (i = 0; i < se->nfly; i++) {
        out_free(&se->fly[i]);
    }
    for (i = 0; i < se->nq; i++) {
        free(se->q[i].d);
    }
    free(se->q);
    upload_end(se);
    memset(se, 0, sizeof(*se));
}

static session *ses_get(gcs_ssx *s, void *key, const gc_u8 addr[4], gc_u16 port, gc_u16 peer_port, gc_u32 now)
{
    int i;
    session *free_s = NULL, *oldest = NULL;
    for (i = 0; i < GCSX_SESSIONS; i++) {
        session *se = &s->ses[i];
        if (se->used && se->key == key && se->peer_port == peer_port && se->port == port &&
            !memcmp(se->addr, addr, 4)) {
            return se;
        }
        if (!se->used && !free_s) {
            free_s = se;
        }
        if (se->used && (!oldest || GC_TIME_BEFORE(se->t_last, oldest->t_last))) {
            oldest = se;
        }
    }
    if (!free_s) {
        LOG_W("too many sessions, %u.%u.%u.%u:%u replaced", oldest->addr[0], oldest->addr[1], oldest->addr[2],
              oldest->addr[3], oldest->peer_port);
        ses_free(oldest);
        free_s = oldest;
    }
    memset(free_s, 0, sizeof(*free_s));
    free_s->used = 1;
    free_s->key = key;
    memcpy(free_s->addr, addr, 4);
    free_s->port = port;
    free_s->peer_port = peer_port;
    free_s->t_last = now;
    free_s->key_req_seq = -1;
    free_s->out_seq = 1;
    return free_s;
}

static int seen_check(session *se, gc_u32 seq)
{
    int i;
    for (i = 0; i < se->nseen; i++) {
        if (se->seen[i] == seq) {
            return 1;
        }
    }
    se->seen[se->seen_pos] = seq;
    se->seen_pos = (se->seen_pos + 1) % SEEN;
    if (se->nseen < SEEN) {
        se->nseen++;
    }
    return 0;
}

/* ---- encryption ---- */

static void check_value(int hash, const gc_u8 *d, size_t len, gc_u8 out[8])
{
    gc_u8 h[20];
    if (hash == 2) {
        gcs_sha1(d, len, h);
    } else {
        gcs_md5(d, len, h);
    }
    memcpy(out, h, 8);
}

/* encrypted body: u32 8, IV, u32 plain length, cipher text */
static int seal(gcs_ssx *s, session *se, const gc_u8 *plain, size_t len, wr *out)
{
    size_t padded = (len % 8) ? len + 8 - len % 8 : len, total = padded + 16;
    gc_u8 iv[8], *buf;
    int hash = se->hash ? se->hash : 1;
    buf = (gc_u8 *)calloc(1, total);
    if (!buf) {
        return -1;
    }
    if (len) {
        memcpy(buf, plain, len);
    }
    check_value(hash, buf, len, buf + len);
    memset(buf + padded + 8, 8, 8); /* the game appends one block of 08 bytes */
    rnd_bytes(s, iv, 8);
    gcs_3des_cbc_encrypt(&se->des, iv, buf, buf, total);
    w_u32(out, 8);
    w_raw(out, iv, 8);
    w_u32(out, (gc_u32)len);
    w_raw(out, buf, total);
    free(buf);
    return out->oom ? -1 : 0;
}

/* decrypts body into a malloc'd buffer; returns its length or -1 */
static long unseal(session *se, const gc_u8 *body, size_t blen, gc_u8 **plain)
{
    rd r;
    size_t ivlen, clen;
    const gc_u8 *iv;
    gc_u32 len;
    gc_u8 *buf, chk[8];
    *plain = NULL;
    r.p = body;
    r.n = blen;
    r.off = 0;
    r.bad = 0;
    iv = r_blob(&r, &ivlen);
    len = r_u32(&r);
    if (r.bad || ivlen != 8 || !se->have_key) {
        return -1;
    }
    clen = (blen - r.off) & ~(size_t)7;
    if (len > clen || clen - len < 8) {
        return -1;
    }
    buf = (gc_u8 *)malloc(clen ? clen : 1);
    if (!buf) {
        return -1;
    }
    gcs_3des_cbc_decrypt(&se->des, iv, body + r.off, buf, clen);
    if (len) {
        int ok = 0;
        if (se->hash != 2) {
            check_value(1, buf, len, chk);
            if (!memcmp(chk, buf + len, 8)) {
                ok = 1;
                se->hash = 1;
            }
        }
        if (!ok && se->hash != 1) {
            check_value(2, buf, len, chk);
            if (!memcmp(chk, buf + len, 8)) {
                ok = 1;
                se->hash = 2;
            }
        }
        if (!ok) {
            free(buf);
            return -1;
        }
    }
    *plain = buf;
    return (long)len;
}

/* ---- sending ---- */

static void put_hdr(gc_u8 *h, gc_u32 seq, int type, int noack, int enc)
{
    gc_u32 f = 0x00ffu | (enc ? 0x8000u : 0);
    h[0] = (gc_u8)seq;
    h[1] = (gc_u8)(seq >> 8);
    h[2] = (gc_u8)(seq >> 16);
    h[3] = (gc_u8)(seq >> 24);
    h[4] = (gc_u8)type;
    h[5] = (gc_u8)noack;
    h[6] = (gc_u8)f;
    h[7] = (gc_u8)(f >> 8);
}

static void raw_send(gcs_ssx *s, session *se, const gc_u8 *d, size_t len)
{
    if (s->io.send) {
        s->io.send(s->ctx, se->key, se->addr, se->port, se->peer_port, d, len);
    }
}

static void send_ack(gcs_ssx *s, session *se, gc_u32 seq)
{
    gc_u8 d[HDR + 4];
    /* acks need a fresh sequence too: the game drops every sequence it has
     * seen in the last 90 s, acks included (AIRPLAYSERVER.EXE 0x8144) */
    put_hdr(d, se->out_seq++, MT_ACK, 1, 0);
    d[6] = 0; /* the game's own acks carry 0 there */
    d[7] = 0;
    d[8] = (gc_u8)seq;
    d[9] = (gc_u8)(seq >> 8);
    d[10] = (gc_u8)(seq >> 16);
    d[11] = (gc_u8)(seq >> 24);
    raw_send(s, se, d, sizeof(d));
}

static void pump(gcs_ssx *s, session *se, gc_u32 now)
{
    while (se->nfly < GCSX_WINDOW && se->nq > 0) {
        outmsg *m = &se->fly[se->nfly++];
        *m = se->q[0];
        memmove(se->q, se->q + 1, (size_t)(se->nq - 1) * sizeof(outmsg));
        se->nq--;
        m->t = now;
        m->tries = 1;
        raw_send(s, se, m->d, m->len);
    }
}

/* queues one reliable message; encrypted once the session has its key */
static void send_msg(gcs_ssx *s, session *se, int type, const wr *body, gc_u32 now)
{
    int enc = se->have_key && type != ST_KEY && type != ST_KEY_OK;
    wr out;
    outmsg m;
    gc_u8 h[HDR];
    memset(&out, 0, sizeof(out));
    memset(&m, 0, sizeof(m));
    m.seq = se->out_seq++;
    put_hdr(h, m.seq, type, 0, enc);
    w_raw(&out, h, HDR);
    if (enc) {
        if (seal(s, se, body->d, body->n, &out) != 0) {
            free(out.d);
            return;
        }
    } else if (body->n) {
        w_raw(&out, body->d, body->n);
    }
    if (out.oom || se->nq >= QUEUE_MAX) {
        LOG_W("send queue full, message %d dropped", type);
        free(out.d);
        return;
    }
    if (out.n > 1085) {
        LOG_W("message %d is %u bytes, too large for a bridge", type, (unsigned)out.n);
    }
    m.d = out.d;
    m.len = out.n;
    {
        outmsg *q = (outmsg *)realloc(se->q, (size_t)(se->nq + 1) * sizeof(outmsg));
        if (!q) {
            free(out.d);
            return;
        }
        se->q = q;
        se->q[se->nq++] = m;
    }
    pump(s, se, now);
}

static void got_ack(gcs_ssx *s, session *se, gc_u32 seq, gc_u32 now)
{
    int i;
    for (i = 0; i < se->nfly; i++) {
        if (se->fly[i].seq == seq) {
            out_free(&se->fly[i]);
            memmove(&se->fly[i], &se->fly[i + 1], (size_t)(se->nfly - i - 1) * sizeof(outmsg));
            se->nfly--;
            memset(&se->fly[se->nfly], 0, sizeof(outmsg));
            break;
        }
    }
    pump(s, se, now);
}

/* ---- the RSA key ---- */

static int need_rsa(gcs_ssx *s)
{
    if (s->have_rsa) {
        return 0;
    }
    if (gcs_rsa_generate(&s->rsa, GCSX_RSA_BITS, rnd_bytes, s) != 0) {
        LOG_E("cannot make an RSA key");
        return -1;
    }
    gcs_rsa_modulus_dec(&s->rsa, s->rsa_n, sizeof(s->rsa_n));
    snprintf(s->rsa_e, sizeof(s->rsa_e), "%u", s->rsa.e);
    s->have_rsa = 1;
    LOG_I("RSA key ready (%d bits)", GCSX_RSA_BITS);
    return 0;
}

/* ---- data file ----
 *
 * Tab separated lines:
 *   N <next object id>
 *   S <id> <track> <value> <time> <user> <file hex>
 *   C <id> <completion> <earnings> <time> <user> <file hex>
 */

static void hex_out(FILE *f, const gc_u8 *d, size_t n)
{
    static const char k[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < n; i++) {
        fputc(k[d[i] >> 4], f);
        fputc(k[d[i] & 15], f);
    }
}

static gc_u8 *hex_in(const char *h, size_t *len)
{
    size_t n = strlen(h), i;
    gc_u8 *d;
    if (n % 2 || !n || n / 2 > GCSX_MAX_FILE) {
        return NULL;
    }
    d = (gc_u8 *)malloc(n / 2);
    if (!d) {
        return NULL;
    }
    for (i = 0; i < n / 2; i++) {
        int a = hexval(h[2 * i]), b = hexval(h[2 * i + 1]);
        if (a < 0 || b < 0) {
            free(d);
            return NULL;
        }
        d[i] = (gc_u8)(a * 16 + b);
    }
    *len = n / 2;
    return d;
}

static void save(gcs_ssx *s)
{
    char tmp[280];
    FILE *f;
    int i;
    if (!s->file[0]) {
        return;
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", s->file);
    f = fopen(tmp, "w");
    if (!f) {
        LOG_E("cannot write %s", tmp);
        return;
    }
    fprintf(f, "N\t%lu\n", (unsigned long)s->next_id);
    for (i = 0; i < s->nsh; i++) {
        shadow *x = &s->sh[i];
        fprintf(f, "S\t%lu\t%d\t%lu\t%ld\t%s\t", (unsigned long)x->id, x->track, (unsigned long)x->value, x->time,
                x->user);
        hex_out(f, x->data, x->len);
        fputc('\n', f);
    }
    for (i = 0; i < s->nca; i++) {
        career *x = &s->ca[i];
        fprintf(f, "C\t%lu\t%lu\t%lu\t%ld\t%s\t", (unsigned long)x->id, (unsigned long)x->pct, (unsigned long)x->cash,
                x->time, x->user);
        hex_out(f, x->data, x->len);
        fputc('\n', f);
    }
    if (fclose(f) != 0) {
        LOG_E("cannot write %s", tmp);
        remove(tmp);
        return;
    }
    remove(s->file); /* rename does not replace on Windows */
    if (rename(tmp, s->file) != 0) {
        LOG_E("cannot rename %s to %s", tmp, s->file);
    }
}

static int split(char *line, char **f, int max)
{
    int n = 0;
    char *p = line;
    while (n < max) {
        char *t = strchr(p, '\t');
        f[n++] = p;
        if (!t) {
            break;
        }
        *t = 0;
        p = t + 1;
    }
    return n;
}

/* a whole line of any length (malloc'd), NULL at the end */
static char *read_line(FILE *f)
{
    size_t n = 0, cap = 256;
    char *b = (char *)malloc(cap);
    int c;
    if (!b) {
        return NULL;
    }
    while ((c = fgetc(f)) != EOF && c != '\n') {
        if (n + 2 > cap) {
            char *nb;
            if (cap > 4 * GCSX_MAX_FILE) {
                continue; /* runaway line: skip the rest */
            }
            nb = (char *)realloc(b, cap * 2);
            if (!nb) {
                free(b);
                return NULL;
            }
            b = nb;
            cap *= 2;
        }
        b[n++] = (char)c;
    }
    if (c == EOF && n == 0) {
        free(b);
        return NULL;
    }
    if (n && b[n - 1] == '\r') {
        n--;
    }
    b[n] = 0;
    return b;
}

static shadow *add_shadow(gcs_ssx *s)
{
    if (s->nsh == s->capsh) {
        int cap = s->capsh ? s->capsh * 2 : 32;
        shadow *n = (shadow *)realloc(s->sh, (size_t)cap * sizeof(shadow));
        if (!n) {
            return NULL;
        }
        s->sh = n;
        s->capsh = cap;
    }
    memset(&s->sh[s->nsh], 0, sizeof(shadow));
    return &s->sh[s->nsh++];
}

static career *add_career(gcs_ssx *s)
{
    if (s->nca == s->capca) {
        int cap = s->capca ? s->capca * 2 : 32;
        career *n = (career *)realloc(s->ca, (size_t)cap * sizeof(career));
        if (!n) {
            return NULL;
        }
        s->ca = n;
        s->capca = cap;
    }
    memset(&s->ca[s->nca], 0, sizeof(career));
    return &s->ca[s->nca++];
}

static void load(gcs_ssx *s)
{
    FILE *f = s->file[0] ? fopen(s->file, "r") : NULL;
    char *line;
    if (!f) {
        return;
    }
    while ((line = read_line(f)) != NULL) {
        char *fl[8];
        int n = split(line, fl, 8);
        if (n == 2 && !strcmp(fl[0], "N")) {
            gc_u32 v = (gc_u32)strtoul(fl[1], NULL, 10);
            if (v > s->next_id) {
                s->next_id = v;
            }
        } else if (n == 7 && (!strcmp(fl[0], "S") || !strcmp(fl[0], "C")) && fl[5][0]) {
            size_t len;
            gc_u8 *d = hex_in(fl[6], &len);
            gc_u32 id = (gc_u32)strtoul(fl[1], NULL, 10);
            if (!d) {
                free(line);
                continue;
            }
            if (fl[0][0] == 'S') {
                int track = atoi(fl[2]);
                shadow *x = track >= 0 && track < GCSX_TRACKS ? add_shadow(s) : NULL;
                if (!x) {
                    free(d);
                    free(line);
                    continue;
                }
                x->id = id;
                x->track = track;
                x->value = (gc_u32)strtoul(fl[3], NULL, 10);
                x->time = atol(fl[4]);
                copy_str(x->user, fl[5], NAMESZ);
                x->data = d;
                x->len = len;
            } else {
                career *x = add_career(s);
                if (!x) {
                    free(d);
                    free(line);
                    continue;
                }
                x->id = id;
                x->pct = (gc_u32)strtoul(fl[2], NULL, 10);
                x->cash = (gc_u32)strtoul(fl[3], NULL, 10);
                x->time = atol(fl[4]);
                copy_str(x->user, fl[5], NAMESZ);
                x->data = d;
                x->len = len;
            }
            if (id >= s->next_id) {
                s->next_id = id + 1;
            }
        }
        free(line);
    }
    fclose(f);
    LOG_I("%d shadows and %d careers loaded from %s", s->nsh, s->nca, s->file);
}

/* ---- create / destroy ---- */

gcs_ssx *gcs_ssx_create(const gcs_ssx_io *io, void *ctx, struct gcs_accounts *acc, const char *file)
{
    gcs_ssx *s = (gcs_ssx *)calloc(1, sizeof(*s));
    if (!s) {
        return NULL;
    }
    if (io) {
        s->io = *io;
    }
    s->ctx = ctx;
    s->acc = acc;
    s->rng = (gc_u32)time(NULL) ^ ((gc_u32)clock() << 16) ^ 0x5a5ac3c3u;
    if (!s->rng) {
        s->rng = 1;
    }
    s->next_id = FIRST_OBJECT;
    if (file) {
        copy_str(s->file, file, sizeof(s->file));
    }
    load(s);
    return s;
}

void gcs_ssx_destroy(gcs_ssx *s)
{
    int i;
    if (!s) {
        return;
    }
    for (i = 0; i < GCSX_SESSIONS; i++) {
        ses_free(&s->ses[i]);
    }
    for (i = 0; i < s->nsh; i++) {
        free(s->sh[i].data);
    }
    for (i = 0; i < s->nca; i++) {
        free(s->ca[i].data);
    }
    free(s->sh);
    free(s->ca);
    free(s->plugin);
    free(s);
}

void gcs_ssx_set_time(gcs_ssx *s, long t)
{
    if (s) {
        s->fixed_time = t;
    }
}

int gcs_ssx_set_plugin_data(gcs_ssx *s, const gc_u8 *data, size_t len)
{
    gc_u8 h[16];
    int i;
    free(s->plugin);
    s->plugin = NULL;
    s->plugin_len = 0;
    s->plugin_md5[0] = 0;
    if (!data || !len) {
        return 0;
    }
    if (len > GCSX_MAX_PLUGIN) {
        return -1;
    }
    s->plugin = (gc_u8 *)malloc(len);
    if (!s->plugin) {
        return -1;
    }
    memcpy(s->plugin, data, len);
    s->plugin_len = len;
    gcs_md5(data, len, h);
    for (i = 0; i < 16; i++) {
        snprintf(s->plugin_md5 + 2 * i, 3, "%02x", h[i]);
    }
    return 0;
}

int gcs_ssx_set_plugin(gcs_ssx *s, const char *path)
{
    FILE *f;
    gc_u8 *buf;
    long n;
    int r;
    if (!s) {
        return -1;
    }
    if (!path || !path[0]) {
        return gcs_ssx_set_plugin_data(s, NULL, 0);
    }
    f = fopen(path, "rb");
    if (!f) {
        LOG_W("billing DLL %s not found: SSX players need it (build dll/ssx)", path);
        gcs_ssx_set_plugin_data(s, NULL, 0);
        return -1;
    }
    buf = (gc_u8 *)malloc(GCSX_MAX_PLUGIN);
    n = buf ? (long)fread(buf, 1, GCSX_MAX_PLUGIN, f) : -1;
    fclose(f);
    if (n <= 0 || n >= GCSX_MAX_PLUGIN) {
        LOG_W("billing DLL %s: unusable", path);
        free(buf);
        return -1;
    }
    r = gcs_ssx_set_plugin_data(s, buf, (size_t)n);
    free(buf);
    if (r == 0) {
        LOG_I("billing DLL %s: %ld bytes, MD5 %s", path, n, s->plugin_md5);
    }
    return r;
}

/* ---- downloads ---- */

static void send_file(gcs_ssx *s, session *se, const gc_u8 *d, size_t len, int with_id, gc_u32 id, gc_u32 now)
{
    size_t off = 0;
    do {
        size_t k = len - off > CHUNK ? CHUNK : len - off;
        wr w;
        memset(&w, 0, sizeof(w));
        w_u32(&w, (gc_u32)len);
        w_u32(&w, (gc_u32)off);
        w_u32(&w, (gc_u32)(off + k - 1));
        w_u32(&w, (gc_u32)k);
        w_raw(&w, d + off, k);
        if (with_id) {
            w_u32(&w, id);
        }
        send_msg(s, se, ST_CHUNK, &w, now);
        free(w.d);
        off += k;
    } while (off < len);
}

/* ---- accounts ---- */

typedef struct
{
    gc_u32 version, gameid;
    char client[64];
} acc_hdr;

/* u16 3, u16 x 4 (version numbers), string (MD5 of the billing DLL), u32 game */
static void r_acc_hdr(rd *r, acc_hdr *h)
{
    int i;
    h->version = r_u16(r);
    for (i = 0; i < 4; i++) {
        r_u16(r);
    }
    r_str(r, h->client, sizeof(h->client));
    h->gameid = r_u32(r);
}

static void reply_logon(gcs_ssx *s, session *se, int result, const char *msg, gc_u32 now)
{
    wr w;
    memset(&w, 0, sizeof(w));
    w_u8(&w, (gc_u32)result);
    w_u8(&w, 0);
    w_u32(&w, 0);
    w_u8(&w, 0);
    w_str(&w, msg);
    send_msg(s, se, ST_LOGON, &w, now);
    free(w.d);
}

static void answer_hash(const char *answer, char out[41])
{
    char low[64];
    size_t i;
    for (i = 0; answer[i] && i + 1 < sizeof(low); i++) {
        char c = answer[i];
        low[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    low[i] = 0;
    gcs_sha1_hex(low, strlen(low), out);
}

static void h_logon(gcs_ssx *s, session *se, rd *r, gc_u32 now)
{
    acc_hdr h;
    char user[64], pass[64], imei[32], canon[NAMESZ];
    int res;
    r_acc_hdr(r, &h);
    r_str(r, user, sizeof(user));
    r_str(r, pass, sizeof(pass));
    r_str(r, imei, sizeof(imei));
    if (r->bad || !s->acc) {
        reply_logon(s, se, LR_AUTH, "", now);
        return;
    }
    if (!gcs_accounts_exists(s->acc, user, canon)) {
        LOG_I("logon of unknown user '%s'", user);
        reply_logon(s, se, LR_NO_USER, "", now);
        return;
    }
    res = gcs_accounts_login(s->acc, canon, pass, imei);
    if (res != GCSA_OK && res != GCSA_CREATED) {
        LOG_I("logon of '%s' refused: %s", canon, gcs_accounts_result_str(res));
        reply_logon(s, se, LR_AUTH, "", now);
        return;
    }
    copy_str(se->user, canon, NAMESZ);
    copy_str(se->imei, imei, sizeof(se->imei));
    LOG_I("'%s' logged in (game %lu)", canon, (unsigned long)h.gameid);
    reply_logon(s, se, LR_OK, "", now);
}

static void h_create(gcs_ssx *s, session *se, rd *r, gc_u32 now)
{
    acc_hdr h;
    char user[64], pass[64], question[64], answer[64], imei[32], canon[NAMESZ], ah[41];
    int res;
    r_acc_hdr(r, &h);
    r_str(r, user, sizeof(user));
    r_str(r, pass, sizeof(pass));
    r_str(r, question, sizeof(question));
    r_str(r, answer, sizeof(answer));
    r_str(r, imei, sizeof(imei));
    if (r->bad || !s->acc) {
        reply_logon(s, se, LR_AUTH, "", now);
        return;
    }
    if (gcs_accounts_exists(s->acc, user, canon)) {
        reply_logon(s, se, LR_NAME_TAKEN, "", now);
        return;
    }
    res = gcs_accounts_login(s->acc, user, pass, imei);
    if (res != GCSA_CREATED && res != GCSA_OK) {
        LOG_I("account '%s' not created: %s", user, gcs_accounts_result_str(res));
        reply_logon(s, se, LR_AUTH, "", now);
        return;
    }
    gcs_accounts_exists(s->acc, user, canon);
    gcs_accounts_set(s->acc, canon, "ssx_question", question);
    answer_hash(answer, ah);
    gcs_accounts_set(s->acc, canon, "ssx_answer", ah);
    copy_str(se->user, canon, NAMESZ);
    copy_str(se->imei, imei, sizeof(se->imei));
    LOG_I("account '%s' created", canon);
    reply_logon(s, se, LR_OK, "", now);
}

static void reply_u8(gcs_ssx *s, session *se, int type, int v, gc_u32 now)
{
    wr w;
    memset(&w, 0, sizeof(w));
    w_u8(&w, (gc_u32)v);
    send_msg(s, se, type, &w, now);
    free(w.d);
}

static void h_change_pw(gcs_ssx *s, session *se, rd *r, gc_u32 now)
{
    acc_hdr h;
    char user[64], old[64], pw[64], question[64], answer[64], canon[NAMESZ], ah[41];
    int res;
    r_acc_hdr(r, &h);
    r_str(r, user, sizeof(user));
    r_str(r, old, sizeof(old));
    r_str(r, pw, sizeof(pw));
    r_str(r, question, sizeof(question));
    r_str(r, answer, sizeof(answer));
    if (r->bad || !s->acc || !gcs_accounts_exists(s->acc, user, canon)) {
        reply_u8(s, se, ST_CHANGE_PW, 2, now);
        return;
    }
    res = gcs_accounts_check(s->acc, canon, old);
    if (res != GCSA_OK || !pw[0] || gcs_accounts_set_password(s->acc, canon, pw) != 0) {
        reply_u8(s, se, ST_CHANGE_PW, 1, now);
        return;
    }
    if (question[0]) {
        gcs_accounts_set(s->acc, canon, "ssx_question", question);
        answer_hash(answer, ah);
        gcs_accounts_set(s->acc, canon, "ssx_answer", ah);
    }
    LOG_I("'%s' changed the password", canon);
    reply_u8(s, se, ST_CHANGE_PW, 0, now);
}

static void h_question(gcs_ssx *s, session *se, rd *r, gc_u32 now)
{
    acc_hdr h;
    char user[64], canon[NAMESZ];
    wr w;
    r_acc_hdr(r, &h);
    r_str(r, user, sizeof(user));
    memset(&w, 0, sizeof(w));
    if (r->bad || !s->acc || !gcs_accounts_exists(s->acc, user, canon)) {
        w_u8(&w, 2);
        w_str(&w, "");
    } else {
        w_u8(&w, 0);
        w_strn(&w, gcs_accounts_get(s->acc, canon, "ssx_question"), 50);
    }
    w_u8(&w, 0);
    send_msg(s, se, ST_QUESTION, &w, now);
    free(w.d);
}

static void h_password(gcs_ssx *s, session *se, rd *r, gc_u32 now)
{
    acc_hdr h;
    char user[64], answer[64], canon[NAMESZ], ah[41], pw[9];
    const char *stored;
    wr w;
    int res = 0, i;
    r_acc_hdr(r, &h);
    r_str(r, user, sizeof(user));
    r_str(r, answer, sizeof(answer));
    memset(&w, 0, sizeof(w));
    pw[0] = 0;
    if (r->bad || !s->acc || !gcs_accounts_exists(s->acc, user, canon)) {
        res = 2;
    } else {
        stored = gcs_accounts_get(s->acc, canon, "ssx_answer");
        answer_hash(answer, ah);
        if (!stored[0] || strcmp(stored, ah)) {
            res = 5; /* "wrong answer" */
        } else {
            /* only a hash of the old password is kept: hand out a new one */
            static const char k[] = "abcdefghjkmnpqrstuvwxyz23456789";
            for (i = 0; i < 8; i++) {
                pw[i] = k[rnd32(s) % (sizeof(k) - 1)];
            }
            pw[8] = 0;
            if (gcs_accounts_set_password(s->acc, canon, pw) != 0) {
                res = 2;
                pw[0] = 0;
            } else {
                LOG_I("'%s' got a new password (forgotten)", canon);
            }
        }
    }
    w_u8(&w, (gc_u32)res);
    w_u8(&w, 0);
    w_str(&w, pw);
    send_msg(s, se, ST_PASSWORD, &w, now);
    free(w.d);
}

static void h_username(gcs_ssx *s, session *se, rd *r, gc_u32 now)
{
    acc_hdr h;
    char imei[32];
    const char *found = NULL;
    wr w;
    int i, n;
    r_acc_hdr(r, &h);
    r_str(r, imei, sizeof(imei));
    if (!r->bad && imei[0] && s->acc) {
        n = gcs_accounts_count(s->acc);
        for (i = 0; i < n; i++) {
            const char *u = gcs_accounts_name_at(s->acc, i);
            if (u && !strcmp(gcs_accounts_get(s->acc, u, "imei"), imei)) {
                found = u;
                break;
            }
        }
    }
    memset(&w, 0, sizeof(w));
    w_u8(&w, found ? 0 : 2);
    w_str(&w, found ? found : "");
    w_str(&w, "");
    send_msg(s, se, ST_USERNAME, &w, now);
    free(w.d);
}

/* ---- the object tree ---- */

typedef struct
{
    gc_u32 id;
    gc_u16 type;
    gc_u32 v1, v2, v3;
    const char *name;
} entry;

static int track_of_folder(gc_u32 id)
{
    if (id >= ID_PEAK1 + 1 && id <= ID_PEAK1 + 6) {
        return (int)(id - ID_PEAK1 - 1);
    }
    if (id >= ID_PEAK2 + 1 && id <= ID_PEAK2 + 5) {
        return (int)(id - ID_PEAK2 - 1) + 6;
    }
    if (id >= ID_PEAK3 + 1 && id <= ID_PEAK3 + 4) {
        return (int)(id - ID_PEAK3 - 1) + 11;
    }
    return -1;
}

static int better(const shadow *a, const shadow *b)
{
    if (k_scored[a->track]) {
        return a->value > b->value;
    }
    if (!a->value || !b->value) {
        return a->value != 0; /* no time is the worst */
    }
    return a->value < b->value;
}

static int career_better(const career *a, const career *b)
{
    return a->pct != b->pct ? a->pct > b->pct : a->cash > b->cash;
}

static void send_list(gcs_ssx *s, session *se, gc_u32 parent, const entry *e, int n, gc_u32 now)
{
    int packets = n ? (n + PER_LIST - 1) / PER_LIST : 1, p;
    if (packets > 255) {
        packets = 255;
        n = 255 * PER_LIST;
    }
    for (p = 0; p < packets; p++) {
        int first = p * PER_LIST, cnt = n - first > PER_LIST ? PER_LIST : n - first, i;
        wr w;
        memset(&w, 0, sizeof(w));
        w_u8(&w, (gc_u32)cnt);
        w_u8(&w, (gc_u32)p);
        w_u8(&w, (gc_u32)packets);
        w_u32(&w, parent);
        for (i = first; i < first + cnt; i++) {
            w_u32(&w, e[i].id);
            w_u16(&w, e[i].type);
            w_u16(&w, 0); /* not an upload object */
            w_u32(&w, e[i].v1);
            w_u32(&w, e[i].v2);
            w_u32(&w, e[i].v3);
            w_u32(&w, 0);
            w_strn(&w, e[i].name, 20);
            w_str(&w, "");
        }
        send_msg(s, se, ST_OBJLIST, &w, now);
        free(w.d);
    }
}

static void h_objlist(gcs_ssx *s, session *se, rd *r, gc_u32 now)
{
    static const struct
    {
        gc_u32 id;
        const char *name;
    } root[] = { { ID_SHADOWS, "Shadow Racing" }, { ID_UP_SHADOWS, "Upload Shadows" }, { ID_MY_CAREER, "My Career" }, { ID_CAREERS, "View Career" }, { ID_UP_CAREER, "Upload Career" }, { ID_FRESH_GEAR, "Fresh Gear" }, { ID_TIPS, "Tips and Tricks" }, { ID_DISCONNECT, "Disconnect" } };
    entry e[GCSX_LIST_MAX > 16 ? GCSX_LIST_MAX : 16];
    int n = 0, i, t;
    gc_u32 parent = r_u32(r);
    if (r->bad) {
        return;
    }
    memset(e, 0, sizeof(e));
    if (parent == ID_ROOT) {
        for (i = 0; i < (int)(sizeof(root) / sizeof(root[0])); i++) {
            e[n].id = root[i].id;
            e[n++].name = root[i].name;
        }
    } else if (parent == ID_SHADOWS) {
        e[0].id = ID_PEAK1;
        e[0].name = "Peak 1";
        e[1].id = ID_PEAK2;
        e[1].name = "Peak 2";
        e[2].id = ID_PEAK3;
        e[2].name = "Peak 3";
        n = 3;
    } else if (parent == ID_PEAK1 || parent == ID_PEAK2 || parent == ID_PEAK3) {
        int first = parent == ID_PEAK1 ? 0 : parent == ID_PEAK2 ? 6
                                                                : 11;
        int cnt = parent == ID_PEAK1 ? 6 : parent == ID_PEAK2 ? 5
                                                              : 4;
        for (i = 0; i < cnt; i++) {
            e[n].id = parent + 1 + (gc_u32)i;
            e[n++].name = k_tracks[first + i];
        }
    } else if ((t = track_of_folder(parent)) >= 0) {
        /* the shadows of a track, best first: name, time or score, date */
        const shadow *pick[GCSX_LIST_MAX];
        int np = 0, k;
        for (i = 0; i < s->nsh; i++) {
            const shadow *x = &s->sh[i];
            if (x->track != t) {
                continue;
            }
            for (k = np; k > 0 && better(x, pick[k - 1]); k--) {
                if (k < GCSX_LIST_MAX) {
                    pick[k] = pick[k - 1];
                }
            }
            if (k < GCSX_LIST_MAX) {
                pick[k] = x;
                if (np < GCSX_LIST_MAX) {
                    np++;
                }
            }
        }
        for (i = 0; i < np; i++) {
            e[n].id = pick[i]->id;
            e[n].type = (gc_u16)(TYPE_SHADOW0 + t);
            e[n].v1 = pick[i]->value;
            e[n].v2 = (gc_u32)(i + 1);
            e[n].v3 = (gc_u32)pick[i]->time;
            e[n++].name = pick[i]->user;
        }
    } else if (parent == ID_MY_CAREER || parent == ID_CAREERS) {
        const career *pick[GCSX_LIST_MAX];
        int np = 0, k;
        for (i = 0; i < s->nca; i++) {
            const career *x = &s->ca[i];
            if (parent == ID_MY_CAREER && (!se->user[0] || !ieq(x->user, se->user))) {
                continue;
            }
            for (k = np; k > 0 && career_better(x, pick[k - 1]); k--) {
                if (k < GCSX_LIST_MAX) {
                    pick[k] = pick[k - 1];
                }
            }
            if (k < GCSX_LIST_MAX) {
                pick[k] = x;
                if (np < GCSX_LIST_MAX) {
                    np++;
                }
            }
        }
        for (i = 0; i < np; i++) {
            e[n].id = pick[i]->id;
            e[n].type = TYPE_CAREER;
            e[n].v1 = pick[i]->pct;
            e[n].v2 = pick[i]->cash;
            e[n].v3 = (gc_u32)pick[i]->time;
            e[n++].name = pick[i]->user;
        }
    } else {
        LOG_D("list of unknown object %lu", (unsigned long)parent);
    }
    LOG_D("list %lu: %d entries for '%s'", (unsigned long)parent, n, se->user);
    send_list(s, se, parent, e, n, now);
}

static void h_download(gcs_ssx *s, session *se, rd *r, gc_u32 now)
{
    gc_u32 id = r_u32(r);
    int i;
    if (r->bad) {
        return;
    }
    for (i = 0; i < s->nsh; i++) {
        if (s->sh[i].id == id) {
            LOG_I("'%s' downloads the %s shadow of '%s'", se->user, k_tracks[s->sh[i].track], s->sh[i].user);
            send_file(s, se, s->sh[i].data, s->sh[i].len, 1, id, now);
            return;
        }
    }
    for (i = 0; i < s->nca; i++) {
        if (s->ca[i].id == id) {
            LOG_I("'%s' views the career of '%s'", se->user, s->ca[i].user);
            send_file(s, se, s->ca[i].data, s->ca[i].len, 1, id, now);
            return;
        }
    }
    LOG_W("download of unknown object %lu", (unsigned long)id);
}

/* ---- uploads ---- */

static void finish_upload(gcs_ssx *s, session *se, gc_u32 now)
{
    long t = now_s(s);
    int i;
    if (!se->user[0]) {
        LOG_W("upload without logon dropped");
    } else if (se->up_code == UP_SHADOW) {
        int track = (int)se->up_b;
        int magic = se->up_size >= 6 ? (se->up_buf[0] | (se->up_buf[1] << 8)) : 0;
        int ftrack = se->up_size >= 6 ? (short)(se->up_buf[4] | (se->up_buf[5] << 8)) : -1;
        if (track < 0 || track >= GCSX_TRACKS || magic != 0x3e9 || ftrack != track) {
            LOG_W("shadow of '%s' refused (track %d, file %04x/%d)", se->user, track, magic, ftrack);
        } else {
            shadow *x = NULL;
            for (i = 0; i < s->nsh; i++) {
                if (s->sh[i].track == track && ieq(s->sh[i].user, se->user)) {
                    x = &s->sh[i];
                    free(x->data);
                    break;
                }
            }
            if (!x) {
                x = add_shadow(s);
            }
            if (x) {
                x->id = s->next_id++;
                x->track = track;
                copy_str(x->user, se->user, NAMESZ);
                x->value = se->up_a;
                x->time = t;
                x->data = se->up_buf;
                x->len = se->up_size;
                se->up_buf = NULL;
                LOG_I("'%s' uploaded a %s shadow (%s %lu)", se->user, k_tracks[track],
                      k_scored[track] ? "score" : "ms", (unsigned long)x->value);
                save(s);
            }
        }
    } else if (se->up_code == UP_CAREER) {
        career *x = NULL;
        for (i = 0; i < s->nca; i++) {
            if (ieq(s->ca[i].user, se->user)) {
                x = &s->ca[i];
                free(x->data);
                break;
            }
        }
        if (!x) {
            x = add_career(s);
        }
        if (x) {
            x->id = s->next_id++;
            copy_str(x->user, se->user, NAMESZ);
            x->pct = se->up_a;
            x->cash = se->up_b;
            x->time = t;
            x->data = se->up_buf;
            x->len = se->up_size;
            se->up_buf = NULL;
            LOG_I("'%s' uploaded the career (%lu, earnings %lu)", se->user, (unsigned long)x->pct,
                  (unsigned long)x->cash);
            save(s);
        }
    } else {
        LOG_W("upload with unknown code %u from '%s' dropped", se->up_code, se->user);
    }
    upload_end(se);
    {
        wr w;
        memset(&w, 0, sizeof(w));
        send_msg(s, se, ST_UP_DONE, &w, now);
        free(w.d);
    }
}

static void h_upload(gcs_ssx *s, session *se, rd *r, int by_name, gc_u32 now)
{
    char name[100], desc[260];
    gc_u32 size;
    wr w;
    upload_end(se);
    if (by_name) {
        r_str(r, name, sizeof(name));
        se->up_code = 0;
    } else {
        se->up_code = (gc_u16)r_u16(r);
    }
    size = r_u32(r);
    r_str(r, desc, sizeof(desc));
    se->up_a = r_u32(r);
    se->up_b = r_u32(r);
    if (r->bad || !size || size > GCSX_MAX_FILE) {
        LOG_W("upload of %lu bytes refused", (unsigned long)size);
        return;
    }
    se->up_buf = (gc_u8 *)calloc(1, size);
    se->up_map = (gc_u8 *)calloc(1, (size + 7) / 8);
    if (!se->up_buf || !se->up_map) {
        upload_end(se);
        return;
    }
    se->up = 1;
    se->up_size = size;
    se->up_got = 0;
    LOG_D("upload %u from '%s': %lu bytes (%lu, %lu)", se->up_code, se->user, (unsigned long)size,
          (unsigned long)se->up_a, (unsigned long)se->up_b);
    memset(&w, 0, sizeof(w));
    send_msg(s, se, ST_UP_READY, &w, now);
    free(w.d);
}

static void h_chunk(gcs_ssx *s, session *se, rd *r, gc_u32 now)
{
    gc_u32 start = r_u32(r), end = r_u32(r), len = r_u32(r), i;
    if (r->bad || !se->up || len != end - start + 1 || start > end || end >= se->up_size || len > r->n - r->off) {
        return;
    }
    for (i = 0; i < len; i++) {
        gc_u32 at = start + i;
        if (!(se->up_map[at / 8] & (1u << (at % 8)))) {
            se->up_map[at / 8] |= (gc_u8)(1u << (at % 8));
            se->up_buf[at] = r->p[r->off + i];
            se->up_got++;
        }
    }
    if (se->up_got == se->up_size) {
        finish_upload(s, se, now);
    }
}

/* ---- discovery and key exchange ---- */

static void h_key_req(gcs_ssx *s, session *se, gc_u32 now)
{
    wr w;
    if (need_rsa(s) != 0) {
        return;
    }
    se->have_key = 0;
    se->hash = 0;
    memset(&w, 0, sizeof(w));
    w_str(&w, s->rsa_n);
    w_str(&w, s->rsa_e);
    send_msg(s, se, ST_KEY, &w, now);
    free(w.d);
}

static void h_key(gcs_ssx *s, session *se, rd *r, gc_u32 now)
{
    size_t len;
    const gc_u8 *blk = r_blob(r, &len);
    gc_u8 key[256];
    int k;
    if (r->bad || !s->have_rsa) {
        return;
    }
    k = gcs_rsa_decrypt_pkcs1(&s->rsa, blk, len, key, sizeof(key));
    if (k < 0) {
        /* no padding: the number itself */
        gc_u8 m[256];
        size_t nb = (size_t)s->rsa.bits / 8, i = 0;
        if (gcs_rsa_private(&s->rsa, blk, len, m) != 0) {
            LOG_W("session key not readable");
            return;
        }
        while (i < nb && !m[i]) {
            i++;
        }
        k = (int)(nb - i);
        if (k <= (int)sizeof(key)) {
            memcpy(key, m + i, (size_t)k);
        }
    }
    if (k != 16) {
        LOG_W("session key of %d bytes", k);
        return;
    }
    gcs_3des_setkey(&se->des, key);
    se->have_key = 1;
    se->hash = 0;
    reply_u8(s, se, ST_KEY_OK, 1, now);
}

static void h_discover(gcs_ssx *s, session *se, rd *r, gc_u32 now)
{
    char md5[64], id[32], imei[32];
    wr w;
    int update;
    r_u16(r);
    r_str(r, md5, sizeof(md5));
    r_str(r, id, sizeof(id));
    r_u16(r);
    r_u16(r);
    r_str(r, imei, sizeof(imei));
    if (r->bad) {
        return;
    }
    update = s->plugin_len && !ieq(md5, s->plugin_md5);
    LOG_I("discovery from %u.%u.%u.%u: billing DLL %s%s", se->addr[0], se->addr[1], se->addr[2], se->addr[3],
          md5[0] ? md5 : "-", update ? ", sending ours" : (s->plugin_len ? " (ours)" : " (we have none)"));
    memset(&w, 0, sizeof(w));
    w_u8(&w, update ? 1 : 0);
    w_str(&w, update ? "PhantomLink" : "");
    send_msg(s, se, ST_DISCOVER, &w, now);
    free(w.d);
}

/* ---- input ---- */

static void dispatch(gcs_ssx *s, session *se, int type, rd *r, gc_u32 now)
{
    switch (type) {
    case MT_KEY_REQ:
        h_key_req(s, se, now);
        break;
    case MT_KEY:
        h_key(s, se, r, now);
        break;
    case MT_DISCOVER:
        h_discover(s, se, r, now);
        break;
    case MT_DISC_FILE:
        if (s->plugin_len) {
            send_file(s, se, s->plugin, s->plugin_len, 0, 0, now);
        }
        break;
    case MT_LOGON:
        h_logon(s, se, r, now);
        break;
    case MT_CREATE:
        h_create(s, se, r, now);
        break;
    case MT_CHANGE_PW:
        h_change_pw(s, se, r, now);
        break;
    case MT_QUESTION:
        h_question(s, se, r, now);
        break;
    case MT_PASSWORD:
        h_password(s, se, r, now);
        break;
    case MT_USERNAME:
        h_username(s, se, r, now);
        break;
    case MT_LOGOUT:
        if (se->user[0]) {
            LOG_I("'%s' logged out", se->user);
        }
        se->user[0] = 0;
        upload_end(se);
        break;
    case MT_OBJLIST:
        h_objlist(s, se, r, now);
        break;
    case MT_FILELIST:
    case MT_MSGLIST:
    {
        wr w;
        memset(&w, 0, sizeof(w));
        w_u8(&w, 0);
        w_u8(&w, 0);
        w_u8(&w, 1);
        send_msg(s, se, type == MT_FILELIST ? ST_FILELIST : ST_MSGLIST, &w, now);
        free(w.d);
        break;
    }
    case MT_PING:
    {
        wr w;
        gc_u32 a = r_u32(r), b = r_u32(r);
        memset(&w, 0, sizeof(w));
        w_u32(&w, a);
        w_u32(&w, b);
        send_msg(s, se, ST_PONG, &w, now);
        free(w.d);
        break;
    }
    case MT_DL_OBJECT:
        h_download(s, se, r, now);
        break;
    case MT_UP_CODE:
    case MT_UP_NAME:
        h_upload(s, se, r, type == MT_UP_NAME, now);
        break;
    case MT_UP_CHUNK:
        h_chunk(s, se, r, now);
        break;
    default:
        LOG_D("message %d ignored", type);
        break;
    }
}

void gcs_ssx_input(gcs_ssx *s, void *key, const gc_u8 addr[4], gc_u16 port, gc_u16 peer_port, const gc_u8 *d,
                   size_t len, gc_u32 now)
{
    session *se;
    gc_u32 seq;
    int type, noack, enc;
    rd r;
    gc_u8 *plain = NULL;
    if (!s || !d) {
        return;
    }
    s->datagrams++;
    if (len < HDR) {
        s->bad++;
        return;
    }
    seq = (gc_u32)d[0] | ((gc_u32)d[1] << 8) | ((gc_u32)d[2] << 16) | ((gc_u32)d[3] << 24);
    type = d[4];
    noack = d[5];
    enc = (d[7] & 0x80) != 0;
    se = ses_get(s, key, addr, port, peer_port, now);
    se->t_last = now;
    if (type == MT_ACK) {
        if (len >= HDR + 4) {
            got_ack(s, se, (gc_u32)d[8] | ((gc_u32)d[9] << 8) | ((gc_u32)d[10] << 16) | ((gc_u32)d[11] << 24), now);
        }
        return;
    }
    if (!noack) {
        send_ack(s, se, seq);
    }
    if (type == MT_KEY_REQ && (se->key_req_seq < 0 || (gc_u32)se->key_req_seq != seq)) {
        /* a new conversation (the game numbers each connection from the start) */
        se->nseen = 0;
        se->seen_pos = 0;
        se->key_req_seq = (int)(seq & 0x7fffffff);
    }
    if (seen_check(se, seq)) {
        return;
    }
    r.off = 0;
    r.bad = 0;
    if (enc) {
        long n = unseal(se, d + HDR, len - HDR, &plain);
        if (n < 0) {
            s->bad++;
            LOG_D("message %d from %u.%u.%u.%u:%u not decryptable", type, addr[0], addr[1], addr[2], addr[3],
                  peer_port);
            return;
        }
        r.p = plain;
        r.n = (size_t)n;
    } else {
        r.p = d + HDR;
        r.n = len - HDR;
    }
    dispatch(s, se, type, &r, now);
    free(plain);
}

void gcs_ssx_poll(gcs_ssx *s, gc_u32 now)
{
    int i, k;
    if (!s) {
        return;
    }
    for (i = 0; i < GCSX_SESSIONS; i++) {
        session *se = &s->ses[i];
        if (!se->used) {
            continue;
        }
        if ((gc_u32)(now - se->t_last) >= GCSX_IDLE_MS) {
            LOG_D("session %u.%u.%u.%u:%u idle, forgotten", se->addr[0], se->addr[1], se->addr[2], se->addr[3],
                  se->peer_port);
            ses_free(se);
            continue;
        }
        for (k = 0; k < se->nfly; k++) {
            outmsg *m = &se->fly[k];
            if ((gc_u32)(now - m->t) >= GCSX_RESEND_MS) {
                if (m->tries >= GCSX_TRIES) {
                    LOG_I("session %u.%u.%u.%u:%u does not answer, closed", se->addr[0], se->addr[1], se->addr[2],
                          se->addr[3], se->peer_port);
                    ses_free(se);
                    break;
                }
                m->t = now;
                m->tries++;
                raw_send(s, se, m->d, m->len);
            }
        }
    }
}

void gcs_ssx_peer_gone(gcs_ssx *s, void *key)
{
    int i;
    if (!s) {
        return;
    }
    for (i = 0; i < GCSX_SESSIONS; i++) {
        if (s->ses[i].used && s->ses[i].key == key) {
            ses_free(&s->ses[i]);
        }
    }
}

void gcs_ssx_get_stats(const gcs_ssx *s, gcs_ssx_stats *st)
{
    int i;
    memset(st, 0, sizeof(*st));
    if (!s) {
        return;
    }
    for (i = 0; i < GCSX_SESSIONS; i++) {
        st->sessions += s->ses[i].used;
    }
    st->shadows = s->nsh;
    st->careers = s->nca;
    st->datagrams = s->datagrams;
    st->bad_datagrams = s->bad;
}
