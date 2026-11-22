/** @file gcs_ptg.c
 *
 *  Pathway to Glory: Ikusa Islands and Pathway to Glory game server, see
 *  gcs_ptg.h and docs/ARENA.md ("Pathway to Glory: Ikusa Islands").
 *
 *  One game per SNAP game room.  The room's creator is the host (the
 *  game's MASTER; ArenaFramework asks the room attribute HOST, which
 *  gcs_snap.c answers with the creator).  The server stands where the
 *  host's phone stands in a Bluetooth game (6R82.APP 0x2e7a0): messages
 *  of a client go to the host, messages of the host go to the clients,
 *  and the types the host passes on to everybody go to all other
 *  players.  On top of that come the bundles and markers that only the
 *  Arena server sends (types 32 and 34), the keep alive and the GPRS
 *  lobby: every phone, the host included, asks the server for a player
 *  id (type 0 -> 9) and the server hands out ids and sides and passes
 *  the player records on (6R72.APP 0x93024).
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#include "gcs_ptg.h"
#include "gcs_rcodec.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIRE_MAX     (4 + GCS_PTG_FRAG) /* 600: the game splits longer messages */
#define BUNDLE_MAX   2048               /* flush a command bundle at this size */
#define SYNC_MAX     65000              /* host's synchronisation records per bundle */
#define SYNC_WAIT_MS 3000               /* records without end marker go on alone */
#define DUP_MS       1500               /* identical copies of one message */
#define SIDE_MAX     4                  /* players per side (BT host 0x8f17c) */
#define REC_LEN      40                 /* player record (type 10) */
#define GDATA_MAX    1024               /* game settings (type 14, 136 bytes) */
#define PING_MS      2000               /* start pings until the phone answers */
#define START_MS     500                /* barrier: start this much later */
#define ST_PLAYING   0x80000000u        /* room status bit: game in progress */

typedef struct
{
    gc_u32 uid;
    gc_u8 tag;       /* 7 bit message counter, echoed in type 33 */
    gc_u16 ack_perm; /* last acknowledgement: 23/34 count, 32 count */
    gc_u16 ack_cmds;
    gc_u8 *bundle; /* commands of the others, not sent yet */
    size_t bundle_n;
    /* GPRS lobby */
    int lobby;   /* got its player id (type 9) */
    gc_u32 id;   /* 1 = master, clients from 2 */
    int has_rec; /* has a player record with a side */
    gc_u32 side; /* 0 or 1 */
    gc_u8 rec[REC_LEN];
    /* last message from this player that may come several times */
    gc_u8 last_type;
    gc_u32 last_hash;
    gc_u32 last_t;
    int live;      /* the phone's network layer runs: it sent game data */
    int ready;     /* sent its type 15 for the current barrier */
    gc_u32 ping_t; /* last start ping */
} player;

typedef struct
{
    int used;
    int classic; /* Pathway to Glory (6R72): no restart bit, no tag */
    gc_u32 room;
    gc_u32 host;   /* 0: none yet or left */
    int host_gone; /* the host left: the game is over for everybody */
    int started;   /* commands went round: leaving ends the game */
    player pl[GCS_PTG_MAX_PLAYERS];
    gc_u8 *sync; /* host's records (type 21) waiting for the end marker */
    size_t sync_n;
    int sync_count;
    gc_u32 sync_t;
    int perm_open; /* host sent 23, the clients make the 41 themselves */
    int playing;   /* room status had bit 31 at the last poll */
    /* GPRS lobby */
    gc_u32 next_id;          /* last id handed out (1: the master's) */
    gc_u32 build;            /* the master's build (type 0) */
    int have_build;
    gc_u8 gdata[GDATA_MAX]; /* the master's game settings (type 14) */
    size_t gdata_n;
    int barriers; /* completed type 15 barriers */
} game;

struct gcs_ptg;

/* one registration per game; the callbacks get this as their context */
typedef struct
{
    struct gcs_ptg *p;
    int classic;
} title;

struct gcs_ptg
{
    void (*log)(void *ctx, int level, const char *line);
    void *ctx;
    gcs_snap_game reg[2]; /* Ikusa Islands, Pathway to Glory */
    title t[2];
    gcs_snap *snap; /* from the last callback */
    gc_u32 now;
    game g[GCS_PTG_MAX_GAMES];
    gcs_ptg_stats st;
};

static void plog(gcs_ptg *p, int level, const char *fmt, ...)
{
    char line[512];
    va_list ap;
    if (!p->log) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    p->log(p->ctx, level, line);
}

#define LOG_W(...) plog(p, 1, "ptg: " __VA_ARGS__)
#define LOG_I(...) plog(p, 2, "ptg: " __VA_ARGS__)
#define LOG_D(...) plog(p, 3, "ptg: " __VA_ARGS__)

static gc_u16 le16(const gc_u8 *d)
{
    return (gc_u16)(d[0] | (d[1] << 8));
}

