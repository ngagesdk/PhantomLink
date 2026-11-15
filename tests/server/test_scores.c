/* SegaChat score service (server/src/gcs_scores.c) for Ashen's online
 * leaderboards.
 *
 * The answers are decoded the way Ashen does it (6R21.APP, docs/ARENA.md):
 * lines split at '\n' and '|' by the same rules, numbers read the same
 * way, the same field counts required and the same limits (1024 bytes of
 * answer, at most 10 rows on the stack).  The second part runs a SegaChat
 * session through gcs_direct and gcs_native with the messages as
 * sc_lib.dll writes them.
 *
 * SPDX-License-Identifier: MIT */
#include "gc_test.h"
#include "gcs_accounts.h"
#include "gcs_core.h"
#include "gcs_direct.h"
#include "gcs_native.h"
#include "gcs_scores.h"

#include <stdio.h>
#include <stdlib.h>

#define ACC   "test_scores_accounts.txt"
#define SDATA "test_scores_data.txt"
#define ASHEN "42318"

static void quiet_log(void *ctx, int level, const char *line)
{
    (void)ctx;
    if (level <= 1) {
        printf("  log %d: %s\n", level, line);
    }
}

/* ---- Ashen's decoder ---- */

/* one line into fields (6R21 0x9c354): returns the field count */
static int a_line(char *buf, int *off, char **f, int max)
{
    int n = 0;
    char *p = buf + *off;
    if (*p == '\n' || *p == 0) {
        return 0;
    }
    while (n < max) {
        f[n++] = p;
        while (*p != '\n' && *p != '|' && *p) {
            p++;
        }
        if (*p == '|') {
            *p++ = 0;
        }
        if (*p == '\n' || *p == 0) {
            break;
        }
    }
    if (*p) {
        *p++ = 0;
    }
    *off = (int)(p - buf);
    return n;
}

/* digits up to the end or a '.', others skipped (6R21 0x9bfe0) */
static long a_num(const char *s)
{
    long v = 0, m = 1;
    int k = 0;
    while (s[k] && s[k] != '.') {
        k++;
    }
    while (k-- > 0) {
        if (s[k] >= '0' && s[k] <= '9') {
            v += (s[k] - '0') * m;
            m *= 10;
        }
    }
    return v;
}

typedef struct
{
    int error; /* 0 ok, 1 format, 2 status (msg) */
    char msg[256];
    int count;
    long rank[10], score[10];
    char name[10][32];
} a_board;

