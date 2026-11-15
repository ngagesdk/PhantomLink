/** @file gcs_scores.c
 *
 *  SegaChat score service, see gcs_scores.h.  The message and answer
 *  formats come from sc_lib.dll ("segachat_send_event",
 *  "segachat_retrieve_req", "segachat_retrieve_resp") and from the
 *  leaderboard code of Ashen (docs/ARENA.md).
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#include "gcs_scores.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct
{
    char gcid[GCSS_MAX_CLASS + 1];
    char stat[GCSS_MAX_STAT + 1];
    char user[GCSS_MAX_NAME + 1];
    long value;
    long time; /* when the value was reported, seconds since 1970 */
} record;

/* skill points and results of a player of an ArenaFramework game */
typedef struct
{
    char gcid[GCSS_MAX_CLASS + 1];
    char user[GCSS_MAX_NAME + 1];
    long points, wins, losses, draws, games;
    long time; /* last report */
} skill;

/* (game class, match, user, kind) already counted */
typedef struct
{
    char key[GCSS_MAX_CLASS + GCSS_MAX_NAME + 40];
} seen;

#define SEEN_RING 512

struct gcs_scores
{
    char file[260];
    gcs_scores_log_fn log;
    void *ctx;
    record *recs;
    int nrecs, caprecs;
    skill *skills;
    int nskills, capskills;
    seen seen[SEEN_RING];
    int seen_next;
    long next_match;
    int dirty;
    unsigned long submits, queries, matches;
};

static void slog(gcs_scores *s, int level, const char *fmt, ...)
{
    char line[300];
    va_list ap;
    if (!s->log) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    s->log(s->ctx, level, line);
}

