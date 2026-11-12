/** @file gcs_snap.c
 *
 *  SNAP game server, see gcs_snap.h.  The message formats come from the
 *  parsers in snapcomm.dll and ArenaFramework.dll (Rifts: Promise of
 *  Power, NAF 2.01.082); docs/ARENA.md lists them.
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#include "gcs_snap.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HDR       16
#define TRAILER   0xBA476611u
#define MAX_MSG   1023 /* 10 bit length */
#define MAX_DGRAM 896  /* the client's receive buffer */
#define OUTQ      32   /* unacknowledged reliable messages per session */
#define RXWIN     32   /* reliable messages buffered out of order */

#define F_REL   0x8000
#define F_ACK   0x4000
#define F_ONE   0x2000 /* single packet (KK_SINGLE_PACKET_DEF), see below */
#define F_LOBBY 0x1000
#define F_MORE  0x0800
#define F_SET   0x0400

/* KK_PACKETINFO (kkGSApi.h of the Rifts game server) as snapcomm.dll fills
 * it from a message: PacketMode is "single" when F_ONE is set, else a
 * series whose PacketSeq (byte 2) counts down to 0; CommandSeq is byte 2
 * as well. ArenaFramework takes a list (users, rooms, lobbies) as
 * complete at PacketSeq 0. Every message here fits one packet, so all
 * carry F_ONE and byte 2 echoes the request's command sequence. */

typedef struct
{
    gc_u32 seq;
    gc_u32 t;
    int tries;
    size_t len;
    gc_u8 d[MAX_MSG];
} outmsg;

typedef struct
{
    int used;
    gc_u32 seq;
    size_t len;
    gc_u8 d[MAX_MSG];
} rxmsg;

typedef struct
{
    int used;
    void *key;
    gc_u8 saddr[4];
    gc_u16 sport, cport;
    gc_u32 uid;
    int logged_in;
    char name[GCSS_MAX_NAME + 1];
    gc_u32 game_class;
    gc_u32 lobby, room;
    gc_u32 tx_rel, tx_unrel; /* next sequence numbers */
    gc_u32 rx_next;          /* next reliable sequence expected */
    gc_u16 seq0[16];         /* op << 8 | command sequence of recent reliable messages with sequence 0 */
    int seq0_n;
    gc_u32 t_last;
    outmsg *outq[OUTQ];
    rxmsg *rx[RXWIN];
} sess;

typedef struct
{
    int used;
    gc_u32 id;
    char name[17];
    gc_u32 max;
    gc_u32 game_class; /* of the game that asked for it first (0: none yet) */
} lobby;

typedef struct
{
    int used;
    gc_u32 id;
    gc_u32 lobby;
    char name[17];
    gc_u32 max;
    gc_u32 status;
    gc_u32 game_class;
} room;

struct gcs_snap
{
    gcs_snap_io io;
    void *ctx;
    sess s[GCSS_MAX_SESSIONS];
    lobby lobbies[GCSS_MAX_LOBBIES];
    room rooms[GCSS_MAX_ROOMS];
    const gcs_snap_game *games[8];
    int n_games;
    gc_u32 next_uid, next_lobby, next_room;
    gc_u32 now;
    gcs_snap_stats st;
};

/* ---- helpers ---- */

static void slog(gcs_snap *s, int level, const char *fmt, ...)
{
    char line[512];
    va_list ap;
    size_t i;
    if (!s->io.log) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    for (i = 0; line[i]; i++) {
        if ((unsigned char)line[i] < 0x20 || line[i] == 0x7f) {
            line[i] = '.';
        }
    }
    s->io.log(s->ctx, level, line);
}

#define LOG_W(...) slog(s, 1, "snap: " __VA_ARGS__)
#define LOG_I(...) slog(s, 2, "snap: " __VA_ARGS__)
#define LOG_D(...) slog(s, 3, "snap: " __VA_ARGS__)

static gc_u32 be32(const gc_u8 *p)
{
    return ((gc_u32)p[0] << 24) | ((gc_u32)p[1] << 16) | ((gc_u32)p[2] << 8) | p[3];
}

static gc_u16 be16(const gc_u8 *p)
{
    return (gc_u16)((p[0] << 8) | p[1]);
}

static void put32(gc_u8 *p, gc_u32 v)
{
    p[0] = (gc_u8)(v >> 24);
    p[1] = (gc_u8)(v >> 16);
    p[2] = (gc_u8)(v >> 8);
    p[3] = (gc_u8)v;
}

static void put16(gc_u8 *p, gc_u16 v)
{
    p[0] = (gc_u8)(v >> 8);
    p[1] = (gc_u8)v;
}

/* name field of 16 bytes (or n) from a C string */
static void put_name(gc_u8 *p, const char *name, size_t n)
{
    size_t k = strlen(name);
    memset(p, 0, n);
    if (k >= n) {
        k = n - 1;
    }
    memcpy(p, name, k);
}

static void get_name(char *out, size_t cap, const gc_u8 *p, size_t n)
{
    size_t k = 0;
    while (k < n && k + 1 < cap && p[k]) {
        out[k] = (char)p[k];
        k++;
    }
    out[k] = 0;
}

