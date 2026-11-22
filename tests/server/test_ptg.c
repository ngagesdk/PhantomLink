/** @file test_ptg.c
 *
 *  Pathway to Glory: Ikusa Islands (game class 2955) and Pathway to
 *  Glory (43520) against simulated games.  The SNAP side is snapcomm.dll as ArenaFramework's Play Now
 *  screens use it (lobby, creating and joining a game room, the room
 *  attribute HOST); the game side rebuilds the receiving half of the
 *  game's network layer (6R82.APP 0x84b9c) and checks the server
 *  against it:
 *
 *   - header bits (type, restart, tag, zlib), pieces of 596 bytes, zlib
 *     messages, bundles of commands (32 -> 16) and of synchronisation
 *     records (34 -> 21, then the game's own 35), permanent damage data
 *     (23, then the game's own 41), acknowledgements (33)
 *   - keep alive (19), commands of every player to all others, client
 *     messages to the host, host messages to the clients,
 *     Field Radio to the other players, status 50 when the host or the
 *     last opponent leaves
 *   - the GPRS lobby the server runs (6R72.APP 0x93024): player ids (0 ->
 *     9), game settings (14), player records with sides (10), side
 *     changes (11), leaving (12), the start (13).
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#include "gc_test.h"
#include "gcs_ptg.h"
#include "gcs_rcodec.h"
#include "gcs_snap.h"
#include "gcs_wwp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const gc_u8 SRV[4] = { 10, 64, 0, 1 };
static gc_u32 g_now = 5000;
static int g_verbose;

/* ---- the game's network layer, receiving half ---- */

#define QMAX 64

typedef struct
{
    gc_u8 *d;
    size_t n;
} qmsg;

typedef struct
{
    qmsg q[55][QMAX]; /* one queue per type */
    int qn[55];
    /* message in pieces, per type (0x84b9c: +744..+768) */
    gc_u8 *buf[55];
    size_t total[55], off[55];
    int left[55], packed[55];
    gc_u32 usize[55];
    gc_u16 c23, c32; /* +736, +738 */
    int drop;        /* +740 */
    gc_u8 acks[QMAX][8];
    int n_acks;
    int status;  /* last type 50 code, -1 none */
    int bad;     /* something the game would refuse */
    int raw34;   /* a type 34 the game would not unpack */
    int classic; /* Pathway to Glory (6R72.APP 0x79884) */
} ptg_client;

static void q_push(ptg_client *c, int type, const gc_u8 *d, size_t n)
{
    qmsg *m;
    if (c->qn[type] == QMAX) {
        c->bad++;
        return;
    }
    m = &c->q[type][c->qn[type]++];
    m->d = (gc_u8 *)malloc(n ? n : 1);
    if (n) {
        memcpy(m->d, d, n);
    }
    m->n = n;
}

static int q_pop(ptg_client *c, int type, gc_u8 *out, size_t cap)
{
    qmsg m;
    int n;
    if (!c->qn[type]) {
        return -1;
    }
    m = c->q[type][0];
    memmove(&c->q[type][0], &c->q[type][1], sizeof(qmsg) * (size_t)(c->qn[type] - 1));
    c->qn[type]--;
    n = (int)(m.n < cap ? m.n : cap);
    if (out && n) {
        memcpy(out, m.d, (size_t)n);
    }
    free(m.d);
    return (int)m.n;
}

static void client_reset(ptg_client *c)
{
    int t, classic = c->classic;
    for (t = 0; t < 55; t++) {
        while (q_pop(c, t, NULL, 0) >= 0) {
        }
        free(c->buf[t]);
    }
    memset(c, 0, sizeof(*c));
    c->status = -1;
    c->classic = classic;
}

static gc_u16 rd16(const gc_u8 *d)
{
    return (gc_u16)(d[0] | (d[1] << 8));
}

static void wr16(gc_u8 *d, gc_u16 v)
{
    d[0] = (gc_u8)v;
    d[1] = (gc_u8)(v >> 8);
}

/* bundle {u16 length, data}... into the queue of 'type' (0x85310) */
static void unbundle(ptg_client *c, int type, const gc_u8 *d, size_t n)
{
    size_t pos = 0;
    while (pos + 2 <= n) {
        size_t l = (gc_u16)rd16(d + pos);
        if (pos + 2 + l > n) {
            c->bad++;
            return;
        }
        q_push(c, type, d + pos + 2, l);
        pos += 2 + l;
    }
}

static void complete(ptg_client *c, int type, const gc_u8 *d, size_t n)
{
    if (type == GCS_PTG_COMMANDS) {
        unbundle(c, GCS_PTG_COMMAND, d, n);
    } else if (type == GCS_PTG_SYNCS) {
        unbundle(c, GCS_PTG_SYNC, d, n);
    } else {
        q_push(c, type, d, n);
    }
    if (type == GCS_PTG_SYNCS) {
        q_push(c, GCS_PTG_SYNC_END, NULL, 0);
    } else if (type == GCS_PTG_PERM) {
        q_push(c, GCS_PTG_PERM_END, NULL, 0);
    }
}

