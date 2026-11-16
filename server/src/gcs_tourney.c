/** @file gcs_tourney.c
 *
 *  Arena tournaments and activities (gcs_tourney.h).
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#include "gcs_tourney.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

typedef struct
{
    int id;
    gcs_tourney_entry e;
} standing;

struct gcs_tourney
{
    char defs_file[260];
    char data_file[260];
    gcs_tourney_log_fn log;
    void *ctx;
    long defs_mtime;
    long defs_size;
    gcs_tourney_def defs[GCST_MAX_TOURNEYS];
    int ndefs;
    standing *st;
    int nst;
    gcs_tourney_activity act[GCST_MAX_ACTIVITIES]; /* ring */
    int nact, act_next;
};

static void tlog(gcs_tourney *t, int level, const char *fmt, ...)
{
    char line[400];
    va_list ap;
    if (!t->log) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    t->log(t->ctx, level, line);
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

static void trim(char *s)
{
    size_t l = strlen(s), k = 0;
    while (l && (s[l - 1] == ' ' || s[l - 1] == '\t' || s[l - 1] == '\r' || s[l - 1] == '\n')) {
        s[--l] = 0;
    }
    while (s[k] == ' ' || s[k] == '\t') {
        k++;
    }
    if (k) {
        memmove(s, s + k, l - k + 1);
    }
}

static int token_ok(const char *s, size_t max)
{
    size_t k;
    if (!s || !s[0]) {
        return 0;
    }
    for (k = 0; s[k]; k++) {
        unsigned char c = (unsigned char)s[k];
        if (c < 0x20 || c == 0x7f || c == '|' || k >= max) {
            return 0;
        }
    }
    return 1;
}

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
    trim(s);
    return s;
}