static void a_decode(const char *csv, a_board *b)
{
    char buf[1024], *f[16];
    int off = 0, n, i;
    memset(b, 0, sizeof(*b));
    CHECK(strlen(csv) < sizeof(buf)); /* the game copies at most 1023 bytes */
    strncpy(buf, csv, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    if (a_line(buf, &off, f, 16) != 1) {
        b->error = 1;
        return;
    }
    {
        long status = a_num(f[0]);
        if (a_line(buf, &off, f, 16) != 1) {
            b->error = 1;
            return;
        }
        if (status != 0) {
            b->error = 2;
            strncpy(b->msg, f[0], sizeof(b->msg) - 1);
            return;
        }
    }
    if (a_line(buf, &off, f, 16) != 7) {
        b->error = 1;
        return;
    }
    b->count = (int)a_num(f[3]);
    CHECK(b->count <= 10); /* more would overrun the game's stack */
    for (i = 0; i < b->count && i < 10; i++) {
        n = a_line(buf, &off, f, 16);
        if (n != 4) {
            b->error = 1;
            return;
        }
        b->rank[i] = a_num(f[1]);
        b->score[i] = a_num(f[2]);
        strncpy(b->name[i], f[0], 31);
    }
}

static const char *itemlist(const char *stat, int limit)
{
    static char r[512];
    snprintf(r, sizeof(r),
             "<itemlist><item name=\"queryid\" value=\"3dabc\" /><item name=\"board\" value=\"HIGHSCORES\" />"
             "<item name=\"stat\" value=\"%s\" /><item name=\"offset\" value=\"0\" /><item name=\"limit\" "
             "value=\"%d\" /><item name=\"periodicity\" value=\"alltime\" /><item name=\"ordering\" "
             "value=\"natural\" /><item name=\"format\" value=\"csv\" /></itemlist>",
             stat, limit);
    return r;
}

static void test_store(void)
{
    gcs_scores *s;
    gcs_scores_stats st;
    char csv[GCSS_MAX_CSV + 24];
    a_board b;
    int i;

    remove(SDATA);
    s = gcs_scores_create(SDATA, quiet_log, NULL);
    CHECK(s != NULL);

    /* empty board: a valid answer with no rows */
    CHECK_EQ(gcs_scores_query(s, ASHEN, itemlist("level_1", 7), csv, sizeof(csv)), 0);
    a_decode(csv, &b);
    CHECK_EQ(b.error, 0);
    CHECK_EQ(b.count, 0);

    /* best value per user; 0 = level not played */
    CHECK_EQ(gcs_scores_submit(s, ASHEN, "Ann", "level_1", "1200", 100), 1);
    CHECK_EQ(gcs_scores_submit(s, ASHEN, "Ann", "level_1", "900", 101), 0);
    CHECK_EQ(gcs_scores_submit(s, ASHEN, "ann", "level_1", "1500", 102), 1); /* names ignore case */
    CHECK_EQ(gcs_scores_submit(s, ASHEN, "Ann", "level_2", "0", 102), 0);
    CHECK_EQ(gcs_scores_submit(s, ASHEN, "Bob", "level_1", "1500", 103), 1);
    CHECK_EQ(gcs_scores_submit(s, ASHEN, "Cy", "level_1", "800", 104), 1);
    CHECK_EQ(gcs_scores_submit(s, "99999", "Dee", "level_1", "5000", 105), 1); /* another game */
    CHECK_EQ(gcs_scores_submit(s, ASHEN, "Eve", "level_1", "12x", 105), -1);
    CHECK_EQ(gcs_scores_submit(s, ASHEN, "Eve", "le|vel", "5", 105), -1);
    CHECK_EQ(gcs_scores_submit(s, ASHEN, "Eve", "level_1", "", 105), -1);
    gcs_scores_commit(s);

    CHECK_EQ(gcs_scores_query(s, ASHEN, itemlist("level_1", 7), csv, sizeof(csv)), 3);
    a_decode(csv, &b);
    CHECK_EQ(b.error, 0);
    CHECK_EQ(b.count, 3);
    CHECK(!strcmp(b.name[0], "Ann")); /* same value: the earlier one first */
    CHECK_EQ(b.score[0], 1500);
    CHECK_EQ(b.rank[0], 1);
    CHECK(!strcmp(b.name[1], "Bob"));
    CHECK_EQ(b.rank[1], 1); /* and the same rank */
    CHECK(!strcmp(b.name[2], "Cy"));
    CHECK_EQ(b.rank[2], 3);
    CHECK_EQ(b.score[2], 800);

    /* limit; never more than 10 rows, whatever the client asks */
    CHECK_EQ(gcs_scores_query(s, ASHEN, itemlist("level_1", 1), csv, sizeof(csv)), 1);
    for (i = 0; i < 30; i++) {
        char name[16], val[16];
        snprintf(name, sizeof(name), "player%02d", i);
        snprintf(val, sizeof(val), "%d", 1000000 + i);
        CHECK_EQ(gcs_scores_submit(s, ASHEN, name, "game_total", val, 200 + i), 1);
    }
    CHECK_EQ(gcs_scores_query(s, ASHEN, itemlist("game_total", 50), csv, sizeof(csv)), GCSS_MAX_ROWS);
    a_decode(csv, &b);
    CHECK_EQ(b.error, 0);
    CHECK_EQ(b.count, 10);
    CHECK(!strcmp(b.name[0], "player29"));
    CHECK_EQ(b.score[0], 1000029);
    CHECK_EQ(b.rank[9], 10);
    CHECK_EQ(gcs_scores_query(s, ASHEN, itemlist("game_total", 0), csv, sizeof(csv)), 1);

    /* ascending order */
    CHECK_EQ(gcs_scores_query(s, ASHEN,
                              "<itemlist><item name=\"stat\" value=\"level_1\" /><item name=\"ordering\" "
                              "value=\"reverse\" /></itemlist>",
                              csv, sizeof(csv)),
             3);
    a_decode(csv, &b);
    CHECK(!strcmp(b.name[0], "Cy"));

    /* errors: the game shows the message line */
    CHECK_EQ(gcs_scores_query(s, ASHEN, "<itemlist></itemlist>", csv, sizeof(csv)), -1);
    a_decode(csv, &b);
    CHECK_EQ(b.error, 2);
    CHECK(!strcmp(b.msg, "Unknown statistic"));
    CHECK_EQ(gcs_scores_query(s, ASHEN,
                              "<itemlist><item name=\"stat\" value=\"level_1\" /><item name=\"format\" "
                              "value=\"xml\" /></itemlist>",
                              csv, sizeof(csv)),
             -1);

    /* the values survive a restart */
    gcs_scores_get_stats(s, &st);
    CHECK_EQ(st.records, 34);
    gcs_scores_destroy(s);
    s = gcs_scores_create(SDATA, quiet_log, NULL);
    gcs_scores_get_stats(s, &st);
    CHECK_EQ(st.records, 34);
    CHECK_EQ(gcs_scores_query(s, ASHEN, itemlist("level_1", 7), csv, sizeof(csv)), 3);
    a_decode(csv, &b);
    CHECK_EQ(b.score[1], 1500);
    gcs_scores_destroy(s);
    remove(SDATA);
}

/* ---- a SegaChat session through gcs_direct and gcs_native ---- */

typedef struct
{
    char rx[16384];
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

/* sends text (in two pieces) and returns what came back */
static const char *say(gcs_direct *d, fake_sock *s, const char *text)
{
    size_t k = strlen(text), half = k / 2;
    s->len = 0;
    s->rx[0] = 0;
    gcs_direct_tcp_input(d, s, (const gc_u8 *)text, half, 1000);
    gcs_direct_tcp_input(d, s, (const gc_u8 *)text + half, k - half, 1000);
    return s->rx;
}

static void xml_unescape(char *s)
{
    static const char *const ents[] = { "&lt;", "<", "&gt;", ">", "&amp;", "&", "&quot;", "\"", "&apos;", "'" };
    char *w = s;
    while (*s) {
        int i, hit = 0;
        for (i = 0; i < 10 && *s == '&'; i += 2) {
            size_t k = strlen(ents[i]);
            if (!strncmp(s, ents[i], k)) {
                *w++ = ents[i + 1][0];
                s += k;
                hit = 1;
                break;
            }
        }
        if (!hit) {
            *w++ = *s++;
        }
    }
    *w = 0;
}

/* the answer as sc_lib takes it apart: a message without a type
 * attribute, id segachat_retrieve_resp, retrieve/response text */
static int response_of(const char *rx, char *csv, size_t cap)
{
    const char *m = strstr(rx, "<message"), *gt, *a, *e;
    size_t k;
    if (!m || !(gt = strchr(m, '>'))) {
        return -1;
    }
    for (a = m; a < gt; a++) {
        if (!strncmp(a, " type=", 6)) {
            return -1; /* sc_lib drops typed messages */
        }
    }
    if (!strstr(m, "id='segachat_retrieve_resp'") || !strstr(m, "game_class_id='" ASHEN "'") ||
        !strstr(m, "event_type='topn'")) {
        return -1;
    }
    a = strstr(gt, "<retrieve><response>");
    e = strstr(gt, "</response></retrieve></message>");
    if (!a || !e || e < a) {
        return -1;
    }
    a += strlen("<retrieve><response>");
    k = (size_t)(e - a);
    if (k >= cap) {
        return -1;
    }
    memcpy(csv, a, k);
    csv[k] = 0;
    xml_unescape(csv);
    return 0;
}

static void query_xml(char *out, size_t cap, const char *stat)
{
    const char *il = itemlist(stat, 7);
    size_t k = (size_t)snprintf(out, cap,
                                "<message to='retrieval@ngage-auth' id='segachat_retrieve_req' event_type='topn' "
                                "game_class_id='" ASHEN "' snap_name='Ash'><retrieve><request>");
    for (; *il && k + 8 < cap; il++) {
        const char *e = *il == '<' ? "&lt;" : *il == '>' ? "&gt;"
                                          : *il == '"'   ? "&quot;"
                                                         : NULL;
        if (e) {
            k += (size_t)snprintf(out + k, cap - k, "%s", e);
        } else {
            out[k++] = *il;
        }
    }
    snprintf(out + k, cap - k, "</request></retrieve></message>");
}

static void test_session(void)
{
    static const gc_u8 ip[4] = { 203, 0, 113, 77 };
    gcs_config cfg;
    gcs_io sio;
    gcs_direct_io dio;
    gcs_core *core;
    gcs_direct *d;
    gcs_accounts *acc;
    fake_sock s;
    const char *r;
    char q[2048], csv[1100];
    a_board b;
    gcs_native_stats nst;

    remove(ACC);
    remove(SDATA);
    gcs_default_config(&cfg);
    strcpy(cfg.arena_accounts, ACC);
    strcpy(cfg.scores_data, SDATA);
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
    CHECK(d != NULL);
    acc = gcs_native_accounts(gcs_core_native(core));

    memset(&s, 0, sizeof(s));
    CHECK_EQ(gcs_direct_tcp_open(d, &s, ip, 3000, GCSD_XMPP, 1000), 0);
    r = say(d, &s,
            "<?xml version='1.0'?><stream:stream to='ngage-auth' xmlns='jabber:client' "
            "xmlns:stream='http://etherx.jabber.org/streams'>");
    CHECK(strstr(r, "<stream:stream") != NULL);
    r = say(d, &s, "<iq type='get' id='segachat_query_username'><query xmlns='jabber:iq:auth'><username>Ash</username></query></iq>");
    CHECK(strstr(r, "type='result'") != NULL);

    /* a report before the login is ignored */
    say(d, &s,
        "<message to='reporter@ngage-auth' id='segachat_send_event' event_type='submit' game_class_id='" ASHEN
        "' match_id='' snap_name='Ash' client_time='1'><item name='level_1'><value>99999</value></item></message>");

    /* Ashen has no Community login: the first SegaChat login creates the account */
    CHECK_EQ(gcs_accounts_exists(acc, "Ash", NULL), 0);
    r = say(d, &s,
            "<iq type='set' id='segachat_auth'><query xmlns='jabber:iq:auth'><username>Ash</username>"
            "<password>ashpw</password><resource>segachat</resource></query></iq>");
    CHECK(strstr(r, "<iq type='result' id='segachat_auth'/>") != NULL);
    CHECK_EQ(gcs_accounts_check(acc, "ash", "ashpw"), GCSA_OK);

    /* a report as sc_lib writes it: nine items, unplayed levels 0 */
    r = say(d, &s,
            "<message to='reporter@ngage-auth' id='segachat_send_event' event_type='submit' game_class_id='" ASHEN
            "' match_id='' snap_name='Ash' client_time='1131012345'>"
            "<item name='level_1'><value>4321</value></item><item name='level_2'><value>1234</value></item>"
            "<item name='level_3'><value>0</value></item><item name='level_4'><value>0</value></item>"
            "<item name='level_5'><value>0</value></item><item name='level_6'><value>0</value></item>"
            "<item name='level_7'><value>0</value></item><item name='level_8'><value>0</value></item>"
            "<item name='game_total'><value>5555</value></item></message>");
    CHECK_EQ(s.len, 0); /* no answer */

    /* someone else's score, straight into the store */
    CHECK_EQ(gcs_scores_submit(gcs_native_scores(gcs_core_native(core)), ASHEN, "Zed", "level_1", "9000", 5), 1);

    query_xml(q, sizeof(q), "level_1");
    r = say(d, &s, q);
    CHECK_EQ(response_of(r, csv, sizeof(csv)), 0);
    a_decode(csv, &b);
    CHECK_EQ(b.error, 0);
    CHECK_EQ(b.count, 2);
    CHECK(!strcmp(b.name[0], "Zed"));
    CHECK(!strcmp(b.name[1], "Ash"));
    CHECK_EQ(b.score[1], 4321);
    CHECK_EQ(b.rank[1], 2);

    query_xml(q, sizeof(q), "level_3");
    r = say(d, &s, q);
    CHECK_EQ(response_of(r, csv, sizeof(csv)), 0);
    a_decode(csv, &b);
    CHECK_EQ(b.error, 0);
    CHECK_EQ(b.count, 0);

    query_xml(q, sizeof(q), "game_total");
    r = say(d, &s, q);
    CHECK_EQ(response_of(r, csv, sizeof(csv)), 0);
    a_decode(csv, &b);
    CHECK_EQ(b.count, 1);
    CHECK_EQ(b.score[0], 5555);

    gcs_native_get_stats(gcs_core_native(core), &nst);
    CHECK_EQ(nst.score_reports, 1);
    CHECK_EQ(nst.score_queries, 3);

    /* wrong password: refused, no second account */
    gcs_direct_tcp_gone(d, &s);
    memset(&s, 0, sizeof(s));
    CHECK_EQ(gcs_direct_tcp_open(d, &s, ip, 3001, GCSD_XMPP, 1000), 0);
    say(d, &s, "<stream:stream to='ngage-auth' xmlns='jabber:client' xmlns:stream='http://etherx.jabber.org/streams'>");
    r = say(d, &s,
            "<iq type='set' id='segachat_auth'><query xmlns='jabber:iq:auth'><username>ash</username>"
            "<password>wrong</password><resource>segachat</resource></query></iq>");
    CHECK(strstr(r, "code='401'") != NULL);
    gcs_direct_tcp_gone(d, &s);

    gcs_direct_destroy(d);
    gcs_core_destroy(core);

    /* kept in the data file */
    {
        gcs_scores *sc = gcs_scores_create(SDATA, quiet_log, NULL);
        gcs_scores_stats st;
        gcs_scores_get_stats(sc, &st);
        CHECK_EQ(st.records, 4); /* Ash: level_1, level_2, game_total; Zed: level_1 */
        gcs_scores_destroy(sc);
    }
    remove(ACC);
    remove(SDATA);
}

int main(void)
{
    RUN(test_store);
    RUN(test_session);
    TEST_MAIN_END();
}
