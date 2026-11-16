/** @file test_pk.c
 *
 *  Pocket Kingdom game server (gcs_pk.c) against simulated games.  The
 *  answers are decoded the way 6RAH.APP does it (docs/ARENA.md, "Pocket
 *  Kingdom game server"): enemy search, scout and attack relay, chat
 *  (also to offline players), the Global Auction with the game's own
 *  loot bookkeeping (loot must neither appear nor vanish), limits,
 *  persistence, and one run through the SNAP game room.
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#include "gc_test.h"
#include "gcs_pk.h"
#include "gcs_snap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DATA_FILE "test-pk.dat"
#define T0        1800000000L

static int g_verbose;

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

static void io_log(void *ctx, int level, const char *line)
{
    (void)ctx;
    if (g_verbose || level <= 0) {
        printf("  [%d] %s\n", level, line);
    }
}

/* ---- simulated games (module level) ---- */

#define MAXQ 32

typedef struct
{
    gc_u8 d[300];
    size_t n;
} pkt;

typedef struct
{
    gc_u32 uid;
    const char *name;
    pkt q[MAXQ];
    int nq;
    gc_u8 seq;
    long loot; /* the game's own loot */
} player;

static player P[4];
static gcs_pk *K;
static const gcs_snap_game *G;

static player *by_uid(gc_u32 uid)
{
    int i;
    for (i = 0; i < 4; i++) {
        if (P[i].uid == uid) {
            return &P[i];
        }
    }
    return NULL;
}

static void out(void *ctx, gc_u32 uid, const gc_u8 *d, size_t n)
{
    player *pl = by_uid(uid);
    (void)ctx;
    CHECK(pl != NULL);
    CHECK(n <= sizeof(pl->q[0].d));
    if (pl && pl->nq < MAXQ && n <= sizeof(pl->q[0].d)) {
        memcpy(pl->q[pl->nq].d, d, n);
        pl->q[pl->nq].n = n;
        pl->nq++;
    }
}

static pkt *take(player *pl, gc_u8 type)
{
    static pkt r;
    int i;
    for (i = 0; i < pl->nq; i++) {
        if (pl->q[i].d[0] == type) {
            r = pl->q[i];
            memmove(&pl->q[i], &pl->q[i + 1], sizeof(pkt) * (size_t)(pl->nq - i - 1));
            pl->nq--;
            return &r;
        }
    }
    return NULL;
}

static void name16(gc_u8 *f, const char *s)
{
    memset(f, 0, 16);
    strncpy((char *)f, s, 15);
}

static void send(player *pl, const gc_u8 *d, size_t n)
{
    G->data(G->ctx, NULL, pl->uid, d, n);
}

static void setup(void)
{
    static const char *names[4] = { "Alice", "Bob", "Carol", "Dave" };
    int i;
    remove(DATA_FILE);
    K = gcs_pk_create(DATA_FILE, io_log, NULL);
    CHECK(K != NULL);
    gcs_pk_set_output(K, out, NULL);
    gcs_pk_set_time(K, T0);
    G = gcs_pk_game(K);
    memset(P, 0, sizeof(P));
    for (i = 0; i < 4; i++) {
        P[i].uid = 0x100 + (gc_u32)i;
        P[i].name = names[i];
        P[i].loot = 100000;
        G->joined(G->ctx, NULL, P[i].uid, P[i].name);
    }
}

static void teardown(void)
{
    gcs_pk_destroy(K);
    K = NULL;
    remove(DATA_FILE);
}

/* message 1 as 0x65ce0 builds it */
static void reg(player *pl, int area, gc_u8 look)
{
    gc_u8 m[24];
    pkt *r;
    memset(m, 0, sizeof(m));
    m[0] = GCSP_REGISTER;
    m[1] = pl->seq++;
    m[2] = (gc_u8)area;
    m[3] = look;
    m[4] = (gc_u8)(look + 1);
    m[5] = (gc_u8)(look + 2);
    m[6] = (gc_u8)(look + 3);
    name16(m + 8, pl->name);
    send(pl, m, sizeof(m));
    r = take(pl, 129);
    CHECK(r != NULL);
    CHECK(r && r->d[1] == m[1]);
}

/* message 6 and the 134 answer as 0x59f18 reads it; returns the count */
static int search(player *pl, int area, char found[5][16])
{
    gc_u8 m[20];
    pkt *r;
    int i, n = 0;
    memset(m, 0, sizeof(m));
    m[0] = GCSP_SEARCH;
    m[1] = pl->seq++;
    m[2] = (gc_u8)area;
    name16(m + 4, pl->name);
    send(pl, m, sizeof(m));
    r = take(pl, 134);
    CHECK(r && r->n == 104 && r->d[1] == m[1]);
    if (!r) {
        return 0;
    }
    for (i = 0; i < 5; i++) {
        const gc_u8 *e = r->d + 4 + 20 * i;
        if (!e[0]) {
            break;
        }
        CHECK(memchr(e, 0, 16) != NULL);
        strcpy(found[n++], (const char *)e);
    }
    return n;
}