static void hex(char *out, size_t cap, const gc_u8 *d, size_t n)
{
    size_t i, k = 0;
    out[0] = 0;
    for (i = 0; i < n && k + 4 < cap; i++) {
        k += (size_t)snprintf(out + k, cap - k, "%02x ", d[i]);
    }
    if (i < n && k + 4 < cap) {
        snprintf(out + k, cap - k, "...");
    }
}

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        int x = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        int y = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (x != y) {
            return 0;
        }
    }
    return *a == *b;
}

static const gcs_snap_game *game_of(gcs_snap *s, gc_u32 gc)
{
    int i;
    for (i = 0; i < s->n_games; i++) {
        if (s->games[i]->game_class == gc) {
            return s->games[i];
        }
    }
    return NULL;
}

/* ---- sessions ---- */

static sess *sess_find(gcs_snap *s, void *key, gc_u16 cport)
{
    int i;
    for (i = 0; i < GCSS_MAX_SESSIONS; i++) {
        if (s->s[i].used && s->s[i].key == key && s->s[i].cport == cport) {
            return &s->s[i];
        }
    }
    return NULL;
}

static sess *sess_by_uid(gcs_snap *s, gc_u32 uid)
{
    int i;
    for (i = 0; i < GCSS_MAX_SESSIONS; i++) {
        if (s->s[i].used && s->s[i].logged_in && s->s[i].uid == uid) {
            return &s->s[i];
        }
    }
    return NULL;
}

static void sess_clear_queues(sess *c)
{
    int i;
    for (i = 0; i < OUTQ; i++) {
        free(c->outq[i]);
        c->outq[i] = NULL;
    }
    for (i = 0; i < RXWIN; i++) {
        free(c->rx[i]);
        c->rx[i] = NULL;
    }
}

static void room_leave(gcs_snap *s, sess *c);

static void sess_free(gcs_snap *s, sess *c, const char *why)
{
    if (c->logged_in) {
        LOG_I("user %08x '%s' gone (%s)", c->uid, c->name, why);
    }
    room_leave(s, c);
    sess_clear_queues(c);
    memset(c, 0, sizeof(*c));
}

/* ---- output ---- */

static void send_raw(gcs_snap *s, sess *c, const gc_u8 *msg, size_t len)
{
    gc_u8 d[MAX_MSG + 4];
    if (len > MAX_MSG) {
        return;
    }
    memcpy(d, msg, len);
    put32(d + len, TRAILER);
    s->io.send(s->ctx, c->key, c->saddr, c->sport, c->cport, d, len + 4);
}

/* Build and send a message.  flags: F_REL/F_LOBBY/F_SET...; reliable
 * messages are kept until acknowledged. */
static int send_msg(gcs_snap *s, sess *c, gc_u16 flags, gc_u8 cmdseq, gc_u8 op, const gc_u8 *payload, size_t plen)
{
    gc_u8 m[MAX_MSG];
    size_t len = HDR + plen;
    if (len > MAX_MSG || len + 4 > MAX_DGRAM) {
        LOG_W("message op %u too long (%u bytes)", op, (unsigned)len);
        return -1;
    }
    put16(m, (gc_u16)(((flags | F_ONE) & 0xfc00) | len));
    m[2] = cmdseq;
    m[3] = op;
    put32(m + 4, c->uid);
    put32(m + 12, 0);
    if (plen) {
        memcpy(m + HDR, payload, plen);
    }
    if (flags & F_REL) {
        int i;
        outmsg *o;
        for (i = 0; i < OUTQ && c->outq[i]; i++) {
        }
        if (i == OUTQ) {
            LOG_W("user %08x: send queue full, op %u dropped", c->uid, op);
            return -1;
        }
        o = (outmsg *)malloc(sizeof(*o));
        if (!o) {
            return -1;
        }
        put32(m + 8, c->tx_rel);
        o->seq = c->tx_rel++;
        o->t = s->now;
        o->tries = 1;
        o->len = len;
        memcpy(o->d, m, len);
        c->outq[i] = o;
    } else {
        put32(m + 8, c->tx_unrel++);
    }
    LOG_D("-> %08x op %u flags %04x seq %u len %u", c->uid, op, flags | F_ONE, be32(m + 8), (unsigned)plen);
    send_raw(s, c, m, len);
    return 0;
}

static void send_ack(gcs_snap *s, sess *c, gc_u32 seq)
{
    gc_u8 m[HDR];
    put16(m, F_ACK | F_ONE | HDR);
    m[2] = 0;
    m[3] = GCSS_OP_ACK;
    put32(m + 4, c->uid);
    put32(m + 8, c->tx_unrel ? c->tx_unrel - 1 : 0);
    put32(m + 12, seq);
    send_raw(s, c, m, HDR);
}

/* result for request op: success {op, value, extra...} or failure {op, reason} */
static void result(gcs_snap *s, sess *c, gc_u16 hflags, gc_u8 cmdseq, gc_u8 op, int ok, gc_u32 value)
{
    gc_u8 p[12];
    put32(p, op);
    put32(p + 4, value);
    put32(p + 8, 0);
    send_msg(s, c, (gc_u16)(F_REL | (hflags & F_LOBBY)), cmdseq, ok ? GCSS_OP_OK : GCSS_OP_FAIL, p, ok ? 12 : 8);
}

/* ---- lobbies and rooms ---- */