static void copy_str(char *dst, const char *src, size_t cap)
{
    size_t k = 0;
    if (cap == 0) {
        return;
    }
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

/* Names that fit a line of the data file and a CSV field. */
static int valid_token(const char *t, size_t max)
{
    size_t k = 0;
    if (!t || !t[0]) {
        return 0;
    }
    for (; t[k]; k++) {
        unsigned char c = (unsigned char)t[k];
        if (c < 0x20 || c == 0x7f || c == '|' || k >= max) {
            return 0;
        }
    }
    return 1;
}

static int parse_value(const char *v, long *out)
{
    char *end;
    long x;
    while (*v == ' ') {
        v++;
    }
    if (!*v) {
        return -1;
    }
    errno = 0;
    x = strtol(v, &end, 10);
    while (*end == ' ') {
        end++;
    }
    if (*end || errno == ERANGE) {
        return -1;
    }
    *out = x;
    return 0;
}

static record *find(gcs_scores *s, const char *gcid, const char *stat, const char *user)
{
    int i;
    for (i = 0; i < s->nrecs; i++) {
        record *r = &s->recs[i];
        if (!strcmp(r->gcid, gcid) && !strcmp(r->stat, stat) && ieq(r->user, user)) {
            return r;
        }
    }
    return NULL;
}

static record *add(gcs_scores *s)
{
    if (s->nrecs >= GCSS_MAX_RECORDS) {
        return NULL;
    }
    if (s->nrecs == s->caprecs) {
        int nc = s->caprecs ? s->caprecs * 2 : 256;
        record *n = (record *)realloc(s->recs, (size_t)nc * sizeof(record));
        if (!n) {
            return NULL;
        }
        s->recs = n;
        s->caprecs = nc;
    }
    memset(&s->recs[s->nrecs], 0, sizeof(record));
    return &s->recs[s->nrecs++];
}

static skill *skill_find(gcs_scores *s, const char *gcid, const char *user, int create)
{
    int i;
    for (i = 0; i < s->nskills; i++) {
        if (!strcmp(s->skills[i].gcid, gcid) && ieq(s->skills[i].user, user)) {
            return &s->skills[i];
        }
    }
    if (!create || s->nskills >= GCSS_MAX_SKILLS) {
        return NULL;
    }
    if (s->nskills == s->capskills) {
        int nc = s->capskills ? s->capskills * 2 : 64;
        skill *n = (skill *)realloc(s->skills, (size_t)nc * sizeof(skill));
        if (!n) {
            return NULL;
        }
        s->skills = n;
        s->capskills = nc;
    }
    memset(&s->skills[s->nskills], 0, sizeof(skill));
    copy_str(s->skills[s->nskills].gcid, gcid, sizeof(s->skills[0].gcid));
    copy_str(s->skills[s->nskills].user, user, sizeof(s->skills[0].user));
    return &s->skills[s->nskills++];
}

/* "class <tab> user <tab> points wins losses draws games time" */
static int load_skill(gcs_scores *s, char *p)
{
    char *fld[8];
    long v[6];
    int n = 0, i;
    skill *k;
    while (n < 8) {
        char *tab;
        fld[n++] = p;
        tab = strchr(p, '\t');
        if (!tab) {
            break;
        }
        *tab = 0;
        p = tab + 1;
    }
    if (n != 8 || !valid_token(fld[0], GCSS_MAX_CLASS) || !valid_token(fld[1], GCSS_MAX_NAME)) {
        return -1;
    }
    for (i = 0; i < 6; i++) {
        if (parse_value(fld[2 + i], &v[i])) {
            return -1;
        }
    }
    if (skill_find(s, fld[0], fld[1], 0)) {
        return -1;
    }
    k = skill_find(s, fld[0], fld[1], 1);
    if (!k) {
        return -1;
    }
    k->points = v[0];
    k->wins = v[1];
    k->losses = v[2];
    k->draws = v[3];
    k->games = v[4];
    k->time = v[5];
    return 0;
}

/* ---- file: "S <tab> class <tab> stat <tab> user <tab> value <tab> time" ---- */

static void save(gcs_scores *s)
{
    char tmp[300];
    FILE *f;
    int i;
    s->dirty = 0;
    if (!s->file[0]) {
        return;
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", s->file);
    f = fopen(tmp, "w");
    if (!f) {
        slog(s, 1, "scores: cannot write %s", tmp);
        return;
    }
    fputs("# PhantomLink SegaChat scores: S gameClass stat user value time\n", f);
    for (i = 0; i < s->nrecs; i++) {
        const record *r = &s->recs[i];
        fprintf(f, "S\t%s\t%s\t%s\t%ld\t%ld\n", r->gcid, r->stat, r->user, r->value, r->time);
    }
    if (s->nskills || s->next_match) {
        fputs("# K gameClass user points wins losses draws games time; N next match id\n", f);
        fprintf(f, "N\t%ld\n", s->next_match);
    }
    for (i = 0; i < s->nskills; i++) {
        const skill *k = &s->skills[i];
        fprintf(f, "K\t%s\t%s\t%ld\t%ld\t%ld\t%ld\t%ld\t%ld\n", k->gcid, k->user, k->points, k->wins, k->losses,
                k->draws, k->games, k->time);
    }
    if (fclose(f) != 0) {
        slog(s, 1, "scores: cannot write %s", tmp);
        return;
    }
    remove(s->file); /* rename does not replace on Windows */
    if (rename(tmp, s->file) != 0) {
        slog(s, 1, "scores: cannot replace %s", s->file);
    }
}

static void load(gcs_scores *s)
{
    char line[512];
    FILE *f = s->file[0] ? fopen(s->file, "r") : NULL;
    int bad = 0;
    if (!f) {
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        char *fld[6], *p = line;
        int n = 0;
        long v, t;
        record *r;
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == 'N' && line[1] == '\t') {
            if (parse_value(line + 2, &v) || v < 0) {
                bad++;
            } else {
                s->next_match = v;
            }
            continue;
        }
        if (line[0] == 'K' && line[1] == '\t') {
            if (load_skill(s, line + 2)) {
                bad++;
            }
            continue;
        }
        if (line[0] != 'S' || line[1] != '\t') {
            continue;
        }
        while (n < 6) {
            char *tab;
            fld[n++] = p;
            tab = strchr(p, '\t');
            if (!tab) {
                break;
            }
            *tab = 0;
            p = tab + 1;
        }
        if (n != 6 || !valid_token(fld[1], GCSS_MAX_CLASS) || !valid_token(fld[2], GCSS_MAX_STAT) ||
            !valid_token(fld[3], GCSS_MAX_NAME) || parse_value(fld[4], &v) || parse_value(fld[5], &t) ||
            find(s, fld[1], fld[2], fld[3])) {
            bad++;
            continue;
        }
        r = add(s);
        if (!r) {
            slog(s, 1, "scores: %s holds more than %d values, the rest is ignored", s->file, GCSS_MAX_RECORDS);
            break;
        }
        copy_str(r->gcid, fld[1], sizeof(r->gcid));
        copy_str(r->stat, fld[2], sizeof(r->stat));
        copy_str(r->user, fld[3], sizeof(r->user));
        r->value = v;
        r->time = t;
    }
    fclose(f);
    if (bad) {
        slog(s, 1, "scores: %d bad lines in %s ignored", bad, s->file);
    }
    slog(s, 2, "scores: %d values and %d players with skill points loaded from %s", s->nrecs, s->nskills, s->file);
}

/* ---- API ---- */

gcs_scores *gcs_scores_create(const char *file, gcs_scores_log_fn log, void *ctx)
{
    gcs_scores *s = (gcs_scores *)calloc(1, sizeof(*s));
    if (!s) {
        return NULL;
    }
    copy_str(s->file, file ? file : "", sizeof(s->file));
    s->log = log;
    s->ctx = ctx;
    load(s);
    return s;
}

void gcs_scores_destroy(gcs_scores *s)
{
    if (!s) {
        return;
    }
    if (s->dirty) {
        save(s);
    }
    free(s->recs);
    free(s->skills);
    free(s);
}

int gcs_scores_submit(gcs_scores *s, const char *game_class, const char *user, const char *stat, const char *value,
                      long now)
{
    record *r;
    long v;
    if (!s) {
        return -1;
    }
    s->submits++;
    if (!valid_token(game_class, GCSS_MAX_CLASS) || !valid_token(user, GCSS_MAX_NAME) ||
        !valid_token(stat, GCSS_MAX_STAT) || !value || parse_value(value, &v)) {
        slog(s, 1, "scores: report of '%s' for '%s'/'%s' rejected", user ? user : "", game_class ? game_class : "",
             stat ? stat : "");
        return -1;
    }
    if (v <= 0) {
        return 0; /* not played (yet) */
    }
    r = find(s, game_class, stat, user);
    if (r && r->value >= v) {
        return 0;
    }
    if (!r) {
        r = add(s);
        if (!r) {
            slog(s, 1, "scores: store full (%d values), '%s' not kept", GCSS_MAX_RECORDS, user);
            return -1;
        }
        copy_str(r->gcid, game_class, sizeof(r->gcid));
        copy_str(r->stat, stat, sizeof(r->stat));
        copy_str(r->user, user, sizeof(r->user));
    }
    slog(s, 2, "scores: %s: new best %s = %ld (game class %s)", user, stat, v, game_class);
    r->value = v;
    r->time = now;
    s->dirty = 1;
    return 1;
}

void gcs_scores_commit(gcs_scores *s)
{
    if (s && s->dirty) {
        save(s);
    }
}

/* ---- queries ---- */

typedef struct
{
    char queryid[32], board[32], stat[GCSS_MAX_STAT + 1], periodicity[16], ordering[16], format[16];
    long offset, limit;
} query;

/* name='..' or name=".." inside one tag [t, e) */
static int tag_attr(const char *t, const char *e, const char *name, char *out, size_t cap)
{
    size_t nl = strlen(name);
    const char *p;
    out[0] = 0;
    for (p = t; p + nl + 2 < e; p++) {
        if ((p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n') && !strncmp(p, name, nl) && p[nl] == '=' &&
            (p[nl + 1] == '\'' || p[nl + 1] == '"')) {
            char q = p[nl + 1];
            const char *v = p + nl + 2;
            size_t k = 0;
            while (v < e && *v != q) {
                if (k + 1 < cap) {
                    out[k++] = *v;
                }
                v++;
            }
            out[k] = 0;
            return v < e;
        }
    }
    return 0;
}

static void parse_itemlist(const char *req, query *q)
{
    const char *p = req;
    memset(q, 0, sizeof(*q));
    q->limit = GCSS_MAX_ROWS;
    copy_str(q->periodicity, "alltime", sizeof(q->periodicity));
    copy_str(q->ordering, "natural", sizeof(q->ordering));
    copy_str(q->format, "csv", sizeof(q->format));
    while ((p = strstr(p, "<item")) != NULL) {
        const char *e = strchr(p, '>');
        char name[32], value[64];
        long n;
        if (!e) {
            break;
        }
        if (p[5] == ' ' && tag_attr(p + 5, e, "name", name, sizeof(name)) &&
            tag_attr(p + 5, e, "value", value, sizeof(value))) {
            if (!strcmp(name, "queryid")) {
                copy_str(q->queryid, value, sizeof(q->queryid));
            } else if (!strcmp(name, "board")) {
                copy_str(q->board, value, sizeof(q->board));
            } else if (!strcmp(name, "stat")) {
                copy_str(q->stat, value, sizeof(q->stat));
            } else if (!strcmp(name, "periodicity")) {
                copy_str(q->periodicity, value, sizeof(q->periodicity));
            } else if (!strcmp(name, "ordering")) {
                copy_str(q->ordering, value, sizeof(q->ordering));
            } else if (!strcmp(name, "format")) {
                copy_str(q->format, value, sizeof(q->format));
            } else if (!strcmp(name, "offset") && !parse_value(value, &n)) {
                q->offset = n;
            } else if (!strcmp(name, "limit") && !parse_value(value, &n)) {
                q->limit = n;
            }
        }
        p = e + 1;
    }
}

static int g_ascending; /* qsort has no context argument */

static int by_rank(const void *a, const void *b)
{
    const record *x = *(const record *const *)a;
    const record *y = *(const record *const *)b;
    if (x->value != y->value) {
        return (x->value > y->value) == !g_ascending ? -1 : 1;
    }
    if (x->time != y->time) {
        return x->time < y->time ? -1 : 1; /* first come, first ranked */
    }
    return strcmp(x->user, y->user);
}

/* field text without the CSV separators */
static void csv_field(char *dst, const char *src, size_t cap)
{
    size_t k;
    copy_str(dst, src, cap);
    for (k = 0; dst[k]; k++) {
        if (dst[k] == '|' || dst[k] == '\n' || dst[k] == '\r') {
            dst[k] = '_';
        }
    }
}

static int error_answer(char *out, size_t cap, const char *msg)
{
    snprintf(out, cap, "1\n%s\n", msg);
    return -1;
}

int gcs_scores_query(gcs_scores *s, const char *game_class, const char *request, char *out, size_t cap)
{
    query q;
    const record **list;
    char rows[GCSS_MAX_ROWS][96];
    char qid[32], board[32], stat[GCSS_MAX_STAT + 1], per[16];
    int total = 0, nrows = 0, i, rank = 0;
    long limit, offset;
    size_t used;
    if (!out || cap < 64) {
        return -1;
    }
    out[0] = 0;
    if (!s) {
        return error_answer(out, cap, "Server error");
    }
    s->queries++;
    parse_itemlist(request ? request : "", &q);
    if (!valid_token(game_class, GCSS_MAX_CLASS)) {
        return error_answer(out, cap, "Unknown game");
    }
    if (!ieq(q.format, "csv")) {
        slog(s, 1, "scores: format '%s' not supported", q.format);
        return error_answer(out, cap, "Unsupported format");
    }
    if (!valid_token(q.stat, GCSS_MAX_STAT)) {
        return error_answer(out, cap, "Unknown statistic");
    }
    if (!ieq(q.periodicity, "alltime")) {
        slog(s, 2, "scores: periodicity '%s' answered with the all-time list", q.periodicity);
    }
    g_ascending = ieq(q.ordering, "reverse") || ieq(q.ordering, "ascending") || ieq(q.ordering, "asc");
    limit = q.limit < 1 ? 1 : q.limit > GCSS_MAX_ROWS ? GCSS_MAX_ROWS
                                                      : q.limit;
    offset = q.offset < 0 ? 0 : q.offset;

    list = (const record **)malloc((size_t)(s->nrecs ? s->nrecs : 1) * sizeof(*list));
    if (!list) {
        return error_answer(out, cap, "Server busy");
    }
    for (i = 0; i < s->nrecs; i++) {
        if (!strcmp(s->recs[i].gcid, game_class) && !strcmp(s->recs[i].stat, q.stat)) {
            list[total++] = &s->recs[i];
        }
    }
    qsort(list, (size_t)total, sizeof(*list), by_rank);

    csv_field(qid, q.queryid, sizeof(qid));
    csv_field(board, q.board, sizeof(board));
    csv_field(stat, q.stat, sizeof(stat));
    csv_field(per, q.periodicity, sizeof(per));
    /* header and rows must fit: the game takes the row count from the
     * header and reads that many rows */
    used = (size_t)snprintf(NULL, 0, "0\nOK\n%s|%s|%s|%d|%ld|%d|%s\n", qid, board, stat, GCSS_MAX_ROWS, offset,
                            total, per);
    for (i = 0; i < total && nrows < limit; i++) {
        char name[GCSS_MAX_NAME + 1], date[40];
        time_t t = (time_t)list[i]->time;
        struct tm *tm = gmtime(&t);
        size_t k;
        if (i == 0 || list[i]->value != list[i - 1]->value) {
            rank = i + 1; /* equal values share a rank: 1, 2, 2, 4 */
        }
        if (i < offset) {
            continue;
        }
        csv_field(name, list[i]->user, sizeof(name));
        if (tm) {
            snprintf(date, sizeof(date), "%04d-%02d-%02d", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
        } else {
            copy_str(date, "-", sizeof(date));
        }
        k = (size_t)snprintf(rows[nrows], sizeof(rows[nrows]), "%s|%d|%ld|%s\n", name, rank, list[i]->value, date);
        if (k >= sizeof(rows[nrows]) || used + k >= cap || used + k >= GCSS_MAX_CSV) {
            break;
        }
        used += k;
        nrows++;
    }
    free(list);

    used = (size_t)snprintf(out, cap, "0\nOK\n%s|%s|%s|%d|%ld|%d|%s\n", qid, board, stat, nrows, offset, total, per);
    for (i = 0; i < nrows && used < cap; i++) {
        size_t k = strlen(rows[i]);
        if (used + k >= cap) {
            break; /* cannot happen: checked above */
        }
        memcpy(out + used, rows[i], k + 1);
        used += k;
    }
    slog(s, 2, "scores: top %ld of %s/%s: %d of %d rows", limit, game_class, q.stat, nrows, total);
    return nrows;
}

void gcs_scores_get_stats(const gcs_scores *s, gcs_scores_stats *st)
{
    memset(st, 0, sizeof(*st));
    if (s) {
        st->records = s->nrecs;
        st->submits = s->submits;
        st->queries = s->queries;
        st->skills = s->nskills;
        st->matches = s->matches;
    }
}

/* ---- ArenaFramework games (gcs_scores.h) ---- */

int gcs_scores_is_af(const char *request)
{
    return request && (strstr(request, "\"$version\"") || strstr(request, "'$version'") ||
                       !strcmp(request, "$version") || !strcmp(request, "queryid"));
}

#define AF_NAMES 8

typedef struct
{
    query q;
    char names[AF_NAMES][GCSS_MAX_NAME + 1];
    int nnames;
    long below;
    int has_below;
} afquery;

static void parse_af(const char *req, afquery *a)
{
    const char *p = req;
    memset(a, 0, sizeof(*a));
    parse_itemlist(req, &a->q);
    while ((p = strstr(p, "<item")) != NULL) {
        const char *e = strchr(p, '>');
        char name[32], value[64];
        long n;
        if (!e) {
            break;
        }
        if (p[5] == ' ' && tag_attr(p + 5, e, "name", name, sizeof(name)) &&
            tag_attr(p + 5, e, "value", value, sizeof(value))) {
            if (!strcmp(name, "name") && value[0] && a->nnames < AF_NAMES) {
                copy_str(a->names[a->nnames++], value, sizeof(a->names[0]));
            } else if (!strcmp(name, "below") && !parse_value(value, &n)) {
                a->below = n;
                a->has_below = 1;
            }
        }
        p = e + 1;
    }
}

static int seen_before(gcs_scores *s, const char *gcid, const char *match, const char *user, char kind)
{
    char key[sizeof(s->seen[0].key)];
    int i;
    if (!match || !match[0]) {
        return 0;
    }
    snprintf(key, sizeof(key), "%c|%s|%s|", kind, gcid, match);
    for (i = 0; key[i]; i++) {
    }
    copy_str(key + i, user, sizeof(key) - (size_t)i);
    for (i = 0; key[i]; i++) {
        key[i] = (char)lower((unsigned char)key[i]);
    }
    for (i = 0; i < SEEN_RING; i++) {
        if (!strcmp(s->seen[i].key, key)) {
            return 1;
        }
    }
    copy_str(s->seen[s->seen_next].key, key, sizeof(key));
    s->seen_next = (s->seen_next + 1) % SEEN_RING;
    return 0;
}

/* items of a report that are not results to keep as best values */
static int reserved_item(const char *name)
{
    static const char *const names[] = { "queryid", "win_lose", "draw", "opponent_id",
                                         "gametime", "roundsplayed", "checksum1", "checksum2",
                                         "match_id", "medal" };
    size_t i;
    if (name[0] == '$') {
        return 1;
    }
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!strcmp(name, names[i])) {
            return 1;
        }
    }
    return 0;
}

static const char *item_value(const gcs_scores_item *items, int n, const char *name)
{
    int i;
    for (i = 0; i < n; i++) {
        if (items[i].name && !strcmp(items[i].name, name)) {
            return items[i].value ? items[i].value : "";
        }
    }
    return NULL;
}

int gcs_scores_af_submit(gcs_scores *s, const char *game_class, const char *user, const char *match_id,
                         const gcs_scores_item *items, int n, long now, char *out, size_t cap)
{
    const char *qid, *pts, *wl, *dr;
    char q[32];
    long v;
    int i, kept = 0;
    if (!out || cap < 64) {
        return -1;
    }
    out[0] = 0;
    if (!s) {
        return error_answer(out, cap, "Server error");
    }
    s->submits++;
    if (!valid_token(game_class, GCSS_MAX_CLASS) || !valid_token(user, GCSS_MAX_NAME)) {
        return error_answer(out, cap, "Unknown player");
    }
    qid = item_value(items, n, "queryid");
    csv_field(q, qid ? qid : "0", sizeof(q));
    if (!match_id) {
        match_id = "";
    }

    /* skill points of a ranked match: added to the total */
    pts = item_value(items, n, "$ps_points");
    if (pts && !parse_value(pts, &v)) {
        if (seen_before(s, game_class, match_id, user, 'P')) {
            slog(s, 1, "scores: %s: skill points of match %s reported again, ignored", user, match_id);
        } else {
            skill *k = skill_find(s, game_class, user, 1);
            if (k) {
                k->points += v;
                k->time = now;
                s->dirty = 1;
                slog(s, 2, "scores: %s: %+ld skill points in match %s (game class %s), total %ld", user, v,
                     match_id[0] ? match_id : "-", game_class, k->points);
            }
        }
    }

    /* result of a match */
    wl = item_value(items, n, "win_lose");
    dr = item_value(items, n, "draw");
    if (wl || dr) {
        long w = 0, d = 0;
        if (wl) {
            parse_value(wl, &w);
        }
        if (dr) {
            parse_value(dr, &d);
        }
        if (seen_before(s, game_class, match_id, user, 'R')) {
            slog(s, 1, "scores: %s: result of match %s reported again, ignored", user, match_id);
        } else {
            skill *k = skill_find(s, game_class, user, 1);
            if (k) {
                k->games++;
                if (d > 0) {
                    k->draws++;
                } else if (w > 0) {
                    k->wins++;
                } else {
                    k->losses++;
                }
                k->time = now;
                s->dirty = 1;
                slog(s, 2, "scores: %s: %s in match %s (game class %s, opponent %s)", user,
                     d > 0 ? "draw" : w > 0 ? "won"
                                            : "lost",
                     match_id[0] ? match_id : "-", game_class,
                     item_value(items, n, "opponent_id") ? item_value(items, n, "opponent_id") : "?");
            }
        }
    }

    /* everything else: best values like Ashen's levels */
    for (i = 0; i < n; i++) {
        if (items[i].name && items[i].value && !reserved_item(items[i].name) &&
            gcs_scores_submit(s, game_class, user, items[i].name, items[i].value, now) > 0) {
            kept++;
        }
    }
    gcs_scores_commit(s);
    snprintf(out, cap, "0\nOK\n%s|1|submit|0\n", q);
    (void)kept;
    return 0;
}

static int g_desc_points; /* qsort has no context argument */

static int by_points(const void *a, const void *b)
{
    const skill *x = *(const skill *const *)a;
    const skill *y = *(const skill *const *)b;
    if (x->points != y->points) {
        return (x->points > y->points) == g_desc_points ? -1 : 1;
    }
    if (x->wins != y->wins) {
        return x->wins > y->wins ? -1 : 1;
    }
    if (x->time != y->time) {
        return x->time < y->time ? -1 : 1;
    }
    return strcmp(x->user, y->user);
}

/* appends one line; 0 if it did not fit */
static int put_line(char *out, size_t cap, size_t *used, const char *fmt, ...)
{
    va_list ap;
    int k;
    if (*used >= cap) {
        return 0;
    }
    va_start(ap, fmt);
    k = vsnprintf(out + *used, cap - *used, fmt, ap);
    va_end(ap);
    if (k < 0 || *used + (size_t)k >= cap || *used + (size_t)k >= GCSS_AF_MAX_CSV) {
        out[*used] = 0;
        return 0;
    }
    *used += (size_t)k;
    return 1;
}

typedef enum
{
    B_SKILLS,
    B_HIGH,
    B_FILTER
} board_kind;

/* header and row field counts that arenabase.dll checks */
static void af_shape(board_kind b, int ev, int *hdr, int *row)
{
    /* ev: 0 getplayer, 1 topn, 2 proximitylist */
    static const int h[3][3] = { { 7, 6, 6 }, { 6, 7, 7 }, { 6, 7, 6 } };
    static const int r[3][3] = { { 6, 7, 7 }, { 4, 4, 4 }, { 3, 3, 3 } };
    *hdr = h[b][ev];
    *row = r[b][ev];
}

int gcs_scores_af_query(gcs_scores *s, const char *game_class, const char *event, const char *user,
                        const char *request, char *out, size_t cap)
{
    static const char *const bname[3] = { "playerskills", "highscores", "hsfilter" };
    afquery a;
    char qid[32], per[16], extra[GCSS_MAX_NAME + 1];
    char *body;
    size_t used = 0, hdr_at;
    board_kind b;
    int ev, hdr, rowf, total = 0, nrows = 0, i;
    long limit, offset;
    if (!out || cap < 64) {
        return -1;
    }
    out[0] = 0;
    if (!s) {
        return error_answer(out, cap, "Server error");
    }
    s->queries++;
    parse_af(request ? request : "", &a);
    csv_field(qid, a.q.queryid[0] ? a.q.queryid : "0", sizeof(qid));
    csv_field(per, a.q.periodicity, sizeof(per));
    if (!valid_token(game_class, GCSS_MAX_CLASS)) {
        return error_answer(out, cap, "Unknown game");
    }
    if (!event) {
        event = "";
    }
    if (!strcmp(event, "getmatchid")) {
        long id;
        if (s->next_match < 1000) {
            s->next_match = 1000 + (long)(time(NULL) % 100000) * 10; /* not 0, not reused after a reset */
        }
        id = s->next_match++;
        s->matches++;
        s->dirty = 1;
        gcs_scores_commit(s);
        snprintf(out, cap, "0\nOK\n%s|%ld|0|1|getmatchid|%s\n", qid, id, per);
        slog(s, 2, "scores: new match id %ld for %s (game class %s)", id, user ? user : "?", game_class);
        return 1;
    }
    if (!strcmp(event, "getplayer")) {
        ev = 0;
    } else if (!strcmp(event, "topn")) {
        ev = 1;
    } else if (!strcmp(event, "proximitylist")) {
        ev = 2;
    } else {
        slog(s, 1, "scores: request '%s' not supported", event);
        return error_answer(out, cap, "Unsupported request");
    }
    if (ieq(a.q.board, "player_skills") || ieq(a.q.board, "playerskills")) {
        b = B_SKILLS;
    } else if (ieq(a.q.board, "hsfilter")) {
        b = B_FILTER;
    } else {
        b = B_HIGH;
        if (!valid_token(a.q.stat, GCSS_MAX_STAT)) {
            return error_answer(out, cap, "Unknown statistic");
        }
    }
    if (!ieq(a.q.periodicity, "alltime")) {
        slog(s, 2, "scores: periodicity '%s' answered with the all-time list", a.q.periodicity);
    }
    af_shape(b, ev, &hdr, &rowf);
    limit = a.q.limit < 1 ? GCSS_AF_MAX_ROWS : a.q.limit > GCSS_AF_MAX_ROWS ? GCSS_AF_MAX_ROWS
                                                                            : a.q.limit;
    offset = a.q.offset < 0 ? 0 : a.q.offset;
    if (ev == 0 && a.nnames == 0 && user && user[0]) {
        copy_str(a.names[a.nnames++], user, sizeof(a.names[0]));
    }
    csv_field(extra, ev == 0 && a.nnames ? a.names[0] : (b == B_SKILLS ? "arena" : a.q.stat), sizeof(extra));

    put_line(out, cap, &used, "0\nOK\n");
    hdr_at = used;
    /* header first with room for the counts; written again at the end */
    body = (char *)malloc(GCSS_AF_MAX_CSV + 1);
    if (!body) {
        return error_answer(out, cap, "Server busy");
    }
    {
        size_t bu = 0;
        int start = 0, rank = 0;
        if (b == B_SKILLS) {
            const skill **list =
                (const skill **)malloc((size_t)(s->nskills ? s->nskills : 1) * sizeof(*list));
            if (!list) {
                free(body);
                return error_answer(out, cap, "Server busy");
            }
            for (i = 0; i < s->nskills; i++) {
                if (!strcmp(s->skills[i].gcid, game_class)) {
                    list[total++] = &s->skills[i];
                }
            }
            g_desc_points = !(ieq(a.q.ordering, "reverse") || ieq(a.q.ordering, "ascending"));
            qsort(list, (size_t)total, sizeof(*list), by_points);
            if (ev == 0) {
                int nm;
                for (nm = 0; nm < a.nnames; nm++) {
                    int r = 0, found = -1;
                    for (i = 0; i < total; i++) {
                        if (i == 0 || list[i]->points != list[i - 1]->points) {
                            r = i + 1;
                        }
                        if (ieq(list[i]->user, a.names[nm])) {
                            found = i;
                            break;
                        }
                    }
                    if (found >= 0) {
                        const skill *k = list[found];
                        char nmf[GCSS_MAX_NAME + 1];
                        csv_field(nmf, k->user, sizeof(nmf));
                        if (!put_line(body, GCSS_AF_MAX_CSV, &bu, "%s|%d|%ld|%ld|%ld|%ld\n", nmf, r, k->points,
                                      k->wins, k->losses, k->draws)) {
                            break;
                        }
                    } else {
                        char nmf[GCSS_MAX_NAME + 1];
                        csv_field(nmf, a.names[nm], sizeof(nmf));
                        /* not ranked yet: rank 0, no points */
                        if (!put_line(body, GCSS_AF_MAX_CSV, &bu, "%s|0|0|0|0|0\n", nmf)) {
                            break;
                        }
                    }
                    nrows++;
                }
            } else {
                if (ev == 2) {
                    const char *who = a.nnames ? a.names[0] : (user ? user : "");
                    long before = a.has_below ? (limit - 1 - a.below) : limit / 2;
                    for (i = 0; i < total; i++) {
                        if (ieq(list[i]->user, who)) {
                            start = i - (int)(before < 0 ? 0 : before);
                            break;
                        }
                    }
                    if (start < 0) {
                        start = 0;
                    }
                } else {
                    start = (int)offset;
                }
                for (i = 0; i < total && nrows < limit; i++) {
                    char nmf[GCSS_MAX_NAME + 1];
                    const skill *k = list[i];
                    if (i == 0 || k->points != list[i - 1]->points) {
                        rank = i + 1;
                    }
                    if (i < start) {
                        continue;
                    }
                    csv_field(nmf, k->user, sizeof(nmf));
                    if (!put_line(body, GCSS_AF_MAX_CSV, &bu, "%s|%d|%ld|%ld|%ld|%ld|%ld\n", nmf, rank, k->points,
                                  k->wins, k->losses, k->draws, k->games)) {
                        break;
                    }
                    nrows++;
                }
                offset = start;
            }
            free(list);
        } else {
            const record **list =
                (const record **)malloc((size_t)(s->nrecs ? s->nrecs : 1) * sizeof(*list));
            const char *stat = a.q.stat;
            if (!list) {
                free(body);
                return error_answer(out, cap, "Server busy");
            }
            for (i = 0; i < s->nrecs; i++) {
                if (!strcmp(s->recs[i].gcid, game_class) && (!stat[0] || !strcmp(s->recs[i].stat, stat))) {
                    list[total++] = &s->recs[i];
                }
            }
            g_ascending = ieq(a.q.ordering, "reverse") || ieq(a.q.ordering, "ascending");
            qsort(list, (size_t)total, sizeof(*list), by_rank);
            if (ev == 2 || ev == 0) {
                const char *who = a.nnames ? a.names[0] : (user ? user : "");
                start = -1;
                for (i = 0; i < total; i++) {
                    if (ieq(list[i]->user, who)) {
                        start = i;
                        break;
                    }
                }
                if (ev == 2) {
                    long before = a.has_below ? (limit - 1 - a.below) : limit / 2;
                    start = start < 0 ? 0 : start - (int)(before < 0 ? 0 : before);
                    if (start < 0) {
                        start = 0;
                    }
                }
            } else {
                start = (int)offset;
            }
            for (i = 0; i < total && (ev != 0 || start >= 0) && nrows < (ev == 0 ? 1 : limit); i++) {
                char nmf[GCSS_MAX_NAME + 1], date[40];
                const record *r = list[i];
                time_t t = (time_t)r->time;
                struct tm *tm = gmtime(&t);
                int ok;
                if (i == 0 || r->value != list[i - 1]->value) {
                    rank = i + 1;
                }
                if (i < start) {
                    continue;
                }
                csv_field(nmf, r->user, sizeof(nmf));
                if (tm) {
                    snprintf(date, sizeof(date), "%04d-%02d-%02d", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
                } else {
                    copy_str(date, "-", sizeof(date));
                }
                if (rowf == 4) {
                    ok = put_line(body, GCSS_AF_MAX_CSV, &bu, "%s|%d|%ld|%s\n", nmf, rank, r->value, date);
                } else {
                    ok = put_line(body, GCSS_AF_MAX_CSV, &bu, "%s|%d|%ld\n", nmf, rank, r->value);
                }
                if (!ok) {
                    break;
                }
                nrows++;
            }
            if (ev != 0) {
                offset = start < 0 ? 0 : start;
            }
            free(list);
        }
        if (hdr == 7) {
            put_line(out, cap, &used, "%s|%d|%ld|%d|%s|%s|%s\n", qid, total, offset, nrows, bname[b], per, extra);
        } else {
            put_line(out, cap, &used, "%s|%d|%ld|%d|%s|%s\n", qid, total, offset, nrows, bname[b], per);
        }
        if (used + bu < cap) {
            memcpy(out + used, body, bu);
            used += bu;
            out[used] = 0;
        } else {
            out[hdr_at] = 0;
            nrows = 0;
        }
    }
    free(body);
    slog(s, 2, "scores: %s %s of game class %s for %s: %d rows (header %d, rows %d fields)", event, bname[b],
         game_class, user ? user : "?", nrows, hdr, rowf);
    return nrows;
}
