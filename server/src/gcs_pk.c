/** @file gcs_pk.c
 *
 *  Pocket Kingdom game server, see gcs_pk.h.  Message layouts come from
 *  the network code of the retail game (6RAH.APP 110804: the senders at
 *  0x65ce0..0x67b80, the receiver at 0x67eb8) and the Global Auction
 *  screens; docs/ARENA.md, "Pocket Kingdom game server".
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#include "gcs_pk.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NAMESZ (GCSP_MAX_NAME + 1)
#define ITEMSZ 7   /* item bytes of an exhibit (category, type, ...)  */
#define SUMSZ  112 /* kingdom summary message                         */
#define CHATSZ 116 /* chat message                                    */

typedef struct
{
    char name[NAMESZ];
    int area;        /* map area (-1: never told)                    */
    gc_u8 look[4];   /* castle, transporter, mark, colour            */
    gc_u32 vp, loot; /* victory points and loot of the last summary  */
    gc_u8 summary[SUMSZ];
    int has_summary;
    long seen; /* last time online, seconds since 1970         */
} kingdom;

typedef struct
{
    gc_u32 id;
    char seller[NAMESZ];
    gc_u8 item[ITEMSZ];
    gc_u32 start;      /* starting price                               */
    gc_u32 price;      /* highest bid, or the starting price           */
    gc_u32 bids;       /* number of accepted bids                      */
    char high[NAMESZ]; /* highest bidder ("" without bids)             */
    long end;          /* seconds since 1970                           */
    int seller_done;   /* the seller collected the result              */
} auction;

typedef struct
{
    gc_u32 auction;
    char user[NAMESZ];
    gc_u32 held; /* loot the bidder paid and gets back unless he wins */
} bid;

typedef struct
{
    char to[NAMESZ];
    gc_u8 m[CHATSZ];
} mail;

typedef struct
{
    gc_u32 uid; /* 0: free */
    char name[NAMESZ];
} player;

/* Scrimmage and Ranked are different rooms: kingdoms only meet kingdoms
 * of their own room (0: unknown, e.g. without SNAP in tests) */
static int same_room(gcs_snap *s, const player *a, const player *b)
{
    gc_u32 ra, rb;
    if (!s) {
        return 1;
    }
    ra = gcs_snap_room_of(s, a->uid);
    rb = gcs_snap_room_of(s, b->uid);
    return !ra || !rb || ra == rb;
}

struct gcs_pk
{
    char file[260];
    void (*log)(void *ctx, int level, const char *line);
    void *ctx;
    gcs_snap_game game;
    void (*out)(void *ctx, gc_u32 uid, const gc_u8 *d, size_t n);
    void *out_ctx;
    long fixed_time;

    player pl[GCSS_MAX_SESSIONS];
    kingdom *k;
    int nk, capk;
    auction *a;
    int na, capa;
    bid *b;
    int nb, capb;
    mail *ml;
    int nml, capml;
    gc_u32 next_id;
    gc_u32 messages, relayed;
};

/* ---- helpers ---- */

static void plog(gcs_pk *p, int level, const char *fmt, ...)
{
    char line[400];
    va_list ap;
    if (!p->log) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    p->log(p->ctx, level, line);
}

#define LOG_E(...) plog(p, 0, "pk: " __VA_ARGS__)
#define LOG_W(...) plog(p, 1, "pk: " __VA_ARGS__)
#define LOG_I(...) plog(p, 2, "pk: " __VA_ARGS__)
#define LOG_D(...) plog(p, 3, "pk: " __VA_ARGS__)

static long now_s(const gcs_pk *p)
{
    return p->fixed_time ? p->fixed_time : (long)time(NULL);
}

static gc_u32 be32(const gc_u8 *d)
{
    return ((gc_u32)d[0] << 24) | ((gc_u32)d[1] << 16) | ((gc_u32)d[2] << 8) | d[3];
}

static void put32(gc_u8 *d, gc_u32 v)
{
    d[0] = (gc_u8)(v >> 24);
    d[1] = (gc_u8)(v >> 16);
    d[2] = (gc_u8)(v >> 8);
    d[3] = (gc_u8)v;
}

static int lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int ieq(const char *a, const char *b)
{
    while (*a && lower((unsigned char)*a) == lower((unsigned char)*b)) {
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

static void copy_name(char *dst, const char *src)
{
    size_t k = 0;
    while (src && src[k] && k < GCSP_MAX_NAME) {
        dst[k] = src[k];
        k++;
    }
    dst[k] = 0;
}

/* a 16 byte name field of a message (stops at 0 or a control char) */
static void get_name(char *dst, const gc_u8 *f)
{
    size_t k = 0;
    while (k < GCSP_MAX_NAME && f[k] >= 0x20 && f[k] != 0x7f) {
        dst[k] = (char)f[k];
        k++;
    }
    dst[k] = 0;
}

static void put_name(gc_u8 *f, const char *name)
{
    size_t k = 0;
    memset(f, 0, NAMESZ);
    while (name[k] && k < GCSP_MAX_NAME) {
        f[k] = (gc_u8)name[k];
        k++;
    }
}

static int grow(void **arr, int *cap, int need, size_t elem)
{
    void *n;
    int c;
    if (need <= *cap) {
        return 0;
    }
    c = *cap ? *cap * 2 : 16;
    while (c < need) {
        c *= 2;
    }
    n = realloc(*arr, (size_t)c * elem);
    if (!n) {
        return -1;
    }
    *arr = n;
    *cap = c;
    return 0;
}

static void hex_out(FILE *f, const gc_u8 *d, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        fprintf(f, "%02x", d[i]);
    }
}

static int hex_in(const char *s, gc_u8 *d, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned v;
        if (!s[2 * i] || !s[2 * i + 1] || sscanf(s + 2 * i, "%2x", &v) != 1) {
            return -1;
        }
        d[i] = (gc_u8)v;
    }
    return 0;
}

