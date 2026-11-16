/** @file gcs_roster.c
 *
 *  SegaChat friends, friend requests and offline messages (gcs_roster.h).
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#include "gcs_roster.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    char a[GCSR_MAX_NAME + 1];
    char b[GCSR_MAX_NAME + 1];
} pair;

typedef struct
{
    char from[GCSR_MAX_NAME + 1];
    char to[GCSR_MAX_NAME + 1];
    char status[GCSR_MAX_STATUS + 1];
} request;

typedef struct
{
    char to[GCSR_MAX_NAME + 1];
    char from[GCSR_MAX_NAME + 1];
    long time;
    char *stanza;
} message;

struct gcs_roster
{
    char file[260];
    gcs_roster_log_fn log;
    void *ctx;
    pair *pairs;
    int npairs;
    request *reqs;
    int nreqs;
    message *msgs;
    int nmsgs;
};

static void rlog(gcs_roster *r, int level, const char *fmt, ...)
{
    char line[300];
    va_list ap;
    if (!r->log) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    r->log(r->ctx, level, line);
}

static void copy_str(char *dst, const char *src, size_t cap)
{
    size_t k = 0;
    while (src && src[k] && k + 1 < cap) {
        dst[k] = src[k];
        k++;
    }
    dst[k] = 0;
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

static int name_ok(const char *s)
{
    size_t k;
    if (!s || !s[0]) {
        return 0;
    }
    for (k = 0; s[k]; k++) {
        unsigned char c = (unsigned char)s[k];
        if (c < 0x20 || c == 0x7f || c == '|' || k >= GCSR_MAX_NAME) {
            return 0;
        }
    }
    return 1;
}

/* text of one line: no '|' and no control characters */
static void clean_text(char *dst, const char *src, size_t cap)
{
    size_t k = 0;
    while (src && *src && k + 1 < cap) {
        unsigned char c = (unsigned char)*src++;
        dst[k++] = (c < 0x20 || c == 0x7f || c == '|') ? ' ' : (char)c;
    }
    dst[k] = 0;
}

static int find_pair(const gcs_roster *r, const char *a, const char *b)
{
    int i;
    for (i = 0; i < r->npairs; i++) {
        if ((ieq(r->pairs[i].a, a) && ieq(r->pairs[i].b, b)) || (ieq(r->pairs[i].a, b) && ieq(r->pairs[i].b, a))) {
            return i;
        }
    }
    return -1;
}

static int find_req(const gcs_roster *r, const char *from, const char *to)
{
    int i;
    for (i = 0; i < r->nreqs; i++) {
        if (ieq(r->reqs[i].from, from) && ieq(r->reqs[i].to, to)) {
            return i;
        }
    }
    return -1;
}

static int count_friends(const gcs_roster *r, const char *user)
{
    int i, n = 0;
    for (i = 0; i < r->npairs; i++) {
        n += ieq(r->pairs[i].a, user) || ieq(r->pairs[i].b, user);
    }
    return n;
}

/* ---- file ---- */