static lobby *lobby_by_id(gcs_snap *s, gc_u32 id)
{
    int i;
    for (i = 0; i < GCSS_MAX_LOBBIES; i++) {
        if (s->lobbies[i].used && s->lobbies[i].id == id) {
            return &s->lobbies[i];
        }
    }
    return NULL;
}

/* by name; created on first use so that any lobby a game asks for exists */
static lobby *lobby_by_name(gcs_snap *s, const char *name, int create)
{
    int i;
    for (i = 0; i < GCSS_MAX_LOBBIES; i++) {
        if (s->lobbies[i].used && ieq(s->lobbies[i].name, name)) {
            return &s->lobbies[i];
        }
    }
    if (!create || !name[0]) {
        return NULL;
    }
    for (i = 0; i < GCSS_MAX_LOBBIES && s->lobbies[i].used; i++) {
    }
    if (i == GCSS_MAX_LOBBIES) {
        return NULL;
    }
    s->lobbies[i].used = 1;
    s->lobbies[i].id = s->next_lobby++;
    s->lobbies[i].max = 200;
    snprintf(s->lobbies[i].name, sizeof(s->lobbies[i].name), "%s", name);
    LOG_I("lobby %08x '%s' created", s->lobbies[i].id, s->lobbies[i].name);
    return &s->lobbies[i];
}

static int lobby_members(gcs_snap *s, gc_u32 id)
{
    int i, n = 0;
    for (i = 0; i < GCSS_MAX_SESSIONS; i++) {
        n += s->s[i].used && s->s[i].logged_in && s->s[i].lobby == id;
    }
    return n;
}

static room *room_by_id(gcs_snap *s, gc_u32 id)
{
    int i;
    for (i = 0; i < GCSS_MAX_ROOMS; i++) {
        if (s->rooms[i].used && s->rooms[i].id == id) {
            return &s->rooms[i];
        }
    }
    return NULL;
}

static int room_members(gcs_snap *s, gc_u32 id)
{
    int i, n = 0;
    for (i = 0; i < GCSS_MAX_SESSIONS; i++) {
        n += s->s[i].used && s->s[i].logged_in && s->s[i].room == id;
    }
    return n;
}

/* 36 byte room entry: name[16], id, users, max, status, lobby */
static void room_entry(gcs_snap *s, gc_u8 *p, const room *r)
{
    put_name(p, r->name, 16);
    put32(p + 16, r->id);
    put32(p + 20, (gc_u32)room_members(s, r->id));
    put32(p + 24, r->max);
    put32(p + 28, r->status);
    put32(p + 32, r->lobby);
}

/* KK_JOINUSER: name[16], user id, property size (0) */
static void user_entry(gc_u8 *p, const sess *c)
{
    put_name(p, c->name, 16);
    put32(p + 16, c->uid);
    put32(p + 20, 0);
}

static void room_leave(gcs_snap *s, sess *c)
{
    room *r;
    int i;
    gc_u8 p[4];
    if (!c->room) {
        return;
    }
    r = room_by_id(s, c->room);
    if (r) {
        const gcs_snap_game *g = game_of(s, r->game_class);
        if (g && g->left) {
            g->left(g->ctx, s, c->uid);
        }
    }
    put32(p, c->uid);
    for (i = 0; i < GCSS_MAX_SESSIONS; i++) {
        sess *o = &s->s[i];
        if (o != c && o->used && o->logged_in && o->room == c->room) {
            send_msg(s, o, F_REL, 0, GCSS_OP_LEAVE, p, 4); /* user left the room */
        }
    }
    LOG_I("user %08x '%s' left room %08x", c->uid, c->name, c->room);
    c->room = 0;
    if (r && room_members(s, r->id) == 0) {
        LOG_I("room %08x '%s' closed", r->id, r->name);
        memset(r, 0, sizeof(*r));
    }
}

static void room_enter(gcs_snap *s, sess *c, room *r)
{
    gc_u8 p[24];
    int i;
    const gcs_snap_game *g;
    if (c->room == r->id) {
        return;
    }
    room_leave(s, c);
    c->room = r->id;
    LOG_I("user %08x '%s' entered room %08x '%s'", c->uid, c->name, r->id, r->name);
    user_entry(p, c);
    for (i = 0; i < GCSS_MAX_SESSIONS; i++) {
        sess *o = &s->s[i];
        if (o != c && o->used && o->logged_in && o->room == r->id) {
            send_msg(s, o, F_REL, 0, GCSS_OP_JOIN, p, 24); /* user entered the room */
        }
    }
    g = game_of(s, r->game_class);
    if (g && g->joined) {
        g->joined(g->ctx, s, c->uid, c->name);
    }
}

/* ---- request handlers ---- */

