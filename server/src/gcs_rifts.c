/** @file gcs_rifts.c
 *
 *  Arena game server of Rifts: Promise of Power, see gcs_rifts.h and
 *  docs/ARENA.md ("Rifts") for the protocol and the stream layouts.
 *
 *  Integers in the game's messages and streams are little endian (the
 *  N-Gage is an ARM in little endian mode); time values are 32 bit.
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#include "gcs_rifts.h"
#include "gcs_rcodec.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* stream key of the game ("ArenaProtocolKey") */
static const unsigned char KEY[GCS_RC_KEYLEN] = { 0x2c, 0xde, 0xcb, 0x4a, 0xa5, 0x6a, 0x35, 0x07 };

#define MAX_PLAYERS   64
#define MAX_ACTIONS   32 /* actions in progress, all players  */
#define MAX_STREAM    (256u * 1024u)
#define NAME_LEN      32  /* char fields in the rows           */
#define MAX_DEVICES   8   /* MAX_NETWORK_PLAYERS               */
#define MAX_SEARCH    24  /* challenges per search             */
#define STATS         265 /* CS_NUM                            */
#define ENTITY_FIXED  (8 + 8 + 4 + 4 + 4 + NAME_LEN + NAME_LEN + 4 * STATS)
#define ATTR_SIZE     24
#define ENTITY_MAX    (ENTITY_FIXED + 4 + 64 * ATTR_SIZE)
#define BATTLE_SIZE   164
#define POWER_VERSION 1

/* battle states */
#define WAITING_FOR_PLAYER 0
#define CHALLENGE_DELETED  1
#define IN_PROGRESS        2
#define GAME_OUT_OF_TIME   3
#define GAME_ENDED         4

/* accept results */
#define ACC_STILL_WAITING 0
#define ACC_LAST_USER     1
#define ACC_FULL          2
#define ACC_DB_ERROR      3
#define ACC_DESTROYED     4
#define ACC_BAD_TEAM      5

/* cancel reasons */
#define CR_ACTION_FAILED    1
#define CR_DATABASE_PROBLEM 4

typedef long long i64;
typedef int i32;

/* ---- stored data ---- */

typedef struct
{
    i64 id;
    i32 kind, sector, time_limit, turn_time_limit, max_rounds, occ_restrict, players_required, players_accepted,
        characters, teams, min_power, max_power, max_points, ranked, friends_only;
    i32 start_time;
    i32 round, state, team_initiative, seed, next_turn_device;
    i32 turn_end, turn_end_from_start;
    char host[NAME_LEN + 1];
    char name[NAME_LEN + 1];
} battle;

typedef struct
{
    i64 battle;
    char user[NAME_LEN + 1];
    i64 chess_clock;
    i32 team, power, device, finished, dropped;
} bplayer;

typedef struct
{
    i64 battle, id;
    char user[NAME_LEN + 1];
    size_t len;
    unsigned char *d; /* the serialized entity, ids patched */
} entity;

typedef struct
{
    i64 battle;
    i32 when, turn, action;
    size_t len;
    unsigned char *d;
} turnrow;

typedef struct
{
    i64 battle;
    char user[NAME_LEN + 1];
    i32 slot;
} saveslot;

typedef struct
{
    char user[NAME_LEN + 1];
    char friend_[NAME_LEN + 1];
} friendrow;

#define VEC(T)      \
    struct          \
    {               \
        T *v;       \
        int n, cap; \
    }

/* ---- runtime ---- */

typedef struct
{
    int used;
    gc_u32 uid;
    char name[NAME_LEN + 1];
    i32 client_credit; /* what the client may still send us   */
    i32 server_credit; /* what we may still send the client   */
} player;

typedef struct
{
    int used;
    gc_u32 uid;
    i32 object;
    int type;
    /* receiving */
    unsigned char *rx;
    size_t rx_total;
    unsigned char got[(MAX_STREAM / GCSR_CHUNK) / 8 + 1];
    /* sending: tx NULL with tx_len 0 sends one empty message */
    int sending;
    unsigned char *tx;
    size_t tx_len, tx_off;
    int tx_empty_sent;
} action;

struct gcs_rifts
{
    gcs_snap_game game;
    void (*log)(void *ctx, int level, const char *line);
    void *ctx;
    char file[256];
    gcs_snap *snap; /* set by the first callback */
    player players[MAX_PLAYERS];
    action actions[MAX_ACTIONS];
    VEC(battle)
    battles;
    VEC(bplayer)
    bplayers;
    VEC(entity)
    entities;
    VEC(turnrow)
    turns;
    VEC(saveslot)
    slots;
    VEC(friendrow)
    friends;
    i64 next_battle, next_entity;
    int dirty;
    gcs_rifts_stats st;
};

static void rlog(gcs_rifts *r, int level, const char *fmt, ...)
{
    char line[512];
    va_list ap;
    if (!r->log) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    r->log(r->ctx, level, line);
}

#define LOG_W(...) rlog(r, 1, "rifts: " __VA_ARGS__)
#define LOG_I(...) rlog(r, 2, "rifts: " __VA_ARGS__)
#define LOG_D(...) rlog(r, 3, "rifts: " __VA_ARGS__)

static int vec_grow(void **v, int *cap, int n, size_t elem)
{
    if (n < *cap) {
        return 0;
    }
    {
        int nc = *cap ? *cap * 2 : 16;
        void *nv = realloc(*v, (size_t)nc * elem);
        if (!nv) {
            return -1;
        }
        *v = nv;
        *cap = nc;
    }
    return 0;
}

#define VEC_PUSH(vec, item)                                                  \
    (vec_grow((void **)&(vec).v, &(vec).cap, (vec).n, sizeof(*(vec).v)) ? -1 \
                                                                        : ((vec).v[(vec).n++] = (item), 0))
#define VEC_DEL(vec, i) ((vec).v[i] = (vec).v[--(vec).n])

static void copy_name(char *dst, const char *src, size_t n)
{
    size_t k = 0;
    while (k < n && src[k]) {
        dst[k] = src[k];
        k++;
    }
    while (k > 0 && dst[k - 1] == ' ') { /* fixed fields may be space padded */
        k--;
    }
    dst[k] = 0;
}

static int name_eq(const char *a, const char *b)
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

static int name_prefix(const char *s, const char *prefix)
{
    for (; *prefix; s++, prefix++) {
        int x = (*s >= 'A' && *s <= 'Z') ? *s + 32 : *s;
        int y = (*prefix >= 'A' && *prefix <= 'Z') ? *prefix + 32 : *prefix;
        if (!*s || x != y) {
            return 0;
        }
    }
    return 1;
}

/* ---- streams ---- */

typedef struct
{
    const unsigned char *d;
    size_t len, pos;
    int bad;
} rd;

typedef struct
{
    unsigned char *d;
    size_t len, cap;
    int oom;
} wr;