static void client_input(ptg_client *c, const gc_u8 *m, size_t n)
{
    gc_u16 h;
    int type;
    if (n < 4) {
        c->bad++;
        return;
    }
    if (n > 4 + GCS_PTG_FRAG + 4) {
        c->bad++; /* longer than the game's pieces */
    }
    h = rd16(m);
    type = h & 0x7f;
    if (c->classic) {
        /* the first byte is the type (more than 51: error), and the
         * header without bit 15 is the queue number: nothing else may
         * be set */
        if (m[0] > 51 || (h & 0x7f00)) {
            c->bad++;
            return;
        }
        type = m[0];
    } else if (type > 54) {
        c->bad++;
        return;
    }
    if (!c->classic && (h & 0x80)) {
        c->c23 = c->c32 = 0;
        c->drop = 0;
    }
    if (type == GCS_PTG_PERM || type == GCS_PTG_SYNCS || type == GCS_PTG_COMMANDS) {
        if (c->drop && !c->classic) {
            return;
        }
        if (type == GCS_PTG_COMMANDS) {
            c->c32++;
        } else {
            c->c23++;
        }
        if (c->n_acks < QMAX) {
            gc_u8 *a = c->acks[c->n_acks++];
            wr16(a, c->c23);
            wr16(a + 2, c->c32);
            wr16(a + 4, (gc_u16)((h >> 8) & 0x7f));
            wr16(a + 6, 0);
        }
    }
    if (type == GCS_PTG_STATUS) {
        c->status = rd16(m + 2) > 1 ? rd16(m + 4) : 0;
        return;
    }
    if (c->left[type]) {
        size_t k;
        c->left[type]--;
        k = c->left[type] ? GCS_PTG_FRAG : c->total[type] - c->off[type];
        if (k > n - 4 || c->off[type] + k > c->total[type]) {
            c->bad++;
            return;
        }
        memcpy(c->buf[type] + c->off[type], m + 4, k);
        c->off[type] += k;
    } else if (h & 0x8000) {
        size_t clen = rd16(m + 2), cnt, first;
        gc_u32 us = (gc_u32)m[4] | ((gc_u32)m[5] << 8) | ((gc_u32)m[6] << 16) | ((gc_u32)m[7] << 24);
        if (us > GCS_PTG_MAX_UNPACK) {
            c->bad++;
            return;
        }
        cnt = (clen + 4 + GCS_PTG_FRAG - 1) / GCS_PTG_FRAG;
        first = cnt == 1 ? clen : GCS_PTG_FRAG - 4;
        if (8 + first > n) {
            c->bad++;
            return;
        }
        free(c->buf[type]);
        c->buf[type] = (gc_u8 *)malloc(clen ? clen : 1);
        memcpy(c->buf[type], m + 8, first);
        c->total[type] = clen;
        c->off[type] = first;
        c->left[type] = (int)cnt - 1;
        c->packed[type] = 1;
        c->usize[type] = us;
    } else {
        size_t total = rd16(m + 2);
        if (total + 4 > 600) {
            size_t cnt = (total + GCS_PTG_FRAG - 1) / GCS_PTG_FRAG;
            free(c->buf[type]);
            c->buf[type] = (gc_u8 *)malloc(total);
            memcpy(c->buf[type], m + 4, GCS_PTG_FRAG);
            c->total[type] = total;
            c->off[type] = GCS_PTG_FRAG;
            c->left[type] = (int)cnt - 1;
            c->packed[type] = 0;
        } else {
            if (4 + total != n) {
                c->bad++;
                return;
            }
            if (type == GCS_PTG_SYNC || type == GCS_PTG_COMMANDS) {
                complete(c, type, m + 4, total);
            } else {
                if (type == GCS_PTG_SYNCS) {
                    c->raw34++; /* queued as it is, never unpacked */
                }
                q_push(c, type, m + 4, total);
            }
            return;
        }
    }
    if (!c->left[type]) {
        if (c->packed[type]) {
            gc_u8 *out = (gc_u8 *)malloc(c->usize[type] ? c->usize[type] : 1);
            long r = gcs_zlib_inflate(c->buf[type], c->total[type], out, c->usize[type]);
            if (r != (long)c->usize[type]) {
                c->bad++;
            } else {
                complete(c, type, out, c->usize[type]);
            }
            free(out);
        } else {
            complete(c, type, c->buf[type], c->total[type]);
        }
        free(c->buf[type]);
        c->buf[type] = NULL;
        c->packed[type] = 0;
    }
}

/* ---- simulated phones (snapcomm.dll side) ---- */

#define MAXIN 64

typedef struct
{
    gc_u8 h[16];
    gc_u8 p[1024];
    size_t n;
} msg;

typedef struct
{
    int key;
    gc_u16 port;
    const char *name;
    gc_u32 uid, tx_rel, rx_next;
    msg in[MAXIN];
    int n_in;
    gc_u32 lobby, room;
    ptg_client c;
} phone;

static phone P[14];
static gcs_snap *S;
static gcs_ptg *PT;

static gc_u32 be32(const gc_u8 *p)
{
    return ((gc_u32)p[0] << 24) | ((gc_u32)p[1] << 16) | ((gc_u32)p[2] << 8) | p[3];
}

static void put32(gc_u8 *p, gc_u32 v)
{
    p[0] = (gc_u8)(v >> 24);
    p[1] = (gc_u8)(v >> 16);
    p[2] = (gc_u8)(v >> 8);
    p[3] = (gc_u8)v;
}

static void io_send(void *ctx, void *key, const gc_u8 addr[4], gc_u16 port, gc_u16 peer_port, const gc_u8 *d,
                    size_t n)
{
    phone *g = (phone *)key;
    size_t pos = 0;
    (void)ctx;
    (void)addr;
    (void)port;
    (void)peer_port;
    CHECK(n >= 20 && be32(d + n - 4) == 0xBA476611u);
    while (pos + 16 <= n - 4) {
        gc_u16 fl = (gc_u16)((d[pos] << 8) | d[pos + 1]);
        size_t len = fl & 0x3ff;
        if (len < 16 || pos + len > n - 4) {
            CHECK(0);
            return;
        }
        if (fl & 0x8000) {
            gc_u8 a[20];
            memset(a, 0, sizeof(a));
            a[0] = 0x60;
            a[1] = 0x10;
            put32(a + 4, g->uid);
            put32(a + 12, be32(d + pos + 8));
            put32(a + 16, 0xBA476611u);
            gcs_snap_input(S, &g->key, SRV, GCSS_PORT, g->port, a, 20, g_now);
            g->rx_next = be32(d + pos + 8) + 1;
        }
        if (d[pos + 3] == GCSS_OP_DATA && !(fl & 0x1000)) {
            /* room data: the game's GameDataReceived, one message each */
            client_input(&g->c, d + pos + 16, len - 16);
        } else if (d[pos + 3] != 0 && g->n_in < MAXIN) {
            msg *m = &g->in[g->n_in++];
            memcpy(m->h, d + pos, 16);
            m->n = len - 16;
            memcpy(m->p, d + pos + 16, m->n);
        }
        pos += len;
    }
}