static void h_login(gcs_snap *s, sess *c, const gc_u8 *h, const gc_u8 *p, size_t n)
{
    char user[41], canon[GCSS_MAX_NAME + 1], extra[61];
    gc_u8 r[12];
    gc_u32 gc = 0;
    if (n < 40) {
        LOG_W("login too short (%u bytes)", (unsigned)n);
        return;
    }
    get_name(user, sizeof(user), p, 40);
    get_name(extra, sizeof(extra), p + 40, n >= 100 ? 60 : n - 40);
    if (n >= 104) {
        size_t off;
        gc = be32(p + 100); /* game class (the "GCID" ArenaFramework logs) */
        /* not verified on hardware: look for a known game class in the rest
         * of the login, too */
        for (off = 100; !game_of(s, gc) && off + 4 <= n; off += 4) {
            if (game_of(s, be32(p + off))) {
                gc = be32(p + off);
                LOG_I("game class %u found at offset %u of the login", gc, (unsigned)off);
            }
        }
    }
    if (!s->io.auth || !s->io.auth(s->ctx, c->key, user, canon)) {
        gc_u8 f[8];
        LOG_W("login '%s' refused (no Arena login with this name on this N-Gage)", user);
        put32(f, 1);
        put32(f + 4, 1);
        c->uid = 0;
        send_msg(s, c, 0, h[2], GCSS_OP_FAIL, f, 8);
        return;
    } else {
        int i;
        /* one session per user: a new login replaces the old one */
        for (i = 0; i < GCSS_MAX_SESSIONS; i++) {
            sess *o = &s->s[i];
            if (o != c && o->used && o->logged_in && ieq(o->name, canon)) {
                sess_free(s, o, "logged in again");
            }
        }
        /* the client starts its sequence numbers again with every login
         * (also when it repeats one whose answer got lost) */
        room_leave(s, c);
        sess_clear_queues(c);
        c->logged_in = 1;
        c->uid = s->next_uid++;
        snprintf(c->name, sizeof(c->name), "%s", canon);
        c->game_class = gc;
        c->lobby = 0;
        c->tx_rel = 0;
        c->rx_next = 0;
        c->seq0_n = 0;
        s->st.logins++;
        LOG_I("user %08x '%s' logged in (game class %u, '%s')", c->uid, c->name, gc, extra);
    }
    put32(r, 0); /* port: keep talking to this one */
    put32(r + 4, 0);
    put32(r + 8, c->uid);
    send_msg(s, c, 0, h[2], GCSS_OP_LOGIN_OK, r, 12);
}

static void h_lobby_info(gcs_snap *s, sess *c, const gc_u8 *h, const gc_u8 *p, size_t n)
{
    char name[17];
    gc_u8 e[20];
    int i, count = 0;
    get_name(name, sizeof(name), p, n < 16 ? n : 16);
    if (name[0]) {
        lobby *l = lobby_by_name(s, name, 1);
        if (l && !l->game_class) {
            l->game_class = c->game_class;
        }
        if (l) {
            put_name(e, l->name, 16);
            put32(e + 16, l->id);
            send_msg(s, c, F_REL, h[2], GCSS_OP_LOBBY_INFO, e, 20);
            count = 1;
        }
    } else {
        for (i = 0; i < GCSS_MAX_LOBBIES; i++) {
            if (s->lobbies[i].used) {
                put_name(e, s->lobbies[i].name, 16);
                put32(e + 16, s->lobbies[i].id);
                send_msg(s, c, F_REL, h[2], GCSS_OP_LOBBY_INFO, e, 20);
                count++;
            }
        }
    }
    LOG_I("user %08x: lobby '%s': %d found", c->uid, name, count);
    result(s, c, be16(h), h[2], GCSS_OP_LOBBY_INFO, 1, (gc_u32)count);
}

static void h_lobbies(gcs_snap *s, sess *c, const gc_u8 *h)
{
    gc_u8 p[12 + GCSS_MAX_LOBBIES * 28];
    int i, k = 0;
    (void)h;
    for (i = 0; i < GCSS_MAX_LOBBIES; i++) {
        lobby *l = &s->lobbies[i];
        if (l->used) {
            gc_u8 *e = p + 12 + 28 * k++;
            put_name(e, l->name, 16);
            put32(e + 16, l->id);
            put32(e + 20, (gc_u32)lobby_members(s, l->id));
            put32(e + 24, l->max);
        }
    }
    put32(p, 0);
    put32(p + 4, (gc_u32)k);
    put32(p + 8, (gc_u32)k);
    send_msg(s, c, F_REL | F_LOBBY, h[2], GCSS_OP_LOBBIES, p, 12 + 28 * (size_t)k);
}

/* op 71 (sent when Rifts creates a challenge): the
 * lobbies of the login's game class. The answer is a list message of
 * op 71 only, no op 40: ArenaFramework hands a result for op 71 to the
 * same callback and would start a new, empty list with it. Header
 * {game class, total, count}, entries of 36 bytes: name [16], users, -,
 * max users, status, lobby id (ArenaFramework keeps all but the second
 * word, finds LobbyToJoin by name and enters it by id). */
static void h_game_lobbies(gcs_snap *s, sess *c, const gc_u8 *h)
{
    gc_u8 out[12 + GCSS_MAX_LOBBIES * 36];
    const gcs_snap_game *g = game_of(s, c->game_class);
    int i, k = 0;
    if (g && g->lobby) {
        lobby *l = lobby_by_name(s, g->lobby, 1);
        if (l && !l->game_class) {
            l->game_class = c->game_class;
        }
    }
    for (i = 0; i < GCSS_MAX_LOBBIES; i++) {
        lobby *l = &s->lobbies[i];
        if (l->used && l->game_class == c->game_class) {
            gc_u8 *e = out + 12 + 36 * k++;
            memset(e, 0, 36);
            put_name(e, l->name, 16);
            put32(e + 16, (gc_u32)lobby_members(s, l->id));
            put32(e + 24, l->max);
            put32(e + 28, 0);
            put32(e + 32, l->id);
        }
    }
    put32(out, c->game_class);
    put32(out + 4, (gc_u32)k);
    put32(out + 8, (gc_u32)k);
    LOG_I("user %08x: lobbies of game class %u: %d", c->uid, c->game_class, k);
    send_msg(s, c, F_REL | (be16(h) & F_LOBBY), h[2], GCSS_OP_GAME_LOBBIES, out, 12 + 36 * (size_t)k);
}