static gc_u32 le32(const gc_u8 *d)
{
    return (gc_u32)d[0] | ((gc_u32)d[1] << 8) | ((gc_u32)d[2] << 16) | ((gc_u32)d[3] << 24);
}

static void put16(gc_u8 *d, gc_u16 v)
{
    d[0] = (gc_u8)v;
    d[1] = (gc_u8)(v >> 8);
}

static void put32le(gc_u8 *d, gc_u32 v)
{
    d[0] = (gc_u8)v;
    d[1] = (gc_u8)(v >> 8);
    d[2] = (gc_u8)(v >> 16);
    d[3] = (gc_u8)(v >> 24);
}

/* FNV-1a over type and data: tells repeated copies of a message apart */
static gc_u32 hash(gc_u8 type, const gc_u8 *d, size_t n)
{
    gc_u32 h = 2166136261u ^ type;
    size_t i;
    h *= 16777619u;
    for (i = 0; i < n; i++) {
        h = (h ^ d[i]) * 16777619u;
    }
    return h ^ (gc_u32)n;
}

/* ---- games and players ---- */

static game *game_of_room(gcs_ptg *p, gc_u32 room, int create)
{
    int i, free_i = -1;
    if (!room) {
        return NULL;
    }
    for (i = 0; i < GCS_PTG_MAX_GAMES; i++) {
        if (p->g[i].used && p->g[i].room == room) {
            return &p->g[i];
        }
        if (!p->g[i].used && free_i < 0) {
            free_i = i;
        }
    }
    if (!create || free_i < 0) {
        return NULL;
    }
    memset(&p->g[free_i], 0, sizeof(game));
    p->g[free_i].used = 1;
    p->g[free_i].room = room;
    p->g[free_i].next_id = 1; /* the master is 1 */
    return &p->g[free_i];
}

static player *player_of(game *g, gc_u32 uid)
{
    int i;
    for (i = 0; i < GCS_PTG_MAX_PLAYERS; i++) {
        if (g->pl[i].uid == uid) {
            return &g->pl[i];
        }
    }
    return NULL;
}

static int players(const game *g)
{
    int i, n = 0;
    for (i = 0; i < GCS_PTG_MAX_PLAYERS; i++) {
        n += g->pl[i].uid != 0;
    }
    return n;
}

static void game_free(game *g)
{
    int i;
    for (i = 0; i < GCS_PTG_MAX_PLAYERS; i++) {
        free(g->pl[i].bundle);
    }
    free(g->sync);
    memset(g, 0, sizeof(*g));
}

/* ---- sending ---- */

/* One message to one player, split like the game splits (6R82.APP
 * 0x84b9c): every piece carries the header; the first one the length
 * (compressed: zlib length and unpacked size), then GCS_PTG_FRAG data
 * bytes per piece (the first compressed piece 592).  Bundles of
 * synchronisation records and permanent damage data always go compressed:
 * the game unpacks type 34, and queues its own 41 behind a 23, only after
 * a compressed or split message; a short plain one is queued as it is. */
static void send_to(gcs_ptg *p, game *g, player *pl, gc_u8 type, const gc_u8 *d, size_t n)
{
    gc_u8 w[WIRE_MAX];
    gc_u8 *z = NULL;
    const gc_u8 *src = d;
    size_t total = n, off = 0, first;
    int pack = type == GCS_PTG_SYNCS || type == GCS_PTG_PERM || 4 + n > WIRE_MAX;
    gc_u8 h0, h1;
    if (!pl || !pl->uid || !p->snap) {
        return;
    }
    if (n > GCS_PTG_MAX_UNPACK) {
        LOG_W("type %u (%u bytes) for %08x too long, dropped", type, (unsigned)n, pl->uid);
        return;
    }
    if (pack) {
        z = (gc_u8 *)malloc(gcs_zlib_stored_bound(n));
        if (!z) {
            return;
        }
        total = gcs_zlib_stored(d, n, z);
        src = z;
    }
    if (total > 0xffff) {
        LOG_W("type %u for %08x: %u bytes do not fit", type, pl->uid, (unsigned)total);
        free(z);
        return;
    }
    if (g->classic) {
        /* Pathway to Glory takes the whole first byte as the type (more
         * than 51 is an error) and the whole header as the queue number
         * (6R72.APP 0x79884): no restart bit, no tag */
        h0 = type;
        h1 = (gc_u8)(pack ? 0x80 : 0);
    } else {
        /* bit 7 on every message: the game restarts its acknowledgement
         * counters and takes the message even after it dropped its
         * queues (6R82.APP 0x845a8 sets the drop flag, 0x84c44 clears it) */
        h0 = (gc_u8)(type | 0x80);
        h1 = (gc_u8)((pl->tag & 0x7f) | (pack ? 0x80 : 0));
        pl->tag = (gc_u8)((pl->tag + 1) & 0x7f);
    }
    w[0] = h0;
    w[1] = h1;
    put16(w + 2, (gc_u16)total);
    if (pack) {
        put32le(w + 4, (gc_u32)n);
        first = total < GCS_PTG_FRAG - 4 ? total : GCS_PTG_FRAG - 4;
        memcpy(w + 8, src, first);
        gcs_snap_send_game_data(p->snap, pl->uid, w, 8 + first);
    } else {
        first = total < GCS_PTG_FRAG ? total : GCS_PTG_FRAG;
        if (first) {
            memcpy(w + 4, src, first);
        }
        gcs_snap_send_game_data(p->snap, pl->uid, w, 4 + first);
    }
    off = first;
    while (off < total) {
        size_t k = total - off < GCS_PTG_FRAG ? total - off : GCS_PTG_FRAG;
        put16(w + 2, (gc_u16)k);
        memcpy(w + 4, src + off, k);
        gcs_snap_send_game_data(p->snap, pl->uid, w, 4 + k);
        off += k;
    }
    p->st.messages_out++;
    LOG_D("-> %08x type %u, %u bytes%s", pl->uid, type, (unsigned)n, pack ? " (zlib)" : "");
    free(z);
}

