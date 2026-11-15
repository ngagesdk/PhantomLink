/** @file gcs_habbo.c
 *
 *  Habbo Console back end, see gcs_habbo.h.  The request and answer
 *  formats come from the console code in Habbo Islands' SULAKEUI.DLL and
 *  SULAKENETWORK.DLL (docs/HABBO.md).
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#include "gcs_habbo.h"
#include "gcs_accounts.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* messenger requests: m=ID, the FUSE client message ID */
#define M_LOGOUT   0
#define M_INIT     12 /* MESSENGERINIT          */
#define M_UPDATE   15 /* MESSENGER_UPDATE       */
#define M_MARKREAD 32 /* MESSENGER_MARKREAD     */
#define M_SENDMSG  33 /* MESSENGER_SENDMSG      */
#define M_ACCEPT   37 /* MESSENGER_ACCEPTBUDDY  */
#define M_DECLINE  38 /* MESSENGER_DECLINEBUDDY */
#define M_REQUEST  39 /* MESSENGER_REQUESTBUDDY */
#define M_REMOVE   40 /* MESSENGER_REMOVEBUDDY  */
#define M_FIND     41 /* FINDUSER               */

#define MAX_P1 64 /* p1 values of one request (mark read) */

/* "lastVisit" of a friend that is online: the game keeps this default and
 * shows "Currently online" instead */
#define ONLINE_PLACEHOLDER "xxx"

typedef struct
{
    unsigned id; /* 1.., 0 = free */
    char name[GCSA_MAX_NAME + 1];
    long last; /* last console request (s), 0 never */
} huser;

typedef struct
{
    unsigned a, b; /* user ids, a < b */
} friendship;

typedef struct
{
    unsigned from, to;
    int delivered; /* sent to the console in this session */
} request;

typedef struct
{
    unsigned id; /* 1.., 0 = free */
    unsigned from, to;
    long time;
    int delivered;
    char text[GCSH_MAX_TEXT + 1];
} message;

typedef struct
{
    char id[40]; /* "" free */
    unsigned uid;
    long last;
    void *key;
} hsession;

struct gcs_habbo
{
    gcs_accounts *acc;
    char file[256];
    gcs_habbo_log_fn log;
    void *ctx;
    huser users[GCSH_MAX_USERS];
    int nusers;
    friendship friends[GCSH_MAX_FRIENDS];
    int nfriends;
    request requests[GCSH_MAX_REQUESTS];
    int nrequests;
    message msgs[GCSH_MAX_MESSAGES];
    int nmsgs;
    unsigned next_user, next_msg;
    hsession sessions[GCSH_SESSIONS];
    unsigned long token_ctr;
    unsigned long logins, sent;
};

/* ---- helpers ---- */

static void hlog(gcs_habbo *h, int level, const char *fmt, ...)
{
    char line[400];
    va_list ap;
    size_t i;
    if (!h->log) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    for (i = 0; line[i]; i++) {
        if ((unsigned char)line[i] < 0x20 || line[i] == 0x7f) {
            line[i] = ' ';
        }
    }
    h->log(h->ctx, level, line);
}

static void copy_str(char *dst, const char *src, size_t cap)
{
    size_t k = strlen(src);
    if (k >= cap) {
        k = cap - 1;
    }
    memcpy(dst, src, k);
    dst[k] = 0;
}

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        if (lower((unsigned char)*a) != lower((unsigned char)*b)) {
            return 0;
        }
    }
    return *a == *b;
}

static int ends_with(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && !strcmp(s + a - b, suffix);
}

static int hexval(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                        : c >= 'A' && c <= 'F'   ? c - 'A' + 10
                                                                 : -1;
}

/* application/x-www-form-urlencoded value [s, e) into out (cap bytes with
 * the terminator).  Control characters other than line breaks are dropped. */
