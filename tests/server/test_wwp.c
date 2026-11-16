/** @file test_wwp.c
 *
 *  Worms World Party (game class 1104) against simulated games:
 *
 *   - SNAP as ArenaFramework's Play Now screens use it: the lobbies of
 *     the game class, creating a room and setting its attributes (op 8),
 *     the attribute notifications to the other members, reading an
 *     attribute (op 9), the room search with criteria (op 72), joining,
 *     and the game data between the two players (ops 15 and 16)
 *   - the SegaChat reporter / retrieval services as arenabase.dll asks
 *     them: match ids, results and skill points of a ranked match, and
 *     the playerskills / highscores lists.  Every answer is decoded the
 *     way arenabase.dll does it (status, message, lines of '|'
 *     separated fields, the same field count in every row, board name
 *     in header field 4, the header and row sizes per board and request).
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#include "gc_test.h"
#include "gcs_accounts.h"
#include "gcs_core.h"
#include "gcs_direct.h"
#include "gcs_native.h"
#include "gcs_scores.h"
#include "gcs_snap.h"
#include "gcs_wwp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DATA_FILE "test-wwp-scores.dat"
#define ACC_FILE  "test-wwp-accounts.txt"

static const gc_u8 SRV[4] = { 10, 64, 0, 1 };
static gc_u32 g_now = 5000;
static int g_verbose;

/* ---- simulated games (snapcomm.dll side) ---- */

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
} game;

static game G[3];
static gcs_snap *S;

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
    game *g = (game *)key;
    size_t pos = 0;
    (void)ctx;
    CHECK(!memcmp(addr, SRV, 4));
    CHECK_EQ(port, GCSS_PORT);
    CHECK_EQ(peer_port, g->port);
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
            CHECK_EQ(be32(d + pos + 8), g->rx_next);
            g->rx_next = be32(d + pos + 8) + 1;
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
    if (n) {
        memcpy(d + 16, p, n);
    }
    put32(d + len, 0xBA476611u);
    gcs_snap_input(S, &g->key, SRV, GCSS_PORT, g->port, d, len + 4, g_now);
}

static msg *take(game *g, gc_u8 op)
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

static void login(game *g)
{
    gc_u8 p[112];
    msg *m;
    memset(p, 0, sizeof(p));
    strcpy((char *)p, g->name);
    strcpy((char *)p + 40, "wwp");
    put32(p + 100, GCS_WWP_GAME_CLASS);
    send_msg(g, 0x1000, 0, GCSS_OP_LOGIN, p, sizeof(p));
    m = take(g, GCSS_OP_LOGIN_OK);
    CHECK(m != NULL);
    g->uid = m ? be32(m->p + 8) : 0;
    CHECK_EQ(gcs_snap_game_class_of(S, g->name), GCS_WWP_GAME_CLASS);
}

/* Play Now: lobby list of the game class, pick one by name, enter it */
static void enter_lobby(game *g, const char *name)
{
    gc_u8 p[16];
    msg *m;
    gc_u32 i, n;
    int found = 0, ranked = 0;
    memset(p, 0, sizeof(p));
    send_msg(g, 0xb000, 3, GCSS_OP_GAME_LOBBIES, p, 0);
    m = take(g, GCSS_OP_GAME_LOBBIES);
    CHECK(m != NULL);
    if (!m) {
        return;
    }
    CHECK_EQ(be32(m->p), GCS_WWP_GAME_CLASS);
    n = be32(m->p + 8);
    CHECK_EQ(m->n, 12 + 36 * n);
    for (i = 0; i < n; i++) {
        const gc_u8 *e = m->p + 12 + 36 * i;
        ranked |= !strcmp((const char *)e, "Ranked");
        if (!strcmp((const char *)e, name)) {
            found = 1;
            g->lobby = be32(e + 32);
        }
    }
    CHECK(found && ranked);
    /* op 14 (lobby list) knows them, too */
    send_msg(g, 0xb000, 4, GCSS_OP_LOBBIES, p, 0);
    m = take(g, GCSS_OP_LOBBIES);
    CHECK(m && be32(m->p + 8) >= 2 && m->n == 12 + 28 * be32(m->p + 8));
    put32(p, g->lobby);
    send_msg(g, 0xb000, 5, GCSS_OP_JOIN, p, 4);
    m = take(g, GCSS_OP_OK);
    CHECK(m && be32(m->p) == GCSS_OP_JOIN && be32(m->p + 4) == g->lobby && (m->h[0] & 0x10));
}