static void send_status(gcs_ptg *p, game *g, player *pl, gc_u16 code)
{
    gc_u8 d[2];
    put16(d, code);
    send_to(p, g, pl, GCS_PTG_STATUS, d, 2);
}

static void flush_bundle(gcs_ptg *p, game *g, player *pl)
{
    if (pl->bundle_n) {
        send_to(p, g, pl, GCS_PTG_COMMANDS, pl->bundle, pl->bundle_n);
        pl->bundle_n = 0;
        g->started = 1;
    }
}

/* an entry {u16 length, data} for a bundle */
static int append(gc_u8 **buf, size_t *n, size_t cap, const gc_u8 *d, size_t len)
{
    gc_u8 *b;
    if (*n + 2 + len > cap) {
        return -1;
    }
    if (!*buf) {
        *buf = (gc_u8 *)malloc(cap);
        if (!*buf) {
            return -1;
        }
    }
    b = *buf;
    put16(b + *n, (gc_u16)len);
    memcpy(b + *n + 2, d, len);
    *n += 2 + len;
    return 0;
}

static void bundle_command(gcs_ptg *p, game *g, player *to, const gc_u8 *d, size_t n)
{
    if (to->bundle_n + 2 + n > BUNDLE_MAX) {
        flush_bundle(p, g, to);
    }
    if (append(&to->bundle, &to->bundle_n, BUNDLE_MAX > 2 + n ? BUNDLE_MAX : 2 + n, d, n)) {
        /* longer than a bundle: alone */
        send_to(p, g, to, GCS_PTG_COMMAND, d, n);
    }
}

static void flush_sync(gcs_ptg *p, game *g, int with_end)
{
    int i;
    for (i = 0; i < GCS_PTG_MAX_PLAYERS; i++) {
        player *pl = &g->pl[i];
        if (!pl->uid || pl->uid == g->host) {
            continue;
        }
        if (with_end) {
            send_to(p, g, pl, GCS_PTG_SYNCS, g->sync ? g->sync : (const gc_u8 *)"", g->sync_n);
        } else {
            /* no end marker came: the records alone, the game makes no 35 */
            size_t pos = 0;
            while (g->sync && pos + 2 <= g->sync_n) {
                size_t l = le16(g->sync + pos);
                send_to(p, g, pl, GCS_PTG_SYNC, g->sync + pos + 2, l);
                pos += 2 + l;
            }
        }
    }
    LOG_I("room %08x: %d synchronisation records (%u bytes) to the clients%s", g->room, g->sync_count,
          (unsigned)g->sync_n, with_end ? "" : " without end marker");
    g->sync_n = 0;
    g->sync_count = 0;
}

/* ---- routing ---- */

static void to_others(gcs_ptg *p, game *g, player *from, gc_u8 type, const gc_u8 *d, size_t n)
{
    int i;
    for (i = 0; i < GCS_PTG_MAX_PLAYERS; i++) {
        player *pl = &g->pl[i];
        if (pl->uid && pl != from) {
            send_to(p, g, pl, type, d, n);
        }
    }
}

/* ---- GPRS lobby (6R72.APP 0x93024, 6R82.APP 0xa1050) ----
 *
 * Every phone, the host included, starts the GPRS lobby with type 0 and
 * waits ("Waiting for side selection", wait_for_server.txt) for type 9
 * from the game server: its player id and whether it is the master.  The
 * master then sends the game settings (14) and its player record (10);
 * everybody waits for the settings (14, the master too), sends its
 * record, waits for its own record back with id, side and the reason for
 * the side (+8: 0 default side, 1 default side full, 2 to balance the
 * forces, 3 nobody on the other side; globalbg_join_verification*.txt),
 * answers 37 and enters the lobby loop: records of the others (10), side
 * changes (11), players leaving or removed (12), the start (13).  In a
 * Bluetooth game the host's phone does the server's part (0x8eb3c). */

static player *player_by_id(game *g, gc_u32 id)
{
    int i;
    for (i = 0; i < GCS_PTG_MAX_PLAYERS; i++) {
        if (g->pl[i].uid && g->pl[i].lobby && g->pl[i].id == id) {
            return &g->pl[i];
        }
    }
    return NULL;
}