static void test_kingdoms(void)
{
    char f[5][16];
    gc_u8 m[20];
    int n, i;
    setup();
    reg(&P[0], 3, 10);
    reg(&P[1], 3, 20);
    reg(&P[2], 4, 30);
    /* Dave online but never registered: not listed */
    n = search(&P[0], 3, f);
    CHECK_EQ(n, 1);
    CHECK(n == 1 && !strcmp(f[0], "Bob"));
    n = search(&P[2], 4, f);
    CHECK_EQ(n, 0);
    /* Carol moves to area 3 (message 5) */
    memset(m, 0, sizeof(m));
    m[0] = GCSP_MOVE;
    m[2] = 3;
    name16(m + 4, "Carol");
    send(&P[2], m, sizeof(m));
    n = search(&P[0], 3, f);
    CHECK_EQ(n, 2);
    for (i = 0; i < n; i++) {
        CHECK(!strcmp(f[i], "Bob") || !strcmp(f[i], "Carol"));
    }
    /* look bytes of the entry: Bob registered 20..23 */
    {
        pkt *r;
        memset(m, 0, sizeof(m));
        m[0] = GCSP_SEARCH;
        m[2] = 3;
        name16(m + 4, "Carol");
        send(&P[2], m, sizeof(m));
        r = take(&P[2], 134);
        CHECK(r != NULL);
        for (i = 0; r && i < 5; i++) {
            if (!strcmp((const char *)r->d + 4 + 20 * i, "Bob")) {
                CHECK(!memcmp(r->d + 4 + 20 * i + 16, "\x14\x15\x16\x17", 4));
            }
        }
    }
    /* Bob leaves the room: no longer found */
    G->left(G->ctx, NULL, P[1].uid);
    n = search(&P[0], 3, f);
    CHECK_EQ(n, 1);
    /* keep alive */
    memset(m, 0, sizeof(m));
    m[0] = GCSP_PING;
    m[1] = 77;
    name16(m + 4, "Alice");
    send(&P[0], m, sizeof(m));
    {
        pkt *r = take(&P[0], 130);
        CHECK(r && r->d[1] == 77);
    }
    teardown();
}

static void test_relay(void)
{
    gc_u8 m[116];
    pkt *r;
    setup();
    reg(&P[0], 1, 0);
    reg(&P[1], 1, 0);
    reg(&P[2], 2, 0);

    /* scout (7): Alice -> Bob, Bob's game answers with 135 */
    memset(m, 0, sizeof(m));
    m[0] = GCSP_SCOUT;
    m[1] = 9;
    name16(m + 4, "Mallory"); /* a forged sender is replaced */
    name16(m + 20, "bob");    /* names compare without case */
    send(&P[0], m, 36);
    r = take(&P[1], GCSP_SCOUT);
    CHECK(r && r->n == 36 && !strcmp((const char *)r->d + 4, "Alice") && r->d[1] == 9);
    memset(m, 0, sizeof(m));
    m[0] = 135;
    m[1] = 9;
    name16(m + 4, "Alice");
    name16(m + 20, "Bob");
    m[40] = 0x42; /* party data */
    send(&P[1], m, 52);
    r = take(&P[0], 135);
    CHECK(r && r->n == 52 && r->d[1] == 9 && r->d[40] == 0x42 && !strcmp((const char *)r->d + 20, "Bob"));

    /* attack (9) on Carol in another area: answered by the server */
    memset(m, 0, sizeof(m));
    m[0] = GCSP_ATTACK;
    m[1] = 10;
    name16(m + 4, "Alice");
    name16(m + 20, "Carol");
    send(&P[0], m, 88);
    CHECK(take(&P[2], GCSP_ATTACK) == NULL);
    r = take(&P[0], 137);
    CHECK(r && r->n == 88 && r->d[1] == 10 && r->d[2] == GCSP_MOVED_AREA);
    CHECK(r && !strcmp((const char *)r->d + 8, "Alice") && !strcmp((const char *)r->d + 24, "Carol"));

    /* scout of somebody offline */
    memset(m, 0, sizeof(m));
    m[0] = GCSP_SCOUT;
    m[1] = 11;
    name16(m + 20, "Nobody");
    send(&P[0], m, 36);
    r = take(&P[0], 135);
    CHECK(r && r->d[1] == 11 && r->d[2] > 252);

    /* attack on Bob: 9 -> Bob, 137 -> Alice, 10 -> Bob */
    memset(m, 0, sizeof(m));
    m[0] = GCSP_ATTACK;
    m[1] = 12;
    name16(m + 20, "Bob");
    send(&P[0], m, 88);
    CHECK(take(&P[1], GCSP_ATTACK) != NULL);
    memset(m, 0, sizeof(m));
    m[0] = 137;
    m[1] = 12;
    name16(m + 8, "Alice");
    name16(m + 24, "Bob");
    send(&P[1], m, 88);
    r = take(&P[0], 137);
    CHECK(r && r->d[1] == 12 && r->d[2] == 0);
    memset(m, 0, sizeof(m));
    m[0] = GCSP_ENGAGE;
    m[1] = 13;
    name16(m + 4, "Alice");
    name16(m + 20, "Bob");
    send(&P[0], m, 36);
    CHECK(take(&P[1], GCSP_ENGAGE) != NULL);

    /* chat (18): online, then to an offline kingdom, then unknown */
    memset(m, 0, sizeof(m));
    m[0] = GCSP_CHAT;
    m[1] = 20;
    name16(m + 4, "Alice");
    name16(m + 20, "Bob");
    strcpy((char *)m + 36, "Hello Bob");
    send(&P[0], m, 116);
    r = take(&P[1], GCSP_CHAT);
    CHECK(r && r->n == 116 && !strcmp((const char *)r->d + 36, "Hello Bob"));
    r = take(&P[0], 146);
    CHECK(r && r->d[1] == 20 && r->d[2] == 1);

    G->left(G->ctx, NULL, P[2].uid);
    m[1] = 21;
    name16(m + 20, "Carol");
    strcpy((char *)m + 36, "See you");
    send(&P[0], m, 116);
    r = take(&P[0], 146);
    CHECK(r && r->d[1] == 21 && r->d[2] == 1);
    m[1] = 22;
    name16(m + 20, "Zed");
    send(&P[0], m, 116);
    r = take(&P[0], 146);
    CHECK(r && r->d[1] == 22 && r->d[2] == 0);
    {
        gcs_pk_stats st;
        gcs_pk_get_stats(K, &st);
        CHECK_EQ(st.mail, 1);
    }
    /* Carol comes back: the message arrives after her registration */
    G->joined(G->ctx, NULL, P[2].uid, "Carol");
    reg(&P[2], 2, 0);
    r = take(&P[2], GCSP_CHAT);
    CHECK(r && !strcmp((const char *)r->d + 36, "See you") && !strcmp((const char *)r->d + 4, "Alice"));
    teardown();
}