static void save(gcs_roster *r)
{
    char tmp[280];
    FILE *f;
    int i;
    if (!r->file[0]) {
        return;
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", r->file);
    f = fopen(tmp, "w");
    if (!f) {
        rlog(r, 1, "roster: cannot write %s", tmp);
        return;
    }
    fprintf(f, "# PhantomLink SegaChat friends (F friends, R requests, M offline messages)\n");
    for (i = 0; i < r->npairs; i++) {
        fprintf(f, "F|%s|%s\n", r->pairs[i].a, r->pairs[i].b);
    }
    for (i = 0; i < r->nreqs; i++) {
        fprintf(f, "R|%s|%s|%s\n", r->reqs[i].from, r->reqs[i].to, r->reqs[i].status);
    }
    for (i = 0; i < r->nmsgs; i++) {
        const char *p;
        fprintf(f, "M|%s|%s|%ld|", r->msgs[i].to, r->msgs[i].from, r->msgs[i].time);
        for (p = r->msgs[i].stanza; *p; p++) {
            if (*p == '\n') {
                fputs("&#10;", f);
            } else if (*p == '\r') {
                fputs("&#13;", f);
            } else {
                fputc(*p, f);
            }
        }
        fputc('\n', f);
    }
    if (fclose(f) != 0) {
        rlog(r, 1, "roster: cannot write %s", tmp);
        remove(tmp);
        return;
    }
    remove(r->file);
    if (rename(tmp, r->file) != 0) {
        rlog(r, 1, "roster: cannot replace %s", r->file);
    }
}

/* splits off the next '|' field; NULL when there is none */
static char *field(char **p)
{
    char *s = *p, *bar;
    if (!s) {
        return NULL;
    }
    bar = strchr(s, '|');
    if (bar) {
        *bar = 0;
        *p = bar + 1;
    } else {
        *p = NULL;
    }
    return s;
}

static int add_message(gcs_roster *r, const char *to, const char *from, long t, const char *stanza);

static void load(gcs_roster *r)
{
    FILE *f;
    char *line;
    int n = 0;
    if (!r->file[0]) {
        return;
    }
    f = fopen(r->file, "r");
    if (!f) {
        return;
    }
    line = (char *)malloc(GCSR_MAX_STANZA * 2 + 200);
    if (!line) {
        fclose(f);
        return;
    }
    while (fgets(line, GCSR_MAX_STANZA * 2 + 200, f)) {
        char *p = line + 2, *a, *b, *c;
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) {
            line[--l] = 0;
        }
        if (l < 2 || line[1] != '|') {
            continue;
        }
        a = field(&p);
        b = field(&p);
        if (!name_ok(a) || !name_ok(b)) {
            continue;
        }
        if (line[0] == 'F') {
            if (find_pair(r, a, b) < 0 && r->npairs < GCSR_MAX_PAIRS) {
                copy_str(r->pairs[r->npairs].a, a, sizeof(r->pairs[0].a));
                copy_str(r->pairs[r->npairs].b, b, sizeof(r->pairs[0].b));
                r->npairs++;
                n++;
            }
        } else if (line[0] == 'R') {
            c = p ? p : (char *)"";
            if (find_req(r, a, b) < 0 && r->nreqs < GCSR_MAX_REQUESTS) {
                copy_str(r->reqs[r->nreqs].from, a, sizeof(r->reqs[0].from));
                copy_str(r->reqs[r->nreqs].to, b, sizeof(r->reqs[0].to));
                clean_text(r->reqs[r->nreqs].status, c, sizeof(r->reqs[0].status));
                r->nreqs++;
                n++;
            }
        } else if (line[0] == 'M') {
            char *t = field(&p);
            char *x, *w;
            if (!t || !p) {
                continue;
            }
            /* undo the newline escapes */
            for (x = w = p; *x;) {
                if (!strncmp(x, "&#10;", 5)) {
                    *w++ = '\n';
                    x += 5;
                } else if (!strncmp(x, "&#13;", 5)) {
                    *w++ = '\r';
                    x += 5;
                } else {
                    *w++ = *x++;
                }
            }
            *w = 0;
            if (add_message(r, a, b, strtol(t, NULL, 10), p) == 0) {
                n++;
            }
        }
    }
    free(line);
    fclose(f);
    rlog(r, 2, "roster: %d records from %s", n, r->file);
}

/* ---- API ---- */

gcs_roster *gcs_roster_create(const char *file, gcs_roster_log_fn log, void *ctx)
{
    gcs_roster *r = (gcs_roster *)calloc(1, sizeof(*r));
    if (!r) {
        return NULL;
    }
    r->log = log;
    r->ctx = ctx;
    copy_str(r->file, file ? file : "", sizeof(r->file));
    r->pairs = (pair *)calloc(GCSR_MAX_PAIRS, sizeof(pair));
    r->reqs = (request *)calloc(GCSR_MAX_REQUESTS, sizeof(request));
    r->msgs = (message *)calloc(GCSR_MAX_MSGS, sizeof(message));
    if (!r->pairs || !r->reqs || !r->msgs) {
        gcs_roster_destroy(r);
        return NULL;
    }
    load(r);
    return r;
}

void gcs_roster_destroy(gcs_roster *r)
{
    int i;
    if (!r) {
        return;
    }
    if (r->msgs) {
        for (i = 0; i < r->nmsgs; i++) {
            free(r->msgs[i].stanza);
        }
    }
    free(r->pairs);
    free(r->reqs);
    free(r->msgs);
    free(r);
}

