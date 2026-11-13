/** @file test_rifts.c
 *
 *  SNAP game server (gcs_snap.c) and the Rifts: Promise of Power game
 *  server (gcs_rifts.c) against simulated games: SNAP login, lobby and
 *  room as ArenaFramework does it, then the Rifts actions of a whole
 *  battle between two players, flow control, persistence.
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#include "gc_test.h"
#include "gcs_rcodec.h"
#include "gcs_rifts.h"
#include "gcs_snap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DATA_FILE "test-rifts.dat"

static const unsigned char KEY[8] = { 0x2c, 0xde, 0xcb, 0x4a, 0xa5, 0x6a, 0x35, 0x07 };
static const gc_u8 SRV[4] = { 10, 64, 0, 1 };

static gc_u32 g_now = 1000;
static int g_verbose;

/* ---- simulated game (snapcomm.dll side) ---- */

#define MAXIN 64

typedef struct
{
    gc_u8 h[16];
    gc_u8 p[1024];
    size_t n;
} msg;

typedef struct
{
    int key; /* address of this is the bridge peer key */
    gc_u16 port;
    const char *name;
    gc_u32 uid;
    gc_u32 tx_rel, rx_rel_next;
    int auto_ack;
    msg in[MAXIN];
    int n_in;
    gc_u32 lobby, room;
    int dropped_datagrams;
} game;

static game G[2];

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

static gc_u32 le32(const unsigned char *p)
{
    return (gc_u32)p[0] | ((gc_u32)p[1] << 8) | ((gc_u32)p[2] << 16) | ((gc_u32)p[3] << 24);
}