static int side_count(const game *g, gc_u32 side, const player *except)
{
    int i, n = 0;
    for (i = 0; i < GCS_PTG_MAX_PLAYERS; i++) {
        const player *pl = &g->pl[i];
        n += pl->uid && pl->has_rec && pl != except && pl->side == side;
    }
    return n;
}

/* 'type' to every player in the lobby that has a record (and to 'from'
 * as well when 'self') */
static void to_recs(gcs_ptg *p, game *g, player *from, int self, gc_u8 type, const gc_u8 *d, size_t n)
{
    int i;
    for (i = 0; i < GCS_PTG_MAX_PLAYERS; i++) {
        player *pl = &g->pl[i];
        if (pl->uid && pl->has_rec && (self || pl != from)) {
            send_to(p, g, pl, type, d, n);
        }
    }
}

static void send_remove(gcs_ptg *p, game *g, player *gone, int self)
{
    gc_u8 d[4];
    put32le(d, gone->id);
    gone->has_rec = 0;
    to_recs(p, g, gone, 0, GCS_PTG_REMOVE, d, 4);
    if (self) {
        send_to(p, g, gone, GCS_PTG_REMOVE, d, 4);
    }
}

static void lobby_hello(gcs_ptg *p, game *g, player *from, const gc_u8 *d, size_t n)
{
    int master = g->host && from->uid == g->host;
    gc_u8 w[8];
    gc_u32 build;
    if (n < 8) {
        LOG_W("room %08x: type 0 from %08x with %u bytes, ignored", g->room, from->uid, (unsigned)n);
        return;
    }
    build = le32(d + 4);
    if (master) {
        if (!d[0]) {
            LOG_W("room %08x: host %08x has no game settings", g->room, from->uid);
        }
        g->build = build;
        g->have_build = 1;
    } else if (g->have_build && build != g->build) {
        LOG_W("room %08x: %08x has build %08x, the host %08x", g->room, from->uid, build, g->build);
        send_status(p, g, from, GCS_PTG_ST_BUILD);
        return;
    }
    if (!from->lobby) {
        from->lobby = 1;
        /* ids start at 1, as the Bluetooth host hands them out (0x8e2ec:
         * its own 1, then 2, 3, ... per client, 0x8ebb4); 0 is no player */
        from->id = master ? 1 : ++g->next_id;
    }
    memset(w, 0, sizeof(w));
    put32le(w, from->id);
    w[4] = (gc_u8)master;
    send_to(p, g, from, GCS_PTG_WELCOME, w, sizeof(w));
    LOG_I("room %08x: %08x build %08x, player id %u%s", g->room, from->uid, build, from->id,
          master ? " (master)" : "");
    if (!master && g->gdata_n) {
        send_to(p, g, from, GCS_PTG_GAMEDATA, g->gdata, g->gdata_n);
    }
}

static void lobby_game_data(gcs_ptg *p, game *g, player *from, const gc_u8 *d, size_t n)
{
    int i;
    if (from->uid != g->host) {
        LOG_W("room %08x: game settings from %08x, not the host, ignored", g->room, from->uid);
        return;
    }
    if (n > GDATA_MAX) {
        LOG_W("room %08x: game settings of %u bytes, ignored", g->room, (unsigned)n);
        return;
    }
    memcpy(g->gdata, d, n);
    g->gdata_n = n;
    /* the master waits for them as well */
    for (i = 0; i < GCS_PTG_MAX_PLAYERS; i++) {
        if (g->pl[i].uid && g->pl[i].lobby) {
            send_to(p, g, &g->pl[i], GCS_PTG_GAMEDATA, d, n);
        }
    }
}

static void lobby_player(gcs_ptg *p, game *g, player *from, const gc_u8 *d, size_t n)
{
    int first = !from->has_rec, i;
    gc_u32 side = from->side, reason = 0;
    if (!from->lobby || n < 12) {
        LOG_W("room %08x: player record from %08x (%u bytes) before its id, ignored", g->room, from->uid,
              (unsigned)n);
        return;
    }
    if (first) {
        /* +4: the default side of the player's GPRS options */
        gc_u32 want = le32(d + 4);
        int n0 = side_count(g, 0, from), n1 = side_count(g, 1, from);
        if (want > 1) {
            want = n0 <= n1 ? 0 : 1;
        }
        side = want;
        if (side_count(g, want, from) > side_count(g, 1 - want, from)) {
            side = 1 - want;
            reason = side_count(g, side, from) ? 2 : 3;
        } else if (side_count(g, want, from) >= SIDE_MAX) {
            side = 1 - want;
            reason = 1;
        }
        LOG_I("room %08x: player %u (%08x) on side %u (default %u, reason %u)", g->room, from->id, from->uid, side,
              want, reason);
    }
    memset(from->rec, 0, REC_LEN);
    memcpy(from->rec, d, n < REC_LEN ? n : REC_LEN);
    put32le(from->rec, from->id);
    put32le(from->rec + 4, side);
    put32le(from->rec + 8, reason);
    from->side = side;
    from->has_rec = 1;
    /* its own record first: the game takes the first 10 after its own
     * as the answer and gives up if the id is not its own (0x93720) */
    send_to(p, g, from, GCS_PTG_PLAYER, from->rec, REC_LEN);
    to_recs(p, g, from, 0, GCS_PTG_PLAYER, from->rec, REC_LEN);
    if (first) {
        for (i = 0; i < GCS_PTG_MAX_PLAYERS; i++) {
            player *pl = &g->pl[i];
            if (pl->uid && pl->has_rec && pl != from) {
                send_to(p, g, from, GCS_PTG_PLAYER, pl->rec, REC_LEN);
            }
        }
    }
}