static void set_attr(game *g, gc_u8 seq, const char *name, gc_u32 v)
{
    gc_u8 p[8];
    msg *m;
    memcpy(p, name, 4);
    put32(p + 4, v);
    send_msg(g, 0xa000, seq, GCSS_OP_ATTR, p, 8);
    m = take(g, GCSS_OP_OK);
    CHECK(m && be32(m->p) == GCSS_OP_ATTR && m->h[2] == seq);
}

/* criteria as ArenaFramework's SelectGameRooms builds them */
static size_t criteria(gc_u8 *p, gc_u32 stat, gc_u32 mask, gc_u32 max_users, gc_u32 arna)
{
    size_t k = 8;
    memset(p, 0, 8);
    put32(p, 20);
    p[4] = 3;
    memcpy(p + k, "STAT", 4);
    p[k + 4] = 0x21;
    put32(p + k + 5, stat);
    put32(p + k + 9, mask);
    k += 13;
    memcpy(p + k, "USER", 4);
    p[k + 4] = 6;
    put32(p + k + 5, max_users);
    k += 9;
    memcpy(p + k, "ARNA", 4);
    p[k + 4] = 1;
    put32(p + k + 5, arna);
    k += 9;
    return k;
}

static int search(game *g, gc_u32 stat, gc_u32 mask, gc_u32 max_users, gc_u32 arna, gc_u32 *room)
{
    gc_u8 p[64];
    size_t n = criteria(p, stat, mask, max_users, arna);
    msg *m;
    send_msg(g, 0xb000, 30, GCSS_OP_SELECT_LOBBIES, p, n);
    m = take(g, GCSS_OP_SELECT_LOBBIES);
    CHECK(m != NULL);
    if (!m) {
        return -1;
    }
    CHECK(m->h[0] & 0x20);
    CHECK_EQ(m->h[2], 30);
    CHECK_EQ(m->n, 12 + 36 * be32(m->p + 8));
    if (be32(m->p + 8) && room) {
        *room = be32(m->p + 12 + 32);
    }
    return (int)be32(m->p + 8);
}