int gcs_roster_are_friends(const gcs_roster *r, const char *a, const char *b)
{
    return r && a && b && find_pair(r, a, b) >= 0;
}

int gcs_roster_friends(const gcs_roster *r, const char *user, void (*fn)(void *ctx, const char *friend_name),
                       void *ctx)
{
    int i, n = 0;
    if (!r || !user) {
        return 0;
    }
    for (i = 0; i < r->npairs; i++) {
        const char *other = NULL;
        if (ieq(r->pairs[i].a, user)) {
            other = r->pairs[i].b;
        } else if (ieq(r->pairs[i].b, user)) {
            other = r->pairs[i].a;
        }
        if (other) {
            n++;
            if (fn) {
                fn(ctx, other);
            }
        }
    }
    return n;
}

int gcs_roster_add_friends(gcs_roster *r, const char *a, const char *b)
{
    int changed = 0, k;
    if (!r || !name_ok(a) || !name_ok(b) || ieq(a, b)) {
        return -1;
    }
    while ((k = find_req(r, a, b)) >= 0 || (k = find_req(r, b, a)) >= 0) {
        r->reqs[k] = r->reqs[--r->nreqs];
        changed = 1;
    }
    if (find_pair(r, a, b) >= 0) {
        if (changed) {
            save(r);
        }
        return 0;
    }
    if (r->npairs >= GCSR_MAX_PAIRS || count_friends(r, a) >= GCSR_MAX_FRIENDS ||
        count_friends(r, b) >= GCSR_MAX_FRIENDS) {
        if (changed) {
            save(r);
        }
        rlog(r, 1, "roster: no room for the friendship of '%s' and '%s'", a, b);
        return -1;
    }
    copy_str(r->pairs[r->npairs].a, a, sizeof(r->pairs[0].a));
    copy_str(r->pairs[r->npairs].b, b, sizeof(r->pairs[0].b));
    r->npairs++;
    save(r);
    return 1;
}

int gcs_roster_remove(gcs_roster *r, const char *a, const char *b)
{
    int k, changed = 0;
    if (!r || !a || !b) {
        return 0;
    }
    if ((k = find_pair(r, a, b)) >= 0) {
        memmove(&r->pairs[k], &r->pairs[k + 1], (size_t)(r->npairs - k - 1) * sizeof(pair));
        r->npairs--;
        changed = 1;
    }
    while ((k = find_req(r, a, b)) >= 0 || (k = find_req(r, b, a)) >= 0) {
        r->reqs[k] = r->reqs[--r->nreqs];
        changed = 1;
    }
    if (changed) {
        save(r);
    }
    return changed;
}

int gcs_roster_request(gcs_roster *r, const char *from, const char *to, const char *status)
{
    int k;
    if (!r || !name_ok(from) || !name_ok(to) || ieq(from, to)) {
        return -1;
    }
    k = find_req(r, from, to);
    if (k >= 0) {
        clean_text(r->reqs[k].status, status, sizeof(r->reqs[k].status));
        save(r);
        return 0;
    }
    if (r->nreqs >= GCSR_MAX_REQUESTS) {
        rlog(r, 1, "roster: request store full");
        return -1;
    }
    copy_str(r->reqs[r->nreqs].from, from, sizeof(r->reqs[0].from));
    copy_str(r->reqs[r->nreqs].to, to, sizeof(r->reqs[0].to));
    clean_text(r->reqs[r->nreqs].status, status, sizeof(r->reqs[0].status));
    r->nreqs++;
    save(r);
    return 1;
}

int gcs_roster_has_request(const gcs_roster *r, const char *from, const char *to, char *status)
{
    int k = (r && from && to) ? find_req(r, from, to) : -1;
    if (k < 0) {
        return 0;
    }
    if (status) {
        copy_str(status, r->reqs[k].status, GCSR_MAX_STATUS + 1);
    }
    return 1;
}

int gcs_roster_drop_request(gcs_roster *r, const char *from, const char *to)
{
    int k = (r && from && to) ? find_req(r, from, to) : -1;
    if (k < 0) {
        return 0;
    }
    r->reqs[k] = r->reqs[--r->nreqs];
    save(r);
    return 1;
}