static void h_join(gcs_snap *s, sess *c, const gc_u8 *h, const gc_u8 *p, size_t n)
{
    gc_u16 fl = be16(h);
    gc_u32 id = n >= 4 ? be32(p) : 0;
    if (fl & F_LOBBY) {
        lobby *l = lobby_by_id(s, id);
        if (!l && n >= 20) { /* by name */
            char name[17];
            get_name(name, sizeof(name), p, 16);
            l = lobby_by_name(s, name, 1);
            if (l && !l->game_class) {
                l->game_class = c->game_class;
            }
            if (!l) {
                l = lobby_by_id(s, be32(p + 16));
            }
        }
        if (!l) {
            LOG_W("user %08x: join lobby %08x: no such lobby", c->uid, id);
            result(s, c, fl, h[2], GCSS_OP_JOIN, 0, 3); /* KK_OBJECT_CAN_NOT_FIND */
            return;
        }
        room_leave(s, c);
        c->lobby = l->id;
        LOG_I("user %08x '%s' entered lobby %08x '%s'", c->uid, c->name, l->id, l->name);
        result(s, c, fl, h[2], GCSS_OP_JOIN, 1, l->id);
    } else {
        room *r = room_by_id(s, id);
        if (!r) {
            LOG_W("user %08x: join room %08x: no such room", c->uid, id);
            result(s, c, fl, h[2], GCSS_OP_JOIN, 0, 3);
            return;
        }
        if (c->room != r->id && r->max && (gc_u32)room_members(s, r->id) >= r->max) {
            result(s, c, fl, h[2], GCSS_OP_JOIN, 0, 10); /* KK_OBJECT_HAS_MAXIMUM_USER */
            return;
        }
        room_enter(s, c, r);
        result(s, c, fl, h[2], GCSS_OP_JOIN, 1, r->id);
    }
}

static void h_leave(gcs_snap *s, sess *c, const gc_u8 *h)
{
    gc_u16 fl = be16(h);
    gc_u32 id;
    if (fl & F_LOBBY) {
        id = c->lobby;
        room_leave(s, c);
        c->lobby = 0;
        LOG_I("user %08x '%s' left lobby %08x", c->uid, c->name, id);
    } else {
        id = c->room;
        room_leave(s, c);
    }
    result(s, c, fl, h[2], GCSS_OP_LEAVE, 1, id);
}

static void h_create_room(gcs_snap *s, sess *c, const gc_u8 *h, const gc_u8 *p, size_t n)
{
    gc_u8 e[36];
    room *r;
    int i;
    for (i = 0; i < GCSS_MAX_ROOMS && s->rooms[i].used; i++) {
    }
    if (i == GCSS_MAX_ROOMS || !c->lobby) {
        LOG_W("user %08x: create room refused (%s)", c->uid, c->lobby ? "no room left" : "not in a lobby");
        result(s, c, be16(h), h[2], GCSS_OP_CREATE_ROOM, 0, c->lobby ? (gc_u32)-1 : 5);
        return;
    }
    r = &s->rooms[i];
    r->used = 1;
    r->id = s->next_room++;
    r->lobby = c->lobby;
    r->game_class = c->game_class;
    get_name(r->name, sizeof(r->name), p, n < 16 ? n : 16);
    r->max = n >= 20 ? be32(p + 16) : 4;
    if (r->max == 0 || r->max > 64) {
        r->max = 4;
    }
    r->status = 0x00000001u; /* KK_GAME_SERVER_START: a game server is attached */
    LOG_I("user %08x '%s' created room %08x '%s' (max %u) in lobby %08x", c->uid, c->name, r->id, r->name, r->max,
          r->lobby);
    room_entry(s, e, r);
    send_msg(s, c, F_REL | (be16(h) & F_LOBBY), h[2], GCSS_OP_CREATE_ROOM, e, 36);
    result(s, c, be16(h), h[2], GCSS_OP_CREATE_ROOM, 1, r->id);
    /* the creator is in the room now: ArenaFramework takes the result as
     * having entered it and sends no join (CreateRoomToJoin) */
    room_enter(s, c, r);
}

static void h_rooms(gcs_snap *s, sess *c, const gc_u8 *h, const gc_u8 *p, size_t n)
{
    gc_u8 out[12 + 20 * 36];
    int i, k = 0, total = 0;
    gc_u32 lid = c->lobby;
    (void)p;
    (void)n;
    for (i = 0; i < GCSS_MAX_ROOMS; i++) {
        room *r = &s->rooms[i];
        if (r->used && r->lobby == lid) {
            total++;
            if (k < 20) {
                room_entry(s, out + 12 + 36 * k++, r);
            }
        }
    }
    put32(out, lid);
    put32(out + 4, (gc_u32)total);
    put32(out + 8, (gc_u32)k);
    LOG_I("user %08x: room search in lobby %08x: %d", c->uid, lid, total);
    send_msg(s, c, F_REL | (be16(h) & F_LOBBY), h[2], GCSS_OP_ROOMS, out, 12 + 36 * (size_t)k);
}