static void test_rooms(void)
{
    gc_u8 p[64];
    msg *m;
    gc_u32 found = 0;
    game *a = &G[0], *b = &G[1];

    login(a);
    login(b);
    enter_lobby(a, "Freestyle");
    enter_lobby(b, "Freestyle");
    CHECK_EQ(a->lobby, b->lobby);

    /* A creates "My game" for 4 players */
    memset(p, 0, 44);
    strcpy((char *)p, "My game");
    put32(p + 16, 4);
    send_msg(a, 0xb000, 10, GCSS_OP_CREATE_ROOM, p, 44);
    m = take(a, GCSS_OP_CREATE_ROOM);
    CHECK(m && m->n == 36);
    m = take(a, GCSS_OP_OK);
    CHECK(m && be32(m->p) == GCSS_OP_CREATE_ROOM);
    a->room = m ? be32(m->p + 4) : 0;

    /* game settings and status as room attributes */
    set_attr(a, 11, "ARNA", 3); /* e.g. the game mode */
    set_attr(a, 12, "STAT", 0x90000000u);

    /* B's search: matching criteria find the room, others do not */
    CHECK_EQ(search(b, 0x90000000u, 0xf0000000u, 0x7fff, 3, &found), 1);
    CHECK_EQ(found, a->room);
    CHECK_EQ(search(b, 0x90000000u, 0xf0000000u, 0x7fff, 4, NULL), 0); /* other game mode */
    CHECK_EQ(search(b, 0x10000000u, 0xf0000000u, 0x7fff, 3, NULL), 0); /* other status    */
    CHECK_EQ(search(b, 0x90000000u, 0xf0000000u, 1, 3, NULL), 0);      /* too many users  */

    /* op 9 reads it back */
    put32(p, a->room);
    memcpy(p + 4, "ARNA", 4);
    send_msg(b, 0xa000, 31, GCSS_OP_ATTR2, p, 8);
    m = take(b, GCSS_OP_ATTR2);
    CHECK(m && m->n == 12 && be32(m->p) == a->room && !memcmp(m->p + 4, "ARNA", 4) && be32(m->p + 8) == 3);
    m = take(b, GCSS_OP_OK);
    CHECK(m && be32(m->p) == GCSS_OP_ATTR2);

    /* B joins: A hears about it */
    put32(p, found);
    memset(p + 4, 0, 20);
    send_msg(b, 0xa000, 32, GCSS_OP_JOIN, p, 24);
    m = take(b, GCSS_OP_OK);
    CHECK(m && be32(m->p) == GCSS_OP_JOIN && be32(m->p + 4) == a->room);
    b->room = a->room;
    m = take(a, GCSS_OP_JOIN);
    CHECK(m && m->n == 24 && !strcmp((const char *)m->p, b->name) && be32(m->p + 16) == b->uid);

    /* a changed attribute reaches B as op 8 {room, attribute, value} */
    set_attr(a, 13, "ARNB", 15);
    m = take(b, GCSS_OP_ATTR);
    CHECK(m && m->n == 12 && be32(m->p) == a->room && !memcmp(m->p + 4, "ARNB", 4) && be32(m->p + 8) == 15);
    CHECK(take(a, GCSS_OP_ATTR) == NULL);

    /* game data: broadcast and private, reliable */
    memcpy(p, "TURN", 4);
    send_msg(a, 0xa400, 0, GCSS_OP_DATA, p, 4);
    m = take(b, GCSS_OP_DATA);
    CHECK(m && m->n == 4 && !memcmp(m->p, "TURN", 4));
    put32(p, 1);
    put32(p + 4, a->uid);
    memcpy(p + 8, "ACK!", 4);
    send_msg(b, 0xa400, 0, GCSS_OP_PDATA, p, 12);
    m = take(a, GCSS_OP_PDATA);
    CHECK(m && m->n == 12 && !memcmp(m->p + 8, "ACK!", 4));

    /* full now? max 4: still open; the Ranked lobby has no rooms */
    CHECK_EQ(search(b, 0, 0, 0x7fff, 3, NULL), 1);
    login(&G[2]);
    enter_lobby(&G[2], "Ranked");
    CHECK_EQ(search(&G[2], 0, 0, 0x7fff, 3, NULL), 0);

    /* A leaves: B is told */
    send_msg(a, 0xa000, 14, GCSS_OP_LEAVE, p, 0);
    m = take(b, GCSS_OP_LEAVE);
    CHECK(m && be32(m->p) == a->uid);
}

/* ---- arenabase.dll's view of a SegaChat answer ---- */

typedef struct
{
    int status;
    int lines;
    int hdr_fields, row_fields;
    char f[64][40]; /* all fields, header first */
    int nf;
} af_answer;

/* 0x765c (status, message) and 0x7774 (fields, lines): -1 on an error
 * arenabase reports, -2 for rows of different sizes */
static int af_decode(const char *csv, af_answer *a)
{
    const char *p = csv, *nl;
    memset(a, 0, sizeof(*a));
    a->status = atoi(p);
    nl = strchr(p, '\n');
    if (!nl) {
        return -1;
    }
    p = nl + 1;
    if (a->status != 0) {
        return -1;
    }
    nl = strchr(p, '\n'); /* message */
    if (!nl) {
        return -1;
    }
    p = nl + 1;
    while (*p) {
        int fields = 1;
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        size_t i, k = 0;
        for (i = 0; i <= len; i++) {
            if (i == len || p[i] == '|') {
                if (a->nf < 64) {
                    memcpy(a->f[a->nf], p + k, (i - k) < 39 ? i - k : 39);
                    a->f[a->nf][(i - k) < 39 ? i - k : 39] = 0;
                    a->nf++;
                }
                k = i + 1;
                if (i < len) {
                    fields++;
                }
            }
        }
        a->lines++;
        if (a->lines == 1) {
            a->hdr_fields = fields;
        } else if (a->row_fields == 0) {
            a->row_fields = fields;
        } else if (a->row_fields != fields) {
            return -2;
        }
        p = e ? e + 1 : p + len;
    }
    return 0;
}

static const char *row_field(const af_answer *a, int row, int field)
{
    return a->f[a->hdr_fields + row * a->row_fields + field];
}

static gcs_scores *SC;