static void io_log(void *ctx, int level, const char *line)
{
    (void)ctx;
    if (g_verbose || level <= 0) {
        printf("  [%d] %s\n", level, line);
    }
}

static int io_auth(void *ctx, void *key, const char *user, char canon[GCSS_MAX_NAME + 1])
{
    phone *g = (phone *)key;
    (void)ctx;
    if (strcmp(user, g->name)) {
        return 0;
    }
    strcpy(canon, user);
    return 1;
}

static void send_msg(phone *g, gc_u16 flags, gc_u8 seq, gc_u8 op, const gc_u8 *p, size_t n)
{
    gc_u8 d[1100];
    size_t len = 16 + n;
    flags |= 0x2000;
    d[0] = (gc_u8)((flags | len) >> 8);
    d[1] = (gc_u8)(flags | len);
    d[2] = seq;
    d[3] = op;
    put32(d + 4, g->uid);
    put32(d + 8, (flags & 0x8000) ? g->tx_rel++ : 0);
    put32(d + 12, 0);
    if (n) {
        memcpy(d + 16, p, n);
    }
    put32(d + len, 0xBA476611u);
    gcs_snap_input(S, &g->key, SRV, GCSS_PORT, g->port, d, len + 4, g_now);
}

static msg *take(phone *g, gc_u8 op)
{
    static msg m;
    int i;
    for (i = 0; i < g->n_in; i++) {
        if (g->in[i].h[3] == op) {
            m = g->in[i];
            memmove(&g->in[i], &g->in[i + 1], sizeof(msg) * (size_t)(g->n_in - i - 1));
            g->n_in--;
            return &m;
        }
    }
    return NULL;
}

/* the game's Send (0x844b4): {u16 type, u16 length, data} through
 * CArenaFramework slot 0x3c, i.e. op 17 */
static void game_send(phone *g, gc_u8 type, const void *d, size_t n)
{
    gc_u8 b[1024];
    CHECK(n + 4 <= sizeof(b));
    wr16(b, type);
    wr16(b + 2, (gc_u16)n);
    if (n) {
        memcpy(b + 4, d, n);
    }
    send_msg(g, 0xa000, 0, GCSS_OP_TO_SERVER, b, 4 + n);
}

/* the acknowledgements the game wrote while receiving (type 33) */
static void send_acks(phone *g)
{
    int i, n = g->c.n_acks;
    g->c.n_acks = 0;
    for (i = 0; i < n; i++) {
        game_send(g, GCS_PTG_ACK, g->c.acks[i], 8);
    }
}

static gc_u32 g_class = GCS_PTG_GAME_CLASS;

static void login(phone *g)
{
    gc_u8 p[112];
    msg *m;
    memset(p, 0, sizeof(p));
    strcpy((char *)p, g->name);
    strcpy((char *)p + 40, "ptg");
    put32(p + 100, g_class);
    send_msg(g, 0x1000, 0, GCSS_OP_LOGIN, p, sizeof(p));
    m = take(g, GCSS_OP_LOGIN_OK);
    CHECK(m != NULL);
    g->uid = m ? be32(m->p + 8) : 0;
    client_reset(&g->c);
}

static void enter_lobby(phone *g, const char *name)
{
    gc_u8 p[16];
    msg *m;
    gc_u32 i, n;
    int found = 0;
    memset(p, 0, sizeof(p));
    send_msg(g, 0xb000, 3, GCSS_OP_GAME_LOBBIES, p, 0);
    m = take(g, GCSS_OP_GAME_LOBBIES);
    CHECK(m != NULL);
    if (!m) {
        return;
    }
    CHECK_EQ(be32(m->p), g_class);
    n = be32(m->p + 8);
    for (i = 0; i < n; i++) {
        const gc_u8 *e = m->p + 12 + 36 * i;
        if (!strcmp((const char *)e, name)) {
            found = 1;
            g->lobby = be32(e + 32);
        }
    }
    CHECK(found);
    put32(p, g->lobby);
    send_msg(g, 0xb000, 5, GCSS_OP_JOIN, p, 4);
    m = take(g, GCSS_OP_OK);
    CHECK(m && be32(m->p) == GCSS_OP_JOIN);
}

static gc_u32 create_room(phone *g, const char *name, gc_u32 max)
{
    gc_u8 p[44];
    msg *m;
    memset(p, 0, sizeof(p));
    strcpy((char *)p, name);
    put32(p + 16, max);
    send_msg(g, 0xb000, 10, GCSS_OP_CREATE_ROOM, p, 44);
    m = take(g, GCSS_OP_OK);
    CHECK(m && be32(m->p) == GCSS_OP_CREATE_ROOM);
    g->room = m ? be32(m->p + 4) : 0;
    g->n_in = 0;
    return g->room;
}

static void join_room(phone *g, gc_u32 room)
{
    gc_u8 p[24];
    msg *m;
    memset(p, 0, sizeof(p));
    put32(p, room);
    send_msg(g, 0xa000, 32, GCSS_OP_JOIN, p, 24);
    m = take(g, GCSS_OP_OK);
    CHECK(m && be32(m->p) == GCSS_OP_JOIN && be32(m->p + 4) == room);
    g->room = room;
}

/* op 9: the room attribute HOST, which ArenaFramework compares with its
 * own user id (IsHost) */
static gc_u32 room_host(phone *g)
{
    gc_u8 p[8];
    msg *m;
    put32(p, g->room);
    memcpy(p + 4, "HOST", 4);
    send_msg(g, 0xa000, 33, GCSS_OP_ATTR2, p, 8);
    m = take(g, GCSS_OP_ATTR2);
    CHECK(m && m->n == 12 && !memcmp(m->p + 4, "HOST", 4));
    return m ? be32(m->p + 8) : 0;
}

/* op 8 STAT: ArenaFramework's room status; bit 31 = game in progress
 * (set by the host when it starts the game, CArenaFramework 0x7a0c) */
static void set_stat(phone *g, gc_u32 v)
{
    gc_u8 p[8];
    memcpy(p, "STAT", 4);
    put32(p + 4, v);
    send_msg(g, 0xa000, 40, GCSS_OP_ATTR, p, 8);
}