/* ---- persistence ----
 *
 * Tab separated lines:
 *   N <next auction id>
 *   K <name> <area> <look hex8> <vp> <loot> <seen> <summary hex|->
 *   A <id> <seller> <item hex14> <start> <price> <bids> <high|-> <end> <seller_done>
 *   B <auction> <user> <held>
 *   M <to> <chat message hex>
 */

static void save(gcs_pk *p)
{
    char tmp[280];
    FILE *f;
    int i;
    if (!p->file[0]) {
        return;
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", p->file);
    f = fopen(tmp, "w");
    if (!f) {
        LOG_E("cannot write %s", tmp);
        return;
    }
    fprintf(f, "N\t%lu\n", (unsigned long)p->next_id);
    for (i = 0; i < p->nk; i++) {
        kingdom *k = &p->k[i];
        fprintf(f, "K\t%s\t%d\t", k->name, k->area);
        hex_out(f, k->look, 4);
        fprintf(f, "\t%lu\t%lu\t%ld\t", (unsigned long)k->vp, (unsigned long)k->loot, k->seen);
        if (k->has_summary) {
            hex_out(f, k->summary, SUMSZ);
        } else {
            fputc('-', f);
        }
        fputc('\n', f);
    }
    for (i = 0; i < p->na; i++) {
        auction *a = &p->a[i];
        fprintf(f, "A\t%lu\t%s\t", (unsigned long)a->id, a->seller);
        hex_out(f, a->item, ITEMSZ);
        fprintf(f, "\t%lu\t%lu\t%lu\t%s\t%ld\t%d\n", (unsigned long)a->start, (unsigned long)a->price,
                (unsigned long)a->bids, a->high[0] ? a->high : "-", a->end, a->seller_done);
    }
    for (i = 0; i < p->nb; i++) {
        fprintf(f, "B\t%lu\t%s\t%lu\n", (unsigned long)p->b[i].auction, p->b[i].user, (unsigned long)p->b[i].held);
    }
    for (i = 0; i < p->nml; i++) {
        fprintf(f, "M\t%s\t", p->ml[i].to);
        hex_out(f, p->ml[i].m, CHATSZ);
        fputc('\n', f);
    }
    if (fclose(f) != 0) {
        LOG_E("cannot write %s", tmp);
        remove(tmp);
        return;
    }
    remove(p->file); /* rename does not replace on Windows */
    if (rename(tmp, p->file) != 0) {
        LOG_E("cannot rename %s to %s", tmp, p->file);
    }
}

/* split line at tabs into at most max fields (in place) */
static int split(char *line, char **f, int max)
{
    int n = 0;
    char *s = line;
    while (n < max) {
        char *t = strchr(s, '\t');
        f[n++] = s;
        if (!t) {
            break;
        }
        *t = 0;
        s = t + 1;
    }
    return n;
}

static void load(gcs_pk *p)
{
    char line[1024];
    FILE *f = p->file[0] ? fopen(p->file, "r") : NULL;
    if (!f) {
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        char *fl[10];
        int n;
        line[strcspn(line, "\r\n")] = 0;
        n = split(line, fl, 10);
        if (n == 2 && !strcmp(fl[0], "N")) {
            p->next_id = (gc_u32)strtoul(fl[1], NULL, 10);
        } else if (n == 8 && !strcmp(fl[0], "K") && fl[1][0]) {
            kingdom k;
            memset(&k, 0, sizeof(k));
            copy_name(k.name, fl[1]);
            k.area = atoi(fl[2]);
            if (hex_in(fl[3], k.look, 4) != 0) {
                continue;
            }
            k.vp = (gc_u32)strtoul(fl[4], NULL, 10);
            k.loot = (gc_u32)strtoul(fl[5], NULL, 10);
            k.seen = strtol(fl[6], NULL, 10);
            k.has_summary = strcmp(fl[7], "-") != 0 && hex_in(fl[7], k.summary, SUMSZ) == 0;
            if (grow((void **)&p->k, &p->capk, p->nk + 1, sizeof(kingdom)) == 0) {
                p->k[p->nk++] = k;
            }
        } else if (n == 10 && !strcmp(fl[0], "A") && fl[2][0]) {
            auction a;
            memset(&a, 0, sizeof(a));
            a.id = (gc_u32)strtoul(fl[1], NULL, 10);
            copy_name(a.seller, fl[2]);
            if (hex_in(fl[3], a.item, ITEMSZ) != 0) {
                continue;
            }
            a.start = (gc_u32)strtoul(fl[4], NULL, 10);
            a.price = (gc_u32)strtoul(fl[5], NULL, 10);
            a.bids = (gc_u32)strtoul(fl[6], NULL, 10);
            if (strcmp(fl[7], "-") != 0) {
                copy_name(a.high, fl[7]);
            }
            a.end = strtol(fl[8], NULL, 10);
            a.seller_done = atoi(fl[9]) != 0;
            if (grow((void **)&p->a, &p->capa, p->na + 1, sizeof(auction)) == 0) {
                p->a[p->na++] = a;
            }
            if (a.id >= p->next_id) {
                p->next_id = a.id + 1;
            }
        } else if (n == 4 && !strcmp(fl[0], "B") && fl[2][0]) {
            bid b;
            memset(&b, 0, sizeof(b));
            b.auction = (gc_u32)strtoul(fl[1], NULL, 10);
            copy_name(b.user, fl[2]);
            b.held = (gc_u32)strtoul(fl[3], NULL, 10);
            if (grow((void **)&p->b, &p->capb, p->nb + 1, sizeof(bid)) == 0) {
                p->b[p->nb++] = b;
            }
        } else if (n == 3 && !strcmp(fl[0], "M") && fl[1][0]) {
            mail m;
            memset(&m, 0, sizeof(m));
            copy_name(m.to, fl[1]);
            if (hex_in(fl[2], m.m, CHATSZ) == 0 &&
                grow((void **)&p->ml, &p->capml, p->nml + 1, sizeof(mail)) == 0) {
                p->ml[p->nml++] = m;
            }
        }
    }
    fclose(f);
    if (p->next_id == 0) {
        p->next_id = 1;
    }
    LOG_I("%d kingdoms, %d auctions, %d bids, %d waiting messages loaded from %s", p->nk, p->na, p->nb, p->nml,
          p->file);
}

/* ---- lookups ---- */

static player *player_by_uid(gcs_pk *p, gc_u32 uid)
{
    int i;
    for (i = 0; i < GCSS_MAX_SESSIONS; i++) {
        if (p->pl[i].uid && p->pl[i].uid == uid) {
            return &p->pl[i];
        }
    }
    return NULL;
}

/* is the player's SNAP session still there?  (Players who never
 * entered a room get no "left" call, so check before using one.) */
static int alive(gcs_pk *p, gcs_snap *s, player *pl)
{
    const char *n;
    if (!pl->uid) {
        return 0;
    }
    if (!s) {
        return 1; /* tests without SNAP */
    }
    n = gcs_snap_user_of(s, pl->uid);
    if (n && ieq(n, pl->name)) {
        return 1;
    }
    LOG_D("user %08x '%s' is gone", pl->uid, pl->name);
    pl->uid = 0;
    return 0;
}

static player *player_by_name(gcs_pk *p, gcs_snap *s, const char *name)
{
    int i;
    for (i = 0; i < GCSS_MAX_SESSIONS; i++) {
        if (p->pl[i].uid && ieq(p->pl[i].name, name) && alive(p, s, &p->pl[i])) {
            return &p->pl[i];
        }
    }
    return NULL;
}

static kingdom *kingdom_by_name(gcs_pk *p, const char *name)
{
    int i;
    for (i = 0; i < p->nk; i++) {
        if (ieq(p->k[i].name, name)) {
            return &p->k[i];
        }
    }
    return NULL;
}

static kingdom *kingdom_get(gcs_pk *p, const char *name)
{
    kingdom *k = kingdom_by_name(p, name);
    if (k || grow((void **)&p->k, &p->capk, p->nk + 1, sizeof(kingdom)) != 0) {
        return k;
    }
    k = &p->k[p->nk++];
    memset(k, 0, sizeof(*k));
    copy_name(k->name, name);
    k->area = -1;
    return k;
}

static auction *auction_by_id(gcs_pk *p, gc_u32 id)
{
    int i;
    for (i = 0; i < p->na; i++) {
        if (p->a[i].id == id) {
            return &p->a[i];
        }
    }
    return NULL;
}

static bid *bid_of(gcs_pk *p, gc_u32 auction_id, const char *user)
{
    int i;
    for (i = 0; i < p->nb; i++) {
        if (p->b[i].auction == auction_id && ieq(p->b[i].user, user)) {
            return &p->b[i];
        }
    }
    return NULL;
}

static int ended(const gcs_pk *p, const auction *a)
{
    return now_s(p) >= a->end;
}

static gc_u32 secs_left(const gcs_pk *p, const auction *a)
{
    long t = a->end - now_s(p);
    return t > 0 ? (gc_u32)t : 0;
}

/* drop auctions nobody needs any more: collected by the seller and
 * without bids still to collect */
static void auction_gc(gcs_pk *p)
{
    int i = 0;
    while (i < p->na) {
        auction *a = &p->a[i];
        int j, open = 0;
        for (j = 0; j < p->nb; j++) {
            open |= p->b[j].auction == a->id;
        }
        if (a->seller_done && !open) {
            p->a[i] = p->a[--p->na];
        } else {
            i++;
        }
    }
}

static void bid_remove(gcs_pk *p, bid *b)
{
    *b = p->b[--p->nb];
}

/* ---- output ---- */

static void send_to(gcs_pk *p, gcs_snap *s, gc_u32 uid, const gc_u8 *d, size_t n)
{
    if (p->out) {
        p->out(p->out_ctx, uid, d, n);
    } else if (s && gcs_snap_send_user_data(s, uid, d, n) != 0) {
        LOG_W("user %08x: cannot send message %u", uid, d[0]);
    }
}

/* the short answers the game only counts (129, 130, 145, 149, 150) and
 * the chat delivery note (146) */
static void answer4(gcs_pk *p, gcs_snap *s, gc_u32 uid, gc_u8 type, gc_u8 seq, gc_u8 b2)
{
    gc_u8 r[4];
    r[0] = type;
    r[1] = seq;
    r[2] = b2;
    r[3] = 0;
    send_to(p, s, uid, r, sizeof(r));
}

/* ---- kingdoms ---- */

static void deliver_mail(gcs_pk *p, gcs_snap *s, player *pl)
{
    int i = 0, n = 0;
    while (i < p->nml) {
        if (ieq(p->ml[i].to, pl->name)) {
            send_to(p, s, pl->uid, p->ml[i].m, CHATSZ);
            p->ml[i] = p->ml[--p->nml];
            n++;
        } else {
            i++;
        }
    }
    if (n) {
        LOG_I("'%s': %d waiting chat messages delivered", pl->name, n);
        save(p);
    }
}

static void on_register(gcs_pk *p, gcs_snap *s, player *pl, const gc_u8 *d)
{
    kingdom *k = kingdom_get(p, pl->name);
    if (k) {
        k->area = d[2];
        memcpy(k->look, d + 3, 4);
        k->seen = now_s(p);
        LOG_I("'%s' online in area %d (look %u/%u/%u/%u)", pl->name, k->area, d[3], d[4], d[5], d[6]);
        save(p);
    }
    answer4(p, s, pl->uid, GCSP_ANSWER | GCSP_REGISTER, d[1], 0);
    deliver_mail(p, s, pl);
}

static void on_move(gcs_pk *p, player *pl, const gc_u8 *d)
{
    kingdom *k = kingdom_get(p, pl->name);
    if (k && k->area != d[2]) {
        LOG_I("'%s' moved to area %d", pl->name, d[2]);
        k->area = d[2];
        save(p);
    }
}

/* enemy search: up to five online kingdoms in the same area */
static void on_search(gcs_pk *p, gcs_snap *s, player *pl, const gc_u8 *d)
{
    gc_u8 r[4 + 20 * GCSP_SEARCH_MAX];
    kingdom *cand[GCSS_MAX_SESSIONS];
    int i, n = 0, out = 0;
    kingdom *me = kingdom_get(p, pl->name);
    if (me) {
        me->area = d[2];
    }
    for (i = 0; i < GCSS_MAX_SESSIONS; i++) {
        kingdom *k;
        if (!p->pl[i].uid || &p->pl[i] == pl || !alive(p, s, &p->pl[i]) || !same_room(s, pl, &p->pl[i])) {
            continue;
        }
        k = kingdom_by_name(p, p->pl[i].name);
        if (k && k->area == d[2] && !ieq(k->name, pl->name)) {
            cand[n++] = k;
        }
    }
    memset(r, 0, sizeof(r));
    r[0] = GCSP_ANSWER | GCSP_SEARCH;
    r[1] = d[1];
    while (n > 0 && out < GCSP_SEARCH_MAX) {
        int pick = rand() % n;
        gc_u8 *e = r + 4 + 20 * out++;
        put_name(e, cand[pick]->name);
        memcpy(e + 16, cand[pick]->look, 4);
        cand[pick] = cand[--n];
    }
    LOG_I("'%s' searched area %d: %d kingdoms", pl->name, d[2], out);
    send_to(p, s, pl->uid, r, sizeof(r));
}

/* ---- relay ---- */

typedef struct
{
    gc_u8 type;
    size_t len;
    size_t to;   /* offset of the receiver's name */
    size_t from; /* offset of the sender's name   */
} relay_fmt;

static const relay_fmt RELAY[] = {
    { GCSP_SCOUT, 36, 20, 4 },
    { GCSP_ANSWER | GCSP_SCOUT, 52, 4, 20 },
    { GCSP_ATTACK, 88, 20, 4 },
    { GCSP_ANSWER | GCSP_ATTACK, 88, 8, 24 },
    { GCSP_ENGAGE, 36, 20, 4 },
    { GCSP_CHAT, CHATSZ, 20, 4 },
};

/* answer a scout or attack for a receiver who is not there: the
 * attacker's game then reports that the kingdom has moved area */
static void not_there(gcs_pk *p, gcs_snap *s, player *pl, const gc_u8 *d, const char *to)
{
    gc_u8 r[88];
    memset(r, 0, sizeof(r));
    r[1] = d[1];
    r[2] = GCSP_MOVED_AREA;
    if (d[0] == GCSP_SCOUT) {
        r[0] = GCSP_ANSWER | GCSP_SCOUT;
        put_name(r + 4, pl->name);
        put_name(r + 20, to);
        send_to(p, s, pl->uid, r, 52);
    } else {
        r[0] = GCSP_ANSWER | GCSP_ATTACK;
        put_name(r + 8, pl->name);
        put_name(r + 24, to);
        send_to(p, s, pl->uid, r, 88);
    }
}

static void on_relay(gcs_pk *p, gcs_snap *s, player *pl, const relay_fmt *f, const gc_u8 *d)
{
    gc_u8 m[CHATSZ];
    char to[NAMESZ];
    player *dst;
    kingdom *src_k, *dst_k;
    get_name(to, d + f->to);
    memcpy(m, d, f->len);
    put_name(m + f->from, pl->name); /* the sender is who the session says */
    dst = to[0] ? player_by_name(p, s, to) : NULL;
    src_k = kingdom_by_name(p, pl->name);
    dst_k = to[0] ? kingdom_by_name(p, to) : NULL;

    if (f->type == GCSP_CHAT) {
        if (dst) {
            send_to(p, s, dst->uid, m, f->len);
            p->relayed++;
            answer4(p, s, pl->uid, GCSP_ANSWER | GCSP_CHAT, d[1], GCSP_CHAT_OK);
            LOG_I("chat '%s' -> '%s'", pl->name, to);
        } else if (dst_k) {
            int i, waiting = 0;
            for (i = 0; i < p->nml; i++) {
                waiting += ieq(p->ml[i].to, dst_k->name);
            }
            if (waiting < GCSP_MAX_MAIL && grow((void **)&p->ml, &p->capml, p->nml + 1, sizeof(mail)) == 0) {
                copy_name(p->ml[p->nml].to, dst_k->name);
                memcpy(p->ml[p->nml].m, m, CHATSZ);
                p->nml++;
                save(p);
                answer4(p, s, pl->uid, GCSP_ANSWER | GCSP_CHAT, d[1], GCSP_CHAT_OK);
                LOG_I("chat '%s' -> '%s' kept until the next visit", pl->name, dst_k->name);
            } else {
                answer4(p, s, pl->uid, GCSP_ANSWER | GCSP_CHAT, d[1], GCSP_CHAT_FAILED);
                LOG_W("chat '%s' -> '%s': too many waiting messages", pl->name, dst_k->name);
            }
        } else {
            answer4(p, s, pl->uid, GCSP_ANSWER | GCSP_CHAT, d[1], GCSP_CHAT_FAILED);
            LOG_I("chat '%s' -> '%s': no such kingdom", pl->name, to);
        }
        return;
    }

    if (f->type == GCSP_SCOUT || f->type == GCSP_ATTACK) {
        /* the defender must be online in the attacker's area */
        if (!dst || !src_k || !dst_k || src_k->area != dst_k->area || dst == pl || !same_room(s, pl, dst)) {
            LOG_I("%s '%s' -> '%s': not there", f->type == GCSP_SCOUT ? "scout" : "attack", pl->name, to);
            not_there(p, s, pl, d, to);
            return;
        }
    } else if (!dst) {
        LOG_I("message %u '%s' -> '%s': receiver gone", f->type, pl->name, to);
        return;
    }
    send_to(p, s, dst->uid, m, f->len);
    p->relayed++;
    if (f->type == GCSP_ENGAGE) {
        /* the attacker gets his own 10 back: its handler (0x66de0) sees
         * its own name @4 and only then clears the pending attack (type 1,
         * the 10's sequence number); without the echo the mission check
         * (0x28760) ends the attack with "has moved Area!" */
        send_to(p, s, pl->uid, m, f->len);
    }
    LOG_I("message %u '%s' -> '%s'%s", f->type, pl->name, to,
          (f->type & GCSP_ANSWER) && d[2] > 252 ? " (refused)" : "");
}

/* ---- auction ---- */

static int count_exhibits(gcs_pk *p, const char *user)
{
    int i, n = 0;
    for (i = 0; i < p->na; i++) {
        n += !p->a[i].seller_done && ieq(p->a[i].seller, user);
    }
    return n;
}

static int count_bids(gcs_pk *p, const char *user)
{
    int i, n = 0;
    for (i = 0; i < p->nb; i++) {
        n += ieq(p->b[i].user, user);
    }
    return n;
}

/* one 24 byte list entry */
static void entry(const gcs_pk *p, gc_u8 *e, const auction *a, gc_u8 status, gc_u32 own)
{
    put32(e, a->id);
    e[4] = status;
    memcpy(e + 5, a->item, ITEMSZ);
    put32(e + 12, secs_left(p, a));
    put32(e + 16, a->price);
    put32(e + 20, own);
}

static void on_exhibit(gcs_pk *p, gcs_snap *s, player *pl, const gc_u8 *d)
{
    gc_u8 r[8];
    gc_u32 start = be32(d + 28);
    memset(r, 0, sizeof(r));
    r[0] = GCSP_ANSWER | GCSP_EXHIBIT;
    r[1] = d[1];
    if (count_exhibits(p, pl->name) >= GCSP_MAX_EXHIBITS) {
        r[3] = 1; /* too many exhibits */
    } else if (start == 0 || start > GCSP_MAX_LOOT ||
               grow((void **)&p->a, &p->capa, p->na + 1, sizeof(auction)) != 0) {
        r[3] = 2; /* refused */
    } else {
        auction *a = &p->a[p->na++];
        memset(a, 0, sizeof(*a));
        a->id = p->next_id++;
        copy_name(a->seller, pl->name);
        memcpy(a->item, d + 20, ITEMSZ);
        a->start = a->price = start;
        a->end = now_s(p) + GCSP_AUCTION_SECS;
        put32(r + 4, a->id);
        save(p);
        LOG_I("'%s' exhibits item %u/%u as auction %lu, starting at %lu", pl->name, a->item[0], a->item[2],
              (unsigned long)a->id, (unsigned long)start);
    }
    if (r[3]) {
        LOG_I("'%s': exhibit refused (%u)", pl->name, r[3]);
    }
    send_to(p, s, pl->uid, r, sizeof(r));
}

static int cmp_end(const void *x, const void *y)
{
    const auction *a = *(const auction *const *)x, *b = *(const auction *const *)y;
    if (a->end != b->end) {
        return a->end < b->end ? -1 : 1;
    }
    return a->id < b->id ? -1 : (a->id > b->id);
}

static void on_browse(gcs_pk *p, gcs_snap *s, player *pl, const gc_u8 *d)
{
    gc_u8 r[8 + 24 * GCSP_LIST_MAX];
    auction **hit;
    gc_u32 page = be32(d + 12), total = 0, i;
    gc_u8 cat = d[2];
    memset(r, 0, sizeof(r));
    r[0] = GCSP_ANSWER | GCSP_BROWSE;
    r[1] = d[1];
    hit = (auction **)malloc(sizeof(auction *) * (size_t)(p->na ? p->na : 1));
    if (hit) {
        int k;
        for (k = 0; k < p->na; k++) {
            auction *a = &p->a[k];
            /* the category is the first item byte; the other filters
             * (item type, rarity) need the game's item tables */
            if (!ended(p, a) && (cat == 0xff || a->item[0] == cat)) {
                hit[total++] = a;
            }
        }
        qsort(hit, total, sizeof(auction *), cmp_end);
        if (page > total / GCSP_LIST_MAX) {
            page = 0;
        }
        for (i = 0; i < GCSP_LIST_MAX && page * GCSP_LIST_MAX + i < total; i++) {
            auction *a = hit[page * GCSP_LIST_MAX + i];
            bid *b = bid_of(p, a->id, pl->name);
            int leading = a->bids && ieq(a->high, pl->name);
            entry(p, r + 8 + 24 * i, a, (gc_u8)(leading ? 2 : 1), b ? b->held : 0);
        }
        free(hit);
    }
    r[2] = (gc_u8)(page >> 8);
    r[3] = (gc_u8)page;
    put32(r + 4, total);
    LOG_I("'%s' browses category %u page %lu: %lu items", pl->name, cat, (unsigned long)page, (unsigned long)total);
    send_to(p, s, pl->uid, r, sizeof(r));
}

static void on_bid(gcs_pk *p, gcs_snap *s, player *pl, const gc_u8 *d)
{
    gc_u8 r[8];
    gc_u32 id = be32(d + 4), amount = be32(d + 8);
    auction *a = auction_by_id(p, id);
    bid *b = a ? bid_of(p, id, pl->name) : NULL;
    memset(r, 0, sizeof(r));
    r[0] = GCSP_ANSWER | GCSP_BID;
    r[1] = d[1];
    if (!a || ended(p, a)) {
        r[2] = 2; /* too late */
    } else if (ieq(a->seller, pl->name)) {
        r[2] = 4; /* own exhibit */
    } else if (amount == 0 || amount > GCSP_MAX_LOOT) {
        r[2] = 5; /* refused */
    } else if (a->bids ? amount <= a->price : amount < a->start) {
        r[2] = 1; /* someone bid higher */
    } else if (!b && count_bids(p, pl->name) >= GCSP_MAX_BIDS) {
        r[2] = 3; /* too many bids */
    } else if (!b && grow((void **)&p->b, &p->capb, p->nb + 1, sizeof(bid)) != 0) {
        r[2] = 5;
    } else {
        if (!b) {
            b = &p->b[p->nb++];
            memset(b, 0, sizeof(*b));
            b->auction = id;
            copy_name(b->user, pl->name);
        }
        /* the game pays amount minus what it gets back here */
        put32(r + 4, b->held);
        b->held = amount;
        a->price = amount;
        a->bids++;
        copy_name(a->high, pl->name);
        save(p);
        LOG_I("'%s' bids %lu on auction %lu", pl->name, (unsigned long)amount, (unsigned long)id);
    }
    if (r[2]) {
        LOG_I("'%s': bid %lu on auction %lu refused (%u)", pl->name, (unsigned long)amount, (unsigned long)id, r[2]);
    }
    send_to(p, s, pl->uid, r, sizeof(r));
}

/* own exhibits (mode 0, 2) or bids (mode 1, 3); modes 2 and 3 also
 * hand over the results of ended auctions (the game books them) */
static void on_mylist(gcs_pk *p, gcs_snap *s, player *pl, const gc_u8 *d)
{
    gc_u8 r[4 + 24 * GCSP_LIST_MAX];
    int mode = d[3] & 3, n = 0, i, changed = 0;
    memset(r, 0, sizeof(r));
    r[0] = GCSP_ANSWER | GCSP_MYLIST;
    r[1] = d[1];
    r[3] = (gc_u8)mode;
    if (mode == 0 || mode == 2) {
        for (i = 0; i < p->na && n < GCSP_LIST_MAX; i++) {
            auction *a = &p->a[i];
            gc_u8 st;
            if (a->seller_done || !ieq(a->seller, pl->name)) {
                continue;
            }
            if (!ended(p, a)) {
                st = (gc_u8)(a->bids ? 2 : 1); /* running: bids or none yet */
            } else {
                st = (gc_u8)(a->bids ? 3 : 4); /* sold (loot) or no sale (item back) */
                if (mode == 2) {
                    a->seller_done = 1;
                    changed = 1;
                    LOG_I("'%s' collects auction %lu: %s", pl->name, (unsigned long)a->id,
                          a->bids ? "sold" : "no sale");
                }
            }
            entry(p, r + 4 + 24 * n++, a, st, 0);
        }
    } else {
        i = 0;
        while (i < p->nb && n < GCSP_LIST_MAX) {
            bid *b = &p->b[i];
            auction *a;
            gc_u8 st;
            int leading;
            if (!ieq(b->user, pl->name)) {
                i++;
                continue;
            }
            a = auction_by_id(p, b->auction);
            if (!a) { /* lost auction: give the loot back */
                auction z;
                memset(&z, 0, sizeof(z));
                z.id = b->auction;
                z.end = now_s(p);
                entry(p, r + 4 + 24 * n++, &z, 4, b->held);
                if (mode == 3) {
                    bid_remove(p, b);
                    changed = 1;
                    continue;
                }
                i++;
                continue;
            }
            leading = a->bids && ieq(a->high, pl->name);
            if (!ended(p, a)) {
                st = (gc_u8)(leading ? 2 : 1); /* high bid or outbid */
            } else {
                st = (gc_u8)(leading ? 3 : 4); /* won or lost (refund) */
            }
            entry(p, r + 4 + 24 * n++, a, st, b->held);
            if (mode == 3 && st >= 3) {
                LOG_I("'%s' collects auction %lu: %s", pl->name, (unsigned long)a->id, st == 3 ? "won" : "refund");
                bid_remove(p, b);
                changed = 1;
                continue;
            }
            i++;
        }
    }
    if (changed) {
        auction_gc(p);
        save(p);
    }
    LOG_I("'%s' lists own %s (mode %d): %d", pl->name, (mode & 1) ? "bids" : "exhibits", mode, n);
    send_to(p, s, pl->uid, r, sizeof(r));
}

/* ---- summary ---- */

static void on_summary(gcs_pk *p, gcs_snap *s, player *pl, const gc_u8 *d)
{
    kingdom *k = kingdom_get(p, pl->name);
    if (k) {
        memcpy(k->summary, d, SUMSZ);
        k->has_summary = 1;
        k->vp = be32(d + 4);
        k->loot = be32(d + 8);
        k->seen = now_s(p);
        save(p);
        LOG_I("'%s': %lu victory points, %lu loot", pl->name, (unsigned long)k->vp, (unsigned long)k->loot);
    }
    answer4(p, s, pl->uid, GCSP_ANSWER | GCSP_SUMMARY, d[1], 0);
}

/* ---- snap game hooks ---- */

static size_t min_len(gc_u8 type)
{
    switch (type) {
    case GCSP_REGISTER:
        return 24;
    case GCSP_PING:
    case GCSP_LOGOUT:
    case GCSP_MOVE:
    case GCSP_SEARCH:
    case GCSP_MYLIST:
    case GCSP_UNUSED17:
        return 20;
    case GCSP_EXHIBIT:
    case GCSP_BROWSE:
    case GCSP_UNUSED22:
        return 32;
    case GCSP_BID:
        return 28;
    case GCSP_SUMMARY:
        return SUMSZ;
    default:
        return 4;
    }
}

static void g_joined(void *ctx, gcs_snap *s, gc_u32 uid, const char *user)
{
    gcs_pk *p = (gcs_pk *)ctx;
    player *pl = player_by_uid(p, uid);
    int i;
    (void)s;
    if (!pl) {
        player *old = player_by_name(p, NULL, user);
        if (old) {
            old->uid = 0; /* logged in again */
        }
        for (i = 0; i < GCSS_MAX_SESSIONS && !pl; i++) {
            if (!p->pl[i].uid) {
                pl = &p->pl[i];
            }
        }
        if (!pl) {
            LOG_W("no room for '%s'", user);
            return;
        }
    }
    pl->uid = uid;
    copy_name(pl->name, user);
    LOG_D("user %08x '%s' in a game room", uid, pl->name);
}

static void g_left(void *ctx, gcs_snap *s, gc_u32 uid)
{
    gcs_pk *p = (gcs_pk *)ctx;
    player *pl = player_by_uid(p, uid);
    (void)s;
    if (pl) {
        kingdom *k = kingdom_by_name(p, pl->name);
        if (k) {
            k->seen = now_s(p);
        }
        LOG_D("user %08x '%s' left the game room", uid, pl->name);
        pl->uid = 0;
    }
}

static void g_data(void *ctx, gcs_snap *s, gc_u32 uid, const gc_u8 *d, size_t n)
{
    gcs_pk *p = (gcs_pk *)ctx;
    player *pl = player_by_uid(p, uid);
    size_t k;
    if (n < 1) {
        return;
    }
    if (!pl && s && gcs_snap_user_of(s, uid)) {
        /* game data before any room entry: learn the name from SNAP */
        g_joined(p, s, uid, gcs_snap_user_of(s, uid));
        pl = player_by_uid(p, uid);
    }
    if (!pl) {
        LOG_W("user %08x: message %u from an unknown user ignored", uid, d[0]);
        return;
    }
    p->messages++;
    {
        char hx[3 * 40 + 4];
        size_t i, o = 0;
        hx[0] = 0;
        for (i = 0; i < n && i < 40 && o + 4 < sizeof(hx); i++) {
            o += (size_t)snprintf(hx + o, sizeof(hx) - o, "%02x ", d[i]);
        }
        LOG_D("'%s': message %u seq %u (%u bytes): %s%s", pl->name, d[0], n > 1 ? d[1] : 0, (unsigned)n, hx,
              n > 40 ? "..." : "");
    }
    for (k = 0; k < sizeof(RELAY) / sizeof(RELAY[0]); k++) {
        if (RELAY[k].type == d[0]) {
            if (n < RELAY[k].len) {
                LOG_W("'%s': message %u too short (%u bytes)", pl->name, d[0], (unsigned)n);
                return;
            }
            on_relay(p, s, pl, &RELAY[k], d);
            return;
        }
    }
    if (n < min_len(d[0])) {
        LOG_W("'%s': message %u too short (%u bytes)", pl->name, d[0], (unsigned)n);
        return;
    }
    switch (d[0]) {
    case GCSP_REGISTER:
        on_register(p, s, pl, d);
        break;
    case GCSP_PING:
        answer4(p, s, uid, GCSP_ANSWER | GCSP_PING, d[1], 0);
        break;
    case GCSP_LOGOUT:
        LOG_I("'%s' goes offline", pl->name);
        {
            kingdom *kd = kingdom_by_name(p, pl->name);
            if (kd) {
                kd->seen = now_s(p);
            }
        }
        break;
    case GCSP_MOVE:
        on_move(p, pl, d);
        break;
    case GCSP_SEARCH:
        on_search(p, s, pl, d);
        break;
    case GCSP_EXHIBIT:
        on_exhibit(p, s, pl, d);
        break;
    case GCSP_BROWSE:
        on_browse(p, s, pl, d);
        break;
    case GCSP_BID:
        on_bid(p, s, pl, d);
        break;
    case GCSP_MYLIST:
        on_mylist(p, s, pl, d);
        break;
    case GCSP_SUMMARY:
        on_summary(p, s, pl, d);
        break;
    case GCSP_UNUSED17:
    case GCSP_UNUSED22:
        answer4(p, s, uid, (gc_u8)(GCSP_ANSWER | d[0]), d[1], 0);
        break;
    default:
        LOG_W("'%s': unknown message %u (%u bytes)", pl->name, d[0], (unsigned)n);
        break;
    }
}

/* ---- public ---- */

gcs_pk *gcs_pk_create(const char *file, void (*log)(void *ctx, int level, const char *line), void *ctx)
{
    gcs_pk *p = (gcs_pk *)calloc(1, sizeof(*p));
    if (!p) {
        return NULL;
    }
    if (file) {
        snprintf(p->file, sizeof(p->file), "%s", file);
    }
    p->log = log;
    p->ctx = ctx;
    p->next_id = 1;
    p->game.game_class = GCS_PK_GAME_CLASS;
    p->game.name = "Pocket Kingdom";
    p->game.lobby = GCS_PK_LOBBY;
    p->game.ctx = p;
    p->game.joined = g_joined;
    p->game.left = g_left;
    p->game.data = g_data;
    p->game.poll = NULL;
    p->game.rooms = GCS_PK_ROOMS;
    load(p);
    return p;
}

void gcs_pk_destroy(gcs_pk *p)
{
    if (!p) {
        return;
    }
    free(p->k);
    free(p->a);
    free(p->b);
    free(p->ml);
    free(p);
}

const gcs_snap_game *gcs_pk_game(gcs_pk *p)
{
    return p ? &p->game : NULL;
}

void gcs_pk_set_output(gcs_pk *p, void (*fn)(void *ctx, gc_u32 uid, const gc_u8 *d, size_t n), void *ctx)
{
    p->out = fn;
    p->out_ctx = ctx;
}

void gcs_pk_set_time(gcs_pk *p, long t)
{
    p->fixed_time = t;
}

void gcs_pk_get_stats(const gcs_pk *p, gcs_pk_stats *st)
{
    int i;
    memset(st, 0, sizeof(*st));
    if (!p) {
        return;
    }
    for (i = 0; i < GCSS_MAX_SESSIONS; i++) {
        st->online += p->pl[i].uid != 0;
    }
    st->kingdoms = p->nk;
    st->auctions = p->na;
    st->mail = p->nml;
    st->messages = p->messages;
    st->relayed = p->relayed;
}