static int af_query(const char *user, const char *event, const char *items, af_answer *a)
{
    char req[1024], out[GCSS_AF_MAX_CSV + 64];
    snprintf(req, sizeof(req),
             " <itemlist> <item name=\"format\" value=\"csv\"/> <item name=\"$version\" value=\"1\"/>"
             " <item name=\"queryid\" value=\"1104\"/>%s </itemlist>       ",
             items);
    gcs_scores_af_query(SC, "1104", event, user, req, out, sizeof(out));
    if (g_verbose) {
        printf("%s %s:\n%s", user, event, out);
    }
    return af_decode(out, a);
}

static void af_report(const char *user, const char *match, gcs_scores_item *it, int n)
{
    char out[256];
    af_answer a;
    CHECK(gcs_scores_is_af(it[0].name));
    CHECK_EQ(gcs_scores_af_submit(SC, "1104", user, match, it, n, 1700000000L, out, sizeof(out)), 0);
    /* 0x7bec: one line of four fields */
    CHECK_EQ(af_decode(out, &a), 0);
    CHECK_EQ(a.lines, 1);
    CHECK_EQ(a.hdr_fields, 4);
}

static void ranked_match(const char *winner, const char *loser, const char *match, int pts)
{
    char p1[16], p2[16];
    gcs_scores_item w[] = { { "queryid", "7" }, { "$version", "1" }, { "win_lose", "1" }, { "draw", "0" }, { "opponent_id", "0" }, { "gametime", "321" }, { "roundsplayed", "2" }, { "$matchmakingMode", "ranked" } };
    gcs_scores_item l[8];
    gcs_scores_item sw[] = { { "queryid", "8" }, { "$version", "1" }, { "$ps_skilltype", "arena" }, { "$ps_points", p1 } };
    gcs_scores_item sl[] = { { "queryid", "9" }, { "$version", "1" }, { "$ps_skilltype", "arena" }, { "$ps_points", p2 } };
    memcpy(l, w, sizeof(l));
    l[2].value = "0";
    snprintf(p1, sizeof(p1), "%d", pts);
    snprintf(p2, sizeof(p2), "%d", -pts);
    af_report(winner, match, w, 8);
    af_report(loser, match, l, 8);
    af_report(winner, match, sw, 4);
    af_report(loser, match, sl, 4);
}