/* ---- auction ---- */

static gc_u32 exhibit(player *pl, gc_u8 cat, gc_u8 idx, gc_u32 price, int *status)
{
    gc_u8 m[32];
    pkt *r;
    memset(m, 0, sizeof(m));
    m[0] = GCSP_EXHIBIT;
    m[1] = pl->seq++;
    name16(m + 4, pl->name);
    m[20] = cat;
    m[21] = 0xff;
    m[22] = idx;
    m[23] = 7;
    m[25] = 2;
    put32(m + 28, price);
    send(pl, m, sizeof(m));
    r = take(pl, 141);
    CHECK(r && r->n == 8 && r->d[1] == m[1]);
    *status = r ? r->d[3] : -1;
    return r ? be32(r->d + 4) : 0;
}

/* bid as 0x2b9c0 does it: pay amount minus the refund in the answer */
static int bid(player *pl, gc_u32 id, gc_u32 amount)
{
    gc_u8 m[28];
    pkt *r;
    memset(m, 0, sizeof(m));
    m[0] = GCSP_BID;
    m[1] = pl->seq++;
    put32(m + 4, id);
    put32(m + 8, amount);
    name16(m + 12, pl->name);
    send(pl, m, sizeof(m));
    r = take(pl, 143);
    CHECK(r && r->n == 8 && r->d[1] == m[1]);
    if (!r) {
        return -1;
    }
    if (r->d[2] == 0) {
        pl->loot -= (long)amount - (long)be32(r->d + 4);
    }
    return r->d[2];
}

/* message 16 and the 144 answer; books results as 0x2d6b4 / 0x2dc5c do
 * in the collecting modes */
static int mylist(player *pl, int mode, gc_u8 st[10], gc_u32 ids[10], int *items_back, int *items_won)
{
    gc_u8 m[20];
    pkt *r;
    int i, n = 0;
    memset(m, 0, sizeof(m));
    m[0] = GCSP_MYLIST;
    m[1] = pl->seq++;
    m[3] = (gc_u8)mode;
    name16(m + 4, pl->name);
    send(pl, m, sizeof(m));
    r = take(pl, 144);
    CHECK(r && r->n == 244 && r->d[1] == m[1]);
    if (!r) {
        return 0;
    }
    for (i = 0; i < 10; i++) {
        const gc_u8 *e = r->d + 4 + 24 * i;
        if (!e[4]) {
            break;
        }
        st[n] = e[4];
        ids[n] = be32(e);
        n++;
        if (mode == 2 && e[4] == 3) {
            pl->loot += be32(e + 16);
        }
        if (mode == 2 && e[4] == 4) {
            (*items_back)++;
            CHECK_EQ(e[5], 2); /* the item bytes come back */
            CHECK_EQ(e[7], 3);
        }
        if (mode == 3 && e[4] == 3) {
            (*items_won)++;
        }
        if (mode == 3 && e[4] == 4) {
            pl->loot += be32(e + 20);
        }
    }
    return n;
}