static void clear_in(void)
{
    int i;
    for (i = 0; i < 14; i++) {
        P[i].n_in = 0;
    }
}

static void fill(gc_u8 *d, size_t n, gc_u8 seed)
{
    size_t i;
    for (i = 0; i < n; i++) {
        d[i] = (gc_u8)(seed + i * 7 + (i >> 8));
    }
}

static void test_game(void)
{
    phone *a = &P[0], *b = &P[1], *c = &P[2];
    gc_u8 d[2100], r[2100];
    gc_u32 room;
    int i, n;
    gcs_ptg_stats st;

    login(a);
    login(b);
    login(c);
    enter_lobby(a, "Freestyle");
    enter_lobby(b, "Freestyle");
    enter_lobby(c, "Freestyle");
    room = create_room(a, "Battle", 6);
    join_room(b, room);
    join_room(c, room);
    clear_in();

    /* IsHost: the creator */
    CHECK_EQ(room_host(b), a->uid);
    CHECK_EQ(room_host(a), a->uid);

    /* no game yet: no pings */
    gcs_snap_poll(S, g_now);
    CHECK_EQ(a->c.qn[GCS_PTG_PING], 0);
    /* the host starts the game: every phone gets pings until its
     * network layer runs (the first message from the server starts it) */
    set_stat(a, 0x90000000u);
    gcs_snap_poll(S, g_now);
    for (i = 0; i < 3; i++) {
        CHECK_EQ(q_pop(&P[i].c, GCS_PTG_PING, r, sizeof(r)), 0);
        game_send(&P[i], GCS_PTG_KEEPALIVE, NULL, 0); /* the game's answer */
    }
    g_now += 1000;
    gcs_snap_poll(S, g_now);
    CHECK_EQ(b->c.qn[GCS_PTG_PING], 0);
    g_now += 1000;
    gcs_snap_poll(S, g_now);
    CHECK_EQ(q_pop(&b->c, GCS_PTG_PING, r, sizeof(r)), 0); /* the empty 19 does not count */
    q_pop(&a->c, GCS_PTG_PING, NULL, 0);
    q_pop(&c->c, GCS_PTG_PING, NULL, 0);
    game_send(a, 27, "ver", 3); /* now the layers run */
    game_send(b, GCS_PTG_KEEPALIVE, "\0\0\0\0", 4);
    q_pop(&b->c, GCS_PTG_KEEPALIVE, NULL, 0);
    game_send(c, GCS_PTG_COMMAND, "c", 1);
    q_pop(&a->c, GCS_PTG_COMMAND, NULL, 0);
    q_pop(&b->c, GCS_PTG_COMMAND, NULL, 0);
    send_acks(a);
    send_acks(b);
    g_now += 5000;
    gcs_snap_poll(S, g_now);
    for (i = 0; i < 3; i++) {
        CHECK_EQ(P[i].c.qn[GCS_PTG_PING], 0);
    }
    q_pop(&b->c, 27, NULL, 0); /* the host's message went to the clients */
    q_pop(&c->c, 27, NULL, 0);

    /* keep alive: 4 bytes back, nothing to the others */
    game_send(b, GCS_PTG_KEEPALIVE, "\x01\x02\x03\x04", 4);
    n = q_pop(&b->c, GCS_PTG_KEEPALIVE, r, sizeof(r));
    CHECK(n == 4 && !memcmp(r, "\x01\x02\x03\x04", 4));
    CHECK_EQ(a->c.qn[GCS_PTG_KEEPALIVE], 0);
    game_send(b, GCS_PTG_KEEPALIVE, NULL, 0); /* answer to a ping: no reply */
    CHECK_EQ(b->c.qn[GCS_PTG_KEEPALIVE], 0);

    /* a command of a client goes to everybody else as a bundle */
    game_send(b, GCS_PTG_COMMAND, "MOVE soldier 3", 14);
    for (i = 0; i < 3; i++) {
        phone *x = &P[i];
        if (x == b) {
            CHECK_EQ(x->c.qn[GCS_PTG_COMMAND], 0);
            continue;
        }
        n = q_pop(&x->c, GCS_PTG_COMMAND, r, sizeof(r));
        CHECK(n == 14 && !memcmp(r, "MOVE soldier 3", 14));
        CHECK_EQ(x->c.qn[GCS_PTG_COMMANDS], 0); /* unpacked, not queued */
        CHECK_EQ(x->c.n_acks, 1);
        CHECK_EQ(rd16(x->c.acks[0] + 2), 1);
        send_acks(x);
    }
    /* the host's commands too, in order */
    game_send(a, GCS_PTG_COMMAND, "FIRE 1", 6);
    game_send(a, GCS_PTG_COMMAND, "FIRE 2", 6);
    CHECK_EQ(q_pop(&b->c, GCS_PTG_COMMAND, r, sizeof(r)), 6);
    CHECK(!memcmp(r, "FIRE 1", 6));
    CHECK_EQ(q_pop(&b->c, GCS_PTG_COMMAND, r, sizeof(r)), 6);
    CHECK(!memcmp(r, "FIRE 2", 6));
    CHECK_EQ(c->c.qn[GCS_PTG_COMMAND], 2);
    CHECK_EQ(a->c.qn[GCS_PTG_COMMAND], 0);
    send_acks(b);
    send_acks(c);

    /* no phone is the host of a GPRS game: a client's message (27, the
     * player ended) goes to every other player */
    game_send(c, 27, "turn", 4);
    CHECK_EQ(q_pop(&a->c, 27, r, sizeof(r)), 4);
    CHECK_EQ(q_pop(&b->c, 27, r, sizeof(r)), 4);
    CHECK_EQ(c->c.qn[27], 0);

    /* synchronisation: records of the host, then its end marker; the
     * clients get one bundle (zlib, in pieces) and make the 35 themselves */
    for (i = 0; i < 5; i++) {
        fill(d, 300, (gc_u8)(10 + i));
        game_send(a, GCS_PTG_SYNC, d, 300);
    }
    CHECK_EQ(b->c.qn[GCS_PTG_SYNC], 0);
    game_send(a, GCS_PTG_SYNC_END, NULL, 0);
    for (i = 1; i < 3; i++) {
        phone *x = &P[i];
        int k;
        CHECK_EQ(x->c.qn[GCS_PTG_SYNC], 5);
        CHECK_EQ(x->c.qn[GCS_PTG_SYNC_END], 1);
        CHECK_EQ(x->c.raw34, 0);
        for (k = 0; k < 5; k++) {
            fill(d, 300, (gc_u8)(10 + k));
            CHECK_EQ(q_pop(&x->c, GCS_PTG_SYNC, r, sizeof(r)), 300);
            CHECK(!memcmp(r, d, 300));
        }
        q_pop(&x->c, GCS_PTG_SYNC_END, NULL, 0);
        send_acks(x);
    }
    CHECK_EQ(a->c.qn[GCS_PTG_SYNC], 0);

    /* an empty synchronisation still ends with 35 */
    game_send(a, GCS_PTG_SYNC_END, NULL, 0);
    CHECK_EQ(q_pop(&b->c, GCS_PTG_SYNC_END, NULL, 0), 0);
    CHECK_EQ(b->c.qn[GCS_PTG_SYNC], 0);

    /* permanent damage data: 23 as it is (here in pieces), one 41 */
    fill(d, 1000, 77);
    game_send(a, GCS_PTG_PERM, d, 1000);
    game_send(a, GCS_PTG_PERM_END, NULL, 0);
    CHECK_EQ(q_pop(&c->c, GCS_PTG_PERM, r, sizeof(r)), 1000);
    CHECK(!memcmp(r, d, 1000));
    CHECK_EQ(c->c.qn[GCS_PTG_PERM_END], 1);
    q_pop(&c->c, GCS_PTG_PERM_END, NULL, 0);
    q_pop(&b->c, GCS_PTG_PERM, NULL, 0);
    q_pop(&b->c, GCS_PTG_PERM_END, NULL, 0);
    send_acks(b);
    send_acks(c);

    /* a 41 without 23 goes on */
    game_send(a, GCS_PTG_PERM_END, NULL, 0);
    CHECK_EQ(q_pop(&b->c, GCS_PTG_PERM_END, NULL, 0), 0);
    q_pop(&c->c, GCS_PTG_PERM_END, NULL, 0);

    /* Field Radio: 260-byte chunks to the other players, a repeated
     * copy (the host writes one per ally) only once */
    memset(d, 0, 260);
    d[0] = 2; /* player */
    d[1] = 0; /* chunk */
    d[2] = 3; /* chunks */
    fill(d + 4, 256, 99);
    game_send(b, GCS_PTG_RADIO, d, 260);
    game_send(b, GCS_PTG_RADIO, d, 260);
    d[1] = 1;
    game_send(b, GCS_PTG_RADIO, d, 260);
    CHECK_EQ(a->c.qn[GCS_PTG_RADIO], 2);
    CHECK_EQ(c->c.qn[GCS_PTG_RADIO], 2);
    CHECK_EQ(b->c.qn[GCS_PTG_RADIO], 0);
    CHECK_EQ(q_pop(&c->c, GCS_PTG_RADIO, r, sizeof(r)), 260);
    CHECK(r[0] == 2 && r[1] == 0 && r[2] == 3 && !memcmp(r + 4, d + 4, 256));
    q_pop(&c->c, GCS_PTG_RADIO, NULL, 0);
    q_pop(&a->c, GCS_PTG_RADIO, NULL, 0);
    q_pop(&a->c, GCS_PTG_RADIO, NULL, 0);

    /* type 52: the host passes it on to all */
    game_send(c, GCS_PTG_ALL, "GG", 2);
    CHECK_EQ(q_pop(&a->c, GCS_PTG_ALL, r, sizeof(r)), 2);
    CHECK_EQ(q_pop(&b->c, GCS_PTG_ALL, r, sizeof(r)), 2);

    /* a long host message reaches the clients in pieces */
    fill(d, 900, 5);
    game_send(a, 30, d, 900);
    CHECK_EQ(q_pop(&b->c, 30, r, sizeof(r)), 900);
    CHECK(!memcmp(r, d, 900));
    q_pop(&c->c, 30, NULL, 0);

    /* records without end marker go on alone after a while (no 35) */
    gcs_snap_poll(S, g_now);
    game_send(a, GCS_PTG_SYNC, "late", 4);
    g_now += 1000;
    gcs_snap_poll(S, g_now);
    CHECK_EQ(b->c.qn[GCS_PTG_SYNC], 0);
    g_now += 3000;
    gcs_snap_poll(S, g_now);
    CHECK_EQ(q_pop(&b->c, GCS_PTG_SYNC, r, sizeof(r)), 4);
    CHECK_EQ(b->c.qn[GCS_PTG_SYNC_END], 0);
    q_pop(&c->c, GCS_PTG_SYNC, NULL, 0);

    for (i = 0; i < 3; i++) {
        CHECK_EQ(P[i].c.bad, 0);
        CHECK_EQ(P[i].c.status, -1);
    }

    /* a client leaves: the others play on */
    send_msg(c, 0xa000, 14, GCSS_OP_LEAVE, d, 0);
    CHECK_EQ(a->c.status, -1);
    CHECK_EQ(b->c.status, -1);
    /* the host leaves: game over for the rest (code 3) */
    send_msg(a, 0xa000, 15, GCSS_OP_LEAVE, d, 0);
    CHECK_EQ(b->c.status, GCS_PTG_ST_HOST_LEFT);
    gcs_ptg_get_stats(PT, &st);
    CHECK_EQ(st.games, 1);
    CHECK(st.commands == 4 && st.radio_chunks == 2 && st.duplicates == 1);
    send_msg(b, 0xa000, 16, GCSS_OP_LEAVE, d, 0);
    gcs_ptg_get_stats(PT, &st);
    CHECK_EQ(st.games, 0);
}