static void h_users(gcs_snap *s, sess *c, const gc_u8 *h)
{
    gc_u8 out[12 + 24 * 24];
    int i, k = 0;
    gc_u16 fl = be16(h);
    gc_u32 where = (fl & F_LOBBY) ? c->lobby : c->room;
    for (i = 0; i < GCSS_MAX_SESSIONS && k < 24; i++) {
        sess *o = &s->s[i];
        if (o->used && o->logged_in && ((fl & F_LOBBY) ? o->lobby == where : o->room == where)) {
            user_entry(out + 12 + 24 * k++, o);
        }
    }
    put32(out, where);
    put32(out + 4, (gc_u32)k);
    put32(out + 8, (gc_u32)k);
    send_msg(s, c, F_REL | (fl & F_LOBBY), h[2], GCSS_OP_USERS, out, 12 + 24 * (size_t)k);
}

/* game data from a user to the other members of its room */
static void h_data(gcs_snap *s, sess *c, const gc_u8 *h, const gc_u8 *p, size_t n)
{
    gc_u16 fl = be16(h);
    gc_u8 op = h[3];
    const gc_u8 *data = p;
    size_t dlen = n;
    gc_u32 targets[32];
    int nt = -1, i, k;
    if (op == GCSS_OP_PDATA) {
        gc_u32 cnt;
        if (n < 4) {
            return;
        }
        cnt = be32(p);
        if (cnt > 32 || n < 4 + 4 * cnt) {
            return;
        }
        for (k = 0; k < (int)cnt; k++) {
            targets[k] = be32(p + 4 + 4 * k);
        }
        nt = (int)cnt;
        data = p + 4 + 4 * cnt;
        dlen = n - 4 - 4 * cnt;
    }
    if (!c->room) {
        return;
    }
    for (i = 0; i < GCSS_MAX_SESSIONS; i++) {
        sess *o = &s->s[i];
        int hit = nt < 0;
        if (o == c || !o->used || !o->logged_in || o->room != c->room) {
            continue;
        }
        for (k = 0; k < nt && !hit; k++) {
            hit = targets[k] == o->uid;
        }
        if (hit) {
            /* receivers see the data as broadcast; private data keeps its list */
            send_msg(s, o, (gc_u16)((fl & (F_REL | F_SET))), 0, op, op == GCSS_OP_PDATA ? p : data,
                     op == GCSS_OP_PDATA ? n : dlen);
        }
    }
}

static void h_to_server(gcs_snap *s, sess *c, const gc_u8 *p, size_t n)
{
    room *r = room_by_id(s, c->room);
    const gcs_snap_game *g = game_of(s, r ? r->game_class : c->game_class);
    if (!g || !g->data) {
        LOG_W("user %08x: %u bytes for a game server, but none for game class %u", c->uid, (unsigned)n,
              r ? r->game_class : c->game_class);
        return;
    }
    g->data(g->ctx, s, c->uid, p, n);
}

/* one message from the game, header already converted */
static void dispatch(gcs_snap *s, sess *c, const gc_u8 *h, const gc_u8 *p, size_t n)
{
    gc_u8 op = h[3];
    gc_u16 fl = be16(h);
    char hx[3 * 24 + 4];
    if (op != GCSS_OP_LOGIN && !c->logged_in) {
        LOG_D("op %u before login ignored", op);
        return;
    }
    switch (op) {
    case GCSS_OP_LOGIN:
        h_login(s, c, h, p, n);
        break;
    case GCSS_OP_LOGOUT:
        LOG_I("user %08x '%s' logged out", c->uid, c->name);
        room_leave(s, c);
        c->logged_in = 0;
        c->lobby = 0;
        break;
    case GCSS_OP_LOBBY_INFO:
        h_lobby_info(s, c, h, p, n);
        break;
    case GCSS_OP_LOBBIES:
        h_lobbies(s, c, h);
        break;
    case GCSS_OP_JOIN:
        h_join(s, c, h, p, n);
        break;
    case GCSS_OP_LEAVE:
        h_leave(s, c, h);
        break;
    case GCSS_OP_CREATE_ROOM:
        h_create_room(s, c, h, p, n);
        break;
    case GCSS_OP_ROOMS:
        h_rooms(s, c, h, p, n);
        break;
    case GCSS_OP_USERS:
        h_users(s, c, h);
        break;
    case GCSS_OP_DATA:
    case GCSS_OP_PDATA:
        h_data(s, c, h, p, n);
        break;
    case GCSS_OP_TO_SERVER:
        h_to_server(s, c, p, n);
        break;
    case GCSS_OP_GAME_LOBBIES:
        h_game_lobbies(s, c, h);
        break;
    case GCSS_OP_REMOVED:
    case GCSS_OP_ATTR:
    case GCSS_OP_ATTR2:
    case 12:
    case 13:
    case 37:
    case 67:
    case 70:
    case 72:
    case 73:
    case 74:
    case 75:
        hex(hx, sizeof(hx), p, n);
        LOG_I("user %08x: op %u flags %04x (%u bytes: %s) answered with success", c->uid, op, fl, (unsigned)n, hx);
        result(s, c, fl, h[2], op, 1, 0);
        break;
    default:
        hex(hx, sizeof(hx), p, n);
        LOG_W("user %08x: unknown op %u flags %04x (%u bytes: %s)", c->uid, op, fl, (unsigned)n, hx);
        break;
    }
}

