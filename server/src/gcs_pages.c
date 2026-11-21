/** @file gcs_pages.c
 *
 *  Generated Arena device pages: rankings, tournaments, activities and
 *  games (gcs_pages.h).
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#include "gcs_pages.h"
#include "gcs_accounts.h"
#include "gcs_scores.h"
#include "gcs_tourney.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEV "/n-gage/device"

#define MAX_BOARDS 64
#define TOP_ROWS   10
#define MAX_LIST   50 /* items per list answer */
#define MAX_ACTS   GCST_MAX_ACTIVITIES

/* ---- output buffer ---- */

typedef struct
{
    char *d;
    size_t len, cap;
    int oom;
} buf;

static void put_n(buf *b, const char *s, size_t k)
{
    if (b->oom) {
        return;
    }
    if (b->len + k + 1 > b->cap) {
        size_t nc = (b->len + k + 1) * 2 + 256;
        char *nd = (char *)realloc(b->d, nc);
        if (!nd) {
            b->oom = 1;
            return;
        }
        b->d = nd;
        b->cap = nc;
    }
    memcpy(b->d + b->len, s, k);
    b->len += k;
    b->d[b->len] = 0;
}

static void put(buf *b, const char *s)
{
    put_n(b, s, strlen(s));
}

static void putf(buf *b, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    int k;
    va_start(ap, fmt);
    k = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (k > 0) {
        put_n(b, tmp, (size_t)k < sizeof(tmp) ? (size_t)k : sizeof(tmp) - 1);
    }
}

/* text for XML content and attribute values */
static void putx(buf *b, const char *s)
{
    for (; s && *s; s++) {
        switch (*s) {
        case '<':
            put(b, "&lt;");
            break;
        case '>':
            put(b, "&gt;");
            break;
        case '&':
            put(b, "&amp;");
            break;
        case '"':
            put(b, "&quot;");
            break;
        case '\'':
            put(b, "&apos;");
            break;
        default:
            if ((unsigned char)*s >= 0x20) {
                put_n(b, s, 1);
            }
            break;
        }
    }
}

/* ---- names ---- */

static const struct
{
    const char *gc;
    const char *name;
    const char *about;
} GAMES[] = {
    { "1", "Pocket Kingdom: Own the World",
      "Build your kingdom, send parties to search, scout and attack the kingdoms of other players, and trade in the "
      "Global Auction." },
    { "8053", "Rifts: Promise of Power",
      "Turn-based battles against other players: melee, survivor, defender, king of the hill and capture the flag, "
      "played one turn at a time." },
    { "42318", "Ashen", "Online leaderboards: the best score of every level and of the whole game." },
    { "1104", "Worms World Party",
      "Freestyle and ranked games for 2 to 4 players in game rooms; ranked games earn skill points." },
};
#define N_GAMES ((int)(sizeof(GAMES) / sizeof(GAMES[0])))

const char *gcs_pages_game_name(const char *gc)
{
    int i;
    for (i = 0; gc && i < N_GAMES; i++) {
        if (!strcmp(GAMES[i].gc, gc)) {
            return GAMES[i].name;
        }
    }
    return NULL;
}

static void put_game(buf *b, const char *gc)
{
    const char *n = gcs_pages_game_name(gc);
    if (n) {
        putx(b, n);
    } else {
        put(b, "Game ");
        putx(b, gc);
    }
}

static void put_board(buf *b, const char *stat)
{
    int lv;
    char rest[4];
    if (!strcmp(stat, GCSS_SKILL_POINTS)) {
        put(b, "Skill points");
    } else if (!strcmp(stat, "$wins")) {
        put(b, "Games won");
    } else if (!strcmp(stat, "game_total")) {
        put(b, "Total score");
    } else if (sscanf(stat, "level_%d%3s", &lv, rest) == 1) {
        putf(b, "Level %d", lv);
    } else {
        putx(b, stat);
    }
}