/* the last opponent leaves a running game: code 8 for the host; lobby
 * members are no game */
static void test_last_opponent(void)
{
    phone *a = &P[3], *b = &P[4];
    gcs_ptg_stats st;
    gc_u8 d[4];
    login(a);
    login(b);
    enter_lobby(a, "Ranked");
    enter_lobby(b, "Ranked");
    gcs_ptg_get_stats(PT, &st);
    CHECK_EQ(st.games, 0);
    game_send(a, GCS_PTG_COMMAND, "x", 1); /* in the lobby: ignored */
    CHECK_EQ(b->c.qn[GCS_PTG_COMMAND], 0);
    join_room(b, create_room(a, "Ranked", 2));
    game_send(b, GCS_PTG_COMMAND, "attack", 6);
    CHECK_EQ(a->c.qn[GCS_PTG_COMMAND], 1);
    send_msg(b, 0xa000, 14, GCSS_OP_LEAVE, d, 0);
    CHECK_EQ(a->c.status, GCS_PTG_ST_NO_OPPONENTS);
    CHECK_EQ(a->c.bad, 0);
}

/* the first Pathway to Glory: same messages, plain header */
static void test_classic(void)
{
    phone *a = &P[5], *b = &P[6], *c = &P[7];
    gc_u8 d[2000], r[2000];
    int i;
    g_class = GCS_PTG1_GAME_CLASS;
    for (i = 5; i < 8; i++) {
        P[i].c.classic = 1;
    }
    login(a);
    login(b);
    login(c);
    enter_lobby(a, "Freestyle");
    enter_lobby(b, "Freestyle");
    enter_lobby(c, "Freestyle");
    join_room(b, create_room(a, "Battle", 6));
    join_room(c, a->room);
    clear_in();
    CHECK_EQ(room_host(c), a->uid);
    set_stat(a, 0x90000000u);
    gcs_snap_poll(S, g_now);
    for (i = 5; i < 8; i++) {
        CHECK_EQ(q_pop(&P[i].c, GCS_PTG_PING, r, sizeof(r)), 0);
    }

    game_send(c, GCS_PTG_KEEPALIVE, "abcd", 4);
    CHECK_EQ(q_pop(&c->c, GCS_PTG_KEEPALIVE, r, sizeof(r)), 4);
    for (i = 0; i < 4; i++) {
        game_send(b, GCS_PTG_COMMAND, "go", 2);
    }
    CHECK_EQ(a->c.qn[GCS_PTG_COMMAND], 4);
    CHECK_EQ(c->c.qn[GCS_PTG_COMMAND], 4);
    CHECK_EQ(rd16(a->c.acks[3] + 2), 4); /* counts run on, no restart */
    CHECK_EQ(rd16(a->c.acks[3] + 4), 0); /* no tag */
    send_acks(a);
    send_acks(c);
    for (i = 0; i < 4; i++) {
        fill(d, 400, (gc_u8)i);
        game_send(a, GCS_PTG_SYNC, d, 400);
    }
    game_send(a, GCS_PTG_SYNC_END, NULL, 0);
    CHECK_EQ(b->c.qn[GCS_PTG_SYNC], 4);
    CHECK_EQ(b->c.qn[GCS_PTG_SYNC_END], 1);
    fill(d, 400, 3);
    for (i = 0; i < 4; i++) {
        CHECK_EQ(q_pop(&b->c, GCS_PTG_SYNC, r, sizeof(r)), 400);
    }
    CHECK(!memcmp(r, d, 400));
    fill(d, 900, 9);
    game_send(a, GCS_PTG_PERM, d, 900);
    game_send(a, GCS_PTG_PERM_END, NULL, 0);
    CHECK_EQ(q_pop(&c->c, GCS_PTG_PERM, r, sizeof(r)), 900);
    CHECK(!memcmp(r, d, 900));
    CHECK_EQ(c->c.qn[GCS_PTG_PERM_END], 1);
    memset(d, 0, 260);
    d[0] = 1;
    d[2] = 1;
    game_send(a, GCS_PTG_RADIO, d, 260);
    CHECK_EQ(b->c.qn[GCS_PTG_RADIO], 1);
    CHECK_EQ(c->c.qn[GCS_PTG_RADIO], 1);
    send_msg(a, 0xa000, 15, GCSS_OP_LEAVE, d, 0);
    CHECK_EQ(b->c.status, GCS_PTG_ST_HOST_LEFT);
    CHECK_EQ(c->c.status, GCS_PTG_ST_HOST_LEFT);
    for (i = 5; i < 8; i++) {
        CHECK_EQ(P[i].c.bad, 0);
    }
}