static void test_af_scores(void)
{
    af_answer a;
    long m1, m2;
    remove(DATA_FILE);
    SC = gcs_scores_create(DATA_FILE, io_log, NULL);
    CHECK(SC != NULL);

    /* getmatchid: one line of 6 fields, field 1 the id; ids differ */
    CHECK_EQ(af_query("alice", "getmatchid", "", &a), 0);
    CHECK(a.lines == 1 && a.hdr_fields == 6);
    m1 = atol(a.f[1]);
    CHECK_EQ(af_query("bob", "getmatchid", "", &a), 0);
    m2 = atol(a.f[1]);
    CHECK(m1 > 0 && m2 > 0 && m1 != m2);
    CHECK(!strcmp(a.f[0], "1104"));

    ranked_match("alice", "bob", "5001", 20);
    ranked_match("carol", "alice", "5002", 12);
    ranked_match("alice", "bob", "5003", 18);
    ranked_match("alice", "bob", "5003", 18); /* repeated: counted once */

    /* getplayer playerskills: header 7, rows 6 */
    CHECK_EQ(af_query("alice", "getplayer",
                      " <item name=\"$matchmakingMode\" value=\"ranked\"/>"
                      " <item name=\"board\" value=\"player_skills\"/> <item name=\"name\" value=\"alice\"/>",
                      &a),
             0);
    CHECK_EQ(a.hdr_fields, 7);
    CHECK_EQ(a.row_fields, 6);
    CHECK_EQ(a.lines, 2);
    CHECK(!strcmp(a.f[4], "playerskills"));
    CHECK(!strcmp(row_field(&a, 0, 0), "alice"));
    CHECK_EQ(atoi(row_field(&a, 0, 1)), 1);  /* rank */
    CHECK_EQ(atoi(row_field(&a, 0, 2)), 26); /* 20 - 12 + 18 */
    CHECK_EQ(atoi(row_field(&a, 0, 3)), 2);  /* wins */
    CHECK_EQ(atoi(row_field(&a, 0, 4)), 1);  /* losses */

    /* a player without games: a row with rank 0 */
    CHECK_EQ(af_query("dave", "getplayer", " <item name=\"board\" value=\"player_skills\"/>", &a), 0);
    CHECK(a.lines == 2 && a.row_fields == 6 && !strcmp(row_field(&a, 0, 0), "dave") && atoi(row_field(&a, 0, 1)) == 0);

    /* topn: header 6, rows 7, best first */
    CHECK_EQ(af_query("bob", "topn",
                      " <item name=\"board\" value=\"player_skills\"/> <item name=\"offset\" value=\"0\"/>"
                      " <item name=\"limit\" value=\"7\"/>",
                      &a),
             0);
    CHECK_EQ(a.hdr_fields, 6);
    CHECK_EQ(a.row_fields, 7);
    CHECK_EQ(a.lines, 4);
    CHECK(!strcmp(a.f[4], "playerskills"));
    CHECK_EQ(atoi(a.f[3]), 3);
    CHECK(!strcmp(row_field(&a, 0, 0), "alice"));
    CHECK(!strcmp(row_field(&a, 1, 0), "carol"));
    CHECK(!strcmp(row_field(&a, 2, 0), "bob"));
    CHECK_EQ(atoi(row_field(&a, 2, 2)), -38);

    /* proximitylist: header 6, rows 7, around the player */
    CHECK_EQ(af_query("bob", "proximitylist",
                      " <item name=\"board\" value=\"player_skills\"/> <item name=\"name\" value=\"bob\"/>"
                      " <item name=\"limit\" value=\"2\"/>",
                      &a),
             0);
    CHECK(a.hdr_fields == 6 && a.row_fields == 7 && a.lines == 3);
    CHECK(!strcmp(row_field(&a, 1, 0), "bob"));

    /* other numeric items are best values: highscores through ArenaFramework */
    {
        gcs_scores_item it[] = { { "queryid", "3" }, { "$version", "1" }, { "medal_val", "1500" } };
        af_report("alice", "", it, 3);
        it[2].value = "900";
        af_report("bob", "", it, 3);
    }
    CHECK_EQ(af_query("bob", "topn",
                      " <item name=\"board\" value=\"highscores\"/> <item name=\"stat\" value=\"medal_val\"/>", &a),
             0);
    CHECK(a.hdr_fields == 7 && a.row_fields == 4 && a.lines == 3 && !strcmp(a.f[4], "highscores"));
    CHECK(!strcmp(row_field(&a, 0, 0), "alice") && atoi(row_field(&a, 0, 2)) == 1500);
    CHECK_EQ(af_query("bob", "getplayer",
                      " <item name=\"board\" value=\"highscores\"/> <item name=\"stat\" value=\"medal_val\"/>", &a),
             0);
    CHECK(a.hdr_fields == 6 && a.row_fields == 4 && a.lines == 2 && !strcmp(row_field(&a, 0, 0), "bob") &&
          atoi(row_field(&a, 0, 1)) == 2);
    CHECK_EQ(af_query("bob", "topn",
                      " <item name=\"board\" value=\"hsfilter\"/> <item name=\"stat\" value=\"medal_val\"/>", &a),
             0);
    CHECK(a.hdr_fields == 7 && a.row_fields == 3 && !strcmp(a.f[4], "hsfilter"));

    /* the skill points survive a restart */
    gcs_scores_destroy(SC);
    SC = gcs_scores_create(DATA_FILE, io_log, NULL);
    CHECK_EQ(af_query("bob", "getplayer", " <item name=\"board\" value=\"player_skills\"/>", &a), 0);
    CHECK(a.lines == 2 && atoi(row_field(&a, 0, 2)) == -38 && atoi(row_field(&a, 0, 4)) == 2);
    CHECK_EQ(af_query("bob", "getmatchid", "", &a), 0);
    CHECK(atol(a.f[1]) > m2); /* match ids are not reused */

    /* Ashen's requests (no $version) still get Ashen's answer */
    CHECK(!gcs_scores_is_af("<itemlist><item name=\"queryid\" value=\"3dabc\" /></itemlist>"));
    gcs_scores_destroy(SC);
    remove(DATA_FILE);
}

/* ---- the same through SegaChat (gcs_direct, gcs_native) ---- */

typedef struct
{
    char rx[16384];
    size_t len;
} fake_sock;