/* A reliable unit: one message, or several the client packed with F_MORE
 * (only the first carries the sequence number, the others have 0). */
static void dispatch_unit(gcs_snap *s, sess *c, const gc_u8 *m, size_t len)
{
    size_t pos = 0;
    while (pos + HDR <= len && c->used) {
        size_t l = be16(m + pos) & 0x3ff;
        if (l < HDR || pos + l > len) {
            break;
        }
        dispatch(s, c, m + pos, m + pos + HDR, l - HDR);
        pos += l;
    }
}

/* Reliable messages with sequence number 0 after the first: snapcomm.dll
 * sends some of them on their own (Rifts' room attributes ARNC and ARND
 * after ARNA, ARNB). They are processed when new; repeats are told apart
 * by opcode and command sequence, which is new for every request. 1 if
 * m was seen before (and is remembered now). */
static int seq0_seen(sess *c, const gc_u8 *m)
{
    gc_u16 k = (gc_u16)(m[3] << 8 | m[2]);
    int i;
    for (i = 0; i < c->seq0_n && i < 16; i++) {
        if (c->seq0[i] == k) {
            return 1;
        }
    }
    c->seq0[c->seq0_n++ % 16] = k;
    if (c->seq0_n >= 32) {
        c->seq0_n = 16;
    }
    return 0;
}

/* a reliable message arrived: process in order, buffer what is early */
static void reliable_in(gcs_snap *s, sess *c, const gc_u8 *m, size_t len)
{
    gc_u32 seq = be32(m + 8);
    int i;
    if (seq < c->rx_next) {
        return; /* duplicate */
    }
    if (seq != c->rx_next) {
        if (seq - c->rx_next >= RXWIN) {
            return;
        }
        for (i = 0; i < RXWIN; i++) {
            if (c->rx[i] && c->rx[i]->seq == seq) {
                return;
            }
        }
        for (i = 0; i < RXWIN && c->rx[i]; i++) {
        }
        if (i < RXWIN && (c->rx[i] = (rxmsg *)malloc(sizeof(rxmsg))) != NULL) {
            c->rx[i]->seq = seq;
            c->rx[i]->len = len;
            memcpy(c->rx[i]->d, m, len);
        }
        return;
    }
    dispatch_unit(s, c, m, len);
    c->rx_next++;
    for (;;) {
        rxmsg *r = NULL;
        if (!c->used) {
            return;
        }
        for (i = 0; i < RXWIN; i++) {
            if (c->rx[i] && c->rx[i]->seq == c->rx_next) {
                r = c->rx[i];
                c->rx[i] = NULL;
                break;
            }
        }
        if (!r) {
            break;
        }
        dispatch_unit(s, c, r->d, r->len);
        c->rx_next++;
        free(r);
    }
}

static void acked(sess *c, gc_u32 seq)
{
    int i;
    for (i = 0; i < OUTQ; i++) {
        if (c->outq[i] && c->outq[i]->seq == seq) {
            free(c->outq[i]);
            c->outq[i] = NULL;
        }
    }
}

void gcs_snap_input(gcs_snap *s, void *key, const gc_u8 addr[4], gc_u16 port, gc_u16 peer_port, const gc_u8 *d,
                    size_t n, gc_u32 now)
{
    sess *c;
    size_t pos = 0, end;
    char hx[3 * 32 + 4];
    s->now = now;
    s->st.datagrams++;
    if (n < HDR + 4 || (be32(d + n - 4) & 0xFFFFFF00u) != (TRAILER & 0xFFFFFF00u)) {
        hex(hx, sizeof(hx), d, n);
        LOG_W("datagram to port %u without SNAP trailer (%u bytes: %s)", port, (unsigned)n, hx);
        s->st.bad_datagrams++;
        return;
    }
    end = n - 4;
    c = sess_find(s, key, peer_port);
    if (!c) {
        int i;
        if (d[3] != GCSS_OP_LOGIN) {
            LOG_D("datagram from unknown session (op %u) ignored", d[3]);
            return;
        }
        for (i = 0; i < GCSS_MAX_SESSIONS && s->s[i].used; i++) {
        }
        if (i == GCSS_MAX_SESSIONS) {
            LOG_W("no room for another session");
            return;
        }
        c = &s->s[i];
        memset(c, 0, sizeof(*c));
        c->used = 1;
        c->key = key;
        c->cport = peer_port;
        c->tx_unrel = 1;
    }
    memcpy(c->saddr, addr, 4);
    c->sport = port;
    c->t_last = now;
    while (pos + HDR <= end && c->used) {
        const gc_u8 *m = d + pos;
        gc_u16 fl = be16(m);
        size_t len = fl & 0x3ff;
        if (len < HDR || pos + len > end) {
            hex(hx, sizeof(hx), m, end - pos);
            LOG_W("bad message length %u (%s)", (unsigned)len, hx);
            s->st.bad_datagrams++;
            break;
        }
        LOG_D("<- %08x op %u flags %04x seq %u ack %u len %u", be32(m + 4), m[3], fl & 0xfc00, be32(m + 8),
              be32(m + 12), (unsigned)(len - HDR));
        if (fl & F_ACK) {
            acked(c, be32(m + 12));
        }
        if (fl & F_REL) {
            /* messages packed behind this one with F_MORE that carry
             * sequence number 0 belong to it (snapcomm.dll numbers the
             * datagram's first reliable message only) */
            size_t ulen = len;
            gc_u16 ufl = fl;
            while ((ufl & F_MORE) && pos + ulen + HDR <= end) {
                const gc_u8 *f = d + pos + ulen;
                size_t flen = be16(f) & 0x3ff;
                if (!(be16(f) & F_REL) || be32(f + 8) != 0 || flen < HDR || pos + ulen + flen > end ||
                    ulen + flen > MAX_MSG) {
                    break;
                }
                LOG_D("<- %08x op %u flags %04x (packed with seq %u) len %u", be32(f + 4), f[3], be16(f) & 0xfc00,
                      be32(m + 8), (unsigned)(flen - HDR));
                ufl = be16(f);
                ulen += flen;
            }
            send_ack(s, c, be32(m + 8));
            if (be32(m + 8) == 0 && seq0_seen(c, m) && c->rx_next > 0) {
                hex(hx, sizeof(hx), m + HDR, len - HDR);
                LOG_D("op %u command %u with sequence 0 again (%s): repeat, ignored", m[3], m[2], hx);
            } else if (be32(m + 8) == 0 && c->rx_next > 0) {
                dispatch_unit(s, c, m, ulen); /* unnumbered: in arrival order */
            } else {
                reliable_in(s, c, m, ulen);
            }
            len = ulen;
        } else if (m[3] != GCSS_OP_ACK || len > HDR) {
            dispatch(s, c, m, m + HDR, len - HDR);
        }
        pos += len;
    }
}