static void st32(unsigned char *p, gc_u32 v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static gcs_snap *S;
static gcs_rifts *R;

static void send_msg(game *g, gc_u16 flags, gc_u8 seq, gc_u8 op, const gc_u8 *p, size_t n);

static void io_send(void *ctx, void *key, const gc_u8 addr[4], gc_u16 port, gc_u16 peer_port, const gc_u8 *d,
                    size_t n)
{
    game *g = (game *)key;
    size_t pos = 0;
    (void)ctx;
    CHECK(!memcmp(addr, SRV, 4));
    CHECK_EQ(port, GCSS_PORT);
    CHECK_EQ(peer_port, g->port);
    CHECK(n >= 20 && be32(d + n - 4) == 0xBA476611u);
    CHECK(n <= 896);
    if (g->dropped_datagrams > 0) {
        g->dropped_datagrams--;
        return;
    }
    while (pos + 16 <= n - 4) {
        gc_u16 fl = (gc_u16)((d[pos] << 8) | d[pos + 1]);
        size_t len = fl & 0x3ff;
        if (len < 16 || pos + len > n - 4) {
            CHECK(0);
            return;
        }
        if (fl & 0x8000) {
            gc_u32 seq = be32(d + pos + 8);
            if (g->auto_ack) {
                gc_u8 a[16];
                memset(a, 0, sizeof(a));
                a[0] = 0x60;
                a[1] = 0x10;
                put32(a + 4, g->uid);
                put32(a + 12, seq);
                {
                    gc_u8 dg[20];
                    memcpy(dg, a, 16);
                    put32(dg + 16, 0xBA476611u);
                    gcs_snap_input(S, &g->key, SRV, GCSS_PORT, g->port, dg, 20, g_now);
                }
            }
            if (seq < g->rx_rel_next) {
                pos += len;
                continue; /* duplicate */
            }
            CHECK_EQ(seq, g->rx_rel_next);
            g->rx_rel_next = seq + 1;
        }
        if (d[pos + 3] != 0 && g->n_in < MAXIN) {
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
    game *g = (game *)key;
    (void)ctx;
    if (strcmp(user, g->name)) {
        return 0;
    }
    strcpy(canon, user);
    return 1;
}

static void send_msg(game *g, gc_u16 flags, gc_u8 seq, gc_u8 op, const gc_u8 *p, size_t n)
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
    memcpy(d + 16, p, n);
    put32(d + len, 0xBA476611u);
    gcs_snap_input(S, &g->key, SRV, GCSS_PORT, g->port, d, len + 4, g_now);
}

/* a reliable message with sequence number 0 (not counted) */
static void send_msg0(game *g, gc_u16 flags, gc_u8 seq, gc_u8 op, const gc_u8 *p, size_t n)
{
    gc_u32 keep = g->tx_rel;
    g->tx_rel = 0;
    send_msg(g, flags, seq, op, p, n);
    g->tx_rel = keep;
}

/* Two reliable messages in one datagram, as snapcomm.dll packs them: the
 * first with F_MORE (0x0800) and the sequence number, the second with
 * sequence number 0. */
static void send_packed(game *g, gc_u8 seq1, gc_u8 op1, const gc_u8 *p1, size_t n1, gc_u16 flags2, gc_u8 seq2,
                        gc_u8 op2, const gc_u8 *p2, size_t n2)
{
    gc_u8 d[1100];
    size_t l1 = 16 + n1, l2 = 16 + n2;
    gc_u16 f1 = 0xa800, f2 = (gc_u16)(flags2 | 0x2000);
    d[0] = (gc_u8)((f1 | l1) >> 8);
    d[1] = (gc_u8)(f1 | l1);
    d[2] = seq1;
    d[3] = op1;
    put32(d + 4, g->uid);
    put32(d + 8, g->tx_rel++);
    put32(d + 12, 0);
    memcpy(d + 16, p1, n1);
    d[l1] = (gc_u8)((f2 | l2) >> 8);
    d[l1 + 1] = (gc_u8)(f2 | l2);
    d[l1 + 2] = seq2;
    d[l1 + 3] = op2;
    put32(d + l1 + 4, g->uid);
    put32(d + l1 + 8, 0);
    put32(d + l1 + 12, 0);
    memcpy(d + l1 + 16, p2, n2);
    put32(d + l1 + l2, 0xBA476611u);
    gcs_snap_input(S, &g->key, SRV, GCSS_PORT, g->port, d, l1 + l2 + 4, g_now);
}

/* first message with opcode op (and its index), or NULL */
static msg *take(game *g, gc_u8 op)
{
    int i;
    static msg m;
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

static void login(game *g)
{
    gc_u8 p[120 + 12];
    msg *m;
    memset(p, 0, sizeof(p));
    strcpy((char *)p, g->name);
    strcpy((char *)p + 40, "rifts");
    put32(p + 100, GCS_RIFTS_GAME_CLASS);
    send_msg(g, 0x1000, 0, GCSS_OP_LOGIN, p, sizeof(p));
    m = take(g, GCSS_OP_LOGIN_OK);
    CHECK(m != NULL);
    if (m) {
        CHECK_EQ(m->n, 12);
        CHECK_EQ(be32(m->p), 0); /* same port */
        g->uid = be32(m->p + 8);
        CHECK(g->uid != 0);
    }
    CHECK_EQ(gcs_snap_game_class_of(S, g->name), GCS_RIFTS_GAME_CLASS); /* picks www_root/8053 */
}

/* the bypass flow of ArenaFramework: lobby S79, room BYPASS */
static void enter_room(game *g)
{
    gc_u8 p[44];
    msg *m;
    /* first the lobbies of the game class (op 71): a list message, no op 40 */
    memset(p, 0, sizeof(p));
    send_msg(g, 0xb000, 6, GCSS_OP_GAME_LOBBIES, p, 0);
    m = take(g, GCSS_OP_GAME_LOBBIES);
    CHECK(m != NULL);
    if (m) {
        gc_u32 i, n = be32(m->p + 8);
        int found = 0;
        CHECK(m->h[0] & 0x20); /* single packet: the list is complete */
        CHECK_EQ(m->h[2], 6);
        CHECK_EQ(be32(m->p), GCS_RIFTS_GAME_CLASS);
        CHECK_EQ(m->n, 12 + 36 * n);
        for (i = 0; i < n; i++) {
            found |= !strcmp((const char *)m->p + 12 + 36 * i, "S79");
        }
        CHECK(found);
    }
    CHECK(take(g, GCSS_OP_OK) == NULL);

    memset(p, 0, sizeof(p));
    strcpy((char *)p, "S79");
    send_msg(g, 0x9000, 7, GCSS_OP_LOBBY_INFO, p, 16);
    m = take(g, GCSS_OP_LOBBY_INFO);
    CHECK(m && m->n == 20 && !strcmp((const char *)m->p, "S79"));
    g->lobby = m ? be32(m->p + 16) : 0;
    m = take(g, GCSS_OP_OK);
    CHECK(m && be32(m->p) == GCSS_OP_LOBBY_INFO && be32(m->p + 4) == 1 && m->h[2] == 7);

    /* op 13 and the lobby join packed into one datagram (seen on hardware) */
    {
        gc_u8 q[4] = { 0xe1, 0xe0, 0x05, 0x01 };
        put32(p, g->lobby);
        send_packed(g, 12, 13, q, 4, 0x9000, 8, GCSS_OP_JOIN, p, 4);
        m = take(g, GCSS_OP_OK);
        CHECK(m && be32(m->p) == 13 && m->h[2] == 12);
    }
    m = take(g, GCSS_OP_OK);
    CHECK(m && be32(m->p) == GCSS_OP_JOIN && be32(m->p + 4) == g->lobby && (m->h[0] & 0x10) && m->h[2] == 8);

    put32(p, g->lobby);
    send_msg(g, 0x9000, 9, GCSS_OP_ROOMS, p, 4);
    m = take(g, GCSS_OP_ROOMS);
    CHECK(m != NULL);
    if (m) {
        gc_u32 i, n = be32(m->p + 8);
        CHECK(m->h[0] & 0x20); /* single packet */
        CHECK_EQ(m->n, 12 + 36 * n);
        for (i = 0; i < n; i++) {
            CHECK_EQ(be32(m->p + 12 + 36 * i + 32), g->lobby);
        }
    }

    memset(p, 0, sizeof(p));
    strcpy((char *)p, "BYPASS");
    put32(p + 16, 1);
    send_msg(g, 0x9000, 10, GCSS_OP_CREATE_ROOM, p, 44);
    m = take(g, GCSS_OP_CREATE_ROOM);
    CHECK(m && m->n == 36 && !strcmp((const char *)m->p, "BYPASS"));
    m = take(g, GCSS_OP_OK);
    CHECK(m && be32(m->p) == GCSS_OP_CREATE_ROOM && m->h[2] == 10);
    g->room = m ? be32(m->p + 4) : 0;

    /* the four room attributes ArenaFramework sets after creating the
     * room, as on hardware: ARNA with ARNB packed behind it, then ARNC
     * and ARND on their own with sequence number 0, ARND repeated */
    {
        gc_u8 a[8], b[8];
        int k;
        memset(a, 0, 8);
        memset(b, 0, 8);
        memcpy(a, "ARNA", 4);
        memcpy(b, "ARNB", 4);
        send_packed(g, 20, GCSS_OP_ATTR, a, 8, 0x8000, 21, GCSS_OP_ATTR, b, 8);
        memcpy(a, "ARNC", 4);
        send_msg0(g, 0x8000, 22, GCSS_OP_ATTR, a, 8);
        memcpy(a, "ARND", 4);
        send_msg0(g, 0x8000, 23, GCSS_OP_ATTR, a, 8);
        send_msg0(g, 0x8000, 23, GCSS_OP_ATTR, a, 8);
        for (k = 20; k <= 23; k++) {
            m = take(g, GCSS_OP_OK);
            CHECK(m && be32(m->p) == GCSS_OP_ATTR && m->h[2] == k);
        }
        CHECK(take(g, GCSS_OP_OK) == NULL);
    }

    /* no room join: creating the room entered it (the game data below
     * reaches the Rifts game server only for room members) */
}

/* ---- Rifts client actions ---- */

typedef struct
{
    unsigned char *d;
    size_t len;
    int cancelled;
    int empty; /* answer without data */
} answer;

static void rifts_send(game *g, const unsigned char *m, size_t n)
{
    send_msg(g, 0x8000, 0, GCSS_OP_TO_SERVER, m, n);
}

static void send_credit(game *g, int credit)
{
    unsigned char m[8];
    memset(m, 0, 8);
    m[0] = GCSR_MSG_CREDIT;
    st32(m + 4, (gc_u32)credit);
    rifts_send(g, m, 8);
}

/* collect the answer to object id from the game data the game got */
static int collect(game *g, gc_u32 object, answer *a)
{
    msg *m;
    while ((m = take(g, GCSS_OP_DATA)) != NULL) {
        if (m->p[0] == GCSR_MSG_CREDIT || m->p[0] == GCSR_MSG_PING) {
            continue;
        }
        if (m->p[0] == GCSR_MSG_SERVER_CANCEL && le32(m->p + 4) == object) {
            a->cancelled = 1;
            return 1;
        }
        if (m->p[0] == GCSR_MSG_SERVER && le32(m->p + 4) == object) {
            gc_u32 off = le32(m->p + 12), len = le32(m->p + 16), total = le32(m->p + 20);
            CHECK_EQ(m->n, 24 + len);
            if (total == 0) {
                a->empty = 1;
                return 1;
            }
            if (!a->d) {
                a->d = (unsigned char *)malloc(total);
                a->len = 0;
            }
            memcpy(a->d + off, m->p + 24, len);
            a->len += len;
            if (a->len == total) {
                size_t ul;
                unsigned char *u = gcs_rc_unpack(KEY, a->d, total, &ul);
                CHECK(u != NULL);
                free(a->d);
                a->d = u;
                a->len = ul;
                return 1;
            }
        }
    }
    return 0;
}

static gc_u32 g_object = 0x40001000u;

static answer request(game *g, int type, const unsigned char *s, size_t n)
{
    answer a;
    unsigned char m[24 + 384], *packed;
    size_t pl, off = 0;
    gc_u32 object = g_object++;
    memset(&a, 0, sizeof(a));
    packed = gcs_rc_pack(KEY, s, n, &pl);
    do {
        size_t k = pl - off > 384 ? 384 : pl - off;
        memset(m, 0, 24);
        m[0] = GCSR_MSG_SERVER;
        st32(m + 4, object);
        m[8] = (unsigned char)type;
        st32(m + 12, (gc_u32)off);
        st32(m + 16, (gc_u32)k);
        st32(m + 20, (gc_u32)pl);
        memcpy(m + 24, packed + off, k);
        rifts_send(g, m, 24 + k);
        off += k;
    } while (off < pl);
    free(packed);
    if (!collect(g, object, &a)) {
        /* larger answers wait for flow control credit */
        int i;
        for (i = 0; i < 20 && !collect(g, object, &a); i++) {
            send_credit(g, 4096);
        }
    }
    CHECK(a.cancelled || a.empty || a.d != NULL);
    return a;
}

/* little stream writer */
typedef struct
{
    unsigned char d[16384];
    size_t n;
} sw;

static void w32(sw *w, gc_u32 v)
{
    st32(w->d + w->n, v);
    w->n += 4;
}

static void w64(sw *w, long long v)
{
    w32(w, (gc_u32)((unsigned long long)v & 0xffffffffu));
    w32(w, (gc_u32)((unsigned long long)v >> 32));
}

static void wb(sw *w, const void *d, size_t n)
{
    memcpy(w->d + w->n, d, n);
    w->n += n;
}

static void ws(sw *w, const char *s)
{
    w32(w, (gc_u32)strlen(s));
    wb(w, s, strlen(s));
}

static void wname(sw *w, const char *s, size_t n)
{
    unsigned char b[32];
    memset(b, 0, sizeof(b));
    memcpy(b, s, strlen(s));
    wb(w, b, n);
}

static void battle_row(sw *w, const char *host, const char *name)
{
    w64(w, 0);    /* id: assigned by the server */
    w32(w, 0);    /* kind (melee) */
    w32(w, 17);   /* sector */
    w32(w, 7);    /* time limit, days */
    w32(w, 24);   /* turn time limit, hours */
    w32(w, 30);   /* max rounds */
    w32(w, 0);    /* OCC restrict */
    w32(w, 2);    /* players required */
    w32(w, 1);    /* players accepted */
    w32(w, 3);    /* characters */
    w32(w, 2);    /* teams */
    w32(w, 0);    /* min power */
    w32(w, 1000); /* max power */
    w32(w, 0);    /* max points */
    w32(w, 0);    /* ranked */
    w32(w, 0);    /* friends only */
    w32(w, 0);    /* start */
    w32(w, 0);    /* round */
    w32(w, 0);    /* state */
    w32(w, 0);    /* team initiative */
    w32(w, 1234); /* seed */
    w32(w, -1);   /* next turn device */
    w32(w, 0);
    w32(w, 0);
    wname(w, host, 32);
    wname(w, name, 32);
}

static void entity(sw *w, const char *user, const char *name, int attrs)
{
    int i;
    w64(w, 0);
    w64(w, 0);
    w32(w, 5000); /* resource */
    w32(w, 0);
    w32(w, 0);
    wname(w, user, 32);
    wname(w, name, 32);
    for (i = 0; i < 265; i++) {
        w32(w, (gc_u32)i);
    }
    w32(w, (gc_u32)attrs);
    for (i = 0; i < attrs; i++) {
        w64(w, 0);
        w64(w, 0);
        w32(w, (gc_u32)i);
        w32(w, 100u + (gc_u32)i);
    }
}

/* little stream reader */
typedef struct
{
    const unsigned char *d;
    size_t n, pos;
} sr;

static gc_u32 r32(sr *r)
{
    gc_u32 v = r->pos + 4 <= r->n ? le32(r->d + r->pos) : 0xDEADBEEFu;
    r->pos += 4;
    return v;
}

static long long r64(sr *r)
{
    gc_u32 lo = r32(r), hi = r32(r);
    return (long long)((unsigned long long)lo | ((unsigned long long)hi << 32));
}

static int r8(sr *r)
{
    int v = r->pos < r->n ? r->d[r->pos] : -1;
    r->pos++;
    return v;
}

static void setup(void)
{
    gcs_snap_io io;
    memset(&io, 0, sizeof(io));
    io.send = io_send;
    io.log = io_log;
    io.auth = io_auth;
    S = gcs_snap_create(&io, NULL);
    R = gcs_rifts_create(DATA_FILE, io_log, NULL);
    CHECK(S && R);
    gcs_snap_add_game(S, gcs_rifts_game(R));
}

static void teardown(void)
{
    gcs_snap_destroy(S);
    gcs_rifts_destroy(R);
    S = NULL;
    R = NULL;
}

static void test_snap(void)
{
    game *a = &G[0];
    gc_u8 p[120];
    msg *m;
    gcs_snap_stats st;
    setup();
    memset(G, 0, sizeof(G));
    a->port = 3001;
    a->name = "alice";
    a->auto_ack = 1;

    /* garbage and a login for an unknown name */
    send_msg(a, 0, 0, 99, p, 0);
    CHECK_EQ(a->n_in, 0);
    memset(p, 0, sizeof(p));
    strcpy((char *)p, "mallory");
    send_msg(a, 0, 0, GCSS_OP_LOGIN, p, sizeof(p));
    m = take(a, GCSS_OP_FAIL);
    CHECK(m && be32(m->p) == 1);
    CHECK(take(a, GCSS_OP_LOGIN_OK) == NULL);

    login(a);
    enter_room(a);
    gcs_snap_get_stats(S, &st);
    CHECK_EQ(st.sessions, 1);
    CHECK_EQ(st.rooms, 1);
    CHECK_EQ(st.logins, 1);

    /* a second login of the same user: old session replaced */
    G[1].port = 3002;
    G[1].name = "alice";
    G[1].auto_ack = 1;
    login(&G[1]);
    gcs_snap_get_stats(S, &st);
    CHECK_EQ(st.sessions, 1);
    CHECK_EQ(st.rooms, 0); /* the old session's room closed */

    /* retransmission until acknowledged */
    G[1].auto_ack = 0;
    memset(p, 0, 16);
    strcpy((char *)p, "Scrimmage");
    send_msg(&G[1], 0x9000, 3, GCSS_OP_LOBBY_INFO, p, 16);
    CHECK(take(&G[1], GCSS_OP_LOBBY_INFO) != NULL);
    G[1].n_in = 0;
    g_now += 1100;
    gcs_snap_poll(S, g_now);
    gcs_snap_get_stats(S, &st);
    CHECK(st.resends >= 2);
    G[1].auto_ack = 1;
    g_now += 1100;
    gcs_snap_poll(S, g_now);
    gcs_snap_get_stats(S, &st);
    {
        gc_u32 before = st.resends;
        g_now += 1100;
        gcs_snap_poll(S, g_now);
        gcs_snap_get_stats(S, &st);
        CHECK_EQ(st.resends, before); /* acknowledged now */
    }
    /* logout */
    send_msg(&G[1], 0x9000, 4, GCSS_OP_LOGOUT, p, 0);
    gcs_snap_get_stats(S, &st);
    CHECK_EQ(st.sessions, 0);
    teardown();
}

static void test_battle(void)
{
    game *a = &G[0], *b = &G[1];
    answer r;
    sw w;
    sr rd;
    long long battle_id, ent_a, ent_b;
    gcs_rifts_stats rst;
    int i;

    remove(DATA_FILE);
    setup();
    memset(G, 0, sizeof(G));
    a->port = 4001;
    a->name = "alice";
    a->auto_ack = 1;
    b->port = 4002;
    b->name = "bob";
    b->auto_ack = 1;
    login(a);
    enter_room(a);
    login(b);
    enter_room(b);
    gcs_rifts_get_stats(R, &rst);
    CHECK_EQ(rst.players, 2);

    /* These IDs match ServerActionID in the original Rifts source. */
    CHECK_EQ(GCSR_DOWNLOAD_TURNS, 0);
    CHECK_EQ(GCSR_TURN_COUNT, 1);
    CHECK_EQ(GCSR_DOWNLOAD_CHALLENGE_UNUSED, 10);
    CHECK_EQ(GCSR_DOWNLOAD_CHARACTERS, 11);
    CHECK_EQ(GCSR_END_GAME, 12);
    CHECK_EQ(GCSR_GAME_UPKEEP, 13);
    CHECK_EQ(GCSR_UPDATE_FRIENDS, 14);

    /* The original server leaves its type-10 download action unregistered. */
    w.n = 0;
    w64(&w, 1);
    w32(&w, 0);
    r = request(a, GCSR_DOWNLOAD_CHALLENGE_UNUSED, w.d, w.n);
    CHECK(r.cancelled);
    free(r.d);

    /* ping */
    {
        unsigned char ping[4] = { GCSR_MSG_PING, 0, 0, 0 };
        msg *m;
        rifts_send(a, ping, 4);
        m = take(a, GCSS_OP_DATA);
        CHECK(m && m->p[0] == GCSR_MSG_PING);
    }

    /* server configuration */
    w.n = 0;
    w32(&w, 0);
    w32(&w, 43253);
    w32(&w, 0x1A);
    r = request(a, GCSR_SERVER_CONFIG, w.d, w.n);
    rd.d = r.d;
    rd.n = r.len;
    rd.pos = 0;
    CHECK_EQ(r32(&rd), 2);
    CHECK_EQ(r32(&rd), 0);
    CHECK_EQ(r32(&rd), 0);
    CHECK_EQ(r32(&rd), 1);     /* power rank version */
    CHECK_EQ(r32(&rd), 43253); /* the game's own versions */
    CHECK_EQ(r32(&rd), 0x1A);
    CHECK_EQ(r32(&rd), 17);
    rd.pos += 17 * 20;
    CHECK_EQ(r32(&rd), 14);
    rd.pos += 14 * 8;
    CHECK_EQ(rd.pos, r.len);
    free(r.d);

    /* name validation */
    w.n = 0;
    ws(&w, "Sh1theads");
    r = request(a, GCSR_NAME_VALIDATION, w.d, w.n);
    CHECK(r.d && r.len == 4 && le32(r.d) == 0);
    free(r.d);
    w.n = 0;
    ws(&w, "Duel of Fates");
    r = request(a, GCSR_NAME_VALIDATION, w.d, w.n);
    CHECK(r.d && r.len == 4 && le32(r.d) == 1);
    free(r.d);

    /* alice creates a challenge with two characters */
    w.n = 0;
    battle_row(&w, "alice", "Duel");
    w64(&w, -1);
    w32(&w, 0); /* team */
    w32(&w, 2);
    entity(&w, "alice", "Kara", 2);
    entity(&w, "alice", "Vex", 0);
    w32(&w, 1);   /* save slot */
    w32(&w, 120); /* power ranking */
    r = request(a, GCSR_CREATE_CHALLENGE, w.d, w.n);
    CHECK(r.d && r.len == 8 + 2 * 8);
    battle_id = r.d ? (long long)le32(r.d) : 0;
    ent_a = r.d ? (long long)le32(r.d + 8) : 0;
    CHECK(battle_id > 0 && ent_a > 0);
    free(r.d);

    /* bob finds it; alice does not see her own challenge */
    w.n = 0;
    w32(&w, 0); /* no paging data */
    w32(&w, 0);
    w32(&w, 1000);
    w32(&w, (gc_u32)-1);
    w32(&w, 2);
    wb(&w, "\0\0", 2);
    wname(&w, "du", 16);
    w32(&w, 10);
    ws(&w, "bob");
    r = request(b, GCSR_FIND_CHALLENGE, w.d, w.n);
    rd.d = r.d;
    rd.n = r.len;
    rd.pos = 0;
    CHECK_EQ(r32(&rd), 1);
    CHECK_EQ(r64(&rd), battle_id);
    rd.pos = 4 + 164;
    CHECK_EQ(r64(&rd), battle_id); /* the host's player row */
    rd.pos = 4 + 164 + 68;
    CHECK_EQ(r32(&rd), 4);
    CHECK_EQ(r32(&rd), 1);
    CHECK_EQ(rd.pos, r.len);
    CHECK(r.d && !memcmp(r.d + 4 + 164 + 36, "alice", 6));
    free(r.d);
    w.n = 0;
    w32(&w, 0);
    w32(&w, 0);
    w32(&w, 1000);
    w32(&w, (gc_u32)-1);
    w32(&w, 2);
    wb(&w, "\0\0", 2);
    wname(&w, "", 16);
    w32(&w, 10);
    ws(&w, "alice");
    r = request(a, GCSR_FIND_CHALLENGE, w.d, w.n);
    CHECK(r.d && le32(r.d) == 0);
    free(r.d);

    /* bob accepts on the other team: the battle starts */
    w.n = 0;
    w64(&w, battle_id);
    w32(&w, 1);
    w32(&w, 1);
    entity(&w, "bob", "Rook", 1);
    w32(&w, 2);
    w32(&w, 90);
    r = request(b, GCSR_ACCEPT_CHALLENGE, w.d, w.n);
    rd.d = r.d;
    rd.n = r.len;
    rd.pos = 0;
    CHECK_EQ(r8(&rd), 1); /* last user */
    CHECK_EQ(r8(&rd), 1); /* device id */
    CHECK_EQ(r32(&rd), 1234);
    ent_b = r64(&rd);
    CHECK(ent_b > ent_a);
    CHECK_EQ(rd.pos, r.len);
    free(r.d);
    /* full now */
    r = request(b, GCSR_ACCEPT_CHALLENGE, w.d, w.n);
    CHECK(r.d && r.len == 6 && r.d[0] == 3); /* not waiting any more */
    free(r.d);

    /* alice checks her games */
    w.n = 0;
    ws(&w, "alice");
    w32(&w, 1);
    w64(&w, battle_id);
    w32(&w, 0);
    w32(&w, 1);
    r = request(a, GCSR_CHALLENGE_STATUS, w.d, w.n);
    CHECK(r.d && r.len == 16 && le32(r.d) == 1 && le32(r.d + 4) == 2 && le32(r.d + 8) == 0);
    free(r.d);

    /* alice uploads her first turn: a command and the end of turn */
    w.n = 0;
    w64(&w, battle_id);
    w32(&w, 1);
    w32(&w, 0);
    w32(&w, 2);
    w32(&w, 8);
    wb(&w, "\x11\0\0\0\1\2\3\4", 8);
    w32(&w, 12);
    wb(&w, "\x12\1\0\0\1\0\0\0\0\0\0\0", 12); /* MessageEndTurn, next device 1 */
    r = request(a, GCSR_UPLOAD_TURNS, w.d, w.n);
    CHECK(r.d && r.len == 1 && r.d[0] == 1);
    free(r.d);
    /* the same upload again is fine, a different one is not */
    r = request(a, GCSR_UPLOAD_TURNS, w.d, w.n);
    CHECK(r.d && r.len == 1 && r.d[0] == 1);
    free(r.d);
    w.d[w.n - 1] = 9;
    r = request(a, GCSR_UPLOAD_TURNS, w.d, w.n);
    CHECK(r.d && r.len == 1 && r.d[0] == 0);
    free(r.d);

    /* bob: turn count, turns, characters, upkeep */
    w.n = 0;
    w64(&w, battle_id);
    w32(&w, 0);
    r = request(b, GCSR_TURN_COUNT, w.d, w.n);
    CHECK(r.d && r.len == 8 && le32(r.d) == 2 && le32(r.d + 4) == 1);
    free(r.d);
    r = request(b, GCSR_DOWNLOAD_TURNS, w.d, w.n);
    CHECK(r.d && r.len == 4 + 12 + 16 && le32(r.d) == 2 && le32(r.d + 4) == 8 && r.d[8] == 0x11 &&
          le32(r.d + 16) == 12 && r.d[20] == 0x12);
    free(r.d);
    w.n = 0;
    w64(&w, battle_id);
    ws(&w, "bob");
    r = request(b, GCSR_DOWNLOAD_CHARACTERS, w.d, w.n);
    rd.d = r.d;
    rd.n = r.len;
    rd.pos = 0;
    CHECK_EQ(r32(&rd), 2);
    CHECK_EQ(r64(&rd), battle_id);
    CHECK_EQ(r64(&rd), ent_a);
    CHECK(r.d && !memcmp(r.d + 4 + 28, "alice", 6));
    CHECK_EQ(r.len, 4 + (1156 + 48) + 1156);
    /* the attribute rows carry the ids too */
    CHECK(r.d && le32(r.d + 4 + 1156) == (gc_u32)battle_id && le32(r.d + 4 + 1156 + 8) == (gc_u32)ent_a);
    free(r.d);
    w.n = 0;
    w.d[0] = 0;
    w.n = 1;
    w64(&w, battle_id);
    w32(&w, 1);
    w32(&w, 1);
    w64(&w, battle_id);
    w32(&w, 0);
    r = request(b, GCSR_GAME_UPKEEP, w.d, w.n);
    rd.d = r.d;
    rd.n = r.len;
    rd.pos = 0;
    CHECK_EQ(r64(&rd), battle_id);
    rd.pos = 164;
    CHECK(r32(&rd) > 0);  /* server time */
    CHECK_EQ(r8(&rd), 0); /* not over */
    rd.pos += 8;          /* nobody dropped */
    CHECK_EQ(r32(&rd), 2);
    free(r.d);

    /* a big turn: the answer needs flow control credit */
    w.n = 0;
    w64(&w, battle_id);
    w32(&w, 2);
    w32(&w, 1);
    w32(&w, 10);
    for (i = 0; i < 9; i++) {
        unsigned char big[1000];
        memset(big, i, sizeof(big));
        big[0] = 0x11;
        w32(&w, 1000);
        wb(&w, big, 1000);
    }
    w32(&w, 12);
    wb(&w, "\x12\1\0\0\0\0\0\0\0\0\0\0", 12);
    r = request(b, GCSR_UPLOAD_TURNS, w.d, w.n);
    CHECK(r.d && r.len == 1 && r.d[0] == 1);
    free(r.d);
    w.n = 0;
    w64(&w, battle_id);
    w32(&w, 1);
    r = request(a, GCSR_DOWNLOAD_TURNS, w.d, w.n);
    CHECK(r.d && r.len == 4 + 9 * 1004 + 16 && le32(r.d) == 10);
    free(r.d);

    /* unknown battle: download turns finds none, a damaged stream is cancelled */
    w.n = 0;
    w64(&w, 999);
    w32(&w, 0);
    r = request(a, GCSR_DOWNLOAD_TURNS, w.d, w.n);
    CHECK(r.d && r.len == 4 && le32(r.d) == 0);
    free(r.d);
    {
        unsigned char m[24 + 16];
        msg *x;
        memset(m, 0, sizeof(m));
        m[0] = GCSR_MSG_SERVER;
        st32(m + 4, 0x77);
        m[8] = GCSR_TURN_COUNT;
        st32(m + 16, 16);
        st32(m + 20, 16);
        rifts_send(a, m, sizeof(m));
        x = take(a, GCSS_OP_DATA);
        while (x && x->p[0] == GCSR_MSG_CREDIT) {
            x = take(a, GCSS_OP_DATA);
        }
        CHECK(x && x->p[0] == GCSR_MSG_SERVER_CANCEL && le32(x->p + 4) == 0x77);
    }

    /* the end: no answer data */
    w.n = 0;
    w64(&w, battle_id);
    ws(&w, "alice");
    w32(&w, 1);
    w32(&w, 0);
    w32(&w, 50);
    r = request(a, GCSR_END_GAME, w.d, w.n);
    CHECK(r.empty);
    w.n = 0;
    ws(&w, "alice");
    w32(&w, 1);
    ws(&w, "bob");
    r = request(a, GCSR_UPDATE_FRIENDS, w.d, w.n);
    CHECK(r.empty);
    gcs_rifts_get_stats(R, &rst);
    CHECK_EQ(rst.battles, 1);
    CHECK(rst.requests >= 18);
    teardown();

    /* everything is still there after a restart */
    setup();
    memset(G, 0, sizeof(G));
    b->port = 4003;
    b->name = "bob";
    b->auto_ack = 1;
    login(b);
    enter_room(b);
    w.n = 0;
    ws(&w, "bob");
    w32(&w, 1);
    w64(&w, battle_id);
    w32(&w, 0);
    w32(&w, 2);
    r = request(b, GCSR_CHALLENGE_STATUS, w.d, w.n);
    CHECK(r.d && r.len == 16 && le32(r.d + 4) == 4); /* GAME_ENDED */
    free(r.d);
    w.n = 0;
    w64(&w, battle_id);
    w32(&w, 0);
    r = request(b, GCSR_TURN_COUNT, w.d, w.n);
    CHECK(r.d && le32(r.d) == 12);
    free(r.d);
    /* bob deletes his save: slot 2 no longer reported */
    w.n = 0;
    ws(&w, "bob");
    w32(&w, 0);
    r = request(b, GCSR_CHALLENGE_STATUS, w.d, w.n);
    CHECK(r.d && r.len == 4 && le32(r.d) == 0);
    free(r.d);
    teardown();
    remove(DATA_FILE);
}

/* random input must never crash or leak (run with GC_SANITIZE=ON) */
static void test_fuzz(void)
{
    game *a = &G[0];
    int i, k, t;
    unsigned char d[1100];
    remove(DATA_FILE);
    setup();
    memset(G, 0, sizeof(G));
    a->port = 5001;
    a->name = "alice";
    a->auto_ack = 1;
    login(a);
    enter_room(a);
    /* SNAP: random messages with a valid trailer */
    for (i = 0; i < 20000; i++) {
        size_t n = 20 + gct_rand() % 200;
        for (k = 0; k < (int)n; k++) {
            d[k] = (unsigned char)gct_rand();
        }
        if (i % 3 == 0) { /* a plausible header: right length, known opcode */
            size_t len = n - 4;
            d[0] = (unsigned char)((d[0] & 0xfc) | (len >> 8));
            d[1] = (unsigned char)len;
            d[3] = (unsigned char)(gct_rand() % 76);
            if (d[3] == GCSS_OP_LOGIN || d[3] == GCSS_OP_LOGOUT) {
                d[3] = GCSS_OP_LOBBY_INFO;
            }
            put32(d + 4, a->uid);
        }
        put32(d + n - 4, 0xBA476611u);
        gcs_snap_input(S, &a->key, SRV, GCSS_PORT, a->port, d, n, g_now);
        a->n_in = 0;
        if (i % 1000 == 0) {
            g_now += 1500;
            gcs_snap_poll(S, g_now);
        }
    }
    /* Rifts: every action with random (validly packed) streams */
    memset(G, 0, sizeof(G));
    a->port = 5002;
    a->name = "alice";
    a->auto_ack = 1;
    login(a);
    enter_room(a);
    {
        for (i = 0; i < 3000; i++) {
            unsigned char plain[600], m[24 + 384], *pk;
            size_t n = gct_rand() % sizeof(plain), pl;
            t = (int)(gct_rand() % 16);
            for (k = 0; k < (int)n; k++) {
                plain[k] = (unsigned char)(gct_rand() % 4 == 0 ? gct_rand() : (gct_rand() % 3));
            }
            pk = gcs_rc_pack(KEY, plain, n, &pl);
            if (pl <= 384) {
                memset(m, 0, 24);
                m[0] = GCSR_MSG_SERVER;
                st32(m + 4, 0x50000000u + (gc_u32)i);
                m[8] = (unsigned char)t;
                st32(m + 16, (gc_u32)pl);
                st32(m + 20, (gc_u32)pl);
                memcpy(m + 24, pk, pl);
                rifts_send(a, m, 24 + pl);
            }
            free(pk);
            send_credit(a, 100000);
            a->n_in = 0;
        }
    }
    {
        gcs_rifts_stats rst;
        gcs_rifts_get_stats(R, &rst);
        CHECK(rst.requests > 1000);
    }
    teardown();
    remove(DATA_FILE);
}

int main(int argc, char **argv)
{
    g_verbose = argc > 1 && !strcmp(argv[1], "-v");
    RUN(test_snap);
    RUN(test_battle);
    RUN(test_fuzz);
    TEST_MAIN_END();
}