static void io_tcp_send(void *ctx, void *sock, const gc_u8 *d, size_t len)
{
    fake_sock *s = (fake_sock *)sock;
    (void)ctx;
    if (s->len + len < sizeof(s->rx)) {
        memcpy(s->rx + s->len, d, len);
        s->len += len;
        s->rx[s->len] = 0;
    }
}

static void io_tcp_close(void *ctx, void *sock, int reset)
{
    (void)ctx, (void)sock, (void)reset;
}

static void io_udp_send(void *ctx, gc_u16 service, const gc_u8 ip[4], gc_u16 port, const gc_u8 *d, size_t len)
{
    (void)ctx, (void)service, (void)ip, (void)port, (void)d, (void)len;
}

static void s_write(void *c, gcs_conn *k, const gc_u8 *d, size_t n)
{
    (void)c, (void)k, (void)d, (void)n;
}

static void s_close(void *c, gcs_conn *k)
{
    (void)c, (void)k;
}

static const char *say(gcs_direct *d, fake_sock *s, const char *text)
{
    s->len = 0;
    s->rx[0] = 0;
    gcs_direct_tcp_input(d, s, (const gc_u8 *)text, strlen(text), 1000);
    return s->rx;
}

/* item list escaped as sc_lib puts it into <request> */
static void escape(char *out, size_t cap, const char *in)
{
    size_t k = 0;
    for (; *in && k + 8 < cap; in++) {
        const char *e = *in == '<' ? "&lt;" : *in == '>' ? "&gt;"
                                          : *in == '"'   ? "&quot;"
                                                         : NULL;
        if (e) {
            k += (size_t)snprintf(out + k, cap - k, "%s", e);
        } else {
            out[k++] = *in;
        }
    }
    out[k] = 0;
}

/* sc_lib: untyped message, id segachat_retrieve_resp; arenabase: the
 * request's event type and the game class */
static int sc_response(const char *rx, const char *from, const char *event, char *csv, size_t cap)
{
    const char *m = strstr(rx, "<message"), *gt, *a, *e;
    char want[64];
    size_t k, i, o = 0;
    if (!m || !(gt = strchr(m, '>'))) {
        return -1;
    }
    for (a = m; a < gt; a++) {
        if (!strncmp(a, " type=", 6)) {
            return -1;
        }
    }
    snprintf(want, sizeof(want), "event_type='%s'", event);
    if (!strstr(m, "id='segachat_retrieve_resp'") || !strstr(m, "game_class_id='1104'") || !strstr(m, want)) {
        return -1;
    }
    snprintf(want, sizeof(want), "from='%s@", from);
    if (!strstr(m, want)) {
        return -1;
    }
    a = strstr(gt, "<retrieve><response>");
    e = strstr(gt, "</response></retrieve></message>");
    if (!a || !e) {
        return -1;
    }
    a += strlen("<retrieve><response>");
    k = (size_t)(e - a);
    for (i = 0; i < k && o + 1 < cap; i++) {
        if (!strncmp(a + i, "&amp;", 5)) {
            csv[o++] = '&';
            i += 4;
        } else if (!strncmp(a + i, "&lt;", 4)) {
            csv[o++] = '<';
            i += 3;
        } else if (!strncmp(a + i, "&gt;", 4)) {
            csv[o++] = '>';
            i += 3;
        } else {
            csv[o++] = a[i];
        }
    }
    csv[o] = 0;
    return 0;
}

