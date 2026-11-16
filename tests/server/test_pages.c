/* Arena Play menu: tournaments (server/src/gcs_tourney.c) and the
 * generated device pages (gcs_pages.c): rankings, tournaments,
 * activities and games.  Lists are decoded the way cafmplay.dll reads
 * them (itemNode with index, contentId, gameId, username, mediaPath and
 * the label as text; currentStart, listTotal).  The last part fetches
 * pages over HTTP through gcs_direct and gcs_native after a SegaChat
 * score report.
 *
 * SPDX-License-Identifier: MIT */
#include "gc_test.h"
#include "gcs_accounts.h"
#include "gcs_core.h"
#include "gcs_direct.h"
#include "gcs_native.h"
#include "gcs_pages.h"
#include "gcs_scores.h"
#include "gcs_tourney.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define DEFS  "test_pages_tournaments.txt"
#define TDATA "test_pages_tournaments.dat"
#define ACC   "test_pages_accounts.txt"

#define DAY 86400L

static void quiet_log(void *ctx, int level, const char *line)
{
    (void)ctx;
    if (level <= 1) {
        printf("  log %d: %s\n", level, line);
    }
}

/* ---- the list as cafmplay.dll reads it ---- */

typedef struct
{
    long start, total;
    int n;
    long index[64], content[64], game[64];
    char user[64][40], label[64][160];
} xlist;

static int xattr(const char *tag, const char *end, const char *name, char *out, size_t cap)
{
    size_t nl = strlen(name);
    const char *p;
    for (p = tag; p + nl + 2 < end; p++) {
        if (p[-1] == ' ' && !strncmp(p, name, nl) && p[nl] == '=' && p[nl + 1] == '"') {
            const char *v = p + nl + 2;
            size_t k = 0;
            while (v < end && *v != '"' && k + 1 < cap) {
                out[k++] = *v++;
            }
            out[k] = 0;
            return 1;
        }
    }
    return 0;
}

static int parse_list(const char *x, xlist *l)
{
    const char *p;
    char v[64];
    memset(l, 0, sizeof(*l));
    if (strncmp(x, "<?xml", 5) || !strstr(x, "<list>") || !strstr(x, "</list>")) {
        return -1;
    }
    p = strstr(x, "<currentStart ");
    if (!p || !xattr(p, strchr(p, '>'), "value", v, sizeof(v))) {
        return -1;
    }
    l->start = atol(v);
    p = strstr(x, "<listTotal ");
    if (!p || !xattr(p, strchr(p, '>'), "value", v, sizeof(v))) {
        return -1;
    }
    l->total = atol(v);
    for (p = strstr(x, "<itemNode "); p && l->n < 64; p = strstr(p + 1, "<itemNode ")) {
        const char *gt = strchr(p, '>'), *close = strstr(p, "</itemNode>");
        size_t k;
        if (!gt || !close) {
            return -1;
        }
        CHECK(xattr(p, gt, "index", v, sizeof(v)));
        l->index[l->n] = atol(v);
        CHECK(xattr(p, gt, "contentId", v, sizeof(v)));
        l->content[l->n] = atol(v);
        CHECK(xattr(p, gt, "gameId", v, sizeof(v)));
        l->game[l->n] = atol(v);
        CHECK(xattr(p, gt, "username", l->user[l->n], sizeof(l->user[0])));
        CHECK(xattr(p, gt, "mediaPath", v, sizeof(v)));
        k = (size_t)(close - gt - 1);
        if (k >= sizeof(l->label[0])) {
            k = sizeof(l->label[0]) - 1;
        }
        memcpy(l->label[l->n], gt + 1, k);
        l->label[l->n][k] = 0;
        l->n++;
    }
    return 0;
}

static char *render(const gcs_pages *p, const char *path, const char *q, const char *user, long now)
{
    size_t len = 0;
    const char *type = NULL;
    char full[256];
    char *d;
    snprintf(full, sizeof(full), "/n-gage/device%s", path);
    d = gcs_pages_render(p, full, q, user, now, &len, &type);
    CHECK(d != NULL);
    if (d) {
        CHECK_EQ(strlen(d), len);
        CHECK(!strcmp(type, strstr(path, "_x.jsp") ? GCSP_LIST_TYPE : GCSP_PAGE_TYPE));
        if (!strstr(path, "_x.jsp")) {
            CHECK(!strncmp(d, "<?xml", 5) && strstr(d, "</body></html>") != NULL);
        }
    }
    return d;
}

