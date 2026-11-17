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
 *  Arena server sends (types 32 and 34) and the keep alive.
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
#define SEEN_N       16                 /* lobby messages remembered per player */
#define PING_MS      2000               /* start pings until the phone answers */
#define ST_PLAYING   0x80000000u        /* room status bit: game in progress */

typedef struct
{
    gc_u32 uid;
    gc_u8 tag;       /* 7 bit message counter, echoed in type 33 */
    gc_u16 ack_perm; /* last acknowledgement: 23/34 count, 32 count */
    gc_u16 ack_cmds;
    gc_u8 *bundle; /* commands of the others, not sent yet */
    size_t bundle_n;
    gc_u32 seen[SEEN_N]; /* lobby messages from the host already delivered */
    int seen_n;
    /* last message from this player that may come several times */
    gc_u8 last_type;
    gc_u32 last_hash;
    gc_u32 last_t;
    int live;      /* the phone's network layer runs: it sent game data */
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
 * synchronisation records always go compressed: the game unpacks type 34
 * only after a compressed or split message. */
static void send_to(gcs_ptg *p, game *g, player *pl, gc_u8 type, const gc_u8 *d, size_t n)
{
    gc_u8 w[WIRE_MAX];
    gc_u8 *z = NULL;
    const gc_u8 *src = d;
    size_t total = n, off = 0, first;
    int pack = type == GCS_PTG_SYNCS || 4 + n > WIRE_MAX;
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

static int seen_before(player *pl, gc_u32 h)
{
    int i;
    for (i = 0; i < pl->seen_n; i++) {
        if (pl->seen[i] == h) {
            return 1;
        }
    }
    if (pl->seen_n == SEEN_N) {
        memmove(pl->seen, pl->seen + 1, sizeof(pl->seen[0]) * (SEEN_N - 1));
        pl->seen_n--;
    }
    pl->seen[pl->seen_n++] = h;
    return 0;
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

/* Lobby messages of the host (version answer 9, player data 10, game
 * data 14): the host's phone writes them to every player's connection;
 * over GPRS all of these are the one connection to the server, so every
 * client would see each of them several times. */
static int lobby_type(gc_u8 t)
{
    return t == 9 || t == 10 || t == 14;
}

static void route(gcs_ptg *p, game *g, player *from, gc_u8 type, const gc_u8 *d, size_t n)
{
    int from_host = from->uid == g->host;
    player *host = g->host ? player_of(g, g->host) : NULL;
    int i;
    switch (type) {
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
        if (host) {
            send_to(p, g, host, type, d, n);
        } else {
            LOG_D("room %08x: type %u from %08x, no host", g->room, type, from->uid);
        }
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
    if (lobby_type(type)) {
        gc_u32 h = hash(type, d, n);
        for (i = 0; i < GCS_PTG_MAX_PLAYERS; i++) {
            player *pl = &g->pl[i];
            if (!pl->uid || pl == from) {
                continue;
            }
            if (seen_before(pl, h)) {
                p->st.duplicates++;
                continue;
            }
            send_to(p, g, pl, type, d, n);
        }
        return;
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
    free(pl->bundle);
    memset(pl, 0, sizeof(*pl));
    n = players(g);
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
             * phone writes Field Radio and lobby messages once per player */
            if ((type == GCS_PTG_RADIO || lobby_type(type)) && from->last_type == type && from->last_hash == hh &&
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