static void lobby_side(gcs_ptg *p, game *g, player *from, const gc_u8 *d, size_t n)
{
    player *who;
    gc_u32 side;
    gc_u8 w[8];
    if (n < 8) {
        return;
    }
    who = player_by_id(g, le32(d));
    side = le32(d + 4);
    if (!who || !who->has_rec || side > 1 || (who != from && from->uid != g->host)) {
        LOG_D("room %08x: side change %u -> %u from %08x refused", g->room, le32(d), side, from->uid);
        return;
    }
    if (side != who->side && side_count(g, side, who) >= SIDE_MAX) {
        LOG_I("room %08x: side %u full, player %u stays", g->room, side, who->id);
        return;
    }
    who->side = side;
    put32le(who->rec + 4, side);
    put32le(w, who->id);
    put32le(w + 4, side);
    to_recs(p, g, from, 1, GCS_PTG_SIDE, w, 8);
    LOG_I("room %08x: player %u now on side %u", g->room, who->id, side);
}

static void lobby_remove(gcs_ptg *p, game *g, player *from, const gc_u8 *d, size_t n)
{
    player *who = n >= 4 ? player_by_id(g, le32(d)) : NULL;
    if (!who || !who->has_rec) {
        return;
    }
    if (who == from) {
        LOG_I("room %08x: player %u leaves", g->room, who->id);
        send_remove(p, g, who, 0);
    } else if (from->uid == g->host) {
        LOG_I("room %08x: the host removes player %u", g->room, who->id);
        send_remove(p, g, who, 1);
    }
}

/* returns 1 when the message belonged to the lobby */
static int lobby(gcs_ptg *p, game *g, player *from, gc_u8 type, const gc_u8 *d, size_t n)
{
    int i;
    switch (type) {
    case GCS_PTG_HELLO:
        lobby_hello(p, g, from, d, n);
        return 1;
    case GCS_PTG_GAMEDATA:
        lobby_game_data(p, g, from, d, n);
        return 1;
    case GCS_PTG_PLAYER:
        lobby_player(p, g, from, d, n);
        return 1;
    case GCS_PTG_SIDE:
        lobby_side(p, g, from, d, n);
        return 1;
    case GCS_PTG_REMOVE:
        lobby_remove(p, g, from, d, n);
        return 1;
    case GCS_PTG_JOINED:
        if (n >= 4 && !le32(d) && from->has_rec) {
            LOG_I("room %08x: player %u declined side %u", g->room, from->id, from->side);
            send_remove(p, g, from, 0);
        }
        return 1;
    case GCS_PTG_START:
        if (from->uid != g->host) {
            return 0; /* to the host, as before */
        }
        /* The master's 13 is only a request ({u32 players}, the rest is
         * whatever was on its stack, 0x939d4); the start the phones
         * read (0x93d04) is {u32, u32 mission, u8, u8, u8 late join,
         * u8 continue}: with the last byte 0 the mission object keeps the
         * campaign's own start (mp_campaigns/<name>.txt "start_from",
         * 0xcdfc0), with 1 it would take mission and the two bytes from
         * here.  The master waits for its own copy too. */
        {
            gc_u8 w[12];
            memset(w, 0, sizeof(w));
            if (n >= 4) {
                memcpy(w, d, 4);
            }
            LOG_I("room %08x: the host starts the game", g->room);
            for (i = 0; i < GCS_PTG_MAX_PLAYERS; i++) {
                if (g->pl[i].uid && g->pl[i].lobby) {
                    g->pl[i].ready = 0;
                    send_to(p, g, &g->pl[i], type, w, sizeof(w));
                }
            }
            g->barriers = 0;
        }
        return 1;
    case GCS_PTG_WELCOME:
    case GCS_PTG_COUNTDOWN:
        LOG_W("room %08x: %08x sent server message type %u, ignored", g->room, from->uid, type);
        return 1;
    default:
        return 0;
    }
}

/* ---- GPRS game: what the original game server did ----
 *
 * In a GPRS game no phone is the host: the lobby hands every phone the
 * part of a client (6R72.APP 0x6dbc0: sync object with master 0, battle
 * object with host 0).  So the server answers what a host phone would
 * answer in a Bluetooth game. */