/* days since 1970-01-01 of a proleptic Gregorian date */
static long days_from_civil(long y, long m, long d)
{
    long era, yoe, doy, doe;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

long gcs_tourney_parse_time(const char *s)
{
    int y, mo, d, h = 0, mi = 0, n;
    char rest[8];
    if (!s) {
        return -1;
    }
    n = sscanf(s, "%4d-%2d-%2d %2d:%2d%7s", &y, &mo, &d, &h, &mi, rest);
    if (n != 3 && n != 5) {
        return -1;
    }
    if (y < 1970 || y > 9999 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59) {
        return -1;
    }
    return days_from_civil(y, mo, d) * 86400L + h * 3600L + mi * 60L;
}

/* ---- definitions ---- */

static void load_defs(gcs_tourney *t)
{
    FILE *f;
    char line[1024];
    int ln = 0;
    t->ndefs = 0;
    if (!t->defs_file[0]) {
        return;
    }
    f = fopen(t->defs_file, "r");
    if (!f) {
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        char *p = line, *fid, *gc, *stat, *mode, *start, *end, *name, *text;
        gcs_tourney_def d;
        int i, dup = 0;
        ln++;
        trim(line);
        if (!line[0] || line[0] == '#') {
            continue;
        }
        fid = field(&p);
        gc = field(&p);
        stat = field(&p);
        mode = field(&p);
        start = field(&p);
        end = field(&p);
        name = field(&p);
        text = p ? p : (char *)"";
        trim(text);
        memset(&d, 0, sizeof(d));
        d.id = fid ? atoi(fid) : 0;
        d.start = gcs_tourney_parse_time(start);
        d.end = gcs_tourney_parse_time(end);
        if (!mode || !strcmp(mode, "best")) {
            d.mode = GCST_BEST;
        } else if (!strcmp(mode, "low")) {
            d.mode = GCST_LOW;
        } else if (!strcmp(mode, "sum")) {
            d.mode = GCST_SUM;
        } else {
            d.mode = -1;
        }
        if (d.id <= 0 || !token_ok(gc, GCST_MAX_CLASS) || !token_ok(stat, GCST_MAX_STAT) || d.mode < 0 ||
            d.start < 0 || d.end <= d.start || !token_ok(name, GCST_MAX_TITLE)) {
            tlog(t, 1, "tournaments: %s line %d ignored (id|game class|statistic|best/low/sum|start|end|name|text)",
                 t->defs_file, ln);
            continue;
        }
        for (i = 0; i < t->ndefs; i++) {
            dup |= t->defs[i].id == d.id;
        }
        if (dup || t->ndefs >= GCST_MAX_TOURNEYS) {
            tlog(t, 1, "tournaments: %s line %d ignored (id %d used twice, or more than %d tournaments)", t->defs_file,
                 ln, d.id, GCST_MAX_TOURNEYS);
            continue;
        }
        copy_str(d.game_class, gc, sizeof(d.game_class));
        copy_str(d.stat, stat, sizeof(d.stat));
        copy_str(d.name, name, sizeof(d.name));
        {
            size_t k = 0, w = 0;
            for (; text[k] && w + 1 < sizeof(d.text); k++) {
                unsigned char c = (unsigned char)text[k];
                d.text[w++] = (c < 0x20 || c == 0x7f) ? ' ' : (char)c;
            }
            d.text[w] = 0;
        }
        t->defs[t->ndefs++] = d;
    }
    fclose(f);
    tlog(t, 2, "tournaments: %d from %s", t->ndefs, t->defs_file);
}

void gcs_tourney_refresh(gcs_tourney *t)
{
    struct stat sb;
    if (!t || !t->defs_file[0]) {
        return;
    }
    if (stat(t->defs_file, &sb) != 0) {
        if (t->defs_mtime || t->ndefs) {
            t->ndefs = 0;
            t->defs_mtime = 0;
            t->defs_size = 0;
        }
        return;
    }
    if ((long)sb.st_mtime != t->defs_mtime || (long)sb.st_size != t->defs_size) {
        t->defs_mtime = (long)sb.st_mtime;
        t->defs_size = (long)sb.st_size;
        load_defs(t);
    }
}

/* ---- data file ---- */

static const gcs_tourney_def *def_of(const gcs_tourney *t, int id)
{
    int i;
    for (i = 0; i < t->ndefs; i++) {
        if (t->defs[i].id == id) {
            return &t->defs[i];
        }
    }
    return NULL;
}

static void save(gcs_tourney *t)
{
    char tmp[280];
    FILE *f;
    int i;
    if (!t->data_file[0]) {
        return;
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", t->data_file);
    f = fopen(tmp, "w");
    if (!f) {
        tlog(t, 1, "tournaments: cannot write %s", tmp);
        return;
    }
    fprintf(f, "# PhantomLink tournament standings (S) and activities (A)\n");
    for (i = 0; i < t->nst; i++) {
        /* standings of tournaments the operator removed are dropped
         * (unless the list could not be read at all) */
        if (t->ndefs && !def_of(t, t->st[i].id)) {
            continue;
        }
        fprintf(f, "S|%d|%s|%ld|%ld|%ld\n", t->st[i].id, t->st[i].e.user, t->st[i].e.value, t->st[i].e.count,
                t->st[i].e.time);
    }
    for (i = 0; i < t->nact; i++) {
        const gcs_tourney_activity *a =
            &t->act[(t->act_next - t->nact + i + GCST_MAX_ACTIVITIES) % GCST_MAX_ACTIVITIES];
        fprintf(f, "A|%ld|%s|%s|%s|%ld\n", a->time, a->game_class, a->user, a->stat, a->value);
    }
    if (fclose(f) != 0) {
        remove(tmp);
        tlog(t, 1, "tournaments: cannot write %s", tmp);
        return;
    }
    remove(t->data_file);
    if (rename(tmp, t->data_file) != 0) {
        tlog(t, 1, "tournaments: cannot replace %s", t->data_file);
    }
}

static void add_activity(gcs_tourney *t, long time, const char *gc, const char *user, const char *stat, long value)
{
    gcs_tourney_activity *a = &t->act[t->act_next];
    memset(a, 0, sizeof(*a));
    a->time = time;
    copy_str(a->game_class, gc, sizeof(a->game_class));
    copy_str(a->user, user, sizeof(a->user));
    copy_str(a->stat, stat, sizeof(a->stat));
    a->value = value;
    t->act_next = (t->act_next + 1) % GCST_MAX_ACTIVITIES;
    if (t->nact < GCST_MAX_ACTIVITIES) {
        t->nact++;
    }
}

static void load_data(gcs_tourney *t)
{
    FILE *f;
    char line[512];
    if (!t->data_file[0]) {
        return;
    }
    f = fopen(t->data_file, "r");
    if (!f) {
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        char *p = line + 2;
        trim(line);
        if (line[0] == 'S' && line[1] == '|') {
            char *id = field(&p), *user = field(&p), *v = field(&p), *cnt = field(&p), *tm = field(&p);
            if (id && atoi(id) > 0 && token_ok(user, GCST_MAX_NAME) && v && t->nst < GCST_MAX_STANDINGS) {
                standing *s = &t->st[t->nst++];
                memset(s, 0, sizeof(*s));
                s->id = atoi(id);
                copy_str(s->e.user, user, sizeof(s->e.user));
                s->e.value = strtol(v, NULL, 10);
                s->e.count = cnt ? strtol(cnt, NULL, 10) : 1;
                s->e.time = tm ? strtol(tm, NULL, 10) : 0;
            }
        } else if (line[0] == 'A' && line[1] == '|') {
            char *tm = field(&p), *gc = field(&p), *user = field(&p), *stat = field(&p), *v = field(&p);
            if (tm && token_ok(gc, GCST_MAX_CLASS) && token_ok(user, GCST_MAX_NAME) && token_ok(stat, GCST_MAX_STAT) &&
                v) {
                add_activity(t, strtol(tm, NULL, 10), gc, user, stat, strtol(v, NULL, 10));
            }
        }
    }
    fclose(f);
}

/* ---- API ---- */

gcs_tourney *gcs_tourney_create(const char *defs_file, const char *data_file, gcs_tourney_log_fn log, void *ctx)
{
    gcs_tourney *t = (gcs_tourney *)calloc(1, sizeof(*t));
    if (!t) {
        return NULL;
    }
    t->st = (standing *)calloc(GCST_MAX_STANDINGS, sizeof(standing));
    if (!t->st) {
        free(t);
        return NULL;
    }
    t->log = log;
    t->ctx = ctx;
    copy_str(t->defs_file, defs_file ? defs_file : "", sizeof(t->defs_file));
    copy_str(t->data_file, data_file ? data_file : "", sizeof(t->data_file));
    gcs_tourney_refresh(t);
    load_data(t);
    return t;
}

void gcs_tourney_destroy(gcs_tourney *t)
{
    if (t) {
        free(t->st);
        free(t);
    }
}

int gcs_tourney_state(const gcs_tourney_def *d, long now)
{
    if (now < d->start) {
        return GCST_UPCOMING;
    }
    return now < d->end ? GCST_RUNNING : GCST_FINISHED;
}

void gcs_tourney_report(gcs_tourney *t, const char *game_class, const char *user, const char *stat, long value,
                        int delta, int new_best, long now)
{
    int i, j, changed = 0;
    if (!t || !token_ok(game_class, GCST_MAX_CLASS) || !token_ok(user, GCST_MAX_NAME) || !stat) {
        return;
    }
    gcs_tourney_refresh(t);
    for (i = 0; i < t->ndefs; i++) {
        const gcs_tourney_def *d = &t->defs[i];
        standing *s = NULL;
        if (strcmp(d->game_class, game_class) || strcmp(d->stat, stat) || gcs_tourney_state(d, now) != GCST_RUNNING) {
            continue;
        }
        if ((d->mode == GCST_SUM) != (delta != 0)) {
            continue; /* sum adds per match values, best/low compare absolute ones */
        }
        if (d->mode != GCST_SUM && value <= 0) {
            continue;
        }
        if (d->mode == GCST_SUM && value == 0) {
            continue;
        }
        for (j = 0; j < t->nst; j++) {
            if (t->st[j].id == d->id && ieq(t->st[j].e.user, user)) {
                s = &t->st[j];
                break;
            }
        }
        if (!s) {
            if (t->nst >= GCST_MAX_STANDINGS) {
                tlog(t, 1, "tournaments: standings full, '%s' not entered in %d", user, d->id);
                continue;
            }
            s = &t->st[t->nst++];
            memset(s, 0, sizeof(*s));
            s->id = d->id;
            copy_str(s->e.user, user, sizeof(s->e.user));
            s->e.value = d->mode == GCST_SUM ? 0 : value;
            s->e.time = now;
            changed = 1;
            tlog(t, 2, "tournaments: '%s' enters '%s'", user, d->name);
        }
        if (d->mode == GCST_SUM) {
            s->e.value += value;
            s->e.time = now;
            changed = 1;
        } else if ((d->mode == GCST_BEST && value > s->e.value) || (d->mode == GCST_LOW && value < s->e.value)) {
            s->e.value = value;
            s->e.time = now;
            changed = 1;
        }
        s->e.count++;
    }
    if ((delta && !strcmp(stat, "$wins") && value > 0) || (!delta && new_best)) {
        add_activity(t, now, game_class, user, stat, value);
        changed = 1;
    }
    if (changed) {
        save(t);
    }
}

int gcs_tourney_count(const gcs_tourney *t)
{
    return t ? t->ndefs : 0;
}

const gcs_tourney_def *gcs_tourney_find(const gcs_tourney *t, int id)
{
    return t ? def_of(t, id) : NULL;
}

/* order of the list: running, upcoming, finished; by start (finished:
 * the latest end first) */
static long order_key(const gcs_tourney_def *d, long now, int *group)
{
    static const int g[3] = { 1, 0, 2 };
    int st = gcs_tourney_state(d, now);
    *group = g[st];
    return st == GCST_FINISHED ? -d->end : d->start;
}

const gcs_tourney_def *gcs_tourney_at(const gcs_tourney *t, int i, long now)
{
    int k, j, rank;
    if (!t || i < 0 || i >= t->ndefs) {
        return NULL;
    }
    /* the i-th in order: count the ones before each candidate */
    for (k = 0; k < t->ndefs; k++) {
        int gk;
        long kk = order_key(&t->defs[k], now, &gk);
        rank = 0;
        for (j = 0; j < t->ndefs; j++) {
            int gj;
            long kj = order_key(&t->defs[j], now, &gj);
            if (gj < gk || (gj == gk && (kj < kk || (kj == kk && t->defs[j].id < t->defs[k].id)))) {
                rank++;
            }
        }
        if (rank == i) {
            return &t->defs[k];
        }
    }
    return NULL;
}

static int g_low; /* qsort has no context argument */

static int by_value(const void *a, const void *b)
{
    const gcs_tourney_entry *x = (const gcs_tourney_entry *)a;
    const gcs_tourney_entry *y = (const gcs_tourney_entry *)b;
    if (x->value != y->value) {
        return (x->value < y->value) == g_low ? -1 : 1;
    }
    if (x->time != y->time) {
        return x->time < y->time ? -1 : 1;
    }
    return strcmp(x->user, y->user);
}

static gcs_tourney_entry *sorted(const gcs_tourney *t, int id, int *count)
{
    const gcs_tourney_def *d = def_of(t, id);
    gcs_tourney_entry *e;
    int i, n = 0;
    *count = 0;
    if (!d) {
        return NULL;
    }
    e = (gcs_tourney_entry *)malloc((size_t)t->nst * sizeof(*e) + 1);
    if (!e) {
        return NULL;
    }
    for (i = 0; i < t->nst; i++) {
        if (t->st[i].id == id) {
            e[n++] = t->st[i].e;
        }
    }
    g_low = d->mode == GCST_LOW;
    qsort(e, (size_t)n, sizeof(*e), by_value);
    for (i = 0; i < n; i++) {
        e[i].rank = i + 1;
    }
    *count = n;
    return e;
}

int gcs_tourney_standings(const gcs_tourney *t, int id, gcs_tourney_entry *out, int max, int *total)
{
    gcs_tourney_entry *e;
    int n, k;
    if (total) {
        *total = 0;
    }
    if (!t || !out || max <= 0) {
        return 0;
    }
    e = sorted(t, id, &n);
    if (!e) {
        return 0;
    }
    k = n < max ? n : max;
    memcpy(out, e, (size_t)k * sizeof(*e));
    free(e);
    if (total) {
        *total = n;
    }
    return k;
}

int gcs_tourney_rank(const gcs_tourney *t, int id, const char *user, gcs_tourney_entry *out)
{
    gcs_tourney_entry *e;
    int n, i, r = 0;
    if (!t || !user) {
        return 0;
    }
    e = sorted(t, id, &n);
    if (!e) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (ieq(e[i].user, user)) {
            r = i + 1;
            if (out) {
                *out = e[i];
            }
            break;
        }
    }
    free(e);
    return r;
}

int gcs_tourney_activities(const gcs_tourney *t, gcs_tourney_activity *out, int max)
{
    int i, n;
    if (!t || !out || max <= 0) {
        return 0;
    }
    n = t->nact < max ? t->nact : max;
    for (i = 0; i < n; i++) {
        out[i] = t->act[(t->act_next - 1 - i + 2 * GCST_MAX_ACTIVITIES) % GCST_MAX_ACTIVITIES];
    }
    return n;
}