static void test_auction(void)
{
    int s, n, back = 0, won = 0, k;
    gc_u32 a1, a2, ids[10];
    gc_u8 st[10];
    long total;
    setup();
    total = P[0].loot + P[1].loot + P[2].loot;

    a1 = exhibit(&P[0], 5, 9, 1000, &s);
    CHECK_EQ(s, 0);
    CHECK(a1 != 0);
    a2 = exhibit(&P[0], 2, 3, 500, &s);
    CHECK_EQ(s, 0);
    exhibit(&P[0], 2, 3, 0, &s);
    CHECK_EQ(s, 2); /* refused */

    /* browse (14) as 0x2c5a0 reads it: total at 4, entries at 8 */
    {
        gc_u8 m[32];
        pkt *r;
        memset(m, 0, sizeof(m));
        m[0] = GCSP_BROWSE;
        m[1] = 5;
        m[2] = 0xff;
        m[3] = m[4] = m[5] = m[6] = m[7] = m[8] = 0xff;
        name16(m + 16, "Bob");
        send(&P[1], m, sizeof(m));
        r = take(&P[1], 142);
        CHECK(r && r->n == 248 && be32(r->d + 4) == 2);
        if (r) {
            const gc_u8 *e = r->d + 8;
            CHECK_EQ(be32(e), a1); /* both end together: lower id first */
            CHECK_EQ(e[4], 1);
            CHECK_EQ(e[5], 5);
            CHECK_EQ(e[6], 0xff);
            CHECK_EQ(e[7], 9);
            CHECK_EQ(e[8], 7);
            CHECK_EQ(be32(e + 12), GCSP_AUCTION_SECS);
            CHECK_EQ(be32(e + 16), 1000);
            CHECK_EQ(r->d[8 + 48 + 4], 0); /* end of list */
        }
        m[2] = 2; /* category 2 only */
        send(&P[1], m, sizeof(m));
        r = take(&P[1], 142);
        CHECK(r && be32(r->d + 4) == 1 && be32(r->d + 8) == a2);
    }

    CHECK_EQ(bid(&P[0], a1, 2000), 4); /* own exhibit */
    CHECK_EQ(bid(&P[1], a1, 999), 1);  /* below the starting price */
    CHECK_EQ(bid(&P[1], a1, 1000), 0);
    CHECK_EQ(bid(&P[2], a1, 1000), 1); /* not higher */
    CHECK_EQ(bid(&P[2], a1, 1500), 0);
    CHECK_EQ(bid(&P[1], a1, 2000), 0); /* Bob raises: pays the difference */
    CHECK_EQ(P[1].loot, 100000 - 2000);
    CHECK_EQ(P[2].loot, 100000 - 1500);
    CHECK_EQ(bid(&P[1], 4711, 2000), 2); /* no such auction */

    /* peek (mode 1): Bob leads, Carol is outbid */
    n = mylist(&P[1], 1, st, ids, &back, &won);
    CHECK(n == 1 && st[0] == 2 && ids[0] == a1);
    n = mylist(&P[2], 1, st, ids, &back, &won);
    CHECK(n == 1 && st[0] == 1);
    n = mylist(&P[0], 0, st, ids, &back, &won);
    CHECK_EQ(n, 2);
    for (k = 0; k < n; k++) {
        CHECK_EQ(st[k], ids[k] == a1 ? 2 : 1);
    }
    /* collecting before the end changes nothing */
    n = mylist(&P[2], 3, st, ids, &back, &won);
    CHECK(n == 1 && st[0] == 1);

    /* the auctions end */
    gcs_pk_set_time(K, T0 + GCSP_AUCTION_SECS + 1);
    CHECK_EQ(bid(&P[2], a1, 3000), 2);          /* too late */
    n = mylist(&P[0], 2, st, ids, &back, &won); /* seller collects */
    CHECK_EQ(n, 2);
    CHECK_EQ(back, 1);
    CHECK_EQ(P[0].loot, 100000 + 2000);
    n = mylist(&P[0], 2, st, ids, &back, &won);
    CHECK_EQ(n, 0); /* collected once */
    n = mylist(&P[1], 3, st, ids, &back, &won);
    CHECK(n == 1 && st[0] == 3);
    CHECK_EQ(won, 1);
    n = mylist(&P[2], 3, st, ids, &back, &won);
    CHECK(n == 1 && st[0] == 4);
    CHECK_EQ(P[2].loot, 100000); /* refunded */
    n = mylist(&P[2], 3, st, ids, &back, &won);
    CHECK_EQ(n, 0);
    CHECK_EQ(P[0].loot + P[1].loot + P[2].loot, total);
    {
        gcs_pk_stats st2;
        gcs_pk_get_stats(K, &st2);
        CHECK_EQ(st2.auctions, 0); /* everything collected */
    }
    teardown();
}