void gcs_snap_poll(gcs_snap *s, gc_u32 now)
{
    int i, k;
    const gcs_snap_game *g;
    s->now = now;
    for (i = 0; i < GCSS_MAX_SESSIONS; i++) {
        sess *c = &s->s[i];
        if (!c->used) {
            continue;
        }
        if ((gc_u32)(now - c->t_last) >= GCSS_IDLE_MS) {
            sess_free(s, c, "silent");
            continue;
        }
        for (k = 0; k < OUTQ && c->used; k++) {
            outmsg *o = c->outq[k];
            if (!o || (gc_u32)(now - o->t) < GCSS_RESEND_MS) {
                continue;
            }
            if (o->tries >= GCSS_MAX_TRIES) {
                sess_free(s, c, "no acknowledgement");
                break;
            }
            o->tries++;
            o->t = now;
            s->st.resends++;
            send_raw(s, c, o->d, o->len);
        }
    }
    for (i = 0; i < s->n_games; i++) {
        g = s->games[i];
        if (g->poll) {
            g->poll(g->ctx, s, now);
        }
    }
}

void gcs_snap_peer_gone(gcs_snap *s, void *key)
{
    int i;
    for (i = 0; i < GCSS_MAX_SESSIONS; i++) {
        if (s->s[i].used && s->s[i].key == key) {
            sess_free(s, &s->s[i], "bridge connection lost");
        }
    }
}

int gcs_snap_send_game_data(gcs_snap *s, gc_u32 uid, const gc_u8 *d, size_t n)
{
    sess *c = sess_by_uid(s, uid);
    if (!c) {
        return -1;
    }
    return send_msg(s, c, F_REL, 0, GCSS_OP_DATA, d, n);
}

int gcs_snap_add_game(gcs_snap *s, const gcs_snap_game *g)
{
    if (s->n_games >= (int)(sizeof(s->games) / sizeof(s->games[0]))) {
        return -1;
    }
    s->games[s->n_games++] = g;
    return 0;
}

gcs_snap *gcs_snap_create(const gcs_snap_io *io, void *ctx)
{
    gcs_snap *s = (gcs_snap *)calloc(1, sizeof(*s));
    if (!s) {
        return NULL;
    }
    s->io = *io;
    s->ctx = ctx;
    s->next_uid = 0x00010001u;
    s->next_lobby = 0x00000101u;
    s->next_room = 0x00100001u;
    return s;
}

void gcs_snap_destroy(gcs_snap *s)
{
    int i;
    if (!s) {
        return;
    }
    for (i = 0; i < GCSS_MAX_SESSIONS; i++) {
        sess_clear_queues(&s->s[i]);
    }
    free(s);
}

void gcs_snap_get_stats(const gcs_snap *s, gcs_snap_stats *st)
{
    int i;
    *st = s->st;
    st->sessions = 0;
    st->rooms = 0;
    for (i = 0; i < GCSS_MAX_SESSIONS; i++) {
        st->sessions += s->s[i].used && s->s[i].logged_in;
    }
    for (i = 0; i < GCSS_MAX_ROOMS; i++) {
        st->rooms += s->rooms[i].used;
    }
}

gc_u32 gcs_snap_game_class_of(const gcs_snap *s, const char *user)
{
    int i;
    for (i = 0; user && user[0] && i < GCSS_MAX_SESSIONS; i++) {
        if (s->s[i].used && s->s[i].logged_in && ieq(s->s[i].name, user)) {
            return s->s[i].game_class;
        }
    }
    return 0;
}