/* Type 15 barrier (sync object 0x131d58, battle start 0xc336c): every
 * phone drops old 15s, sends one and waits for one back; the second
 * word of the answer is the start time in the server's clock (the
 * phones convert it with the offset they learnt from type 31).  Once
 * every player of the game has sent its 15, all get the answer.  Before
 * every mission each phone also waits for the end of the synchronisation
 * records (35, queued behind a 34) and for the permanent damage data (41,
 * queued behind a 23; 0xd03e4 with the GPRS flag +0x480).  The server
 * keeps no damage between missions, so both go out empty after every
 * barrier; extra empty ones stay queued and are harmless. */
static void barrier_check(gcs_ptg *p, game *g)
{
    gc_u8 w[8];
    int i, n = 0;
    for (i = 0; i < GCS_PTG_MAX_PLAYERS; i++) {
        player *pl = &g->pl[i];
        if (pl->uid && pl->lobby) {
            if (!pl->ready) {
                return;
            }
            n++;
        }
    }
    if (!n) {
        return;
    }
    put32le(w, 1);
    put32le(w + 4, p->now + START_MS);
    g->barriers++;
    for (i = 0; i < GCS_PTG_MAX_PLAYERS; i++) {
        player *pl = &g->pl[i];
        if (pl->uid && pl->lobby) {
            pl->ready = 0;
            send_to(p, g, pl, GCS_PTG_READY, w, sizeof(w));
            send_to(p, g, pl, GCS_PTG_SYNCS, (const gc_u8 *)"", 0);
            send_to(p, g, pl, GCS_PTG_PERM, (const gc_u8 *)"", 0);
        }
    }
    LOG_I("room %08x: barrier %d, %d players", g->room, g->barriers, n);
}

/* Type 31, 20 bytes {u32 sent, u32 host received, u32 host sent, u32,
 * u16, u16}: the clock ping a phone sends at the start of a battle
 * (0xc3de8, which waits for all answers) and every few hundred frames
 * after (0xc3ecc).  The host fills in its clock twice (0xc3f80) and
 * sends it back; the server uses its own milliseconds. */
static void answer_time(gcs_ptg *p, game *g, player *from, const gc_u8 *d, size_t n)
{
    gc_u8 w[32];
    if (n < 12 || n > sizeof(w)) {
        LOG_W("room %08x: clock ping of %u bytes from %08x", g->room, (unsigned)n, from->uid);
        return;
    }
    memcpy(w, d, n);
    put32le(w + 4, p->now);
    put32le(w + 8, p->now);
    send_to(p, g, from, GCS_PTG_TIME, w, n);
}

static void route(gcs_ptg *p, game *g, player *from, gc_u8 type, const gc_u8 *d, size_t n)
{
    int from_host = from->uid == g->host;
    int i;
    if (lobby(p, g, from, type, d, n)) {
        return;
    }
    switch (type) {
    case GCS_PTG_READY:
        if (from->lobby) {
            from->ready = 1;
            barrier_check(p, g);
        }
        return;
    case GCS_PTG_TIME:
        answer_time(p, g, from, d, n);
        return;
    case GCS_PTG_KEEPALIVE:
        if (n == 4) {
            send_to(p, g, from, GCS_PTG_KEEPALIVE, d, n);
        }
        return; /* 0 bytes: the answer to a ping */
    case GCS_PTG_ACK:
        if (n >= 4) {
            from->ack_perm = le16(d);
            from->ack_cmds = le16(d + 2);
        }
        return;
    case GCS_PTG_PING:
    case GCS_PTG_COMMANDS:
    case GCS_PTG_SYNCS:
    case GCS_PTG_STATUS:
        LOG_W("room %08x: %08x sent server message type %u, ignored", g->room, from->uid, type);
        return;
    case GCS_PTG_COMMAND:
        p->st.commands++;
        for (i = 0; i < GCS_PTG_MAX_PLAYERS; i++) {
            if (g->pl[i].uid && &g->pl[i] != from) {
                bundle_command(p, g, &g->pl[i], d, n);
            }
        }
        return;
    case GCS_PTG_RADIO:
        p->st.radio_chunks++;
        to_others(p, g, from, type, d, n);
        return;
    case GCS_PTG_ALL:
        to_others(p, g, from, type, d, n);
        return;
    default:
        break;
    }
    if (!from_host) {
        /* no phone is the host in a GPRS game: what a Bluetooth client
         * sends its host (27 ended, 38 quit, 44 results, ...) is for
         * every other player */
        to_others(p, g, from, type, d, n);
        return;
    }
    /* from the host */
    switch (type) {
    case GCS_PTG_SYNC:
        if (!g->sync_count) {
            g->sync_t = p->now;
        }
        if (append(&g->sync, &g->sync_n, SYNC_MAX, d, n) == 0) {
            g->sync_count++;
        } else {
            LOG_W("room %08x: synchronisation records over %u bytes", g->room, SYNC_MAX);
        }
        return;
    case GCS_PTG_SYNC_END:
        flush_sync(p, g, 1);
        return;
    case GCS_PTG_PERM:
        g->perm_open = 1;
        to_others(p, g, from, type, d, n);
        return;
    case GCS_PTG_PERM_END:
        if (g->perm_open) {
            g->perm_open = 0; /* the clients queue a 41 behind every 23 */
            return;
        }
        break;
    default:
        break;
    }
    to_others(p, g, from, type, d, n);
}