static void url_decode(const char *s, const char *e, char *out, size_t cap)
{
    size_t k = 0;
    while (s < e && k + 1 < cap) {
        int c = (unsigned char)*s++;
        if (c == '+') {
            c = ' ';
        } else if (c == '%' && e - s >= 2 && hexval(s[0]) >= 0 && hexval(s[1]) >= 0) {
            c = hexval(s[0]) * 16 + hexval(s[1]);
            s += 2;
        }
        if ((c < 0x20 && c != '\n' && c != '\r') || c == 0x7f) {
            continue;
        }
        out[k++] = (char)c;
    }
    out[k] = 0;
}

/* Cuts s to at most max bytes without splitting a UTF-8 sequence. */
static void utf8_cut(char *s, size_t max)
{
    size_t k = strlen(s);
    if (k <= max) {
        return;
    }
    k = max;
    while (k > 0 && ((unsigned char)s[k] & 0xc0) == 0x80) {
        k--;
    }
    s[k] = 0;
}

static unsigned parse_id(const char *s)
{
    unsigned long v = 0;
    if (!*s) {
        return 0;
    }
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || v > 0xffffffUL) {
            return 0;
        }
        v = v * 10 + (unsigned long)(*s - '0');
    }
    return (unsigned)v;
}

/* "dd-mm-yyyy hh:mm" in the server's local time, "-" for never */
static void format_time(long t, char *out, size_t cap)
{
    time_t tt = (time_t)t;
    struct tm *tm;
    if (t <= 0 || (tm = localtime(&tt)) == NULL || !strftime(out, cap, "%d-%m-%Y %H:%M", tm)) {
        copy_str(out, "-", cap);
    }
}

/* ---- binary answers (Java DataOutputStream, big-endian) ---- */

typedef struct
{
    unsigned char *d;
    size_t len, cap;
    int oom;
} obuf;

static void ob_add(obuf *b, const void *s, size_t k)
{
    if (b->oom) {
        return;
    }
    if (b->len + k + 1 > b->cap) {
        size_t nc = (b->len + k + 1) * 2 + 64;
        unsigned char *nd = (unsigned char *)realloc(b->d, nc);
        if (!nd) {
            b->oom = 1;
            return;
        }
        b->d = nd;
        b->cap = nc;
    }
    memcpy(b->d + b->len, s, k);
    b->len += k;
}

static void ob_u8(obuf *b, unsigned v)
{
    unsigned char c = (unsigned char)v;
    ob_add(b, &c, 1);
}

static void ob_u16(obuf *b, unsigned v)
{
    unsigned char c[2];
    c[0] = (unsigned char)(v >> 8);
    c[1] = (unsigned char)v;
    ob_add(b, c, 2);
}

static void ob_id(obuf *b, unsigned id) /* a Java long */
{
    unsigned char c[8];
    memset(c, 0, 4);
    c[4] = (unsigned char)(id >> 24);
    c[5] = (unsigned char)(id >> 16);
    c[6] = (unsigned char)(id >> 8);
    c[7] = (unsigned char)id;
    ob_add(b, c, 8);
}

static void ob_str(obuf *b, const char *s) /* writeUTF */
{
    size_t k = strlen(s);
    if (k > 0xffff) {
        k = 0xffff;
    }
    ob_u16(b, (unsigned)k);
    ob_add(b, s, k);
}

/* ---- users ---- */

static huser *user_by_id(gcs_habbo *h, unsigned id)
{
    int i;
    for (i = 0; id && i < h->nusers; i++) {
        if (h->users[i].id == id) {
            return &h->users[i];
        }
    }
    return NULL;
}

static huser *user_find(gcs_habbo *h, const char *name)
{
    int i;
    for (i = 0; i < h->nusers; i++) {
        if (ieq(h->users[i].name, name)) {
            return &h->users[i];
        }
    }
    return NULL;
}

static huser *user_add(gcs_habbo *h, unsigned id, const char *name, long last)
{
    huser *u;
    if (h->nusers >= GCSH_MAX_USERS || !id || !name[0]) {
        return NULL;
    }
    u = &h->users[h->nusers++];
    u->id = id;
    copy_str(u->name, name, sizeof(u->name));
    u->last = last;
    if (id >= h->next_user) {
        h->next_user = id + 1;
    }
    return u;
}