/* ---- GPRS lobby ---- */

static gc_u32 rd32(const gc_u8 *d)
{
    return (gc_u32)d[0] | ((gc_u32)d[1] << 8) | ((gc_u32)d[2] << 16) | ((gc_u32)d[3] << 24);
}

static void wr32(gc_u8 *d, gc_u32 v)
{
    d[0] = (gc_u8)v;
    d[1] = (gc_u8)(v >> 8);
    d[2] = (gc_u8)(v >> 16);
    d[3] = (gc_u8)(v >> 24);
}

/* type 0 as 6R72.APP 0x93158 sends it: has settings, 3 bytes the game
 * leaves uninitialised, build */
static void hello(phone *g, int settings, gc_u32 build)
{
    gc_u8 d[8] = { 0, 0x6b, 0xf3, 0xff };
    d[0] = (gc_u8)settings;
    wr32(d + 4, build);
    game_send(g, GCS_PTG_HELLO, d, 8);
}

/* type 9: returns the player id, *master the flag */
static gc_u32 welcome(phone *g, int *master)
{
    gc_u8 r[16];
    CHECK_EQ(q_pop(&g->c, GCS_PTG_WELCOME, r, sizeof(r)), 8);
    *master = r[4];
    return rd32(r);
}

/* the player record the game sends (0x93664): +4 default side, +20 name */
static void send_record(phone *g, gc_u32 side)
{
    gc_u8 d[40];
    memset(d, 0, sizeof(d));
    wr32(d, 0x55555555u); /* not filled in by the game */
    wr32(d + 4, side);
    wr32(d + 8, 0x77777777u);
    strcpy((char *)d + 20, g->name);
    game_send(g, GCS_PTG_PLAYER, d, 40);
}