static void rd_bytes(rd *s, void *out, size_t n)
{
    if (s->bad || s->pos + n > s->len) {
        s->bad = 1;
        memset(out, 0, n);
        return;
    }
    memcpy(out, s->d + s->pos, n);
    s->pos += n;
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

static i64 le64(const unsigned char *p)
{
    return (i64)((unsigned long long)le32(p) | ((unsigned long long)le32(p + 4) << 32));
}

static i32 rd_i32(rd *s)
{
    unsigned char b[4];
    rd_bytes(s, b, 4);
    return (i32)le32(b);
}

static i64 rd_i64(rd *s)
{
    unsigned char b[8];
    rd_bytes(s, b, 8);
    return le64(b);
}

static int rd_u8(rd *s)
{
    unsigned char b;
    rd_bytes(s, &b, 1);
    return b;
}

/* String: i32 length, bytes */
static void rd_str(rd *s, char *out, size_t cap)
{
    i32 n = rd_i32(s);
    size_t i;
    out[0] = 0;
    if (n < 0 || (size_t)n > s->len - (s->pos > s->len ? s->len : s->pos)) {
        s->bad = 1;
        return;
    }
    for (i = 0; i < (size_t)n; i++) {
        unsigned char c = s->d[s->pos + i];
        if (i + 1 < cap) {
            out[i] = (char)c;
            out[i + 1] = 0;
        }
    }
    s->pos += (size_t)n;
}

static void wr_bytes(wr *w, const void *d, size_t n)
{
    if (w->oom || !n) {
        return;
    }
    if (w->len + n > w->cap) {
        size_t nc = (w->len + n) * 2 + 64;
        unsigned char *nd = (unsigned char *)realloc(w->d, nc);
        if (!nd) {
            w->oom = 1;
            return;
        }
        w->d = nd;
        w->cap = nc;
    }
    memcpy(w->d + w->len, d, n);
    w->len += n;
}

static void wr_i32(wr *w, i32 v)
{
    unsigned char b[4];
    st32(b, (gc_u32)v);
    wr_bytes(w, b, 4);
}

static void wr_i64(wr *w, i64 v)
{
    unsigned char b[8];
    st32(b, (gc_u32)((unsigned long long)v & 0xffffffffu));
    st32(b + 4, (gc_u32)((unsigned long long)v >> 32));
    wr_bytes(w, b, 8);
}

static void wr_u8(wr *w, int v)
{
    unsigned char b = (unsigned char)v;
    wr_bytes(w, &b, 1);
}

static void wr_str(wr *w, const char *s)
{
    wr_i32(w, (i32)strlen(s));
    wr_bytes(w, s, strlen(s));
}

static void wr_name(wr *w, const char *s)
{
    char b[NAME_LEN];
    size_t k = strlen(s);
    memset(b, 0, sizeof(b));
    memcpy(b, s, k < NAME_LEN ? k : NAME_LEN - 1);
    wr_bytes(w, b, NAME_LEN);
}

/* ---- rows on the wire ---- */

static void rd_battle(rd *s, battle *b)
{
    char t[NAME_LEN];
    b->id = rd_i64(s);
    b->kind = rd_i32(s);
    b->sector = rd_i32(s);
    b->time_limit = rd_i32(s);
    b->turn_time_limit = rd_i32(s);
    b->max_rounds = rd_i32(s);
    b->occ_restrict = rd_i32(s);
    b->players_required = rd_i32(s);
    b->players_accepted = rd_i32(s);
    b->characters = rd_i32(s);
    b->teams = rd_i32(s);
    b->min_power = rd_i32(s);
    b->max_power = rd_i32(s);
    b->max_points = rd_i32(s);
    b->ranked = rd_i32(s);
    b->friends_only = rd_i32(s);
    b->start_time = rd_i32(s);
    b->round = rd_i32(s);
    b->state = rd_i32(s);
    b->team_initiative = rd_i32(s);
    b->seed = rd_i32(s);
    b->next_turn_device = rd_i32(s);
    b->turn_end = rd_i32(s);
    b->turn_end_from_start = rd_i32(s);
    rd_bytes(s, t, NAME_LEN);
    copy_name(b->host, t, NAME_LEN);
    rd_bytes(s, t, NAME_LEN);
    copy_name(b->name, t, NAME_LEN);
}

static void wr_battle(wr *w, const battle *b)
{
    wr_i64(w, b->id);
    wr_i32(w, b->kind);
    wr_i32(w, b->sector);
    wr_i32(w, b->time_limit);
    wr_i32(w, b->turn_time_limit);
    wr_i32(w, b->max_rounds);
    wr_i32(w, b->occ_restrict);
    wr_i32(w, b->players_required);
    wr_i32(w, b->players_accepted);
    wr_i32(w, b->characters);
    wr_i32(w, b->teams);
    wr_i32(w, b->min_power);
    wr_i32(w, b->max_power);
    wr_i32(w, b->max_points);
    wr_i32(w, b->ranked);
    wr_i32(w, b->friends_only);
    wr_i32(w, b->start_time);
    wr_i32(w, b->round);
    wr_i32(w, b->state);
    wr_i32(w, b->team_initiative);
    wr_i32(w, b->seed);
    wr_i32(w, b->next_turn_device);
    wr_i32(w, b->turn_end);
    wr_i32(w, b->turn_end_from_start);
    wr_name(w, b->host);
    wr_name(w, b->name);
}

static void rd_bplayer(rd *s, bplayer *p)
{
    char t[NAME_LEN];
    p->battle = rd_i64(s);
    p->chess_clock = rd_i64(s);
    p->team = rd_i32(s);
    p->power = rd_i32(s);
    p->device = rd_i32(s);
    p->finished = rd_i32(s);
    p->dropped = rd_i32(s);
    rd_bytes(s, t, NAME_LEN);
    copy_name(p->user, t, NAME_LEN);
}

static void wr_bplayer(wr *w, const bplayer *p)
{
    wr_i64(w, p->battle);
    wr_i64(w, p->chess_clock);
    wr_i32(w, p->team);
    wr_i32(w, p->power);
    wr_i32(w, p->device);
    wr_i32(w, p->finished);
    wr_i32(w, p->dropped);
    wr_name(w, p->user);
}

/* A serialized entity (fixed part + attribute list); 0 on error. */
static size_t rd_entity(rd *s, unsigned char *out)
{
    size_t start = s->pos;
    i32 n;
    unsigned char fixed[ENTITY_FIXED];
    rd_bytes(s, fixed, ENTITY_FIXED);
    n = rd_i32(s);
    if (s->bad || n < 0 || n > 64 || s->pos + (size_t)n * ATTR_SIZE > s->len) {
        s->bad = 1;
        return 0;
    }
    s->pos += (size_t)n * ATTR_SIZE;
    memcpy(out, s->d + start, s->pos - start);
    return s->pos - start;
}

static void entity_set_ids(unsigned char *d, size_t len, i64 battle_id, i64 entity_id, int device)
{
    i32 n = (i32)le32(d + ENTITY_FIXED), i;
    unsigned char b[8];
    st32(b, (gc_u32)((unsigned long long)battle_id & 0xffffffffu));
    st32(b + 4, (gc_u32)((unsigned long long)battle_id >> 32));
    memcpy(d, b, 8);
    if (entity_id >= 0) {
        st32(d + 8, (gc_u32)((unsigned long long)entity_id & 0xffffffffu));
        st32(d + 12, (gc_u32)((unsigned long long)entity_id >> 32));
    }
    if (device >= 0) {
        st32(d + 24, (gc_u32)device);
    }
    for (i = 0; i < n && ENTITY_FIXED + 4 + (size_t)(i + 1) * ATTR_SIZE <= len; i++) {
        unsigned char *a = d + ENTITY_FIXED + 4 + (size_t)i * ATTR_SIZE;
        memcpy(a, b, 8);
        memcpy(a + 8, d + 8, 8);
    }
}

static i32 entity_device(const unsigned char *d)
{
    return (i32)le32(d + 24);
}

static void entity_user(const unsigned char *d, char *out)
{
    copy_name(out, (const char *)d + 28, NAME_LEN);
}

/* ---- data access ---- */

static battle *find_battle(gcs_rifts *r, i64 id)
{
    int i;
    for (i = 0; i < r->battles.n; i++) {
        if (r->battles.v[i].id == id) {
            return &r->battles.v[i];
        }
    }
    return NULL;
}

static bplayer *find_bplayer(gcs_rifts *r, i64 b, const char *user)
{
    int i;
    for (i = 0; i < r->bplayers.n; i++) {
        if (r->bplayers.v[i].battle == b && name_eq(r->bplayers.v[i].user, user)) {
            return &r->bplayers.v[i];
        }
    }
    return NULL;
}

static int is_friend(gcs_rifts *r, const char *user, const char *friend_)
{
    int i;
    for (i = 0; i < r->friends.n; i++) {
        if (name_eq(r->friends.v[i].user, user) && name_eq(r->friends.v[i].friend_, friend_)) {
            return 1;
        }
    }
    return 0;
}

static i32 now32(void)
{
    return (i32)time(NULL);
}

static i32 game_end(const battle *b)
{
    return b->start_time + b->time_limit * 24 * 3600;
}

static void update_end_time(battle *b, i32 start, i32 src_device)
{
    b->turn_end = start + b->turn_time_limit * 3600;
    b->turn_end_from_start = (src_device != b->next_turn_device) ? 0 : b->turn_end;
}

static void update_end_from_start(battle *b, i32 start)
{
    b->turn_end_from_start = start + 30 * 60;
}

/* turns after `turn`, ordered by turn and action; returns the count and
 * the index of the last one in *last (-1 if none) */
static int turns_since(gcs_rifts *r, i64 b, i32 turn, int *order, int max, int *last)
{
    int i, n = 0, k, j;
    for (i = 0; i < r->turns.n && n < max; i++) {
        const turnrow *t = &r->turns.v[i];
        if (t->battle == b && t->turn > turn) {
            order[n++] = i;
        }
    }
    for (k = 1; k < n; k++) { /* insertion sort, the lists are short */
        int v = order[k];
        const turnrow *tv = &r->turns.v[v];
        for (j = k - 1; j >= 0; j--) {
            const turnrow *tj = &r->turns.v[order[j]];
            if (tj->turn < tv->turn || (tj->turn == tv->turn && tj->action <= tv->action)) {
                break;
            }
            order[j + 1] = order[j];
        }
        order[j + 1] = v;
    }
    *last = n ? order[n - 1] : -1;
    return n;
}

static i32 turn_device(const turnrow *t)
{
    return t->len >= 8 ? (i32)le32(t->d + 4) : -1; /* MessageEndTurn.m_iDeviceID */
}

static void remove_battle_rows(gcs_rifts *r, i64 b, const char *only_user)
{
    int i;
    for (i = r->bplayers.n - 1; i >= 0; i--) {
        if (r->bplayers.v[i].battle == b && (!only_user || name_eq(r->bplayers.v[i].user, only_user))) {
            VEC_DEL(r->bplayers, i);
        }
    }
    for (i = r->entities.n - 1; i >= 0; i--) {
        if (r->entities.v[i].battle == b && (!only_user || name_eq(r->entities.v[i].user, only_user))) {
            free(r->entities.v[i].d);
            VEC_DEL(r->entities, i);
        }
    }
}

/* the user no longer keeps the game in this save slot */
static void invalidate_slot(gcs_rifts *r, i32 slot, const char *user)
{
    int i;
    for (i = r->slots.n - 1; i >= 0; i--) {
        saveslot *s = &r->slots.v[i];
        battle *b;
        if (s->slot != slot || !name_eq(s->user, user)) {
            continue;
        }
        b = find_battle(r, s->battle);
        if (b) {
            if (b->state == WAITING_FOR_PLAYER) {
                if (name_eq(b->host, user)) {
                    b->state = CHALLENGE_DELETED;
                    LOG_I("challenge %lld deleted by its host '%s'", (long long)b->id, user);
                } else if (find_bplayer(r, b->id, user)) {
                    remove_battle_rows(r, b->id, user);
                    if (b->players_accepted > 0) {
                        b->players_accepted--;
                    }
                    LOG_I("'%s' withdrew from challenge %lld", user, (long long)b->id);
                }
            } else if (b->state == IN_PROGRESS) {
                bplayer *p = find_bplayer(r, b->id, user);
                if (p) {
                    p->dropped = 1;
                }
            }
        }
        VEC_DEL(r->slots, i);
        r->dirty = 1;
    }
}

static void register_slot(gcs_rifts *r, i64 b, const char *user, i32 slot)
{
    saveslot s;
    int i;
    for (i = r->slots.n - 1; i >= 0; i--) {
        if (r->slots.v[i].slot == slot && name_eq(r->slots.v[i].user, user)) {
            VEC_DEL(r->slots, i);
        }
    }
    memset(&s, 0, sizeof(s));
    s.battle = b;
    s.slot = slot;
    copy_name(s.user, user, NAME_LEN);
    VEC_PUSH(r->slots, s);
    r->dirty = 1;
}

/* ---- persistence ---- */

#define FILE_MAGIC "PLRIFTS1"

static void save(gcs_rifts *r)
{
    char tmp[300];
    FILE *f;
    wr w;
    int i;
    if (!r->dirty) {
        return;
    }
    r->dirty = 0;
    if (!r->file[0]) {
        return;
    }
    memset(&w, 0, sizeof(w));
    wr_bytes(&w, FILE_MAGIC, 8);
    wr_i64(&w, r->next_battle);
    wr_i64(&w, r->next_entity);
    wr_i32(&w, r->battles.n);
    for (i = 0; i < r->battles.n; i++) {
        wr_battle(&w, &r->battles.v[i]);
    }
    wr_i32(&w, r->bplayers.n);
    for (i = 0; i < r->bplayers.n; i++) {
        wr_bplayer(&w, &r->bplayers.v[i]);
    }
    wr_i32(&w, r->entities.n);
    for (i = 0; i < r->entities.n; i++) {
        wr_i32(&w, (i32)r->entities.v[i].len);
        wr_bytes(&w, r->entities.v[i].d, r->entities.v[i].len);
    }
    wr_i32(&w, r->turns.n);
    for (i = 0; i < r->turns.n; i++) {
        const turnrow *t = &r->turns.v[i];
        wr_i64(&w, t->battle);
        wr_i32(&w, t->when);
        wr_i32(&w, t->turn);
        wr_i32(&w, t->action);
        wr_i32(&w, (i32)t->len);
        wr_bytes(&w, t->d, t->len);
    }
    wr_i32(&w, r->slots.n);
    for (i = 0; i < r->slots.n; i++) {
        wr_i64(&w, r->slots.v[i].battle);
        wr_i32(&w, r->slots.v[i].slot);
        wr_name(&w, r->slots.v[i].user);
    }
    wr_i32(&w, r->friends.n);
    for (i = 0; i < r->friends.n; i++) {
        wr_name(&w, r->friends.v[i].user);
        wr_name(&w, r->friends.v[i].friend_);
    }
    if (w.oom) {
        LOG_W("out of memory saving %s", r->file);
        free(w.d);
        return;
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", r->file);
    f = fopen(tmp, "wb");
    if (!f || fwrite(w.d, 1, w.len, f) != w.len) {
        LOG_W("cannot write %s", tmp);
        if (f) {
            fclose(f);
        }
        free(w.d);
        return;
    }
    free(w.d);
    if (fclose(f) != 0) {
        LOG_W("cannot write %s", tmp);
        return;
    }
    remove(r->file);
    if (rename(tmp, r->file) != 0) {
        LOG_W("cannot replace %s", r->file);
    }
}

static void load(gcs_rifts *r)
{
    FILE *f = fopen(r->file, "rb");
    unsigned char *buf;
    long n;
    rd s;
    i32 cnt, i;
    if (!f) {
        return;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 8 || n > 64L * 1024 * 1024) {
        fclose(f);
        return;
    }
    buf = (unsigned char *)malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n || memcmp(buf, FILE_MAGIC, 8)) {
        LOG_W("%s: not a Rifts data file, ignored", r->file);
        fclose(f);
        free(buf);
        return;
    }
    fclose(f);
    memset(&s, 0, sizeof(s));
    s.d = buf;
    s.len = (size_t)n;
    s.pos = 8;
    r->next_battle = rd_i64(&s);
    r->next_entity = rd_i64(&s);
    cnt = rd_i32(&s);
    for (i = 0; i < cnt && !s.bad; i++) {
        battle b;
        rd_battle(&s, &b);
        VEC_PUSH(r->battles, b);
    }
    cnt = rd_i32(&s);
    for (i = 0; i < cnt && !s.bad; i++) {
        bplayer p;
        rd_bplayer(&s, &p);
        VEC_PUSH(r->bplayers, p);
    }
    cnt = rd_i32(&s);
    for (i = 0; i < cnt && !s.bad; i++) {
        entity e;
        i32 len = rd_i32(&s);
        if (len < (i32)(ENTITY_FIXED + 4) || (size_t)len > s.len - s.pos) {
            s.bad = 1;
            break;
        }
        memset(&e, 0, sizeof(e));
        e.d = (unsigned char *)malloc((size_t)len);
        if (!e.d) {
            break;
        }
        rd_bytes(&s, e.d, (size_t)len);
        e.len = (size_t)len;
        e.battle = le64(e.d);
        e.id = le64(e.d + 8);
        entity_user(e.d, e.user);
        VEC_PUSH(r->entities, e);
    }
    cnt = rd_i32(&s);
    for (i = 0; i < cnt && !s.bad; i++) {
        turnrow t;
        i32 len;
        memset(&t, 0, sizeof(t));
        t.battle = rd_i64(&s);
        t.when = rd_i32(&s);
        t.turn = rd_i32(&s);
        t.action = rd_i32(&s);
        len = rd_i32(&s);
        if (len < 0 || (size_t)len > s.len - s.pos) {
            s.bad = 1;
            break;
        }
        t.d = (unsigned char *)malloc(len ? (size_t)len : 1);
        if (!t.d) {
            break;
        }
        rd_bytes(&s, t.d, (size_t)len);
        t.len = (size_t)len;
        VEC_PUSH(r->turns, t);
    }
    cnt = rd_i32(&s);
    for (i = 0; i < cnt && !s.bad; i++) {
        saveslot sl;
        char t[NAME_LEN];
        sl.battle = rd_i64(&s);
        sl.slot = rd_i32(&s);
        rd_bytes(&s, t, NAME_LEN);
        copy_name(sl.user, t, NAME_LEN);
        VEC_PUSH(r->slots, sl);
    }
    cnt = rd_i32(&s);
    for (i = 0; i < cnt && !s.bad; i++) {
        friendrow fr;
        char t[NAME_LEN];
        rd_bytes(&s, t, NAME_LEN);
        copy_name(fr.user, t, NAME_LEN);
        rd_bytes(&s, t, NAME_LEN);
        copy_name(fr.friend_, t, NAME_LEN);
        VEC_PUSH(r->friends, fr);
    }
    if (s.bad) {
        LOG_W("%s is damaged, read what was readable", r->file);
    }
    free(buf);
    LOG_I("%d battles, %d characters, %d turns loaded from %s", r->battles.n, r->entities.n, r->turns.n, r->file);
}

/* ---- the actions ---- */

/* result of a handler: the answer stream (may be empty), or no answer
 * data at all, or a cancel */
#define ANS_STREAM 0
#define ANS_NONE   1
#define ANS_CANCEL 2

typedef struct
{
    gcs_rifts *r;
    const char *user; /* SNAP login of the requester */
    rd in;
    wr out;
    int cancel_reason;
} req;

static int a_server_config(req *q)
{
    i32 client_power = rd_i32(&q->in), save_version = rd_i32(&q->in), protocol = rd_i32(&q->in);
    static const i32 occ[17][5] = { { 1, 10, 10, 10, 10 }, { 2, 10, 10, 10, 10 }, { 3, 10, 10, 10, 10 }, { 4, 20, 10, 10, 10 }, { 5, 10, 10, 10, 10 }, { 6, 10, 10, 10, 10 }, { 7, 10, 10, 10, 10 }, { 8, 10, 10, 10, 10 }, { 9, 10, 10, 10, 10 }, { 10, 10, 10, 10, 10 }, { 11, 10, 10, 10, 10 }, { 12, 10, 10, 10, 10 }, { 13, 10, 10, 10, 10 }, { 14, 10, 10, 10, 10 }, { 15, 10, 10, 10, 10 }, { 16, 10, 10, 10, 10 }, { 17, 10, 10, 10, 10 } };
    int i, k;
    /* ServerConfig {max challenges, other, power rank version} */
    wr_i32(&q->out, 2);
    wr_i32(&q->out, 0);
    wr_i32(&q->out, client_power);
    wr_i32(&q->out, POWER_VERSION);
    wr_i32(&q->out, save_version); /* same versions as the game: no warning */
    wr_i32(&q->out, protocol);
    if (client_power != POWER_VERSION) {
        wr_i32(&q->out, 17);
        for (i = 0; i < 17; i++) {
            for (k = 0; k < 5; k++) {
                wr_i32(&q->out, occ[i][k]);
            }
        }
        wr_i32(&q->out, 14);
        for (i = 1; i <= 14; i++) {
            wr_i32(&q->out, i);
            wr_i32(&q->out, 1000);
        }
    }
    return ANS_STREAM;
}

/* the patterns of the original word list, [] classes and case-insensitive */
static int pattern_at(const char *s, const char *p)
{
    while (*p) {
        int c = (*s >= 'A' && *s <= 'Z') ? *s + 32 : (unsigned char)*s;
        if (!c) {
            return 0;
        }
        if (*p == '[') {
            int hit = 0;
            p++;
            while (*p && *p != ']') {
                hit |= *p == c;
                p++;
            }
            if (!*p || !hit) {
                return 0;
            }
            p++;
        } else {
            if (*p != c) {
                return 0;
            }
            p++;
        }
        s++;
    }
    return 1;
}

static int name_acceptable(const char *name)
{
    static const char *const bad[] = { "sh[i1]t", "f[u*]ck", "c[u*]nt" };
    size_t i, k;
    for (i = 0; name[i]; i++) {
        for (k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
            if (pattern_at(name + i, bad[k])) {
                return 0;
            }
        }
    }
    return 1;
}

static int a_name_validation(req *q)
{
    char name[128];
    rd_str(&q->in, name, sizeof(name));
    wr_i32(&q->out, name_acceptable(name));
    return ANS_STREAM;
}

/* shared part of upload characters, create and accept challenge */
typedef struct
{
    i64 battle;
    i32 team, n, slot, power;
    unsigned char *ent[4];
    size_t len[4];
} upload;

static int rd_upload(req *q, upload *u)
{
    i32 i;
    memset(u, 0, sizeof(*u));
    u->battle = rd_i64(&q->in);
    u->team = rd_i32(&q->in);
    u->n = rd_i32(&q->in);
    if (u->n < 0 || u->n > 4) {
        return -1;
    }
    for (i = 0; i < u->n; i++) {
        u->ent[i] = (unsigned char *)malloc(ENTITY_MAX);
        if (!u->ent[i]) {
            return -1;
        }
        u->len[i] = rd_entity(&q->in, u->ent[i]);
        if (!u->len[i]) {
            return -1;
        }
    }
    u->slot = rd_i32(&q->in);
    u->power = rd_i32(&q->in);
    return q->in.bad ? -1 : 0;
}

static void free_upload(upload *u)
{
    int i;
    for (i = 0; i < 4; i++) {
        free(u->ent[i]);
    }
}

/* add the player and the characters to battle u->battle */
static int store_upload(gcs_rifts *r, upload *u, int device, wr *ids)
{
    bplayer p;
    i32 i;
    char user[NAME_LEN + 1];
    if (u->n < 1) {
        return -1;
    }
    entity_user(u->ent[0], user);
    memset(&p, 0, sizeof(p));
    p.battle = u->battle;
    copy_name(p.user, user, NAME_LEN);
    p.team = u->team;
    p.power = u->power;
    p.device = device >= 0 ? device : entity_device(u->ent[0]);
    if (VEC_PUSH(r->bplayers, p)) {
        return -1;
    }
    for (i = 0; i < u->n; i++) {
        entity e;
        memset(&e, 0, sizeof(e));
        e.battle = u->battle;
        e.id = r->next_entity++;
        e.len = u->len[i];
        e.d = u->ent[i];
        entity_set_ids(e.d, e.len, e.battle, e.id, device);
        entity_user(e.d, e.user);
        if (VEC_PUSH(r->entities, e)) {
            return -1;
        }
        u->ent[i] = NULL; /* owned by the store now */
        wr_i64(ids, e.id);
    }
    invalidate_slot(r, u->slot, user);
    register_slot(r, u->battle, user, u->slot);
    r->dirty = 1;
    return 0;
}

static int a_upload_characters(req *q)
{
    upload u;
    int ret = ANS_CANCEL;
    if (rd_upload(q, &u) == 0 && find_battle(q->r, u.battle) && store_upload(q->r, &u, -1, &q->out) == 0) {
        ret = ANS_STREAM;
    }
    q->cancel_reason = CR_DATABASE_PROBLEM;
    free_upload(&u);
    return ret;
}

static int a_create_challenge(req *q)
{
    gcs_rifts *r = q->r;
    battle b;
    upload u;
    wr ids;
    int ret = ANS_CANCEL;
    rd_battle(&q->in, &b);
    memset(&ids, 0, sizeof(ids));
    q->cancel_reason = CR_DATABASE_PROBLEM;
    if (rd_upload(q, &u) == 0 && u.n >= 1) {
        b.id = r->next_battle++;
        u.battle = b.id;
        if (!b.host[0]) {
            entity_user(u.ent[0], b.host);
        }
        if (VEC_PUSH(r->battles, b) == 0) {
            if (store_upload(r, &u, -1, &ids) == 0) {
                wr_i64(&q->out, b.id);
                wr_bytes(&q->out, ids.d, ids.len);
                LOG_I("'%s' created challenge %lld '%s' (kind %d, %d players, %d characters)", b.host, (long long)b.id,
                      b.name, b.kind, b.players_required, u.n);
                ret = ANS_STREAM;
            } else {
                r->battles.n--;
            }
        }
    }
    free(ids.d);
    free_upload(&u);
    r->dirty = 1;
    return ret;
}

static int a_accept_challenge(req *q)
{
    gcs_rifts *r = q->r;
    upload u;
    battle *b;
    int result = ACC_DB_ERROR, device = 0, i;
    i32 seed = 0;
    wr ids;
    char user[NAME_LEN + 1];
    memset(&ids, 0, sizeof(ids));
    if (rd_upload(q, &u) != 0 || u.n < 1) {
        free_upload(&u);
        q->cancel_reason = CR_DATABASE_PROBLEM;
        return ANS_CANCEL;
    }
    entity_user(u.ent[0], user);
    b = find_battle(r, u.battle);
    if (!b) {
        result = ACC_DESTROYED;
    } else if (b->state != WAITING_FOR_PLAYER) {
        result = b->state == CHALLENGE_DELETED ? ACC_DESTROYED : ACC_DB_ERROR;
    } else if (b->players_accepted >= b->players_required) {
        result = ACC_FULL;
    } else {
        int teams[4] = { 0, 0, 0, 0 }, nteams = 0, nplayers = 0, open_slots, more;
        for (i = 0; i < r->bplayers.n; i++) {
            const bplayer *p = &r->bplayers.v[i];
            if (p->battle == b->id && p->team >= 0 && p->team < 4) {
                teams[p->team]++;
                nplayers++;
            }
        }
        for (i = 0; i < 4; i++) {
            nteams += teams[i] > 0;
        }
        open_slots = b->teams - nteams;
        more = b->players_required - nplayers;
        if (u.team < 0 || u.team > 3 || (open_slots == more && teams[u.team] > 0)) {
            result = ACC_BAD_TEAM;
        } else {
            device = b->players_accepted;
            seed = b->seed;
            b->players_accepted++;
            if (b->players_accepted == b->players_required) {
                b->state = IN_PROGRESS;
                b->start_time = now32();
                update_end_time(b, b->start_time, device);
                result = ACC_LAST_USER;
            } else {
                result = ACC_STILL_WAITING;
            }
            if (store_upload(r, &u, device, &ids) != 0) {
                b->players_accepted--;
                result = ACC_DB_ERROR;
            }
        }
    }
    wr_u8(&q->out, result);
    wr_u8(&q->out, device);
    wr_i32(&q->out, seed);
    if (result == ACC_STILL_WAITING || result == ACC_LAST_USER) {
        wr_bytes(&q->out, ids.d, ids.len);
        LOG_I("'%s' accepted challenge %lld as device %d%s", user, (long long)u.battle, device,
              result == ACC_LAST_USER ? ", the battle starts" : "");
    } else {
        LOG_I("accepting challenge %lld failed (%d)", (long long)u.battle, result);
    }
    free(ids.d);
    free_upload(&u);
    r->dirty = 1;
    return ANS_STREAM;
}

static int a_find_challenge(req *q)
{
    gcs_rifts *r = q->r;
    i32 size, min_power, max_power, kind, nplayers, max_rows;
    int friends_only, ranked_only, i, k, found = 0;
    char gname[17], user[NAME_LEN + 1];
    int hits[MAX_SEARCH];
    i32 nplayer_rows = 0;
    size = rd_i32(&q->in);
    if (size > 0) {
        if ((size_t)size > q->in.len) {
            q->in.bad = 1;
        } else {
            q->in.pos += (size_t)size; /* paging data of an earlier answer */
        }
    }
    min_power = rd_i32(&q->in);
    max_power = rd_i32(&q->in);
    kind = rd_i32(&q->in);
    nplayers = rd_i32(&q->in);
    friends_only = rd_u8(&q->in);
    ranked_only = rd_u8(&q->in);
    rd_bytes(&q->in, gname, 16);
    gname[16] = 0;
    copy_name(gname, gname, 16);
    max_rows = rd_i32(&q->in);
    rd_str(&q->in, user, sizeof(user));
    (void)max_rows;
    if (q->in.bad) {
        wr_i32(&q->out, 0);
        wr_i32(&q->out, 0);
        return ANS_STREAM;
    }
    for (i = 0; i < r->battles.n && found < MAX_SEARCH; i++) {
        const battle *b = &r->battles.v[i];
        if (b->players_accepted >= b->players_required || name_eq(b->host, user) || b->ranked != ranked_only ||
            (kind != -1 && b->kind != kind) || b->players_required != nplayers || b->min_power < min_power ||
            b->max_power > max_power || b->state != WAITING_FOR_PLAYER || find_bplayer(r, b->id, user)) {
            continue;
        }
        if (friends_only ? (!b->friends_only || !is_friend(r, b->host, user)) : b->friends_only) {
            continue;
        }
        if (gname[0] && !name_prefix(b->name, gname)) {
            continue;
        }
        hits[found++] = i;
    }
    wr_i32(&q->out, found);
    for (k = 0; k < found; k++) {
        wr_battle(&q->out, &r->battles.v[hits[k]]);
    }
    for (k = 0; k < found; k++) {
        for (i = 0; i < r->bplayers.n; i++) {
            if (r->bplayers.v[i].battle == r->battles.v[hits[k]].id) {
                wr_bplayer(&q->out, &r->bplayers.v[i]);
                nplayer_rows++;
            }
        }
    }
    wr_i32(&q->out, 4);
    wr_i32(&q->out, nplayer_rows);
    LOG_I("'%s' searched challenges (kind %d, %d players, '%s'): %d found", user, kind, nplayers, gname, found);
    return ANS_STREAM;
}

static int a_challenge_status(req *q)
{
    gcs_rifts *r = q->r;
    char user[NAME_LEN + 1];
    i32 n, i, k;
    i64 ids[32];
    i32 turn[32], slot[32];
    int order[512], last;
    rd_str(&q->in, user, sizeof(user));
    n = rd_i32(&q->in);
    if (n < 0 || n > 32) {
        q->in.bad = 1;
        n = 0;
    }
    for (i = 0; i < n; i++) {
        ids[i] = rd_i64(&q->in);
        turn[i] = rd_i32(&q->in);
        slot[i] = rd_i32(&q->in);
    }
    if (q->in.bad) {
        q->cancel_reason = CR_ACTION_FAILED;
        return ANS_CANCEL;
    }
    /* save slots the user no longer reports are gone (deleted saves) */
    for (k = r->slots.n - 1; k >= 0; k--) {
        int active = 0;
        if (k >= r->slots.n || !name_eq(r->slots.v[k].user, user)) {
            continue;
        }
        for (i = 0; i < n; i++) {
            active |= slot[i] == r->slots.v[k].slot;
        }
        if (!active) {
            invalidate_slot(r, r->slots.v[k].slot, user);
        }
    }
    wr_i32(&q->out, n);
    for (i = 0; i < n; i++) {
        battle *b = find_battle(r, ids[i]);
        i32 state = b ? b->state : CHALLENGE_DELETED, avail = 0, next = b ? b->next_turn_device : -1;
        if (b && b->state == IN_PROGRESS) {
            int cnt;
            if (game_end(b) <= now32()) {
                b->state = GAME_OUT_OF_TIME;
                r->dirty = 1;
            }
            cnt = turns_since(r, b->id, turn[i], order, 512, &last);
            if (cnt > 0) {
                next = turn_device(&r->turns.v[last]);
            }
            avail = cnt;
        }
        wr_i32(&q->out, state);
        wr_i32(&q->out, avail);
        wr_i32(&q->out, next);
    }
    LOG_D("'%s' asked for the state of %d games", user, n);
    return ANS_STREAM;
}

/* the turn list part of download turns and game upkeep */
static int download_turns(req *q)
{
    gcs_rifts *r = q->r;
    i64 b = rd_i64(&q->in);
    i32 turn = rd_i32(&q->in), i;
    int order[4096], last, n;
    if (q->in.bad) {
        q->cancel_reason = 0;
        return ANS_CANCEL;
    }
    n = turns_since(r, b, turn, order, 4096, &last);
    wr_i32(&q->out, n);
    for (i = 0; i < n; i++) {
        const turnrow *t = &r->turns.v[order[i]];
        wr_i32(&q->out, (i32)t->len);
        wr_bytes(&q->out, t->d, t->len);
    }
    LOG_D("battle %lld: %d turn messages after turn %d sent", (long long)b, n, turn);
    return ANS_STREAM;
}

static int a_turn_count(req *q)
{
    gcs_rifts *r = q->r;
    i64 b = rd_i64(&q->in);
    i32 turn = rd_i32(&q->in);
    int order[4096], last, n;
    if (q->in.bad) {
        q->cancel_reason = 0;
        return ANS_CANCEL;
    }
    n = turns_since(r, b, turn, order, 4096, &last);
    wr_i32(&q->out, n);
    if (n) {
        wr_i32(&q->out, turn_device(&r->turns.v[last]));
    }
    return ANS_STREAM;
}

static int a_upload_turns(req *q)
{
    gcs_rifts *r = q->r;
    i64 bid = rd_i64(&q->in);
    i32 turn = rd_i32(&q->in), mydev = rd_i32(&q->in), n = rd_i32(&q->in), i, ends = 0, action;
    turnrow *rows;
    battle *b;
    int ok = 0, order[4096], last, have;
    i32 now = now32();
    if (q->in.bad || n < 0 || n > 4096) {
        wr_u8(&q->out, 0);
        return ANS_STREAM;
    }
    rows = (turnrow *)calloc((size_t)(n ? n : 1), sizeof(turnrow));
    if (!rows) {
        wr_u8(&q->out, 0);
        return ANS_STREAM;
    }
    for (i = 0; i < n && !q->in.bad; i++) {
        i32 len = rd_i32(&q->in);
        if (len < 0 || (size_t)len > q->in.len - q->in.pos) {
            q->in.bad = 1;
            break;
        }
        rows[i].d = (unsigned char *)malloc(len ? (size_t)len : 1);
        if (!rows[i].d) {
            q->in.bad = 1;
            break;
        }
        rd_bytes(&q->in, rows[i].d, (size_t)len);
        rows[i].len = (size_t)len;
        rows[i].battle = bid;
        rows[i].turn = turn;
        rows[i].action = i;
        rows[i].when = now;
        if (len > 0 && rows[i].d[0] == GCSR_MSG_END_TURN) {
            ends++;
        }
    }
    /* several turns in one upload: number them back from the last one */
    if (!q->in.bad && ends > 1) {
        ends--;
        action = 0;
        for (i = 0; i < n; i++, action++) {
            rows[i].turn -= ends;
            rows[i].action = action;
            if (rows[i].len > 0 && rows[i].d[0] == GCSR_MSG_END_TURN) {
                ends--;
                action = -1;
            }
        }
    }
    b = find_battle(r, bid);
    if (q->in.bad) {
        ok = 0;
    } else if (n == 0) {
        ok = 1;
    } else if (b) {
        b->next_turn_device = turn_device(&rows[n - 1]);
        update_end_time(b, now, mydev);
        r->dirty = 1;
        have = turns_since(r, bid, rows[0].turn - 1, order, 4096, &last);
        if (have == 0) {
            for (i = 0; i < n; i++) {
                if (VEC_PUSH(r->turns, rows[i]) == 0) {
                    rows[i].d = NULL;
                }
            }
            ok = 1;
            LOG_I("battle %lld: turn %d from device %d (%d messages), next device %d", (long long)bid, turn, mydev, n,
                  b->next_turn_device);
        } else if (have == n) { /* the same upload again (answer lost) */
            ok = 1;
            for (i = 0; i < n; i++) {
                const turnrow *t = &r->turns.v[order[i]];
                if (t->turn != rows[i].turn || t->action != rows[i].action || t->len != rows[i].len ||
                    memcmp(t->d, rows[i].d, t->len)) {
                    ok = 0;
                }
            }
        }
    }
    for (i = 0; i < n; i++) {
        free(rows[i].d);
    }
    free(rows);
    wr_u8(&q->out, ok);
    return ANS_STREAM;
}

static int a_download_characters(req *q)
{
    gcs_rifts *r = q->r;
    i64 b = rd_i64(&q->in);
    char user[NAME_LEN + 1];
    int i, n = 0;
    rd_str(&q->in, user, sizeof(user));
    if (q->in.bad) {
        return ANS_NONE;
    }
    for (i = 0; i < r->entities.n; i++) {
        n += r->entities.v[i].battle == b && !name_eq(r->entities.v[i].user, user);
    }
    wr_i32(&q->out, n);
    for (i = 0; i < r->entities.n; i++) {
        const entity *e = &r->entities.v[i];
        if (e->battle == b && !name_eq(e->user, user)) {
            wr_bytes(&q->out, e->d, e->len);
        }
    }
    return ANS_STREAM;
}

static int a_end_game(req *q)
{
    gcs_rifts *r = q->r;
    i64 bid = rd_i64(&q->in);
    char user[NAME_LEN + 1];
    battle *b;
    bplayer *p;
    rd_str(&q->in, user, sizeof(user));
    b = find_battle(r, bid);
    if (b && b->state != GAME_ENDED) {
        b->state = GAME_ENDED;
        r->dirty = 1;
    }
    p = b ? find_bplayer(r, bid, user) : NULL;
    if (p && p->finished != 1) {
        p->finished = 1;
        r->dirty = 1;
    }
    LOG_I("'%s' finished battle %lld", user, (long long)bid);
    return ANS_NONE;
}

static int a_game_upkeep(req *q)
{
    gcs_rifts *r = q->r;
    int update = rd_u8(&q->in), i, game_over = 0, force = 0;
    battle given, *b;
    i32 my_device = 0, req_device = 0, now = now32();
    unsigned char dropped[MAX_DEVICES];
    char dropped_name[MAX_DEVICES][NAME_LEN + 1];
    memset(&given, 0, sizeof(given));
    memset(dropped, 0, sizeof(dropped));
    if (update) {
        rd_battle(&q->in, &given);
    } else {
        given.id = rd_i64(&q->in);
        req_device = rd_i32(&q->in);
        my_device = rd_i32(&q->in);
    }
    if (q->in.bad) {
        q->cancel_reason = CR_ACTION_FAILED;
        return ANS_CANCEL;
    }
    b = find_battle(r, given.id);
    if (update) {
        if (b) {
            *b = given;
            r->dirty = 1;
        }
        return ANS_NONE;
    }
    if (b) {
        int alive = 0, per_team[4] = { 0, 0, 0, 0 }, nteams = 0;
        for (i = 0; i < r->bplayers.n; i++) {
            const bplayer *p = &r->bplayers.v[i];
            if (p->battle != b->id) {
                continue;
            }
            if (!p->dropped) {
                alive++;
                if (p->team >= 0 && p->team < 4) {
                    per_team[p->team]++;
                }
            } else if (p->device >= 0 && p->device < MAX_DEVICES) {
                dropped[p->device] = 1;
                copy_name(dropped_name[p->device], p->user, NAME_LEN);
            }
        }
        for (i = 0; i < 4; i++) {
            nteams += per_team[i] > 0;
        }
        game_over = alive <= 1 || nteams <= 1;
        if (now < b->turn_end && (b->turn_end_from_start == 0 || now < b->turn_end_from_start)) {
            if (b->next_turn_device == -1) {
                b->next_turn_device = req_device;
                update_end_time(b, now, my_device);
                force = 1;
            } else if (b->next_turn_device == my_device && b->turn_end_from_start == 0) {
                update_end_from_start(b, now);
                force = 1;
            }
        }
        if (force) {
            r->dirty = 1;
        }
        wr_battle(&q->out, b);
    } else {
        given.next_turn_device = req_device;
        given.state = CHALLENGE_DELETED;
        wr_battle(&q->out, &given);
    }
    wr_i32(&q->out, now);
    wr_u8(&q->out, game_over);
    wr_bytes(&q->out, dropped, MAX_DEVICES);
    for (i = 0; i < MAX_DEVICES; i++) {
        if (dropped[i]) {
            wr_str(&q->out, dropped_name[i]);
        }
    }
    return download_turns(q); /* the request goes on with battle id and turn */
}

static int a_update_friends(req *q)
{
    gcs_rifts *r = q->r;
    char user[NAME_LEN + 1], f[NAME_LEN + 1];
    i32 n, i;
    rd_str(&q->in, user, sizeof(user));
    n = rd_i32(&q->in);
    if (q->in.bad || n < 0 || n > 1000) {
        return ANS_NONE;
    }
    for (i = r->friends.n - 1; i >= 0; i--) {
        if (name_eq(r->friends.v[i].user, user)) {
            VEC_DEL(r->friends, i);
        }
    }
    for (i = 0; i < n && !q->in.bad; i++) {
        friendrow fr;
        rd_str(&q->in, f, sizeof(f));
        copy_name(fr.user, user, NAME_LEN);
        copy_name(fr.friend_, f, NAME_LEN);
        VEC_PUSH(r->friends, fr);
    }
    r->dirty = 1;
    LOG_D("'%s' has %d friends", user, n);
    return ANS_NONE;
}

static const char *action_name(int t)
{
    static const char *const names[] = { "download turns", "turn count", "upload turns", "upload characters",
                                         "create challenge", "server config", "name validation", "find challenge",
                                         "accept challenge", "challenge status", "download challenge", "download characters",
                                         "end game", "game upkeep", "update friends" };
    return (t >= 0 && t < (int)(sizeof(names) / sizeof(names[0]))) ? names[t] : "unknown";
}

/* ---- game messages ---- */

static player *player_by_uid(gcs_rifts *r, gc_u32 uid)
{
    int i;
    for (i = 0; i < MAX_PLAYERS; i++) {
        if (r->players[i].used && r->players[i].uid == uid) {
            return &r->players[i];
        }
    }
    return NULL;
}

static void send_game(gcs_rifts *r, gc_u32 uid, const unsigned char *d, size_t n)
{
    if (r->snap) {
        gcs_snap_send_game_data(r->snap, uid, d, n);
    }
}

static void send_credit(gcs_rifts *r, gc_u32 uid, i32 credit)
{
    unsigned char m[8];
    memset(m, 0, sizeof(m));
    m[0] = GCSR_MSG_CREDIT;
    st32(m + 4, (gc_u32)credit);
    send_game(r, uid, m, 8);
}

static void send_cancel(gcs_rifts *r, gc_u32 uid, i32 object, i32 reason)
{
    unsigned char m[12];
    memset(m, 0, sizeof(m));
    m[0] = GCSR_MSG_SERVER_CANCEL;
    st32(m + 4, (gc_u32)object);
    st32(m + 8, (gc_u32)reason);
    send_game(r, uid, m, 12);
}

static void action_free(action *a)
{
    free(a->rx);
    free(a->tx);
    memset(a, 0, sizeof(*a));
}

/* send what the client's credit allows */
static void pump(gcs_rifts *r, action *a)
{
    player *p = player_by_uid(r, a->uid);
    unsigned char m[24 + GCSR_CHUNK];
    if (!p) {
        action_free(a);
        return;
    }
    while (a->sending) {
        size_t chunk = 0, msize;
        if (a->tx) {
            if (a->tx_off >= a->tx_len) {
                action_free(a);
                return;
            }
            chunk = a->tx_len - a->tx_off > GCSR_CHUNK ? GCSR_CHUNK : a->tx_len - a->tx_off;
        } else if (a->tx_empty_sent) {
            action_free(a);
            return;
        }
        msize = 24 + chunk;
        if ((i32)msize > p->server_credit) {
            return; /* wait for MSG_FLOW_EXTEND_CREDIT */
        }
        memset(m, 0, 24);
        m[0] = GCSR_MSG_SERVER;
        st32(m + 4, (gc_u32)a->object);
        m[8] = (unsigned char)a->type;
        st32(m + 12, (gc_u32)a->tx_off);
        st32(m + 16, (gc_u32)chunk);
        st32(m + 20, (gc_u32)a->tx_len);
        if (chunk) {
            memcpy(m + 24, a->tx + a->tx_off, chunk);
        }
        p->server_credit -= (i32)msize;
        send_game(r, a->uid, m, msize);
        a->tx_off += chunk;
        if (!a->tx) {
            a->tx_empty_sent = 1;
        }
    }
}

static void run_action(gcs_rifts *r, action *a)
{
    player *p = player_by_uid(r, a->uid);
    req q;
    int ans;
    unsigned char *plain = NULL;
    size_t plen = 0;
    memset(&q, 0, sizeof(q));
    q.r = r;
    q.user = p ? p->name : "";
    if (a->rx_total) {
        plain = gcs_rc_unpack(KEY, a->rx, a->rx_total, &plen);
        if (!plain) {
            LOG_W("user %08x: %s: stream damaged", a->uid, action_name(a->type));
            send_cancel(r, a->uid, a->object, CR_ACTION_FAILED);
            r->st.failed++;
            action_free(a);
            return;
        }
    }
    q.in.d = plain;
    q.in.len = plen;
    switch (a->type) {
    case GCSR_DOWNLOAD_TURNS:
        ans = download_turns(&q);
        break;
    case GCSR_TURN_COUNT:
        ans = a_turn_count(&q);
        break;
    case GCSR_UPLOAD_TURNS:
        ans = a_upload_turns(&q);
        break;
    case GCSR_UPLOAD_CHARACTERS:
        ans = a_upload_characters(&q);
        break;
    case GCSR_CREATE_CHALLENGE:
        ans = a_create_challenge(&q);
        break;
    case GCSR_SERVER_CONFIG:
        ans = a_server_config(&q);
        break;
    case GCSR_NAME_VALIDATION:
        ans = a_name_validation(&q);
        break;
    case GCSR_FIND_CHALLENGE:
        ans = a_find_challenge(&q);
        break;
    case GCSR_ACCEPT_CHALLENGE:
        ans = a_accept_challenge(&q);
        break;
    case GCSR_CHALLENGE_STATUS:
        ans = a_challenge_status(&q);
        break;
    case GCSR_DOWNLOAD_CHARACTERS:
        ans = a_download_characters(&q);
        break;
    case GCSR_END_GAME:
        ans = a_end_game(&q);
        break;
    case GCSR_GAME_UPKEEP:
        ans = a_game_upkeep(&q);
        break;
    case GCSR_UPDATE_FRIENDS:
        ans = a_update_friends(&q);
        break;
    default:
        LOG_W("user %08x: unknown action type %d", a->uid, a->type);
        q.cancel_reason = CR_ACTION_FAILED;
        ans = ANS_CANCEL;
    }
    free(plain);
    save(r);
    r->st.requests++;
    if (ans == ANS_CANCEL || q.out.oom) {
        LOG_I("user %08x: %s failed", a->uid, action_name(a->type));
        send_cancel(r, a->uid, a->object, q.cancel_reason);
        r->st.failed++;
        free(q.out.d);
        action_free(a);
        return;
    }
    free(a->rx);
    a->rx = NULL;
    a->sending = 1;
    if (ans == ANS_STREAM) {
        a->tx = gcs_rc_pack(KEY, q.out.d ? q.out.d : (const unsigned char *)"", q.out.len, &a->tx_len);
        if (!a->tx) {
            free(q.out.d);
            send_cancel(r, a->uid, a->object, CR_ACTION_FAILED);
            action_free(a);
            return;
        }
    }
    LOG_D("user %08x: %s: %u bytes answer", a->uid, action_name(a->type), (unsigned)q.out.len);
    free(q.out.d);
    pump(r, a);
}

static action *find_action(gcs_rifts *r, gc_u32 uid, i32 object)
{
    int i;
    for (i = 0; i < MAX_ACTIONS; i++) {
        if (r->actions[i].used && r->actions[i].uid == uid && r->actions[i].object == object) {
            return &r->actions[i];
        }
    }
    return NULL;
}

static void on_server_msg(gcs_rifts *r, gc_u32 uid, const unsigned char *d, size_t n)
{
    i32 object, offset, len, total;
    int type;
    action *a;
    size_t chunk;
    if (n < 24) {
        return;
    }
    object = (i32)le32(d + 4);
    type = d[8];
    offset = (i32)le32(d + 12);
    len = (i32)le32(d + 16);
    total = (i32)le32(d + 20);
    if (len < 0 || len > GCSR_CHUNK || (size_t)len + 24 > n || total < 0 || (size_t)total > MAX_STREAM ||
        offset < 0 || offset % GCSR_CHUNK != 0 || offset + len > total) {
        LOG_W("user %08x: bad server message (offset %d, length %d of %d)", uid, offset, len, total);
        return;
    }
    a = find_action(r, uid, object);
    if (!a) {
        int i;
        for (i = 0; i < MAX_ACTIONS && r->actions[i].used; i++) {
        }
        if (i == MAX_ACTIONS) {
            LOG_W("user %08x: too many actions in progress", uid);
            send_cancel(r, uid, object, CR_ACTION_FAILED);
            return;
        }
        a = &r->actions[i];
        memset(a, 0, sizeof(*a));
        a->used = 1;
        a->uid = uid;
        a->object = object;
        a->type = type;
        a->rx_total = (size_t)total;
        if (total) {
            a->rx = (unsigned char *)malloc((size_t)total);
            if (!a->rx) {
                action_free(a);
                return;
            }
        }
        LOG_D("user %08x: %s (object %08x, %d bytes)", uid, action_name(type), (unsigned)object, total);
    }
    if (a->sending) {
        return; /* a late duplicate */
    }
    if (!a->rx_total) {
        run_action(r, a);
        return;
    }
    chunk = (size_t)offset / GCSR_CHUNK;
    if (!(a->got[chunk / 8] & (1 << (chunk % 8)))) {
        a->got[chunk / 8] |= (unsigned char)(1 << (chunk % 8));
        memcpy(a->rx + offset, d + 24, (size_t)len);
    }
    {
        size_t k, chunks = (a->rx_total + GCSR_CHUNK - 1) / GCSR_CHUNK;
        for (k = 0; k < chunks; k++) {
            if (!(a->got[k / 8] & (1 << (k % 8)))) {
                return;
            }
        }
    }
    run_action(r, a);
}

static void g_data(void *ctx, gcs_snap *s, gc_u32 uid, const gc_u8 *d, size_t n)
{
    gcs_rifts *r = (gcs_rifts *)ctx;
    player *p;
    r->snap = s;
    if (n < 1) {
        return;
    }
    p = player_by_uid(r, uid);
    if (!p) {
        LOG_W("data from user %08x who is not in a Rifts room", uid);
        return;
    }
    if (d[0] == GCSR_MSG_SERVER || d[0] == GCSR_MSG_SERVER_CANCEL || d[0] == GCSR_MSG_CREDIT) {
        /* flow control: give the client more credit when half is used */
        p->client_credit -= (i32)n;
        if (p->client_credit < GCSR_CREDIT / 2) {
            p->client_credit += GCSR_CREDIT / 2;
            send_credit(r, uid, GCSR_CREDIT / 2);
        }
    }
    switch (d[0]) {
    case GCSR_MSG_SERVER:
        on_server_msg(r, uid, d, n);
        break;
    case GCSR_MSG_SERVER_CANCEL:
        if (n >= 12) {
            action *a = find_action(r, uid, (i32)le32(d + 4));
            if (a) {
                LOG_I("user %08x cancelled %s", uid, action_name(a->type));
                action_free(a);
            }
        }
        break;
    case GCSR_MSG_CREDIT:
        if (n >= 8) {
            int i;
            p->server_credit += (i32)le32(d + 4);
            for (i = 0; i < MAX_ACTIONS; i++) {
                if (r->actions[i].used && r->actions[i].uid == uid && r->actions[i].sending) {
                    pump(r, &r->actions[i]);
                }
            }
        }
        break;
    case GCSR_MSG_PING:
    {
        unsigned char m[4] = { GCSR_MSG_PING, 0, 0, 0 };
        send_game(r, uid, m, 4);
        break;
    }
    default:
        LOG_W("user %08x: unexpected game message %u (%u bytes)", uid, d[0], (unsigned)n);
    }
}

static void g_joined(void *ctx, gcs_snap *s, gc_u32 uid, const char *user)
{
    gcs_rifts *r = (gcs_rifts *)ctx;
    int i;
    r->snap = s;
    if (player_by_uid(r, uid)) {
        return;
    }
    for (i = 0; i < MAX_PLAYERS && r->players[i].used; i++) {
    }
    if (i == MAX_PLAYERS) {
        LOG_W("too many players");
        return;
    }
    memset(&r->players[i], 0, sizeof(r->players[i]));
    r->players[i].used = 1;
    r->players[i].uid = uid;
    copy_name(r->players[i].name, user, NAME_LEN);
    r->players[i].client_credit = GCSR_CREDIT;
    r->players[i].server_credit = GCSR_CREDIT;
    LOG_I("'%s' (%08x) is at the game server", user, uid);
}

static void g_left(void *ctx, gcs_snap *s, gc_u32 uid)
{
    gcs_rifts *r = (gcs_rifts *)ctx;
    player *p = player_by_uid(r, uid);
    int i;
    r->snap = s;
    for (i = 0; i < MAX_ACTIONS; i++) {
        if (r->actions[i].used && r->actions[i].uid == uid) {
            action_free(&r->actions[i]);
        }
    }
    if (p) {
        LOG_I("'%s' (%08x) left the game server", p->name, uid);
        memset(p, 0, sizeof(*p));
    }
}

static void g_poll(void *ctx, gcs_snap *s, gc_u32 now)
{
    gcs_rifts *r = (gcs_rifts *)ctx;
    (void)now;
    r->snap = s;
    save(r);
}

gcs_rifts *gcs_rifts_create(const char *file, void (*log)(void *ctx, int level, const char *line), void *ctx)
{
    gcs_rifts *r = (gcs_rifts *)calloc(1, sizeof(*r));
    if (!r) {
        return NULL;
    }
    r->log = log;
    r->ctx = ctx;
    r->next_battle = 1;
    r->next_entity = 1;
    if (file) {
        snprintf(r->file, sizeof(r->file), "%s", file);
    }
    if (r->file[0]) {
        load(r);
    }
    r->game.game_class = GCS_RIFTS_GAME_CLASS;
    r->game.name = "Rifts: Promise of Power";
    r->game.ctx = r;
    r->game.joined = g_joined;
    r->game.left = g_left;
    r->game.data = g_data;
    r->game.poll = g_poll;
    return r;
}

const gcs_snap_game *gcs_rifts_game(gcs_rifts *r)
{
    return &r->game;
}

void gcs_rifts_destroy(gcs_rifts *r)
{
    int i;
    if (!r) {
        return;
    }
    save(r);
    for (i = 0; i < MAX_ACTIONS; i++) {
        action_free(&r->actions[i]);
    }
    for (i = 0; i < r->entities.n; i++) {
        free(r->entities.v[i].d);
    }
    for (i = 0; i < r->turns.n; i++) {
        free(r->turns.v[i].d);
    }
    free(r->battles.v);
    free(r->bplayers.v);
    free(r->entities.v);
    free(r->turns.v);
    free(r->slots.v);
    free(r->friends.v);
    free(r);
}

void gcs_rifts_get_stats(const gcs_rifts *r, gcs_rifts_stats *st)
{
    int i;
    *st = r->st;
    st->players = 0;
    for (i = 0; i < MAX_PLAYERS; i++) {
        st->players += r->players[i].used;
    }
    st->battles = r->battles.n;
}