static void write_defs(const char *text)
{
    FILE *f = fopen(DEFS, "w");
    CHECK(f != NULL);
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static void test_time(void)
{
    CHECK_EQ(gcs_tourney_parse_time("1970-01-01"), 0);
    CHECK_EQ(gcs_tourney_parse_time("2026-12-01"), 1796083200L);
    CHECK_EQ(gcs_tourney_parse_time("2026-12-01 18:30"), 1796083200L + 18 * 3600 + 30 * 60);
    CHECK_EQ(gcs_tourney_parse_time("2024-02-29"), 1709164800L);
    CHECK_EQ(gcs_tourney_parse_time("2026-13-01"), -1);
    CHECK_EQ(gcs_tourney_parse_time("2026-12-01 25:00"), -1);
    CHECK_EQ(gcs_tourney_parse_time("yesterday"), -1);
    CHECK_EQ(gcs_tourney_parse_time("2026-12-01 18:30x"), -1);
}

static void test_tourney(void)
{
    long t0 = gcs_tourney_parse_time("2026-12-01");
    gcs_tourney *t;
    gcs_tourney_entry e[8];
    gcs_tourney_activity a[8];
    int total;
    remove(DEFS);
    remove(TDATA);
    write_defs("# id|game class|statistic|mode|start|end|name|description\n"
               "1|42318|game_total|best|2026-12-01|2026-12-31|Ashen Cup|Best total.\n"
               "2|1104|$ps_points|sum|2026-12-01|2026-12-08|Worms week|Skill points.\n"
               "3|42318|level_1|low|2026-12-01|2026-12-31|Speed run|Lowest wins.\n"
               "4|42318|game_total|best|2027-01-01|2027-02-01|Next cup|Later.\n"
               "5|42318|game_total|best|2026-11-01|2026-11-30|Old cup|Done.\n"
               "x|bad line\n"
               "1|42318|game_total|best|2026-12-01|2026-12-31|Dup|id used twice\n"
               "6|42318|game_total|most|2026-12-01|2026-12-31|Bad mode|\n");
    t = gcs_tourney_create(DEFS, TDATA, quiet_log, NULL);
    CHECK(t != NULL);
    CHECK_EQ(gcs_tourney_count(t), 5);
    /* order: running (1, 2, 3 by start, then id), upcoming 4, finished 5 */
    CHECK_EQ(gcs_tourney_at(t, 0, t0 + DAY)->id, 1);
    CHECK_EQ(gcs_tourney_at(t, 2, t0 + DAY)->id, 3);
    CHECK_EQ(gcs_tourney_at(t, 3, t0 + DAY)->id, 4);
    CHECK_EQ(gcs_tourney_at(t, 4, t0 + DAY)->id, 5);
    CHECK(gcs_tourney_at(t, 5, t0) == NULL);
    CHECK_EQ(gcs_tourney_state(gcs_tourney_find(t, 4), t0), GCST_UPCOMING);
    CHECK_EQ(gcs_tourney_state(gcs_tourney_find(t, 1), t0), GCST_RUNNING);
    CHECK_EQ(gcs_tourney_state(gcs_tourney_find(t, 5), t0), GCST_FINISHED);

    /* before the start: nothing */
    gcs_tourney_report(t, "42318", "early", "game_total", 999, 0, 1, t0 - 1);
    CHECK_EQ(gcs_tourney_standings(t, 1, e, 8, &total), 0);
    /* best */
    gcs_tourney_report(t, "42318", "ann", "game_total", 500, 0, 1, t0 + 10);
    gcs_tourney_report(t, "42318", "ben", "game_total", 700, 0, 1, t0 + 20);
    gcs_tourney_report(t, "42318", "ann", "game_total", 400, 0, 0, t0 + 30); /* lower: stays 500 */
    gcs_tourney_report(t, "42318", "cid", "game_total", 700, 0, 1, t0 + 40); /* tie: ben was first */
    CHECK_EQ(gcs_tourney_standings(t, 1, e, 8, &total), 3);
    CHECK_EQ(total, 3);
    CHECK(!strcmp(e[0].user, "ben") && e[0].value == 700 && e[0].rank == 1);
    CHECK(!strcmp(e[1].user, "cid"));
    CHECK(!strcmp(e[2].user, "ann") && e[2].value == 500 && e[2].count == 2);
    CHECK_EQ(gcs_tourney_rank(t, 1, "ANN", NULL), 3);
    CHECK_EQ(gcs_tourney_rank(t, 1, "nobody", NULL), 0);
    /* low */
    gcs_tourney_report(t, "42318", "ann", "level_1", 90, 0, 1, t0 + 50);
    gcs_tourney_report(t, "42318", "ben", "level_1", 80, 0, 1, t0 + 60);
    gcs_tourney_report(t, "42318", "ann", "level_1", 70, 0, 0, t0 + 70);
    CHECK_EQ(gcs_tourney_standings(t, 3, e, 8, NULL), 2);
    CHECK(!strcmp(e[0].user, "ann") && e[0].value == 70);
    /* sum: per match values only (delta), not the absolute total */
    gcs_tourney_report(t, "1104", "wim", GCSS_SKILL_POINTS, 12, 1, 0, t0 + 80);
    gcs_tourney_report(t, "1104", "wim", GCSS_SKILL_POINTS, -4, 1, 0, t0 + 90);
    gcs_tourney_report(t, "1104", "wim", GCSS_SKILL_POINTS, 99, 0, 0, t0 + 95);
    gcs_tourney_report(t, "1104", "wim", GCSS_SKILL_POINTS, 5, 1, 0, t0 + 8 * DAY); /* after the end */
    CHECK_EQ(gcs_tourney_standings(t, 2, e, 8, NULL), 1);
    CHECK_EQ(e[0].value, 8);
    /* activities: new bests and won games, newest first */
    gcs_tourney_report(t, "1104", "wim", GCSS_WINS, 1, 1, 0, t0 + 100);
    gcs_tourney_report(t, "1104", "wim", GCSS_WINS, 0, 1, 0, t0 + 101); /* lost: no activity */
    CHECK_EQ(gcs_tourney_activities(t, a, 8), 7);
    CHECK(!strcmp(a[0].user, "wim") && !strcmp(a[0].stat, GCSS_WINS));
    CHECK(!strcmp(a[1].user, "ben") && !strcmp(a[1].stat, "level_1") && a[1].value == 80);
    gcs_tourney_destroy(t);

    /* the standings and activities survive a restart */
    t = gcs_tourney_create(DEFS, TDATA, quiet_log, NULL);
    CHECK_EQ(gcs_tourney_standings(t, 1, e, 8, NULL), 3);
    CHECK(!strcmp(e[0].user, "ben"));
    CHECK_EQ(gcs_tourney_activities(t, a, 8), 7);
    CHECK(!strcmp(a[0].stat, GCSS_WINS));

    /* the operator edits the list: read again on the next use */
    write_defs("7|42318|game_total|best|2026-12-01|2026-12-31|New cup with a longer line|x\n");
    gcs_tourney_refresh(t);
    CHECK_EQ(gcs_tourney_count(t), 1);
    CHECK(gcs_tourney_find(t, 1) == NULL);
    CHECK_EQ(gcs_tourney_find(t, 7)->start, t0);
    gcs_tourney_destroy(t);
    remove(DEFS);
    remove(TDATA);
}

static void test_pages(void)
{
    long t0 = gcs_tourney_parse_time("2026-12-01");
    gcs_scores *sc = gcs_scores_create(NULL, quiet_log, NULL);
    gcs_tourney *t;
    gcs_accounts *acc;
    gcs_pages p;
    gcs_scores_item it[4];
    char out[256], *d;
    xlist l;
    int i;

    remove(DEFS);
    remove(TDATA);
    remove(ACC);
    write_defs("1|42318|game_total|best|2026-12-01|2026-12-31|Ashen <Cup> & more|Best total of December.\n"
               "2|42318|game_total|best|2026-11-01|2026-11-30|November cup|\n"
               "3|1104|$ps_points|sum|2027-01-01|2027-01-08|Worms week|\n");
    t = gcs_tourney_create(DEFS, TDATA, quiet_log, NULL);
    acc = gcs_accounts_create(ACC, quiet_log, NULL);
    CHECK(gcs_accounts_login(acc, "Alice", "pw1", NULL) == GCSA_CREATED);
    CHECK(gcs_accounts_login(acc, "alfred", "pw2", NULL) == GCSA_CREATED);
    CHECK(gcs_accounts_login(acc, "Bob", "pw3", NULL) == GCSA_CREATED);
    gcs_scores_set_listener(sc, (gcs_scores_listener)NULL, NULL);
    p.scores = sc;
    p.tourney = t;
    p.accounts = acc;

    /* empty store */
    d = render(&p, "/community/ranking_list_x.jsp", "locale=1&start=0&disp=10&platform=device", "Alice", t0);
    CHECK_EQ(parse_list(d, &l), 0);
    CHECK_EQ(l.total, 0);
    free(d);
    d = render(&p, "/global/play/rankings/spotlight.jsp", NULL, "Alice", t0);
    CHECK(strstr(d, "Nobody is ranked yet") != NULL);
    free(d);

    /* scores: Ashen (best values), Worms (skill points and results) */
    for (i = 0; i < 12; i++) {
        char user[16], v[16];
        snprintf(user, sizeof(user), "p%02d", i);
        snprintf(v, sizeof(v), "%d", 100 + i * 10);
        gcs_scores_submit(sc, "42318", user, "game_total", v, t0 + i);
    }
    gcs_scores_submit(sc, "42318", "Alice", "game_total", "50", t0 + 100);
    gcs_scores_submit(sc, "42318", "Alice", "level_1", "7", t0 + 100);
    it[0].name = "$ps_points";
    it[0].value = "15";
    it[1].name = "win_lose";
    it[1].value = "1";
    it[2].name = "queryid";
    it[2].value = "5";
    gcs_scores_af_submit(sc, "1104", "Bob", "m1", it, 3, t0 + 200, out, sizeof(out));
    it[0].value = "-3";
    it[1].value = "0";
    gcs_scores_af_submit(sc, "1104", "Alice", "m1", it, 3, t0 + 200, out, sizeof(out));

    /* Rankings: the games */
    d = render(&p, "/community/ranking_list_x.jsp", "locale=1&start=0&disp=10", "Alice", t0);
    CHECK_EQ(parse_list(d, &l), 0);
    CHECK_EQ(l.total, 2);
    CHECK_EQ(l.n, 2);
    CHECK(!strcmp(l.label[0], "Ashen") && l.game[0] == 42318 && l.index[0] == 0);
    CHECK(!strcmp(l.label[1], "Worms World Party") && l.game[1] == 1104);
    free(d);
    /* paging */
    d = render(&p, "/community/ranking_list_x.jsp", "start=1&disp=1", "Alice", t0);
    CHECK_EQ(parse_list(d, &l), 0);
    CHECK(l.start == 1 && l.total == 2 && l.n == 1 && l.index[0] == 1);
    free(d);
    /* Find players */
    d = render(&p, "/community/ranking_list_x.jsp", "cmd=search&username=AL&start=0&disp=10", "Bob", t0);
    CHECK_EQ(parse_list(d, &l), 0);
    CHECK_EQ(l.total, 2);
    CHECK(!strcmp(l.user[0], "Alice") && !strcmp(l.user[1], "alfred"));
    free(d);

    /* a game's rankings: top 10, the user's own rank below */
    d = render(&p, "/global/play/rankings/post_game.jsp", "gameId=42318&locale=1", "Alice", t0);
    CHECK(strstr(d, "<title>Ashen</title>") != NULL);
    CHECK(strstr(d, "Total score</b> (13 players)") != NULL);
    CHECK(strstr(d, "1. p11 210<br/>") != NULL);
    CHECK(strstr(d, "10. p02 120<br/>") != NULL);
    CHECK(!strstr(d, "11. p01"));
    CHECK(strstr(d, "...<br/>13. Alice 50<br/>") != NULL);
    CHECK(strstr(d, "Level 1</b> (1 player)") != NULL);
    free(d);
    d = render(&p, "/global/play/rankings/post_game.jsp", "gameId=1104", "Alice", t0);
    CHECK(strstr(d, "Skill points</b> (2 players)<br/>1. Bob 15 (1-0-0)<br/>2. Alice -3 (0-1-0)") != NULL);
    free(d);
    /* View my rank / a player */
    d = render(&p, "/global/play/rankings/post_player.jsp", "locale=1", "Alice", t0);
    CHECK(strstr(d, "Total score: 50<br/>Rank 13 of 13") != NULL);
    CHECK(strstr(d, "Skill points: -3<br/>Rank 2 of 2<br/>Won 0, lost 1, drawn 0") != NULL);
    free(d);
    d = render(&p, "/global/play/rankings/post_player.jsp", "username=p11", "Alice", t0);
    CHECK(strstr(d, "Total score: 210<br/>Rank 1 of 13") != NULL);
    free(d);
    d = render(&p, "/global/play/rankings/post_player.jsp", "username=nobody%3Cx%3E", "Alice", t0);
    CHECK(strstr(d, "<title>nobody&lt;x&gt;</title>") != NULL);
    CHECK(strstr(d, "No rankings yet") != NULL);
    free(d);
    d = render(&p, "/global/play/rankings/spotlight.jsp", NULL, "Alice", t0);
    CHECK(strstr(d, "No. 1: p11 (210)") != NULL);
    CHECK(strstr(d, "No. 1: Bob (15)") != NULL);
    free(d);

    /* tournaments */
    gcs_tourney_report(t, "42318", "ann", "game_total", 500, 0, 1, t0 + 10);
    gcs_tourney_report(t, "42318", "Alice", "game_total", 300, 0, 1, t0 + 20);
    d = render(&p, "/community/tournament_list_x.jsp", "start=0&disp=10", "Alice", t0 + DAY);
    CHECK_EQ(parse_list(d, &l), 0);
    CHECK_EQ(l.total, 3);
    CHECK(!strcmp(l.label[0], "Ashen &lt;Cup&gt; &amp; more (running)") && l.content[0] == 1 && l.game[0] == 42318);
    CHECK(!strcmp(l.label[1], "Worms week (upcoming)") && l.content[1] == 3);
    CHECK(!strcmp(l.label[2], "November cup (finished)") && l.content[2] == 2);
    free(d);
    d = render(&p, "/global/play/tournaments/post_tournaments.jsp", "contentId=1&locale=1", "Alice", t0 + DAY);
    CHECK(strstr(d, "<title>Ashen &lt;Cup&gt; &amp; more</title>") != NULL);
    CHECK(strstr(d, "1 Dec 2026 00:00 UTC to 31 Dec 2026 00:00 UTC") != NULL);
    CHECK(strstr(d, "Running now") != NULL);
    CHECK(strstr(d, "Standings</b> (2 players)<br/>1. ann 500<br/>2. Alice 300<br/>") != NULL);
    free(d);
    d = render(&p, "/global/play/tournaments/post_tournaments.jsp", "contentId=99", "Alice", t0);
    CHECK(strstr(d, "does not exist") != NULL);
    free(d);
    d = render(&p, "/global/play/tournaments/news.jsp", NULL, "Alice", t0 + DAY);
    CHECK(strstr(d, "Leader: ann (500)") != NULL);
    CHECK(strstr(d, "Starts 1 Jan 2027") != NULL);
    CHECK(!strstr(d, "November cup"));
    free(d);
    d = render(&p, "/global/play/tournaments/winners.jsp", NULL, "Alice", t0 + DAY);
    CHECK(strstr(d, "November cup</b> (29 Nov 2026)<br/>Nobody took part.") != NULL);
    free(d);
    /* after the end: final standings and winners */
    d = render(&p, "/global/play/activities/winners.jsp", NULL, "Alice", t0 + 40 * DAY);
    CHECK(strstr(d, "Ashen &lt;Cup&gt; &amp; more</b> (30 Dec 2026)<br/>1. ann 500<br/>2. Alice 300<br/>") != NULL);
    free(d);

    /* activities */
    d = render(&p, "/community/activity_list_x.jsp", NULL, "Alice", t0);
    CHECK_EQ(parse_list(d, &l), 0);
    CHECK_EQ(l.total, 2);
    CHECK(!strcmp(l.user[0], "Alice") && l.content[0] == 1);
    CHECK(strstr(l.label[0], "Alice: new best in Ashen, Total score 300") != NULL);
    free(d);
    d = render(&p, "/global/play/activities/post_activity.jsp", "contentId=2", "Alice", t0);
    CHECK(strstr(d, "ann: new best in Ashen, Total score 500") != NULL);
    free(d);
    d = render(&p, "/global/play/activities/news.jsp", NULL, "Alice", t0);
    CHECK(strstr(d, "1 Dec 2026 00:00 UTC<br/>Alice: new best") != NULL);
    free(d);

    /* games */
    d = render(&p, "/community/game_list_x.jsp", "start=0&disp=10", "Alice", t0);
    CHECK_EQ(parse_list(d, &l), 0);
    CHECK_EQ(l.total, 4);
    CHECK(!strcmp(l.label[0], "Pocket Kingdom: Own the World") && l.game[0] == 1);
    free(d);
    d = render(&p, "/global/play/games/post_game.jsp", "gameId=1104", "Alice", t0);
    CHECK(strstr(d, "<title>Worms World Party</title>") != NULL);
    CHECK(strstr(d, "1. Bob 15") != NULL);
    free(d);

    /* not generated here */
    {
        size_t len;
        const char *type;
        CHECK(gcs_pages_render(&p, "/n-gage/device/community/skin_list_x.jsp", NULL, "", t0, &len, &type) == NULL);
        CHECK(gcs_pages_render(&p, "/n-gage/servlets/msg", NULL, "", t0, &len, &type) == NULL);
    }

    gcs_accounts_destroy(acc);
    gcs_tourney_destroy(t);
    gcs_scores_destroy(sc);
    remove(DEFS);
    remove(TDATA);
    remove(ACC);
}

/* ---- over HTTP, after a SegaChat score report ---- */

typedef struct
{
    char rx[65536];
    size_t len;
    int fin, reset;
} fake_sock;

static void io_tcp_send(void *ctx, void *sock, const gc_u8 *d, size_t len)
{
    fake_sock *s = (fake_sock *)sock;
    (void)ctx;
    if (s->len + len < sizeof(s->rx)) {
        memcpy(s->rx + s->len, d, len);
        s->len += len;
        s->rx[s->len] = 0;
    }
}

static void io_tcp_close(void *ctx, void *sock, int reset)
{
    fake_sock *s = (fake_sock *)sock;
    (void)ctx;
    if (reset) {
        s->reset = 1;
    } else {
        s->fin = 1;
    }
}

static void io_udp_send(void *ctx, gc_u16 service, const gc_u8 ip[4], gc_u16 port, const gc_u8 *d, size_t len)
{
    (void)ctx, (void)service, (void)ip, (void)port, (void)d, (void)len;
}

static void s_write(void *c, gcs_conn *k, const gc_u8 *d, size_t n)
{
    (void)c, (void)k, (void)d, (void)n;
}

static void s_close(void *c, gcs_conn *k)
{
    (void)c, (void)k;
}

static void test_http(void)
{
    static const gc_u8 ip[4] = { 203, 0, 113, 9 };
    gcs_config cfg;
    gcs_io sio;
    gcs_direct_io dio;
    gcs_core *core;
    gcs_direct *d;
    fake_sock s, h;
    const char *req;
    char tbuf[64];
    time_t now = time(NULL);
    struct tm *tm = gmtime(&now);
    char defs[300];

    remove(ACC);
    remove(DEFS);
    remove(TDATA);
    /* a tournament running now */
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d", tm);
    snprintf(defs, sizeof(defs), "1|42318|game_total|best|%s|9999-01-01|Forever cup|Never ends.\n", tbuf);
    write_defs(defs);
    gcs_default_config(&cfg);
    strcpy(cfg.arena_accounts, ACC);
    strcpy(cfg.tournaments, DEFS);
    strcpy(cfg.tournament_data, TDATA);
    cfg.scores_data[0] = 0;
    cfg.roster_data[0] = 0;
    cfg.pk_data[0] = 0;
    cfg.rifts_data[0] = 0;
    cfg.habbo_data[0] = 0;
    memset(&sio, 0, sizeof(sio));
    sio.write = s_write;
    sio.close = s_close;
    core = gcs_core_create(&cfg, &sio, NULL, 1);
    CHECK(core != NULL);
    memset(&dio, 0, sizeof(dio));
    dio.tcp_send = io_tcp_send;
    dio.tcp_close = io_tcp_close;
    dio.udp_send = io_udp_send;
    d = gcs_direct_create(core, &dio, NULL);

    /* Ashen reports a score over SegaChat */
    memset(&s, 0, sizeof(s));
    CHECK_EQ(gcs_direct_tcp_open(d, &s, ip, 5000, GCSD_XMPP, 1000), 0);
    req = "<stream:stream to='ngage-auth' xmlns='jabber:client' xmlns:stream='http://etherx.jabber.org/streams'>"
          "<iq type='set' id='segachat_auth'><query xmlns='jabber:iq:auth'><username>Ash</username>"
          "<password>pw</password><resource>segachat</resource></query></iq>"
          "<message to='reporter@ngage-auth' id='segachat_send_event' event_type='submit' game_class_id='42318' "
          "snap_name='Ash' client_time='0' match_id=''><item name='game_total'><value>4321</value></item></message>";
    gcs_direct_tcp_input(d, &s, (const gc_u8 *)req, strlen(req), 1000);
    CHECK(strstr(s.rx, "id='segachat_auth'/>") != NULL);

    /* the pages show it */
    memset(&h, 0, sizeof(h));
    CHECK_EQ(gcs_direct_tcp_open(d, &h, ip, 5001, GCSD_HTTP, 1000), 0);
    req = "GET /n-gage/device/global/play/rankings/post_game.jsp;jsessionid=x?gameId=42318&locale=1&platform=device "
          "HTTP/1.1\r\nHost: arena.n-gage.com\r\n\r\n";
    gcs_direct_tcp_input(d, &h, (const gc_u8 *)req, strlen(req), 1000);
    CHECK(strstr(h.rx, "200 OK") != NULL);
    CHECK(strstr(h.rx, "application/xhtml+xml") != NULL);
    CHECK(strstr(h.rx, "1. Ash 4321<br/>") != NULL);
    gcs_direct_tcp_gone(d, &h);
    memset(&h, 0, sizeof(h));
    CHECK_EQ(gcs_direct_tcp_open(d, &h, ip, 5002, GCSD_HTTP, 1000), 0);
    req = "GET /n-gage/device/community/tournament_list_x.jsp?locale=1&start=0&disp=10&platform=device HTTP/1.1\r\n"
          "Host: arena.n-gage.com\r\n\r\n";
    gcs_direct_tcp_input(d, &h, (const gc_u8 *)req, strlen(req), 1000);
    CHECK(strstr(h.rx, "text/xml") != NULL);
    CHECK(strstr(h.rx, ">Forever cup (running)</itemNode>") != NULL);
    gcs_direct_tcp_gone(d, &h);
    memset(&h, 0, sizeof(h));
    CHECK_EQ(gcs_direct_tcp_open(d, &h, ip, 5003, GCSD_HTTP, 1000), 0);
    req = "GET /n-gage/device/global/play/tournaments/post_tournaments.jsp?contentId=1 HTTP/1.1\r\n\r\n";
    gcs_direct_tcp_input(d, &h, (const gc_u8 *)req, strlen(req), 1000);
    CHECK(strstr(h.rx, "1. Ash 4321<br/>") != NULL);

    gcs_direct_tcp_gone(d, &s);
    gcs_direct_tcp_gone(d, &h);
    gcs_direct_destroy(d);
    gcs_core_destroy(core);
    remove(ACC);
    remove(DEFS);
    remove(TDATA);
}

int main(void)
{
    RUN(test_time);
    RUN(test_tourney);
    RUN(test_pages);
    RUN(test_http);
    TEST_MAIN_END();
}