static void test_limits_and_persistence(void)
{
    int s, i;
    gc_u32 id = 0, first = 0;
    setup();
    reg(&P[0], 6, 1);
    for (i = 0; i < GCSP_MAX_EXHIBITS; i++) {
        id = exhibit(&P[1], 1, (gc_u8)i, 100, &s);
        CHECK_EQ(s, 0);
        if (!first) {
            first = id;
        }
    }
    exhibit(&P[1], 1, 99, 100, &s);
    CHECK_EQ(s, 1); /* too many exhibits */
    for (i = 0; i < GCSP_MAX_BIDS; i++) {
        CHECK_EQ(bid(&P[0], first + (gc_u32)i, 200), 0);
    }
    id = exhibit(&P[2], 1, 1, 100, &s);
    CHECK_EQ(bid(&P[0], id, 200), 3);    /* too many bids */
    CHECK_EQ(bid(&P[0], first, 300), 0); /* raising an old bid is fine */

    /* reload */
    gcs_pk_destroy(K);
    K = gcs_pk_create(DATA_FILE, io_log, NULL);
    gcs_pk_set_output(K, out, NULL);
    gcs_pk_set_time(K, T0);
    G = gcs_pk_game(K);
    for (i = 0; i < 4; i++) {
        G->joined(G->ctx, NULL, P[i].uid, P[i].name);
    }
    {
        gcs_pk_stats st;
        gcs_pk_get_stats(K, &st);
        CHECK_EQ(st.auctions, GCSP_MAX_EXHIBITS + 1);
        CHECK_EQ(st.kingdoms, 1);
    }
    CHECK_EQ(bid(&P[0], first, 300), 1); /* the price was kept */
    CHECK_EQ(bid(&P[3], first, 301), 0);
    CHECK_EQ(bid(&P[0], id, 200), 3); /* the bids were kept */
    exhibit(&P[2], 1, 1, 100, &s);
    CHECK_EQ(s, 0);
    {
        char f[5][16];
        reg(&P[3], 6, 1);
        CHECK_EQ(search(&P[3], 6, f), 1); /* Alice's kingdom known again */
    }
    teardown();
}

static void test_fuzz(void)
{
    int i;
    gc_u8 m[300];
    setup();
    for (i = 0; i < 20000; i++) {
        size_t n = gct_rand() % sizeof(m), k;
        for (k = 0; k < n; k++) {
            m[k] = (gc_u8)gct_rand();
        }
        if (n > 0 && (gct_rand() & 1)) {
            static const gc_u8 types[] = { 1, 2, 3, 5, 6, 7, 9, 10, 13, 14, 15, 16, 17, 18, 21, 22, 135, 137 };
            m[0] = types[gct_rand() % sizeof(types)];
        }
        G->data(G->ctx, NULL, P[gct_rand() % 4].uid, m, n);
        if ((i & 255) == 0) {
            int j;
            for (j = 0; j < 4; j++) {
                P[j].nq = 0;
            }
            gcs_pk_set_time(K, T0 + i * 10L);
        }
    }
    CHECK(1);
    teardown();
}

/* ---- through SNAP ---- */

static const gc_u8 SRV[4] = { 10, 64, 0, 1 };
static gcs_snap *S;
static struct
{
    int key;
    gc_u32 uid, tx;
    gc_u8 got[64][300];
    size_t gotn[64];
    int ngot;
    int okops[64];
    gc_u32 okval[64];
    int nok;
    gc_u32 lobby;
    gc_u8 rooms[12 + 20 * 36];
    size_t nrooms;
    gc_u8 sel[12 + 20 * 40];
    size_t nsel;
    gc_u8 lob[12 + 20 * 36];
    size_t nlob;
    gc_u8 attr[24];
    size_t nattr;
} N;