/* the first type 10 after its own must be its own (0x93720): id, side,
 * reason; the name stays */
static void own_record(phone *g, gc_u32 id, gc_u32 side, gc_u32 reason)
{
    gc_u8 r[64];
    CHECK_EQ(q_pop(&g->c, GCS_PTG_PLAYER, r, sizeof(r)), 40);
    CHECK_EQ(rd32(r), id);
    CHECK_EQ(rd32(r + 4), side);
    CHECK_EQ(rd32(r + 8), reason);
    CHECK(!strcmp((const char *)r + 20, g->name));
}

static void lobby_run(int classic)
{
    static const char *names[2][3] = { { "ivan", "judy", "kate" }, { "leo", "mia", "ned" } };
    phone *a = &P[8 + classic * 3], *b = a + 1, *c = a + 2;
    gc_u8 gd[136], r[256], w[12];
    int m, i;
    gc_u32 ib, ic;
    g_class = classic ? GCS_PTG1_GAME_CLASS : GCS_PTG_GAME_CLASS;
    for (i = 0; i < 3; i++) {
        phone *x = i == 0 ? a : i == 1 ? b : c;
        x->port = (gc_u16)(5001 + classic * 3 + i);
        x->name = names[classic][i];
        x->c.classic = classic;
        x->c.status = -1;
        login(x);
        enter_lobby(x, "Freestyle");
    }
    /* as on hardware 2026-11-19: the host is alone in the room when its
     * network layer starts */
    create_room(a, "Battle", 6);
    clear_in();
    set_stat(a, 0x90000000u);
    gcs_snap_poll(S, g_now);
    q_pop(&a->c, GCS_PTG_PING, NULL, 0);
    game_send(a, GCS_PTG_KEEPALIVE, NULL, 0);
    hello(a, 1, 0x559dbe46u);
    CHECK_EQ(welcome(a, &m), 1); /* ids from 1, as the BT host */
    CHECK_EQ(m, 1);
    /* the master: settings and its record; it waits for both */
    fill(gd, sizeof(gd), 3);
    game_send(a, GCS_PTG_GAMEDATA, gd, sizeof(gd));
    send_record(a, 0);
    /* the master's 136 bytes plus the server's block (0x88..0xbb) */
    CHECK_EQ(q_pop(&a->c, GCS_PTG_GAMEDATA, r, sizeof(r)), 188);
    CHECK(!memcmp(r, gd, 136));
    CHECK_EQ(rd32(r + 0xa4), 60); /* briefing countdown, s */
    CHECK(r[0xb4] == 8 && r[0xb6] == 16);
    own_record(a, 1, 0, 0);
    wr32(w, 1);
    game_send(a, GCS_PTG_JOINED, w, 4);

    /* the first client: its id, the settings, then its record; it also
     * wants side 0, which already has more players */
    join_room(b, a->room);
    gcs_snap_poll(S, g_now);
    q_pop(&b->c, GCS_PTG_PING, NULL, 0);
    hello(b, 0, 0x559dbe46u);
    ib = welcome(b, &m);
    CHECK(ib == 2 && m == 0);
    CHECK_EQ(q_pop(&b->c, GCS_PTG_GAMEDATA, r, sizeof(r)), 188);
    CHECK_EQ(b->c.qn[GCS_PTG_PLAYER], 0); /* nothing before its own record */
    send_record(b, 0);
    own_record(b, ib, 1, 3); /* nobody on side 1: join it instead? */
    CHECK_EQ(q_pop(&b->c, GCS_PTG_PLAYER, r, sizeof(r)), 40); /* the master's */
    CHECK_EQ(rd32(r), 1);
    CHECK_EQ(q_pop(&a->c, GCS_PTG_PLAYER, r, sizeof(r)), 40);
    CHECK_EQ(rd32(r), ib);
    wr32(w, 1);
    game_send(b, GCS_PTG_JOINED, w, 4);

    /* a client of another build: code 6 */
    join_room(c, a->room);
    hello(c, 0, 0x12345678u);
    CHECK_EQ(c->c.status, GCS_PTG_ST_BUILD);
    CHECK_EQ(c->c.qn[GCS_PTG_WELCOME], 0);
    /* the right build: side 0 again (both sides equal) */
    c->c.status = -1;
    hello(c, 0, 0x559dbe46u);
    ic = welcome(c, &m);
    CHECK(ic == 3 && m == 0);
    q_pop(&c->c, GCS_PTG_GAMEDATA, NULL, 0);
    send_record(c, 0);
    own_record(c, ic, 0, 0);
    CHECK_EQ(c->c.qn[GCS_PTG_PLAYER], 2);
    CHECK_EQ(a->c.qn[GCS_PTG_PLAYER], 1);
    CHECK_EQ(b->c.qn[GCS_PTG_PLAYER], 1);
    q_pop(&a->c, GCS_PTG_PLAYER, NULL, 0);
    q_pop(&b->c, GCS_PTG_PLAYER, NULL, 0);
    while (q_pop(&c->c, GCS_PTG_PLAYER, NULL, 0) >= 0) {
    }

    /* side changes: a client only for itself, the master for anybody */
    wr32(w, ib);
    wr32(w + 4, 0);
    game_send(c, GCS_PTG_SIDE, w, 8);
    CHECK_EQ(b->c.qn[GCS_PTG_SIDE], 0);
    game_send(b, GCS_PTG_SIDE, w, 8);
    for (i = 0; i < 3; i++) {
        phone *x = i == 0 ? a : i == 1 ? b : c;
        CHECK_EQ(q_pop(&x->c, GCS_PTG_SIDE, r, sizeof(r)), 8);
        CHECK(rd32(r) == ib && rd32(r + 4) == 0);
    }
    wr32(w, ic);
    wr32(w + 4, 1);
    game_send(a, GCS_PTG_SIDE, w, 8);
    CHECK_EQ(q_pop(&c->c, GCS_PTG_SIDE, r, sizeof(r)), 8);
    CHECK_EQ(rd32(r + 4), 1);
    q_pop(&a->c, GCS_PTG_SIDE, NULL, 0);
    q_pop(&b->c, GCS_PTG_SIDE, NULL, 0);

    /* the master removes c: everybody hears it, c too */
    wr32(w, ic);
    game_send(a, GCS_PTG_REMOVE, w, 4);
    CHECK_EQ(q_pop(&a->c, GCS_PTG_REMOVE, r, sizeof(r)), 4);
    CHECK_EQ(q_pop(&b->c, GCS_PTG_REMOVE, r, sizeof(r)), 4);
    CHECK_EQ(q_pop(&c->c, GCS_PTG_REMOVE, r, sizeof(r)), 4);
    CHECK_EQ(rd32(r), ic);
    send_msg(c, 0xa000, 14, GCSS_OP_LEAVE, r, 0);
    CHECK_EQ(a->c.qn[GCS_PTG_REMOVE], 0); /* told once */

    /* the start: to everybody, the master included; only the first word
     * of the master's request stays, the rest (garbage from its stack)
     * becomes 0: the campaign starts where its file says */
    fill(w, 12, 1);
    game_send(a, GCS_PTG_START, w, 12);
    CHECK_EQ(q_pop(&a->c, GCS_PTG_START, r, sizeof(r)), 12);
    CHECK_EQ(q_pop(&b->c, GCS_PTG_START, r, sizeof(r)), 12);
    CHECK(!memcmp(r, w, 4));
    CHECK(rd32(r + 4) == 0 && rd32(r + 8) == 0);

    /* mission start (6R72.APP 0x6dbc0): every phone sends 15 and waits
     * for 15 back; the server answers when all have sent theirs, with
     * the start time, and adds the empty synchronisation records (34 ->
     * the phone's own 35) and permanent damage data (23 -> 41) the
     * phones wait for next (0xd03e4) */
    gcs_snap_poll(S, g_now);
    wr32(w, 1);
    wr32(w + 4, 0x12345678u);
    game_send(a, GCS_PTG_READY, w, 8);
    CHECK_EQ(a->c.qn[GCS_PTG_READY], 0);
    game_send(b, GCS_PTG_READY, w, 8);
    for (i = 0; i < 2; i++) {
        phone *x = i == 0 ? a : b;
        CHECK_EQ(q_pop(&x->c, GCS_PTG_READY, r, sizeof(r)), 8);
        CHECK(rd32(r) == 1 && rd32(r + 4) == g_now + 500);
        CHECK_EQ(x->c.qn[GCS_PTG_SYNC], 0);
        CHECK_EQ(q_pop(&x->c, GCS_PTG_SYNC_END, NULL, 0), 0);
        CHECK_EQ(q_pop(&x->c, GCS_PTG_PERM, NULL, 0), 0);
        CHECK_EQ(q_pop(&x->c, GCS_PTG_PERM_END, NULL, 0), 0);
        send_acks(x);
    }
    /* the next barrier starts empty */
    game_send(b, GCS_PTG_READY, w, 8);
    CHECK_EQ(b->c.qn[GCS_PTG_READY], 0);

    /* clock ping (0xc3de8, 0xc3ecc): back to the sender with the server's
     * clock in words 1 and 2 */
    memset(gd, 0, 20);
    wr32(gd, 4242);
    wr32(gd + 12, 7);
    game_send(b, GCS_PTG_TIME, gd, 20);
    CHECK_EQ(q_pop(&b->c, GCS_PTG_TIME, r, sizeof(r)), 20);
    CHECK(rd32(r) == 4242 && rd32(r + 4) == g_now && rd32(r + 8) == g_now && rd32(r + 12) == 7);
    CHECK_EQ(a->c.qn[GCS_PTG_TIME], 0);

    /* a client leaving the room: the master hears it, and a barrier
     * that waited for it goes through */
    send_msg(b, 0xa000, 15, GCSS_OP_LEAVE, r, 0);
    CHECK_EQ(q_pop(&a->c, GCS_PTG_REMOVE, r, sizeof(r)), 4);
    CHECK_EQ(rd32(r), ib);
    game_send(a, GCS_PTG_READY, w, 8);
    CHECK_EQ(q_pop(&a->c, GCS_PTG_READY, r, sizeof(r)), 8);
    send_acks(a);
    for (i = 0; i < 3; i++) {
        phone *x = i == 0 ? a : i == 1 ? b : c;
        CHECK_EQ(x->c.bad, 0);
    }
    send_msg(a, 0xa000, 16, GCSS_OP_LEAVE, r, 0);
}