static void put_date(buf *b, long t, int with_time)
{
    /* civil from days (UTC) */
    long z = (t >= 0 ? t : t - 86399) / 86400, s = t - z * 86400;
    long era, doe, yoe, y, doy, mp, d, m;
    static const char *const MON[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    y += m <= 2;
    putf(b, "%ld %s %ld", d, MON[m - 1], y);
    if (with_time) {
        putf(b, " %02ld:%02ld UTC", s / 3600, (s / 60) % 60);
    }
}

/* ---- query string ---- */

static int hexv(int c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* value of key in a query ("a=1&b=2"), URL-decoded; 1 if present */
static int param(const char *q, const char *key, char *out, size_t cap)
{
    size_t kl = strlen(key);
    out[0] = 0;
    while (q && *q) {
        const char *amp = strchr(q, '&');
        size_t l = amp ? (size_t)(amp - q) : strlen(q);
        if (l > kl && !strncmp(q, key, kl) && q[kl] == '=') {
            size_t i = kl + 1, k = 0;
            while (i < l && k + 1 < cap) {
                if (q[i] == '%' && i + 2 < l + 1 && hexv(q[i + 1]) >= 0 && hexv(q[i + 2]) >= 0) {
                    out[k++] = (char)(hexv(q[i + 1]) * 16 + hexv(q[i + 2]));
                    i += 3;
                } else if (q[i] == '+') {
                    out[k++] = ' ';
                    i++;
                } else {
                    out[k++] = q[i++];
                }
            }
            out[k] = 0;
            return 1;
        }
        q = amp ? amp + 1 : NULL;
    }
    return 0;
}

static long param_num(const char *q, const char *key, long def)
{
    char v[24];
    char *end;
    long x;
    if (!param(q, key, v, sizeof(v)) || !v[0]) {
        return def;
    }
    x = strtol(v, &end, 10);
    return *end ? def : x;
}

static int lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int iprefix(const char *s, const char *prefix)
{
    while (*prefix && lower((unsigned char)*s) == lower((unsigned char)*prefix)) {
        s++;
        prefix++;
    }
    return *prefix == 0;
}

/* ---- page and list frames ---- */

static void page_begin(buf *b, const char *title)
{
    put(b, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
           "<!DOCTYPE html PUBLIC \"-//WAPFORUM//DTD XHTML Mobile 1.0//EN\" "
           "\"http://www.wapforum.org/DTD/xhtml-mobile10.dtd\">\n"
           "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>");
    putx(b, title);
    put(b, "</title><style type=\"text/css\">"
           "body { background-color: #080808; color: #eeeeee; font-family: monospace; font-size: 14px; margin: 4px; }"
           ".header { background-color: #292929; color: #ffffff; border: 2px solid #777777; padding: 5px; "
           "font-size: 18px; font-weight: bold; margin-bottom: 4px; }"
           ".section { background-color: #a60000; color: #ffffff; border: 1px solid #660000; padding: 4px; "
           "font-weight: bold; margin: 4px 0; }"
           ".content { background-color: #211313; color: #dddddd; border: 1px solid #633333; padding: 6px; }"
           ".content p { margin: 4px 0; } .date, .h { color: #ffcc66; font-weight: bold; }"
           "a { color: #ffcc33; text-decoration: none; }"
           ".footer { background-color: #252525; color: #aaaaaa; border: 1px solid #555555; padding: 5px; "
           "text-align: center; font-size: 11px; margin-top: 4px; }"
           "</style></head><body bgcolor=\"#080808\" text=\"#eeeeee\" "
           "style=\"background-color:#080808;color:#eeeeee\"><div class=\"header\">N-GAGE ARENA</div>"
           "<p class=\"section\">&gt; ");
    putx(b, title);
    put(b, "</p><div class=\"content\">");
}

static void page_end(buf *b)
{
    put(b, "</div><div class=\"footer\">N-GAGE ARENA &#183; PHANTOMLINK</div></body></html>");
}

typedef struct
{
    buf *b;
    long start, disp, total, shown;
} list;

static void list_begin(list *l, buf *b, const char *q)
{
    memset(l, 0, sizeof(*l));
    l->b = b;
    l->start = param_num(q, "start", 0);
    l->disp = param_num(q, "disp", MAX_LIST);
    if (l->start < 0) {
        l->start = 0;
    }
    if (l->disp <= 0 || l->disp > MAX_LIST) {
        l->disp = MAX_LIST;
    }
}

/* an item of the list; label already XML text */
static void list_item(list *l, long content_id, const char *game, const char *username, const char *label)
{
    long i = l->total++;
    buf *b = l->b;
    if (i < l->start || l->shown >= l->disp) {
        return;
    }
    l->shown++;
    putf(b, "<itemNode index=\"%ld\" contentId=\"%ld\" gameId=\"%s\" username=\"", i, content_id,
         game && game[0] ? game : "0");
    putx(b, username ? username : "");
    put(b, "\" mediaPath=\"\">");
    put(b, label);
    put(b, "</itemNode>");
}

static char *list_finish(list *l, buf *items, size_t *len)
{
    buf out;
    memset(&out, 0, sizeof(out));
    putf(&out, "<?xml version=\"1.0\" encoding=\"utf-8\"?><list><currentStart value=\"%ld\"/><listTotal value=\"%ld\"/>",
         l->start, l->total);
    if (items->d) {
        put(&out, items->d);
    }
    put(&out, "</list>");
    free(items->d);
    if (out.oom) {
        free(out.d);
        return NULL;
    }
    *len = out.len;
    return out.d;
}

/* ---- rankings ---- */

static int boards_of(const gcs_pages *p, gcs_scores_board *bd, const char *gc)
{
    int n = gcs_scores_boards(p->scores, bd, MAX_BOARDS), i, k = 0;
    for (i = 0; i < n; i++) {
        if (!gc || !strcmp(bd[i].game_class, gc)) {
            bd[k++] = bd[i];
        }
    }
    return k;
}

/* lowest is best: never for the stores here (gcs_scores keeps the highest) */
#define ASC 0

static void ranking_list(const gcs_pages *p, buf *b, const char *q)
{
    gcs_scores_board bd[MAX_BOARDS];
    char seen[MAX_BOARDS][GCSS_MAX_CLASS + 1];
    int n = boards_of(p, bd, NULL), i, j, ns = 0;
    list l;
    buf items;
    char cmd[16], who[64];
    memset(&items, 0, sizeof(items));
    list_begin(&l, &items, q);
    param(q, "cmd", cmd, sizeof(cmd));
    if (!strcmp(cmd, "search")) {
        /* Find players: accounts whose name starts with the text (the
         * framework adds username= to other requests too, so only
         * cmd=search makes this a search) */
        param(q, "username", who, sizeof(who));
        for (i = 0; p->accounts && i < gcs_accounts_count(p->accounts); i++) {
            const char *name = gcs_accounts_name_at(p->accounts, i);
            if (name && iprefix(name, who)) {
                buf lab;
                memset(&lab, 0, sizeof(lab));
                putx(&lab, name);
                list_item(&l, i + 1, "0", name, lab.d ? lab.d : "");
                free(lab.d);
            }
        }
    } else {
        for (i = 0; i < n; i++) {
            buf lab;
            for (j = 0; j < ns && strcmp(seen[j], bd[i].game_class); j++) {
            }
            if (j < ns) {
                continue;
            }
            memcpy(seen[ns], bd[i].game_class, sizeof(seen[0]));
            seen[ns++][sizeof(seen[0]) - 1] = 0;
            memset(&lab, 0, sizeof(lab));
            put_game(&lab, bd[i].game_class);
            list_item(&l, ns, bd[i].game_class, "", lab.d ? lab.d : "");
            free(lab.d);
        }
    }
    {
        size_t len;
        char *d = list_finish(&l, &items, &len);
        b->d = d;
        b->len = d ? len : 0;
        b->oom = !d;
    }
}

static void board_table(const gcs_pages *p, buf *b, const char *gc, const char *stat, const char *user)
{
    gcs_scores_entry e[TOP_ROWS], me;
    int total, k = gcs_scores_top(p->scores, gc, stat, ASC, e, TOP_ROWS, &total), i, myrank;
    int skills = !strcmp(stat, GCSS_SKILL_POINTS);
    put(b, "<p><b class=\"h\">");
    put_board(b, stat);
    putf(b, "</b> (%d player%s)<br/>", total, total == 1 ? "" : "s");
    for (i = 0; i < k; i++) {
        putf(b, "%d. ", e[i].rank);
        putx(b, e[i].user);
        putf(b, " %ld", e[i].value);
        if (skills) {
            putf(b, " (%ld-%ld-%ld)", e[i].wins, e[i].losses, e[i].draws);
        }
        put(b, "<br/>");
    }
    myrank = user && user[0] ? gcs_scores_rank(p->scores, gc, stat, ASC, user, &me) : 0;
    if (myrank > TOP_ROWS) {
        putf(b, "...<br/>%d. ", myrank);
        putx(b, me.user);
        putf(b, " %ld<br/>", me.value);
    }
    put(b, "</p>");
}

static void ranking_game(const gcs_pages *p, buf *b, const char *q, const char *user)
{
    gcs_scores_board bd[MAX_BOARDS];
    char gc[GCSS_MAX_CLASS + 1];
    int n, i;
    buf t;
    param(q, "gameId", gc, sizeof(gc));
    n = boards_of(p, bd, gc);
    memset(&t, 0, sizeof(t));
    put_game(&t, gc);
    page_begin(b, t.d ? t.d : "Rankings");
    free(t.d);
    if (!gc[0] || n == 0) {
        put(b, "<p>No rankings for this game yet.</p>");
    }
    /* skill points first, then the statistics */
    for (i = 0; i < n; i++) {
        if (!strcmp(bd[i].stat, GCSS_SKILL_POINTS)) {
            board_table(p, b, gc, bd[i].stat, user);
        }
    }
    for (i = 0; i < n; i++) {
        if (strcmp(bd[i].stat, GCSS_SKILL_POINTS)) {
            board_table(p, b, gc, bd[i].stat, user);
        }
    }
    page_end(b);
}

static void ranking_player(const gcs_pages *p, buf *b, const char *q, const char *user)
{
    gcs_scores_board bd[MAX_BOARDS];
    char who[64];
    int n = boards_of(p, bd, NULL), i, any = 0;
    if (!param(q, "username", who, sizeof(who)) || !who[0]) {
        snprintf(who, sizeof(who), "%s", user ? user : "");
    }
    page_begin(b, who[0] ? who : "Rankings");
    for (i = 0; who[0] && i < n; i++) {
        gcs_scores_entry e;
        int r = gcs_scores_rank(p->scores, bd[i].game_class, bd[i].stat, ASC, who, &e);
        if (!r) {
            continue;
        }
        any = 1;
        put(b, "<p><b class=\"h\">");
        put_game(b, bd[i].game_class);
        put(b, "</b><br/>");
        put_board(b, bd[i].stat);
        putf(b, ": %ld<br/>Rank %d of %d", e.value, r, bd[i].players);
        if (!strcmp(bd[i].stat, GCSS_SKILL_POINTS)) {
            putf(b, "<br/>Won %ld, lost %ld, drawn %ld", e.wins, e.losses, e.draws);
        }
        put(b, "</p>");
    }
    if (!any) {
        put(b, "<p>No rankings yet. Play an Arena game that reports scores to get ranked.</p>");
    }
    page_end(b);
}

static void put_activity(buf *b, const gcs_tourney_activity *a)
{
    putx(b, a->user);
    if (!strcmp(a->stat, "$wins")) {
        put(b, " won a ranked game of ");
        put_game(b, a->game_class);
    } else {
        put(b, ": new best in ");
        put_game(b, a->game_class);
        put(b, ", ");
        put_board(b, a->stat);
        putf(b, " %ld", a->value);
    }
}

static void spotlight(const gcs_pages *p, buf *b, long now)
{
    gcs_scores_board bd[MAX_BOARDS];
    int n = boards_of(p, bd, NULL), i;
    gcs_tourney_activity acts[5];
    int na = gcs_tourney_activities(p->tourney, acts, 5);
    (void)now;
    page_begin(b, "Spotlight");
    if (!n) {
        put(b, "<p>Nobody is ranked yet.</p>");
    }
    for (i = 0; i < n; i++) {
        gcs_scores_entry e;
        if (gcs_scores_top(p->scores, bd[i].game_class, bd[i].stat, ASC, &e, 1, NULL) == 1) {
            put(b, "<p><b class=\"h\">");
            put_game(b, bd[i].game_class);
            put(b, "</b>, ");
            put_board(b, bd[i].stat);
            put(b, "<br/>No. 1: ");
            putx(b, e.user);
            putf(b, " (%ld)</p>", e.value);
        }
    }
    if (na) {
        put(b, "<p><b class=\"h\">Latest</b><br/>");
        for (i = 0; i < na; i++) {
            put_activity(b, &acts[i]);
            put(b, "<br/>");
        }
        put(b, "</p>");
    }
    page_end(b);
}

/* ---- tournaments ---- */

static const char *state_name(int st)
{
    return st == GCST_RUNNING ? "running" : st == GCST_UPCOMING ? "upcoming"
                                                                : "finished";
}

static void tourney_list(const gcs_pages *p, buf *b, const char *q, long now)
{
    list l;
    buf items;
    int i;
    memset(&items, 0, sizeof(items));
    list_begin(&l, &items, q);
    gcs_tourney_refresh(p->tourney);
    for (i = 0; i < gcs_tourney_count(p->tourney); i++) {
        const gcs_tourney_def *d = gcs_tourney_at(p->tourney, i, now);
        buf lab;
        if (!d) {
            continue;
        }
        memset(&lab, 0, sizeof(lab));
        putx(&lab, d->name);
        putf(&lab, " (%s)", state_name(gcs_tourney_state(d, now)));
        list_item(&l, d->id, d->game_class, "", lab.d ? lab.d : "");
        free(lab.d);
    }
    {
        size_t len;
        char *d = list_finish(&l, &items, &len);
        b->d = d;
        b->len = d ? len : 0;
        b->oom = !d;
    }
}

static void tourney_head(buf *b, const gcs_tourney_def *d, long now)
{
    int st = gcs_tourney_state(d, now);
    put(b, "<p>");
    put_game(b, d->game_class);
    put(b, "<br/>");
    put_board(b, d->stat);
    put(b, d->mode == GCST_SUM ? " (added up)" : d->mode == GCST_LOW ? " (lowest wins)"
                                                                     : " (best counts)");
    put(b, "<br/>");
    put_date(b, d->start, 1);
    put(b, " to ");
    put_date(b, d->end, 1);
    putf(b, "<br/>%s</p>", st == GCST_RUNNING ? "Running now" : st == GCST_UPCOMING ? "Not started yet"
                                                                                    : "Finished");
    if (d->text[0]) {
        put(b, "<p>");
        putx(b, d->text);
        put(b, "</p>");
    }
}

static void tourney_page(const gcs_pages *p, buf *b, const char *q, const char *user, long now)
{
    const gcs_tourney_def *d;
    gcs_tourney_entry e[TOP_ROWS], me;
    int k, total, i, r;
    gcs_tourney_refresh(p->tourney);
    d = gcs_tourney_find(p->tourney, (int)param_num(q, "contentId", 0));
    if (!d) {
        page_begin(b, "Tournaments");
        put(b, "<p>This tournament does not exist (any more).</p>");
        page_end(b);
        return;
    }
    page_begin(b, d->name);
    tourney_head(b, d, now);
    k = gcs_tourney_standings(p->tourney, d->id, e, TOP_ROWS, &total);
    putf(b, "<p><b class=\"h\">%s</b> (%d player%s)<br/>",
         gcs_tourney_state(d, now) == GCST_FINISHED ? "Final standings" : "Standings", total, total == 1 ? "" : "s");
    if (!k) {
        put(b, gcs_tourney_state(d, now) == GCST_UPCOMING ? "Play once it has started to enter."
                                                          : "Nobody has entered yet: just play to enter.");
    }
    for (i = 0; i < k; i++) {
        putf(b, "%d. ", e[i].rank);
        putx(b, e[i].user);
        putf(b, " %ld<br/>", e[i].value);
    }
    r = user && user[0] ? gcs_tourney_rank(p->tourney, d->id, user, &me) : 0;
    if (r > TOP_ROWS) {
        putf(b, "...<br/>%d. ", r);
        putx(b, me.user);
        putf(b, " %ld<br/>", me.value);
    }
    put(b, "</p>");
    page_end(b);
}

static void tourney_news(const gcs_pages *p, buf *b, long now)
{
    int i, any = 0;
    gcs_tourney_refresh(p->tourney);
    page_begin(b, "Tournament news");
    for (i = 0; i < gcs_tourney_count(p->tourney); i++) {
        const gcs_tourney_def *d = gcs_tourney_at(p->tourney, i, now);
        int st;
        if (!d || (st = gcs_tourney_state(d, now)) == GCST_FINISHED) {
            continue;
        }
        any = 1;
        put(b, "<p><b class=\"h\">");
        putx(b, d->name);
        put(b, "</b><br/>");
        if (st == GCST_RUNNING) {
            gcs_tourney_entry e;
            put(b, "Running until ");
            put_date(b, d->end, 1);
            if (gcs_tourney_standings(p->tourney, d->id, &e, 1, NULL) == 1) {
                put(b, "<br/>Leader: ");
                putx(b, e.user);
                putf(b, " (%ld)", e.value);
            }
        } else {
            put(b, "Starts ");
            put_date(b, d->start, 1);
        }
        put(b, "<br/>");
        put_game(b, d->game_class);
        put(b, "</p>");
    }
    if (!any) {
        put(b, "<p>No tournaments are running or planned at the moment.</p>");
    }
    page_end(b);
}

static void tourney_winners(const gcs_pages *p, buf *b, long now)
{
    int i, any = 0;
    gcs_tourney_refresh(p->tourney);
    page_begin(b, "Winners");
    for (i = 0; i < gcs_tourney_count(p->tourney); i++) {
        const gcs_tourney_def *d = gcs_tourney_at(p->tourney, i, now);
        gcs_tourney_entry e[3];
        int k, j;
        if (!d || gcs_tourney_state(d, now) != GCST_FINISHED) {
            continue;
        }
        any = 1;
        put(b, "<p><b class=\"h\">");
        putx(b, d->name);
        put(b, "</b> (");
        put_date(b, d->end - 1, 0);
        put(b, ")<br/>");
        k = gcs_tourney_standings(p->tourney, d->id, e, 3, NULL);
        if (!k) {
            put(b, "Nobody took part.");
        }
        for (j = 0; j < k; j++) {
            putf(b, "%d. ", e[j].rank);
            putx(b, e[j].user);
            putf(b, " %ld<br/>", e[j].value);
        }
        put(b, "</p>");
    }
    if (!any) {
        put(b, "<p>No tournament has finished yet.</p>");
    }
    page_end(b);
}

/* ---- activities ---- */

static void activity_list(const gcs_pages *p, buf *b, const char *q)
{
    gcs_tourney_activity a[MAX_ACTS];
    int n = gcs_tourney_activities(p->tourney, a, MAX_ACTS), i;
    list l;
    buf items;
    memset(&items, 0, sizeof(items));
    list_begin(&l, &items, q);
    for (i = 0; i < n; i++) {
        buf lab;
        memset(&lab, 0, sizeof(lab));
        put_activity(&lab, &a[i]);
        {
            /* the label is XML text already: put_activity escapes */
            list_item(&l, (long)(i + 1), a[i].game_class, a[i].user, lab.d ? lab.d : "");
        }
        free(lab.d);
    }
    {
        size_t len;
        char *d = list_finish(&l, &items, &len);
        b->d = d;
        b->len = d ? len : 0;
        b->oom = !d;
    }
}

static void activity_page(const gcs_pages *p, buf *b, const char *q)
{
    gcs_tourney_activity a[MAX_ACTS];
    int n = gcs_tourney_activities(p->tourney, a, MAX_ACTS);
    long i = param_num(q, "contentId", 0);
    page_begin(b, "Activity");
    if (i < 1 || i > n) {
        put(b, "<p>This activity is no longer in the list.</p>");
    } else {
        put(b, "<p>");
        put_date(b, a[i - 1].time, 1);
        put(b, "<br/>");
        put_activity(b, &a[i - 1]);
        put(b, "</p>");
    }
    page_end(b);
}

static void activity_news(const gcs_pages *p, buf *b)
{
    gcs_tourney_activity a[MAX_ACTS];
    int n = gcs_tourney_activities(p->tourney, a, 20), i;
    page_begin(b, "Activity news");
    if (!n) {
        put(b, "<p>No activities yet: new personal bests and won ranked games show up here.</p>");
    }
    for (i = 0; i < n; i++) {
        put(b, "<p>");
        put_date(b, a[i].time, 1);
        put(b, "<br/>");
        put_activity(b, &a[i]);
        put(b, "</p>");
    }
    page_end(b);
}

/* ---- games ---- */

static void game_list(const gcs_pages *p, buf *b, const char *q)
{
    list l;
    buf items;
    int i;
    (void)p;
    memset(&items, 0, sizeof(items));
    list_begin(&l, &items, q);
    for (i = 0; i < N_GAMES; i++) {
        buf lab;
        memset(&lab, 0, sizeof(lab));
        putx(&lab, GAMES[i].name);
        list_item(&l, i + 1, GAMES[i].gc, "", lab.d ? lab.d : "");
        free(lab.d);
    }
    {
        size_t len;
        char *d = list_finish(&l, &items, &len);
        b->d = d;
        b->len = d ? len : 0;
        b->oom = !d;
    }
}

static void game_page(const gcs_pages *p, buf *b, const char *q, const char *user)
{
    char gc[GCSS_MAX_CLASS + 1];
    gcs_scores_board bd[MAX_BOARDS];
    int i, n, found = -1;
    param(q, "gameId", gc, sizeof(gc));
    for (i = 0; i < N_GAMES; i++) {
        if (!strcmp(GAMES[i].gc, gc)) {
            found = i;
        }
    }
    if (found < 0) {
        page_begin(b, "Games");
        put(b, "<p>Unknown game.</p>");
        page_end(b);
        return;
    }
    page_begin(b, GAMES[found].name);
    put(b, "<p>");
    putx(b, GAMES[found].about);
    put(b, "</p>");
    n = boards_of(p, bd, gc);
    for (i = 0; i < n; i++) {
        if (!strcmp(bd[i].stat, GCSS_SKILL_POINTS) || !strcmp(bd[i].stat, "game_total") || n == 1) {
            board_table(p, b, gc, bd[i].stat, user);
        }
    }
    page_end(b);
}

/* ---- dispatch ---- */

char *gcs_pages_render(const gcs_pages *p, const char *path, const char *query, const char *user, long now,
                       size_t *len, const char **type)
{
    buf b;
    int is_list = 0;
    memset(&b, 0, sizeof(b));
    if (!p || !p->scores || !path) {
        return NULL;
    }
    if (!user) {
        user = "";
    }
#define IS(x) (!strcmp(path, DEV x))
    if (IS("/community/ranking_list_x.jsp")) {
        ranking_list(p, &b, query);
        is_list = 1;
    } else if (IS("/global/play/rankings/post_game.jsp")) {
        ranking_game(p, &b, query, user);
    } else if (IS("/global/play/rankings/post_player.jsp")) {
        ranking_player(p, &b, query, user);
    } else if (IS("/global/play/rankings/spotlight.jsp")) {
        spotlight(p, &b, now);
    } else if (IS("/community/tournament_list_x.jsp")) {
        tourney_list(p, &b, query, now);
        is_list = 1;
    } else if (IS("/global/play/tournaments/post_tournaments.jsp")) {
        tourney_page(p, &b, query, user, now);
    } else if (IS("/global/play/tournaments/news.jsp")) {
        tourney_news(p, &b, now);
    } else if (IS("/global/play/tournaments/winners.jsp") || IS("/global/play/activities/winners.jsp")) {
        tourney_winners(p, &b, now);
    } else if (IS("/community/activity_list_x.jsp")) {
        activity_list(p, &b, query);
        is_list = 1;
    } else if (IS("/global/play/activities/post_activity.jsp")) {
        activity_page(p, &b, query);
    } else if (IS("/global/play/activities/news.jsp")) {
        activity_news(p, &b);
    } else if (IS("/community/game_list_x.jsp")) {
        game_list(p, &b, query);
        is_list = 1;
    } else if (IS("/global/play/games/post_game.jsp")) {
        game_page(p, &b, query, user);
    } else {
        return NULL;
    }
#undef IS
    if (b.oom || !b.d) {
        free(b.d);
        return NULL;
    }
    *len = b.len;
    *type = is_list ? GCSP_LIST_TYPE : GCSP_PAGE_TYPE;
    return b.d;
}