/* The console user of an existing account (made on first use). */
static huser *user_get(gcs_habbo *h, const char *name)
{
    char canon[GCSA_MAX_NAME + 1];
    huser *u;
    if (!gcs_accounts_exists(h->acc, name, canon)) {
        return NULL;
    }
    u = user_find(h, canon);
    if (u) {
        copy_str(u->name, canon, sizeof(u->name));
        return u;
    }
    return user_add(h, h->next_user, canon, 0);
}

/* ---- file ---- */

static void esc_out(FILE *f, const char *s)
{
    for (; *s; s++) {
        if (*s == '\\') {
            fputs("\\\\", f);
        } else if (*s == '\t') {
            fputs("\\t", f);
        } else if (*s == '\n') {
            fputs("\\n", f);
        } else if (*s == '\r') {
            fputs("\\r", f);
        } else {
            fputc(*s, f);
        }
    }
}

static void esc_in(char *s)
{
    char *w = s;
    while (*s) {
        if (*s == '\\' && s[1]) {
            s++;
            *w++ = *s == 't' ? '\t' : *s == 'n' ? '\n'
                                  : *s == 'r'   ? '\r'
                                                : *s;
            s++;
        } else {
            *w++ = *s++;
        }
    }
    *w = 0;
}

static void save(gcs_habbo *h)
{
    char tmp[300];
    FILE *f;
    int i;
    if (!h->file[0]) {
        return;
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", h->file);
    f = fopen(tmp, "w");
    if (!f) {
        hlog(h, 1, "habbo: cannot write %s", tmp);
        return;
    }
    fputs("# PhantomLink Habbo Console: U id name lastVisit / F id id / R from to / M id from to time text\n", f);
    for (i = 0; i < h->nusers; i++) {
        fprintf(f, "U\t%u\t", h->users[i].id);
        esc_out(f, h->users[i].name);
        fprintf(f, "\t%ld\n", h->users[i].last);
    }
    for (i = 0; i < h->nfriends; i++) {
        fprintf(f, "F\t%u\t%u\n", h->friends[i].a, h->friends[i].b);
    }
    for (i = 0; i < h->nrequests; i++) {
        fprintf(f, "R\t%u\t%u\n", h->requests[i].from, h->requests[i].to);
    }
    for (i = 0; i < h->nmsgs; i++) {
        const message *m = &h->msgs[i];
        fprintf(f, "M\t%u\t%u\t%u\t%ld\t", m->id, m->from, m->to, m->time);
        esc_out(f, m->text);
        fputc('\n', f);
    }
    if (fclose(f) != 0) {
        hlog(h, 1, "habbo: cannot write %s", tmp);
        return;
    }
    remove(h->file); /* rename does not replace on Windows */
    if (rename(tmp, h->file) != 0) {
        hlog(h, 1, "habbo: cannot replace %s", h->file);
    }
}

/* Splits line at tabs into at most max fields; returns the count. */
static int split_tabs(char *line, char **f, int max)
{
    int n = 0;
    f[n++] = line;
    while (n < max) {
        char *t = strchr(f[n - 1], '\t');
        if (!t) {
            break;
        }
        *t = 0;
        f[n++] = t + 1;
    }
    return n;
}

static int friends_with(gcs_habbo *h, unsigned x, unsigned y);
static request *request_find(gcs_habbo *h, unsigned from, unsigned to);

static void load(gcs_habbo *h)
{
    char line[1024];
    FILE *f = fopen(h->file, "r");
    if (!f) {
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        char *fl[6], *e = line + strlen(line);
        int n;
        while (e > line && (e[-1] == '\n' || e[-1] == '\r')) {
            *--e = 0;
        }
        if (line[0] == '#' || !line[0]) {
            continue;
        }
        n = split_tabs(line, fl, 6);
        if (!strcmp(fl[0], "U") && n >= 4) {
            unsigned id = parse_id(fl[1]);
            esc_in(fl[2]);
            if (id && !user_by_id(h, id) && !user_find(h, fl[2])) {
                user_add(h, id, fl[2], strtol(fl[3], NULL, 10));
            }
        } else if (!strcmp(fl[0], "F") && n >= 3 && h->nfriends < GCSH_MAX_FRIENDS) {
            unsigned a = parse_id(fl[1]), b = parse_id(fl[2]);
            if (user_by_id(h, a) && user_by_id(h, b) && a != b && !friends_with(h, a, b)) {
                h->friends[h->nfriends].a = a < b ? a : b;
                h->friends[h->nfriends].b = a < b ? b : a;
                h->nfriends++;
            }
        } else if (!strcmp(fl[0], "R") && n >= 3 && h->nrequests < GCSH_MAX_REQUESTS) {
            unsigned a = parse_id(fl[1]), b = parse_id(fl[2]);
            if (user_by_id(h, a) && user_by_id(h, b) && a != b && !request_find(h, a, b)) {
                h->requests[h->nrequests].from = a;
                h->requests[h->nrequests].to = b;
                h->requests[h->nrequests].delivered = 0;
                h->nrequests++;
            }
        } else if (!strcmp(fl[0], "M") && n >= 6 && h->nmsgs < GCSH_MAX_MESSAGES) {
            message *m = &h->msgs[h->nmsgs];
            m->id = parse_id(fl[1]);
            m->from = parse_id(fl[2]);
            m->to = parse_id(fl[3]);
            m->time = strtol(fl[4], NULL, 10);
            m->delivered = 0;
            esc_in(fl[5]);
            utf8_cut(fl[5], GCSH_MAX_TEXT);
            copy_str(m->text, fl[5], sizeof(m->text));
            if (m->id && user_by_id(h, m->from) && user_by_id(h, m->to)) {
                if (m->id >= h->next_msg) {
                    h->next_msg = m->id + 1;
                }
                h->nmsgs++;
            }
        }
    }
    fclose(f);
    hlog(h, 2, "habbo: %d users, %d friendships, %d requests, %d messages loaded from %s", h->nusers, h->nfriends,
         h->nrequests, h->nmsgs, h->file);
}

/* ---- friends, requests, messages ---- */

static int friend_index(gcs_habbo *h, unsigned x, unsigned y)
{
    unsigned a = x < y ? x : y, b = x < y ? y : x;
    int i;
    for (i = 0; i < h->nfriends; i++) {
        if (h->friends[i].a == a && h->friends[i].b == b) {
            return i;
        }
    }
    return -1;
}

static int friends_with(gcs_habbo *h, unsigned x, unsigned y)
{
    return friend_index(h, x, y) >= 0;
}

static int friend_count(gcs_habbo *h, unsigned id)
{
    int i, n = 0;
    for (i = 0; i < h->nfriends; i++) {
        n += h->friends[i].a == id || h->friends[i].b == id;
    }
    return n;
}

static request *request_find(gcs_habbo *h, unsigned from, unsigned to)
{
    int i;
    for (i = 0; i < h->nrequests; i++) {
        if (h->requests[i].from == from && h->requests[i].to == to) {
            return &h->requests[i];
        }
    }
    return NULL;
}

static void request_remove(gcs_habbo *h, unsigned from, unsigned to)
{
    int i;
    for (i = 0; i < h->nrequests;) {
        if (h->requests[i].from == from && h->requests[i].to == to) {
            h->requests[i] = h->requests[--h->nrequests];
        } else {
            i++;
        }
    }
}

/* 0 ok (or already friends), -1 full */
static int friend_add(gcs_habbo *h, unsigned x, unsigned y)
{
    request_remove(h, x, y);
    request_remove(h, y, x);
    if (friends_with(h, x, y)) {
        return 0;
    }
    if (h->nfriends >= GCSH_MAX_FRIENDS || friend_count(h, x) >= GCSH_MAX_LIST ||
        friend_count(h, y) >= GCSH_MAX_LIST) {
        return -1;
    }
    h->friends[h->nfriends].a = x < y ? x : y;
    h->friends[h->nfriends].b = x < y ? y : x;
    h->nfriends++;
    return 0;
}

static int inbox_count(gcs_habbo *h, unsigned id)
{
    int i, n = 0;
    for (i = 0; i < h->nmsgs; i++) {
        n += h->msgs[i].to == id;
    }
    return n;
}

/* ---- sessions ---- */

static hsession *session_find(gcs_habbo *h, const char *sid, void *key, long now)
{
    int i;
    hsession *best = NULL;
    for (i = 0; i < GCSH_SESSIONS; i++) {
        hsession *s = &h->sessions[i];
        if (!s->id[0]) {
            continue;
        }
        if (now - s->last > GCSH_SESSION_S) {
            memset(s, 0, sizeof(*s)); /* expired */
            continue;
        }
        if (sid[0]) {
            if (!strcmp(s->id, sid)) {
                return s;
            }
        } else if (key && s->key == key && (!best || s->last > best->last)) {
            best = s; /* no cookie: the latest login of this peer */
        }
    }
    return best;
}

static hsession *session_new(gcs_habbo *h, unsigned uid, void *key, long now)
{
    char seed[160], hex[41];
    int i, slot = 0;
    for (i = 0; i < GCSH_SESSIONS; i++) {
        if (!h->sessions[i].id[0]) {
            slot = i;
            break;
        }
        if (h->sessions[i].last < h->sessions[slot].last) {
            slot = i; /* full: replace the oldest */
        }
    }
    snprintf(seed, sizeof(seed), "habbo|%u|%ld|%lu|%lu|%p", uid, now, (unsigned long)clock(), ++h->token_ctr,
             (void *)&h->sessions[slot]);
    gcs_sha1_hex(seed, strlen(seed), hex);
    snprintf(h->sessions[slot].id, sizeof(h->sessions[slot].id), "PLH%.32s", hex);
    h->sessions[slot].uid = uid;
    h->sessions[slot].last = now;
    h->sessions[slot].key = key;
    return &h->sessions[slot];
}

static int online(gcs_habbo *h, unsigned uid, long now)
{
    int i;
    for (i = 0; i < GCSH_SESSIONS; i++) {
        const hsession *s = &h->sessions[i];
        if (s->id[0] && s->uid == uid && now - s->last <= GCSH_ONLINE_S) {
            return 1;
        }
    }
    return 0;
}

/* ---- records ---- */

/* friend / user: long id, str name, str text, u8 online, [str lastVisit] */
static void put_user(gcs_habbo *h, obuf *b, const huser *u, long now)
{
    char when[32];
    char text[GCSA_MAX_VALUE + 1];
    int on = online(h, u->id, now);
    copy_str(text, gcs_accounts_get(h->acc, u->name, "quote"), sizeof(text));
    ob_id(b, u->id);
    ob_str(b, u->name);
    ob_str(b, text);
    ob_u8(b, (unsigned)on);
    if (!on) {
        format_time(u->last, when, sizeof(when));
        ob_str(b, when);
    }
}

/* message: long id, long sender id, str sender name, str text, str time */
static void put_message(obuf *b, const message *m, const huser *from)
{
    char when[32];
    format_time(m->time, when, sizeof(when));
    ob_id(b, m->id);
    ob_id(b, m->from);
    ob_str(b, from ? from->name : "");
    ob_str(b, m->text);
    ob_str(b, when);
}

/* MESSENGERINIT (full) and MESSENGER_UPDATE: friends, messages, requests,
 * then the longest message the game may send */
static void put_lists(gcs_habbo *h, obuf *b, const huser *me, int full, long now)
{
    int i, n;
    size_t at;
    if (full) {
        ob_str(b, ""); /* read and ignored by the game */
        for (i = 0; i < h->nmsgs; i++) {
            if (h->msgs[i].to == me->id) {
                h->msgs[i].delivered = 0;
            }
        }
        for (i = 0; i < h->nrequests; i++) {
            if (h->requests[i].to == me->id) {
                h->requests[i].delivered = 0;
            }
        }
    }
    /* friends: always all (the game replaces them by id) */
    at = b->len;
    ob_u16(b, 0);
    for (i = n = 0; i < h->nfriends; i++) {
        unsigned other = h->friends[i].a == me->id ? h->friends[i].b : h->friends[i].b == me->id ? h->friends[i].a
                                                                                                 : 0;
        const huser *u = user_by_id(h, other);
        if (u && n < GCSH_MAX_LIST) {
            put_user(h, b, u, now);
            n++;
        }
    }
    if (!b->oom) {
        b->d[at] = (unsigned char)(n >> 8);
        b->d[at + 1] = (unsigned char)n;
    }
    /* messages and requests: the game appends them, so each only once */
    at = b->len;
    ob_u16(b, 0);
    for (i = n = 0; i < h->nmsgs && n < GCSH_MAX_PER_REPLY; i++) {
        message *m = &h->msgs[i];
        if (m->to == me->id && !m->delivered) {
            put_message(b, m, user_by_id(h, m->from));
            m->delivered = 1;
            n++;
        }
    }
    if (!b->oom) {
        b->d[at] = (unsigned char)(n >> 8);
        b->d[at + 1] = (unsigned char)n;
    }
    at = b->len;
    ob_u16(b, 0);
    for (i = n = 0; i < h->nrequests; i++) {
        request *r = &h->requests[i];
        const huser *u = user_by_id(h, r->from);
        if (r->to == me->id && !r->delivered && u) {
            ob_id(b, u->id);
            ob_str(b, u->name);
            r->delivered = 1;
            n++;
        }
    }
    if (!b->oom) {
        b->d[at] = (unsigned char)(n >> 8);
        b->d[at + 1] = (unsigned char)n;
    }
    ob_u16(b, GCSH_MAX_TEXT);
}

/* ---- requests ---- */

typedef struct
{
    char m[8];
    char user[GCSA_MAX_NAME * 3 + 1];
    char pass[GCSA_MAX_VALUE + 1];
    char p1[MAX_P1][64];
    int np1;
    char p2[GCSH_MAX_TEXT * 3 + 1];
} form;

static void parse_form(const char *body, size_t blen, form *f)
{
    const char *p = body, *end = body + blen;
    memset(f, 0, sizeof(*f));
    while (p < end) {
        const char *amp = (const char *)memchr(p, '&', (size_t)(end - p));
        const char *e = amp ? amp : end;
        const char *eq = (const char *)memchr(p, '=', (size_t)(e - p));
        if (eq) {
            size_t kl = (size_t)(eq - p);
            const char *v = eq + 1;
            if (kl == 1 && *p == 'm') {
                url_decode(v, e, f->m, sizeof(f->m));
            } else if (kl == 4 && !memcmp(p, "user", 4)) {
                url_decode(v, e, f->user, sizeof(f->user));
            } else if (kl == 4 && !memcmp(p, "pass", 4)) {
                url_decode(v, e, f->pass, sizeof(f->pass));
            } else if (kl == 2 && !memcmp(p, "p1", 2) && f->np1 < MAX_P1) {
                url_decode(v, e, f->p1[f->np1], sizeof(f->p1[0]));
                f->np1++;
            } else if (kl == 2 && !memcmp(p, "p2", 2)) {
                url_decode(v, e, f->p2, sizeof(f->p2));
            }
        }
        p = e + 1;
    }
}

static void reply_text(gcs_habbo_reply *r, const char *s, obuf *b)
{
    ob_add(b, s, strlen(s));
    r->ctype = "text/plain";
}

static void login(gcs_habbo *h, void *key, const form *f, long now, gcs_habbo_reply *r, obuf *b)
{
    int res = gcs_accounts_login(h->acc, f->user, f->pass, NULL);
    huser *u = res >= 0 ? user_get(h, f->user) : NULL;
    if (!u) {
        hlog(h, 1, "habbo: login '%s' refused: %s", f->user,
             res >= 0 ? "no room for another console user" : gcs_accounts_result_str(res));
        reply_text(r, "1", b);
        return;
    }
    copy_str(r->cookie, session_new(h, u->id, key, now)->id, sizeof(r->cookie));
    u->last = now;
    h->logins++;
    save(h);
    hlog(h, 2, "habbo: '%s' logged in%s", u->name, res == GCSA_CREATED ? " (new account)" : "");
    reply_text(r, "0", b);
}

static void messenger(gcs_habbo *h, void *key, const char *sid, const form *f, long now, gcs_habbo_reply *r,
                      obuf *b)
{
    hsession *s = session_find(h, sid, key, now);
    huser *me = s ? user_by_id(h, s->uid) : NULL;
    unsigned p1 = f->np1 ? parse_id(f->p1[0]) : 0;
    huser *other = user_by_id(h, p1);
    int m = f->m[0] ? atoi(f->m) : -1;
    const char *ok = "0";
    int changed = 0, i;

    if (!me) {
        reply_text(r, m == M_LOGOUT ? "0" : "2", b); /* "2": log in again */
        return;
    }
    s->last = now;
    me->last = now;
    switch (m) {
    case M_LOGOUT:
        hlog(h, 2, "habbo: '%s' logged out", me->name);
        memset(s, 0, sizeof(*s));
        changed = 1;
        break;
    case M_INIT:
    case M_UPDATE:
        put_lists(h, b, me, m == M_INIT, now);
        r->ctype = "application/octet-stream";
        hlog(h, 3, "habbo: %s for '%s'", m == M_INIT ? "init" : "update", me->name);
        return;
    case M_MARKREAD:
        for (i = 0; i < f->np1; i++) {
            unsigned id = parse_id(f->p1[i]);
            int k;
            for (k = 0; k < h->nmsgs; k++) {
                if (h->msgs[k].id == id && h->msgs[k].to == me->id) {
                    h->msgs[k] = h->msgs[--h->nmsgs];
                    changed = 1;
                    break;
                }
            }
        }
        break;
    case M_SENDMSG:
        if (!other || !friends_with(h, me->id, other->id) || !f->p2[0] || h->nmsgs >= GCSH_MAX_MESSAGES ||
            inbox_count(h, other->id) >= GCSH_MAX_INBOX) {
            hlog(h, 1, "habbo: message from '%s' to %u not delivered", me->name, p1);
            ok = "1";
        } else {
            message *msg = &h->msgs[h->nmsgs++];
            memset(msg, 0, sizeof(*msg));
            msg->id = h->next_msg++;
            msg->from = me->id;
            msg->to = other->id;
            msg->time = now;
            {
                char text[sizeof(f->p2)];
                copy_str(text, f->p2, sizeof(text));
                utf8_cut(text, GCSH_MAX_TEXT); /* before the copy cuts it */
                copy_str(msg->text, text, sizeof(msg->text));
            }
            h->sent++;
            changed = 1;
            hlog(h, 2, "habbo: message '%s' -> '%s'", me->name, other->name);
        }
        break;
    case M_ACCEPT:
        if (!other || !request_find(h, other->id, me->id) || friend_add(h, me->id, other->id)) {
            ok = "1";
        } else {
            hlog(h, 2, "habbo: '%s' and '%s' are friends", me->name, other->name);
            save(h);
            put_user(h, b, other, now); /* the game adds it to the list */
            r->ctype = "application/octet-stream";
            return;
        }
        break;
    case M_DECLINE:
        if (other) {
            request_remove(h, other->id, me->id);
            changed = 1;
        }
        break;
    case M_REQUEST:
        if (!other || other == me) {
            ok = "1";
        } else if (friends_with(h, me->id, other->id) || request_find(h, me->id, other->id)) {
            /* nothing to do */
        } else if (request_find(h, other->id, me->id)) {
            /* both asked: friends at once (shown after the next update) */
            ok = friend_add(h, me->id, other->id) ? "1" : "0";
            changed = 1;
        } else if (h->nrequests >= GCSH_MAX_REQUESTS) {
            ok = "1";
        } else {
            h->requests[h->nrequests].from = me->id;
            h->requests[h->nrequests].to = other->id;
            h->requests[h->nrequests].delivered = 0;
            h->nrequests++;
            changed = 1;
            hlog(h, 2, "habbo: friend request '%s' -> '%s'", me->name, other->name);
        }
        break;
    case M_REMOVE:
        i = other ? friend_index(h, me->id, other->id) : -1;
        if (i >= 0) {
            h->friends[i] = h->friends[--h->nfriends];
            changed = 1;
        }
        break;
    case M_FIND:
        other = f->np1 ? user_get(h, f->p1[0]) : NULL;
        if (!other) {
            ok = "1"; /* "No Habbo found!" */
        } else {
            save(h); /* a new console user */
            put_user(h, b, other, now);
            r->ctype = "application/octet-stream";
            return;
        }
        break;
    default:
        hlog(h, 1, "habbo: unknown request m=%s from '%s'", f->m, me->name);
        ok = "1";
        break;
    }
    if (changed) {
        save(h);
    }
    reply_text(r, ok, b);
}

/* "1;PhantomLink;http://HOST/habbo/": the servlets on the same host and
 * port as the locator (the game's Host header) */
static void locator(const char *host, gcs_habbo_reply *r, obuf *b)
{
    char line[200], hb[100];
    size_t k = 0;
    for (; *host && k + 1 < sizeof(hb); host++) {
        char c = *host;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' ||
            c == ':') {
            hb[k++] = c;
        }
    }
    hb[k] = 0;
    snprintf(line, sizeof(line), "1;PhantomLink;http://%s/habbo/\r\n", k ? hb : "builder.sulake.com:8080");
    reply_text(r, line, b);
}