static void test_lobby(void)
{
    lobby_run(0);
}

static void test_lobby_classic(void)
{
    lobby_run(1);
}

int main(int argc, char **argv)
{
    gcs_snap_io io = { io_send, io_log, io_auth };
    int i;
    g_verbose = argc > 1 && !strcmp(argv[1], "-v");
    P[0].name = "alice";
    P[1].name = "bob";
    P[2].name = "carol";
    P[3].name = "dave";
    P[4].name = "erin";
    P[5].name = "frank";
    P[6].name = "gina";
    P[7].name = "hank";
    for (i = 0; i < 8; i++) {
        P[i].port = (gc_u16)(4001 + i);
        P[i].c.status = -1;
    }
    S = gcs_snap_create(&io, NULL);
    PT = gcs_ptg_create(io_log, NULL);
    /* Worms World Party first, as in gcs_native.c: same lobby names */
    gcs_snap_add_game(S, gcs_wwp_game());
    gcs_snap_add_game(S, gcs_ptg_game(PT));
    gcs_snap_add_game(S, gcs_ptg1_game(PT));
    RUN(test_game);
    RUN(test_last_opponent);
    RUN(test_classic);
    RUN(test_lobby);
    RUN(test_lobby_classic);
    gcs_snap_destroy(S);
    gcs_ptg_destroy(PT);
    for (i = 0; i < 14; i++) {
        client_reset(&P[i].c);
    }
    TEST_MAIN_END();
}