int gcs_roster_requests(const gcs_roster *r, const char *user, int to_user,
                        void (*fn)(void *ctx, const char *other, const char *status), void *ctx)
{
    int i, n = 0;
    if (!r || !user) {
        return 0;
    }
    for (i = 0; i < r->nreqs; i++) {
        if (ieq(to_user ? r->reqs[i].to : r->reqs[i].from, user)) {
            n++;
            if (fn) {
                fn(ctx, to_user ? r->reqs[i].from : r->reqs[i].to, r->reqs[i].status);
            }
        }
    }
    return n;
}

static void drop_message(gcs_roster *r, int k)
{
    free(r->msgs[k].stanza);
    memmove(&r->msgs[k], &r->msgs[k + 1], (size_t)(r->nmsgs - k - 1) * sizeof(message));
    r->nmsgs--;
}

static int add_message(gcs_roster *r, const char *to, const char *from, long t, const char *stanza)
{
    int i, mine = 0, oldest = -1;
    size_t l = strlen(stanza);
    char *copy;
    if (!name_ok(to) || !name_ok(from) || l == 0 || l > GCSR_MAX_STANZA) {
        return -1;
    }
    for (i = 0; i < r->nmsgs; i++) {
        if (ieq(r->msgs[i].to, to)) {
            if (oldest < 0) {
                oldest = i;
            }
            mine++;
        }
    }
    if (mine >= GCSR_MAX_USERMSG && oldest >= 0) {
        drop_message(r, oldest);
    }
    if (r->nmsgs >= GCSR_MAX_MSGS) {
        return -1;
    }
    copy = (char *)malloc(l + 1);
    if (!copy) {
        return -1;
    }
    memcpy(copy, stanza, l + 1);
    copy_str(r->msgs[r->nmsgs].to, to, sizeof(r->msgs[0].to));
    copy_str(r->msgs[r->nmsgs].from, from, sizeof(r->msgs[0].from));
    r->msgs[r->nmsgs].time = t;
    r->msgs[r->nmsgs].stanza = copy;
    r->nmsgs++;
    return 0;
}

int gcs_roster_store_message(gcs_roster *r, const char *to, const char *from, long now, const char *stanza)
{
    if (!r || !stanza || add_message(r, to, from, now, stanza) != 0) {
        if (r) {
            rlog(r, 1, "roster: message for '%s' not kept (store full or too large)", to ? to : "");
        }
        return -1;
    }
    save(r);
    return 0;
}

int gcs_roster_take_messages(gcs_roster *r, const char *user,
                             void (*fn)(void *ctx, const char *from, long time, const char *stanza), void *ctx)
{
    int i = 0, n = 0;
    if (!r || !user) {
        return 0;
    }
    while (i < r->nmsgs) {
        if (ieq(r->msgs[i].to, user)) {
            if (fn) {
                fn(ctx, r->msgs[i].from, r->msgs[i].time, r->msgs[i].stanza);
            }
            drop_message(r, i);
            n++;
        } else {
            i++;
        }
    }
    if (n) {
        save(r);
    }
    return n;
}

void gcs_roster_forget(gcs_roster *r, const char *user)
{
    int i = 0, changed = 0;
    if (!r || !user) {
        return;
    }
    while (i < r->npairs) {
        if (ieq(r->pairs[i].a, user) || ieq(r->pairs[i].b, user)) {
            r->pairs[i] = r->pairs[--r->npairs];
            changed = 1;
        } else {
            i++;
        }
    }
    i = 0;
    while (i < r->nreqs) {
        if (ieq(r->reqs[i].from, user) || ieq(r->reqs[i].to, user)) {
            r->reqs[i] = r->reqs[--r->nreqs];
            changed = 1;
        } else {
            i++;
        }
    }
    i = 0;
    while (i < r->nmsgs) {
        if (ieq(r->msgs[i].to, user) || ieq(r->msgs[i].from, user)) {
            drop_message(r, i);
            changed = 1;
        } else {
            i++;
        }
    }
    if (changed) {
        save(r);
    }
}

void gcs_roster_get_stats(const gcs_roster *r, gcs_roster_stats *st)
{
    memset(st, 0, sizeof(*st));
    if (r) {
        st->friendships = r->npairs;
        st->requests = r->nreqs;
        st->messages = r->nmsgs;
    }
}