static void test_session(void)
{
    static const gc_u8 ip[4] = { 203, 0, 113, 78 };
    gcs_config cfg;
    gcs_io sio;
    gcs_direct_io dio;
    gcs_core *core;
    gcs_direct *d;
    fake_sock s;
    const char *r;
    char q[3000], esc[2000], csv[GCSS_AF_MAX_CSV + 64];
    af_answer a;

    remove(ACC_FILE);
    remove(DATA_FILE);
    gcs_default_config(&cfg);
    strcpy(cfg.arena_accounts, ACC_FILE);
    strcpy(cfg.scores_data, DATA_FILE);
    memset(&sio, 0, sizeof(sio));
    sio.write = s_write;
    sio.close = s_close;
    core = gcs_core_create(&cfg, &sio, NULL, 1);
    CHECK(core != NULL);
    memset(&dio, 0, sizeof(dio));
    dio.tcp_send = io_tcp_send;
    dio.tcp_close = io_tcp_close;
    dio.udp_send = io_udp_send;
    d = gcs_direct_create(core, &dio, NULL);
    CHECK(d != NULL);

    memset(&s, 0, sizeof(s));
    CHECK_EQ(gcs_direct_tcp_open(d, &s, ip, 3100, GCSD_XMPP, 1000), 0);
    say(d, &s, "<?xml version='1.0'?><stream:stream to='ngage-auth' xmlns='jabber:client' "
               "xmlns:stream='http://etherx.jabber.org/streams'>");
    r = say(d, &s, "<iq type='set' id='segachat_auth'><query xmlns='jabber:iq:auth'><username>Worm</username>"
                   "<password>pw</password><resource>segachat</resource></query></iq>");
    CHECK(strstr(r, "type='result'") != NULL);

    /* getmatchid to retrieval1104 (arena.amr JabberServicesName) */
    escape(esc, sizeof(esc),
           " <itemlist> <item name=\"format\" value=\"csv\"/> <item name=\"$version\" value=\"1\"/>"
           " <item name=\"queryid\" value=\"1104\"/> </itemlist>       ");
    snprintf(q, sizeof(q),
             "<message to='retrieval1104@ngage-auth' id='segachat_retrieve_req' event_type='getmatchid' "
             "game_class_id='1104' snap_name='Worm'><retrieve><request>%s</request></retrieve></message>",
             esc);
    r = say(d, &s, q);
    CHECK_EQ(sc_response(r, "retrieval1104", "getmatchid", csv, sizeof(csv)), 0);
    CHECK_EQ(af_decode(csv, &a), 0);
    CHECK(a.hdr_fields == 6 && atol(a.f[1]) > 0);

    /* the skill points of a ranked game to reporter1104: answered */
    r = say(d, &s,
            "<message to='reporter1104@ngage-auth' id='segachat_send_event' event_type='submit' "
            "game_class_id='1104' match_id='77' snap_name='Worm' client_time='1'>"
            "<item name='$ps_skilltype'><value>arena</value></item><item name='$ps_points'><value>25</value></item>"
            "<item name='$version'><value>1</value></item><item name='queryid'><value>1104</value></item></message>");
    CHECK_EQ(sc_response(r, "reporter1104", "submit", csv, sizeof(csv)), 0);
    CHECK_EQ(af_decode(csv, &a), 0);
    CHECK(a.lines == 1 && a.hdr_fields == 4);

    /* "Points Total" */
    escape(esc, sizeof(esc),
           " <itemlist> <item name=\"format\" value=\"csv\"/> <item name=\"$version\" value=\"1\"/>"
           " <item name=\"$matchmakingMode\" value=\"ranked\"/> <item name=\"board\" value=\"player_skills\"/>"
           " <item name=\"queryid\" value=\"1104\"/> <item name=\"name\" value=\"Worm\"/> </itemlist>       ");
    snprintf(q, sizeof(q),
             "<message to='retrieval1104@ngage-auth' id='segachat_retrieve_req' event_type='getplayer' "
             "game_class_id='1104' snap_name='Worm'><retrieve><request>%s</request></retrieve></message>",
             esc);
    r = say(d, &s, q);
    CHECK_EQ(sc_response(r, "retrieval1104", "getplayer", csv, sizeof(csv)), 0);
    CHECK_EQ(af_decode(csv, &a), 0);
    CHECK(a.hdr_fields == 7 && a.row_fields == 6 && !strcmp(a.f[4], "playerskills"));
    CHECK(a.lines == 2 && atoi(row_field(&a, 0, 1)) == 1 && atoi(row_field(&a, 0, 2)) == 25);

    gcs_direct_tcp_gone(d, &s);
    gcs_direct_destroy(d);
    gcs_core_destroy(core);
    remove(ACC_FILE);
    remove(DATA_FILE);
}

int main(int argc, char **argv)
{
    gcs_snap_io io = { io_send, io_log, io_auth };
    g_verbose = argc > 1 && !strcmp(argv[1], "-v");
    G[0].name = "alice";
    G[0].port = 4001;
    G[1].name = "bob";
    G[1].port = 4002;
    G[2].name = "carol";
    G[2].port = 4003;
    S = gcs_snap_create(&io, NULL);
    gcs_snap_add_game(S, gcs_wwp_game());
    RUN(test_rooms);
    gcs_snap_destroy(S);
    RUN(test_af_scores);
    RUN(test_session);
    TEST_MAIN_END();
}