/* ---- API ---- */

gcs_habbo *gcs_habbo_create(gcs_accounts *acc, const char *file, gcs_habbo_log_fn log, void *ctx)
{
    gcs_habbo *h = (gcs_habbo *)calloc(1, sizeof(*h));
    if (!h) {
        return NULL;
    }
    h->acc = acc;
    h->log = log;
    h->ctx = ctx;
    h->next_user = 1;
    h->next_msg = 1;
    copy_str(h->file, file ? file : "", sizeof(h->file));
    if (h->file[0]) {
        load(h);
    }
    return h;
}

void gcs_habbo_destroy(gcs_habbo *h)
{
    free(h);
}

int gcs_habbo_http(gcs_habbo *h, void *key, const char *path, const char *host, const char *sid, const char *body,
                   size_t blen, long now, gcs_habbo_reply *r)
{
    char base[256];
    obuf b;
    form *f;

    memset(r, 0, sizeof(*r));
    memset(&b, 0, sizeof(b));
    r->status = 200;
    copy_str(base, path, sizeof(base));
    base[strcspn(base, "?;")] = 0;

    if (ends_with(base, "locator.html")) {
        locator(host ? host : "", r, &b);
    } else if (ends_with(base, "AuthenticationServlet") || ends_with(base, "MessengerServlet")) {
        f = (form *)malloc(sizeof(*f));
        if (!f) {
            return -1;
        }
        parse_form(body ? body : "", body ? blen : 0, f);
        if (ends_with(base, "AuthenticationServlet")) {
            login(h, key, f, now, r, &b);
        } else {
            messenger(h, key, sid ? sid : "", f, now, r, &b);
        }
        free(f);
    } else {
        r->status = 404;
        r->ctype = "text/plain";
        return 0;
    }
    if (b.oom) {
        free(b.d);
        memset(r, 0, sizeof(*r));
        return -1;
    }
    r->body = b.d;
    r->len = b.len;
    return 0;
}

void gcs_habbo_peer_gone(gcs_habbo *h, void *key)
{
    int i;
    for (i = 0; i < GCSH_SESSIONS; i++) {
        if (h->sessions[i].key == key) {
            h->sessions[i].key = NULL;
        }
    }
}

void gcs_habbo_get_stats(const gcs_habbo *h, gcs_habbo_stats *st)
{
    int i;
    memset(st, 0, sizeof(*st));
    st->users = h->nusers;
    st->friends = h->nfriends;
    st->requests = h->nrequests;
    st->messages = h->nmsgs;
    for (i = 0; i < GCSH_SESSIONS; i++) {
        st->sessions += h->sessions[i].id[0] != 0;
    }
    st->logins = h->logins;
    st->sent = h->sent;
}