/* ---- gcs_snap_game callbacks ---- */

static void g_joined(void *ctx, gcs_snap *s, gc_u32 uid, const char *user)
{
    gcs_ptg *p = ((title *)ctx)->p;
    gc_u32 room = gcs_snap_room_of(s, uid);
    gc_u32 owner = gcs_snap_room_owner(s, room);
    game *g;
    int i;
    p->snap = s;
    if (!owner) {
        return; /* a lobby: games are rooms the players create */
    }
    g = game_of_room(p, room, 1);
    if (!g) {
        LOG_W("no game left for room %08x", room);
        return;
    }
    g->classic = ((title *)ctx)->classic;
    if (player_of(g, uid)) {
        return;
    }
    for (i = 0; i < GCS_PTG_MAX_PLAYERS && g->pl[i].uid; i++) {
    }
    if (i == GCS_PTG_MAX_PLAYERS) {
        player tmp;
        memset(&tmp, 0, sizeof(tmp));
        tmp.uid = uid;
        send_status(p, g, &tmp, GCS_PTG_ST_TOO_MANY);
        LOG_W("room %08x full, '%s' refused", room, user ? user : "?");
        return;
    }
    memset(&g->pl[i], 0, sizeof(player));
    g->pl[i].uid = uid;
    if (!g->host && !g->host_gone) {
        g->host = owner; /* the creator, who also enters first */
    }
    LOG_I("room %08x: '%s' (%08x) joined%s, %d players", room, user ? user : "?", uid,
          g->host == uid ? " as host" : "", players(g));
}

static void g_left(void *ctx, gcs_snap *s, gc_u32 uid)
{
    gcs_ptg *p = ((title *)ctx)->p;
    game *g = game_of_room(p, gcs_snap_room_of(s, uid), 0);
    player *pl = g ? player_of(g, uid) : NULL;
    int i, n;
    p->snap = s;
    if (!pl) {
        return;
    }
    if (pl->has_rec && uid != g->host) {
        send_remove(p, g, pl, 0); /* the BT host does the same (0x8efc0) */
    }
    free(pl->bundle);
    memset(pl, 0, sizeof(*pl));
    n = players(g);
    if (n) {
        barrier_check(p, g); /* the others may be waiting for this one */
    }
    LOG_I("room %08x: %08x left, %d players", g->room, uid, n);
    if (n == 0) {
        game_free(g);
        return;
    }
    if (uid == g->host) {
        g->host = 0;
        g->host_gone = 1;
        for (i = 0; i < GCS_PTG_MAX_PLAYERS; i++) {
            if (g->pl[i].uid) {
                flush_bundle(p, g, &g->pl[i]);
                send_status(p, g, &g->pl[i], GCS_PTG_ST_HOST_LEFT);
            }
        }
    } else if (g->started && n == 1) {
        for (i = 0; i < GCS_PTG_MAX_PLAYERS; i++) {
            if (g->pl[i].uid) {
                flush_bundle(p, g, &g->pl[i]);
                send_status(p, g, &g->pl[i], GCS_PTG_ST_NO_OPPONENTS);
            }
        }
    }
}

/* game data from a phone: one or more framed messages (the game sends
 * one per CArenaFramework call, 6R82.APP 0x844b4) */
static void g_data(void *ctx, gcs_snap *s, gc_u32 uid, const gc_u8 *d, size_t n)
{
    gcs_ptg *p = ((title *)ctx)->p;
    game *g = game_of_room(p, gcs_snap_room_of(s, uid), 0);
    player *from = g ? player_of(g, uid) : NULL;
    size_t pos = 0;
    int i;
    p->snap = s;
    if (!from) {
        LOG_D("%u bytes from %08x outside a game", (unsigned)n, uid);
        return;
    }
    while (pos + 4 <= n) {
        gc_u16 h = le16(d + pos);
        size_t len = le16(d + pos + 2);
        gc_u8 type = (gc_u8)(h & 0x7f);
        const gc_u8 *body = d + pos + 4;
        if (pos + 4 + len > n) {
            LOG_W("room %08x: type %u from %08x cut short (%u of %u bytes)", g->room, type, uid,
                  (unsigned)(n - pos - 4), (unsigned)len);
            break;
        }
        p->st.messages_in++;
        LOG_D("<- %08x type %u, %u bytes%s", uid, type, (unsigned)len, uid == g->host ? " (host)" : "");
        if (!from->live && !(type == GCS_PTG_KEEPALIVE && len == 0)) {
            /* anything but the answer to a start ping */
            from->live = 1;
            LOG_I("room %08x: %08x started", g->room, uid);
        }
        if (h & 0x8000) {
            LOG_W("room %08x: compressed type %u from %08x not expected, ignored", g->room, type, uid);
        } else if (type <= (g->classic ? 51 : 54)) {
            gc_u32 hh = hash(type, body, len);
            /* the same message once more within a moment: the host's
             * phone writes Field Radio once per ally */
            if (type == GCS_PTG_RADIO && from->last_type == type && from->last_hash == hh &&
                (gc_u32)(p->now - from->last_t) < DUP_MS) {
                p->st.duplicates++;
            } else {
                from->last_type = type;
                from->last_hash = hh;
                from->last_t = p->now;
                route(p, g, from, type, body, len);
            }
        }
        pos += 4 + len;
    }
    for (i = 0; i < GCS_PTG_MAX_PLAYERS; i++) {
        flush_bundle(p, g, &g->pl[i]);
    }
}