static void snap_send(void *ctx, void *key, const gc_u8 addr[4], gc_u16 port, gc_u16 peer_port, const gc_u8 *d,
                      size_t n)
{
    size_t pos = 0;
    (void)ctx;
    (void)key;
    (void)addr;
    (void)port;
    (void)peer_port;
    while (pos + 16 <= n - 4) {
        gc_u16 fl = (gc_u16)((d[pos] << 8) | d[pos + 1]);
        size_t len = fl & 0x3ff;
        if (len < 16 || pos + len > n - 4) {
            break;
        }
        if (fl & 0x8000) { /* ack it */
            gc_u8 a[20];
            memset(a, 0, sizeof(a));
            a[0] = 0x60;
            a[1] = 0x10;
            put32(a + 4, N.uid);
            memcpy(a + 12, d + pos + 8, 4);
            put32(a + 16, 0xBA476611u);
            gcs_snap_input(S, &N.key, SRV, GCSS_PORT, 5000, a, 20, 1000);
        }
        if (d[pos + 3] == GCSS_OP_SELECT_ALL && len - 16 <= sizeof(N.sel)) {
            memcpy(N.sel, d + pos + 16, len - 16);
            N.nsel = len - 16;
        }
        if (d[pos + 3] == GCSS_OP_SELECT_LOBBIES && len - 16 <= sizeof(N.lob)) {
            memcpy(N.lob, d + pos + 16, len - 16);
            N.nlob = len - 16;
        }
        if (d[pos + 3] == GCSS_OP_ATTR2 && len - 16 <= sizeof(N.attr)) {
            memcpy(N.attr, d + pos + 16, len - 16);
            N.nattr = len - 16;
        }
        if (d[pos + 3] == GCSS_OP_ROOMS && len - 16 <= sizeof(N.rooms)) {
            memcpy(N.rooms, d + pos + 16, len - 16);
            N.nrooms = len - 16;
        }
        if (d[pos + 3] == GCSS_OP_LOBBY_INFO && len >= 36) {
            N.lobby = be32(d + pos + 32);
        }
        if (d[pos + 3] == GCSS_OP_LOGIN_OK && len >= 28) {
            N.uid = be32(d + pos + 24);
        } else if (d[pos + 3] == GCSS_OP_DATA && N.ngot < 64) {
            memcpy(N.got[N.ngot], d + pos + 16, len - 16);
            N.gotn[N.ngot++] = len - 16;
        } else if ((d[pos + 3] == GCSS_OP_OK || d[pos + 3] == GCSS_OP_CREATE_ROOM) && N.nok < 64 && len >= 24) {
            N.okops[N.nok] = d[pos + 3];
            N.okval[N.nok++] = be32(d + pos + 16);
        }
        pos += len;
    }
}

static int snap_auth(void *ctx, void *key, const char *user, char canon[GCSS_MAX_NAME + 1])
{
    (void)ctx;
    (void)key;
    strcpy(canon, user);
    return 1;
}

static void snap_msg(gc_u16 flags, gc_u8 seq, gc_u8 op, const gc_u8 *p, size_t n)
{
    gc_u8 d[400];
    size_t len = 16 + n;
    flags |= 0x2000;
    d[0] = (gc_u8)((flags | len) >> 8);
    d[1] = (gc_u8)(flags | len);
    d[2] = seq;
    d[3] = op;
    put32(d + 4, N.uid);
    put32(d + 8, (flags & 0x8000) ? N.tx++ : 0);
    put32(d + 12, 0);
    memcpy(d + 16, p, n);
    put32(d + len, 0xBA476611u);
    gcs_snap_input(S, &N.key, SRV, GCSS_PORT, 5000, d, len + 4, 1000);
}

static void test_snap(void)
{
    gcs_snap_io io;
    gcs_pk *pk;
    gc_u8 p[132];
    int i, k, found = 0;
    memset(&io, 0, sizeof(io));
    memset(&N, 0, sizeof(N));
    io.send = snap_send;
    io.log = io_log;
    io.auth = snap_auth;
    S = gcs_snap_create(&io, NULL);
    pk = gcs_pk_create(NULL, io_log, NULL);
    gcs_snap_add_game(S, gcs_pk_game(pk));

    memset(p, 0, sizeof(p));
    strcpy((char *)p, "Alice");
    strcpy((char *)p + 40, "pk");
    put32(p + 100, GCS_PK_GAME_CLASS);
    snap_msg(0x1000, 0, GCSS_OP_LOGIN, p, sizeof(p));
    CHECK(N.uid != 0);
    CHECK_EQ(gcs_snap_game_class_of(S, "Alice"), GCS_PK_GAME_CLASS);

    /* game data before entering a room reaches the game server, too */
    memset(p, 0, sizeof(p));
    p[0] = GCSP_PING;
    p[1] = 32;
    snap_msg(0x8000, 0, GCSS_OP_TO_SERVER, p, 20);
    CHECK(N.ngot == 1 && N.got[0][0] == 130 && N.got[0][1] == 32);

    /* lobby "Scrimmage" (game.rsc), then the room the game enters for
     * the Global Auction */
    memset(p, 0, sizeof(p));
    strcpy((char *)p, "Scrimmage");
    snap_msg(0x9000, 1, GCSS_OP_LOBBY_INFO, p, 16);
    CHECK(N.lobby != 0);
    put32(p, N.lobby);
    snap_msg(0x9000, 2, GCSS_OP_JOIN, p, 4);

    /* the Play Now screens look for the room "Scrimmage" (RoomToJoin)
     * in the room list and go back to the mode selection without it; the
     * game itself never creates it */
    for (k = 0; k < 2; k++) {
        gc_u32 scrim = 0;
        N.nrooms = 0;
        put32(p, N.lobby);
        snap_msg(0x9000, (gc_u8)(4 + k), GCSS_OP_ROOMS, p, 4);
        CHECK(N.nrooms >= 12);
        if (N.nrooms >= 12) {
            gc_u32 j, cnt = be32(N.rooms + 8);
            int auction = 0;
            CHECK_EQ(N.nrooms, 12 + 36 * cnt);
            for (j = 0; j < cnt && 12 + 36 * (j + 1) <= N.nrooms; j++) {
                const gc_u8 *e = N.rooms + 12 + 36 * j;
                if (!strcmp((const char *)e, "Scrimmage")) {
                    /* as ArenaFramework 2.01.047 reads it: users at 16,
                     * status at 24, max at 28, id at 32; IsFull is
                     * users >= max */
                    scrim = be32(e + 32);
                    CHECK(be32(e + 16) < be32(e + 28));
                    CHECK(be32(e + 28) > 2); /* room for many players */
                    CHECK_EQ(be32(e + 24) & 2, 0);
                }
                auction |= !strcmp((const char *)e, "Auction");
            }
            CHECK(scrim != 0);
            CHECK(!auction); /* in its own lobby "Auction" */
        }
        if (k == 0) {
            /* enter it, leave it again: it stays */
            put32(p, scrim);
            N.nok = 0;
            snap_msg(0x8000, 6, GCSS_OP_JOIN, p, 4);
            found = 0;
            for (i = 0; i < N.nok; i++) {
                found |= N.okops[i] == GCSS_OP_OK && N.okval[i] == GCSS_OP_JOIN;
            }
            CHECK(found);
            {
                gcs_pk_stats st;
                gcs_pk_get_stats(pk, &st);
                CHECK_EQ(st.online, 1);
            }
            snap_msg(0x8000, 7, GCSS_OP_LEAVE, p, 4);
        }
    }

    /* a room created outside any lobby (matchmaking "in any lobby") goes
     * into the game's first lobby */
    snap_msg(0x9000, 8, GCSS_OP_LEAVE, p, 4); /* leave the lobby */
    {
        static const gc_u8 crit[] = { 0, 0, 0, 0, 2, 0, 0, 0, 'S', 'T', 'A', 'T', 0x21, 0x30, 0xff, 0xff, 0xf0,
                                      0x30, 0, 0, 0, 'U', 'S', 'E', 'R', 6, 0, 0, 0x7f, 0xff };
        N.nsel = 0;
        snap_msg(0x9000, 9, GCSS_OP_SELECT_ALL, crit, sizeof(crit));
        CHECK(N.nsel == 12 + 40 && be32(N.sel + 4) == 1); /* the fixed room "Ranked" */
        memset(p, 0, sizeof(p));
        strcpy((char *)p, "R mupf");
        put32(p + 16, 2);
        N.nok = 0;
        snap_msg(0x9000, 10, GCSS_OP_CREATE_ROOM, p, 44);
        found = 0;
        for (i = 0; i < N.nok; i++) {
            found |= N.okops[i] == GCSS_OP_OK && N.okval[i] == GCSS_OP_CREATE_ROOM;
        }
        CHECK(found);
        N.nsel = 0;
        {
            gc_u8 crit2[32];
            memset(crit2, 0, sizeof(crit2));
            crit2[4] = 1;
            memcpy(crit2 + 8, "NAME", 4);
            crit2[12] = 0x41;
            strcpy((char *)crit2 + 13, "R mupf");
            snap_msg(0x9000, 11, GCSS_OP_SELECT_ALL, crit2, 29);
        }
        CHECK(N.nsel == 12 + 40 && be32(N.sel + 8) == 1);
        if (N.nsel == 12 + 40) {
            const gc_u8 *e = N.sel + 12;
            CHECK(!strcmp((const char *)e, "R mupf"));
            CHECK_EQ(be32(e + 16), N.lobby); /* created in the game's lobby */
            CHECK_EQ(be32(e + 20), 1);       /* users */
            CHECK_EQ(be32(e + 32), 2);       /* max */
            CHECK(be32(e + 36) != 0);        /* id */
        }
    }

    /* Ranked matchmaking (no room creation): the fixed room "Ranked" fits
     * the game's criteria, Scrimmage and Auction do not */
    {
        static const gc_u8 rk[] = { 0, 0, 0, 0, 6, 0, 0, 0, 'S', 'T', 'A', 'T', 0x21, 0x30, 0xff, 0xff,
                                    0xf0, 0x30, 0, 0, 0, 'U', 'S', 'E', 'R', 6, 0, 0, 0x7f, 0xff,
                                    'A', 'R', 'N', 'A', 1, 0, 0, 0, 0, 'A', 'R', 'N', 'B', 1,
                                    0, 0, 0, 0, 'A', 'R', 'N', 'C', 1, 0, 0, 0, 0, 'A', 'R', 'N',
                                    'D', 1, 0, 0, 0, 0 };
        CHECK_EQ(sizeof(rk), 66); /* as on hardware */
        N.nsel = 0;
        snap_msg(0x9000, 14, GCSS_OP_SELECT_ALL, rk, sizeof(rk));
        CHECK(N.nsel >= 12);
        if (N.nsel >= 12) {
            gc_u32 j, cnt = be32(N.sel + 8);
            int ranked = 0;
            for (j = 0; j < cnt && 12 + 40 * (j + 1) <= N.nsel; j++) {
                const gc_u8 *e = N.sel + 12 + 40 * j;
                CHECK(strcmp((const char *)e, "Scrimmage") != 0 && strcmp((const char *)e, "Auction") != 0);
                if (!strcmp((const char *)e, "Ranked")) {
                    ranked = 1;
                    CHECK_EQ(be32(e + 28) & 0x30000000u, 0x30000000u);
                    CHECK(be32(e + 20) < be32(e + 32));
                }
            }
            CHECK(ranked);
        }
    }

    /* the Global Auction: CArenaFramework slot 0x6c reads the NAME of the
     * current lobby (op 9), looks for the lobby "Auction" by a lobby
     * search (op 72, exactly one entry, its id at +32), then for the room
     * "Auction" in it (op 73: NAME, LOID); the fixed room is found by its
     * name */
    {
        gc_u8 crit[64];
        gc_u32 alobby = 0, slobby;
        memset(p, 0, sizeof(p));
        strcpy((char *)p, "Scrimmage");
        N.lobby = 0;
        snap_msg(0x9000, 12, GCSS_OP_LOBBY_INFO, p, 16);
        slobby = N.lobby;
        CHECK(slobby != 0);
        put32(crit, slobby);
        memcpy(crit + 4, "NAME", 4);
        N.nattr = 0;
        snap_msg(0xb000, 0, GCSS_OP_ATTR2, crit, 8);
        CHECK(N.nattr >= 8 + 10);
        if (N.nattr >= 8 + 10) {
            CHECK_EQ(be32(N.attr), slobby);
            CHECK(!memcmp(N.attr + 4, "NAME", 4));
            CHECK(!strcmp((const char *)N.attr + 8, "Scrimmage"));
        }
        memset(crit, 0, sizeof(crit));
        crit[4] = 1;
        memcpy(crit + 8, "NAME", 4);
        crit[12] = 0x41;
        strcpy((char *)crit + 13, "Auction");
        N.nlob = 0;
        snap_msg(0xb000, 0, GCSS_OP_SELECT_LOBBIES, crit, 29);
        CHECK(N.nlob == 12 + 36 && be32(N.lob + 4) == 1 && be32(N.lob + 8) == 1);
        if (N.nlob == 12 + 36) {
            CHECK(!strcmp((const char *)N.lob + 12, "Auction"));
            alobby = be32(N.lob + 12 + 32);
        }
        CHECK(alobby != 0 && alobby != slobby);
        memset(p, 0, sizeof(p));
        strcpy((char *)p, "Auction");
        N.lobby = 0;
        snap_msg(0x9000, 12, GCSS_OP_LOBBY_INFO, p, 16);
        CHECK_EQ(N.lobby, alobby);
        memset(crit, 0, sizeof(crit));
        crit[4] = 2;
        memcpy(crit + 8, "NAME", 4);
        crit[12] = 0x41;
        strcpy((char *)crit + 13, "Auction");
        memcpy(crit + 29, "LOID", 4);
        crit[33] = 1;
        put32(crit + 34, alobby);
        N.nsel = 0;
        snap_msg(0x9000, 13, GCSS_OP_SELECT_ALL, crit, 38);
        CHECK(N.nsel == 12 + 40 && be32(N.sel + 8) == 1);
        if (N.nsel == 12 + 40) {
            CHECK(!strcmp((const char *)N.sel + 12, "Auction"));
            CHECK_EQ(be32(N.sel + 12 + 16), alobby);
        }
    }

    /* keep alive: 2 -> 130, through SendGameDataToServer */
    memset(p, 0, sizeof(p));
    p[0] = GCSP_PING;
    p[1] = 33;
    strcpy((char *)p + 4, "Alice");
    snap_msg(0x8000, 0, GCSS_OP_TO_SERVER, p, 20);
    found = 0;
    for (i = 0; i < N.ngot; i++) {
        found |= N.gotn[i] == 4 && N.got[i][0] == 130 && N.got[i][1] == 33;
    }
    CHECK(found);
    {
        gcs_pk_stats st;
        gcs_pk_get_stats(pk, &st);
        CHECK_EQ(st.online, 1);
    }
    gcs_snap_destroy(S);
    gcs_pk_destroy(pk);
}

int main(int argc, char **argv)
{
    g_verbose = argc > 1 && !strcmp(argv[1], "-v");
    RUN(test_kingdoms);
    RUN(test_relay);
    RUN(test_auction);
    RUN(test_limits_and_persistence);
    RUN(test_fuzz);
    RUN(test_snap);
    TEST_MAIN_END();
}