/* The game's network layer starts only when the first message from the
 * game server arrives after ArenaFramework's game start (6R82.APP
 * 0x84bac, 6R72.APP 0x79888: flag set, observer slot 0x48 with 3); until
 * then the phone sends nothing.  So once the host sets the room status
 * to "in progress" (STAT bit 31, CArenaFramework 0x7a0c), every phone
 * that has not sent game data yet gets a ping (type 18; the game answers
 * with an empty 19) every PING_MS. */
static void start_pings(gcs_ptg *p, gcs_snap *s, game *g, gc_u32 now)
{
    int playing = (gcs_snap_room_status(s, g->room) & ST_PLAYING) != 0;
    int k;
    if (playing != g->playing) {
        LOG_I("room %08x: game %s", g->room, playing ? "in progress" : "over");
        g->playing = playing;
        for (k = 0; k < GCS_PTG_MAX_PLAYERS; k++) {
            g->pl[k].live = 0; /* a new game starts the layer again */
            g->pl[k].ping_t = now - PING_MS;
        }
    }
    if (!playing) {
        return;
    }
    for (k = 0; k < GCS_PTG_MAX_PLAYERS; k++) {
        player *pl = &g->pl[k];
        if (pl->uid && !pl->live && (gc_u32)(now - pl->ping_t) >= PING_MS) {
            pl->ping_t = now;
            send_to(p, g, pl, GCS_PTG_PING, NULL, 0);
        }
    }
}

static void g_poll(void *ctx, gcs_snap *s, gc_u32 now)
{
    gcs_ptg *p = ((title *)ctx)->p;
    int classic = ((title *)ctx)->classic;
    int i, k;
    p->snap = s;
    p->now = now;
    for (i = 0; i < GCS_PTG_MAX_GAMES; i++) {
        game *g = &p->g[i];
        if (!g->used || g->classic != classic) {
            continue;
        }
        for (k = 0; k < GCS_PTG_MAX_PLAYERS; k++) {
            flush_bundle(p, g, &g->pl[k]);
        }
        start_pings(p, s, g, now);
        if (g->sync_count && (gc_u32)(now - g->sync_t) >= SYNC_WAIT_MS) {
            flush_sync(p, g, 0);
        }
    }
}

gcs_ptg *gcs_ptg_create(void (*log)(void *ctx, int level, const char *line), void *ctx)
{
    gcs_ptg *p = (gcs_ptg *)calloc(1, sizeof(*p));
    int i;
    if (!p) {
        return NULL;
    }
    p->log = log;
    p->ctx = ctx;
    for (i = 0; i < 2; i++) {
        p->t[i].p = p;
        p->t[i].classic = i;
        p->reg[i].game_class = i ? GCS_PTG1_GAME_CLASS : GCS_PTG_GAME_CLASS;
        p->reg[i].name = i ? "Pathway to Glory" : "Pathway to Glory: Ikusa Islands";
        p->reg[i].lobby = GCS_PTG_LOBBIES;
        p->reg[i].ctx = &p->t[i];
        p->reg[i].joined = g_joined;
        p->reg[i].left = g_left;
        p->reg[i].data = g_data;
        p->reg[i].poll = g_poll;
        p->reg[i].rooms = NULL;
    }
    return p;
}

void gcs_ptg_destroy(gcs_ptg *p)
{
    int i;
    if (!p) {
        return;
    }
    for (i = 0; i < GCS_PTG_MAX_GAMES; i++) {
        if (p->g[i].used) {
            game_free(&p->g[i]);
        }
    }
    free(p);
}

const gcs_snap_game *gcs_ptg_game(gcs_ptg *p)
{
    return p ? &p->reg[0] : NULL;
}

const gcs_snap_game *gcs_ptg1_game(gcs_ptg *p)
{
    return p ? &p->reg[1] : NULL;
}

void gcs_ptg_get_stats(const gcs_ptg *p, gcs_ptg_stats *st)
{
    int i;
    *st = p->st;
    st->games = 0;
    for (i = 0; i < GCS_PTG_MAX_GAMES; i++) {
        st->games += p->g[i].used;
    }
}
