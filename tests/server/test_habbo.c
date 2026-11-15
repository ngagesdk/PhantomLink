/* Habbo Console back end (server/src/gcs_habbo.c) for Habbo Islands.
 *
 * The answers are decoded the way the game does it (SULAKEUI.DLL,
 * docs/HABBO.md): the same field order and the same minimum lengths, so
 * a format error here would be "Syntax error in server response!" on the
 * N-Gage.  The second part sends real HTTP requests through gcs_direct
 * and gcs_native, as a GPRS client would.
 *
 * SPDX-License-Identifier: MIT */
#include "gc_test.h"
#include "gcs_accounts.h"
#include "gcs_core.h"
#include "gcs_direct.h"
#include "gcs_habbo.h"
#include "gcs_native.h"

#include <stdio.h>
#include <stdlib.h>

#define ACC   "test_habbo_accounts.txt"
#define HDATA "test_habbo_data.txt"

static void quiet_log(void *ctx, int level, const char *line)
{
    (void)ctx;
    if (level <= 1) {
        printf("  log %d: %s\n", level, line);
    }
}

/* ---- the game's decoder ---- */

typedef struct
{
    unsigned id;
    char name[64], text[128], last[64];
    int online;
} d_user;

typedef struct
{
    unsigned id, from;
    char name[64], text[300], when[64];
} d_msg;

typedef struct
{
    d_user friends[16];
    int nf;
    d_msg msgs[40];
    int nm;
    d_user reqs[16];
    int nr;
    int maxlen;
} d_lists;

static unsigned be16(const unsigned char *p)
{
    return (unsigned)(p[0] << 8 | p[1]);
}

static unsigned rd_long(const unsigned char *p)
{
    return (unsigned)p[4] << 24 | (unsigned)p[5] << 16 | (unsigned)p[6] << 8 | p[7];
}

static void rd_str(const unsigned char *p, unsigned len, char *out, size_t cap)
{
    size_t k = len < cap - 1 ? len : cap - 1;
    memcpy(out, p, k);
    out[k] = 0;
}

/* friend / user record (game: min 13 bytes); returns the bytes used or -1 */
static int dec_user(const unsigned char *p, size_t len, d_user *u)
{
    size_t at = 8;
    unsigned k;
    if (len < 13) {
        return -1;
    }
    memset(u, 0, sizeof(*u));
    u->id = rd_long(p);
    k = be16(p + at);
    at += 2;
    if (len < 13 + k) {
        return -1;
    }
    rd_str(p + at, k, u->name, sizeof(u->name));
    at += k;
    {
        unsigned k2 = be16(p + at);
        at += 2;
        if (len < 13 + k + k2) {
            return -1;
        }
        rd_str(p + at, k2, u->text, sizeof(u->text));
        at += k2;
        u->online = p[at++] != 0;
        if (!u->online) {
            unsigned k3;
            if (len < 15 + k + k2) {
                return -1;
            }
            k3 = be16(p + at);
            at += 2;
            if (len < 15 + k + k2 + k3) {
                return -1;
            }
            rd_str(p + at, k3, u->last, sizeof(u->last));
            at += k3;
        } else {
            strcpy(u->last, "xxx");
        }
    }
    return (int)at;
}

/* message record (game: min 22 bytes) */
static int dec_msg(const unsigned char *p, size_t len, d_msg *m)
{
    size_t at = 16, need = 22;
    unsigned k;
    if (len < need) {
        return -1;
    }
    m->id = rd_long(p);
    m->from = rd_long(p + 8);
    k = be16(p + at);
    at += 2;
    need += k;
    if (len < need) {
        return -1;
    }
    rd_str(p + at, k, m->name, sizeof(m->name));
    at += k;
    k = be16(p + at);
    at += 2;
    need += k;
    if (len < need) {
        return -1;
    }
    rd_str(p + at, k, m->text, sizeof(m->text));
    at += k;
    k = be16(p + at);
    at += 2;
    if (len < need + k) {
        return -1;
    }
    rd_str(p + at, k, m->when, sizeof(m->when));
    at += k;
    return (int)at;
}

/* friend request record (game: min 10 bytes) */
static int dec_req(const unsigned char *p, size_t len, d_user *u)
{
    unsigned k;
    if (len < 10) {
        return -1;
    }
    memset(u, 0, sizeof(*u));
    u->id = rd_long(p);
    k = be16(p + 8);
    if (len < 10 + k) {
        return -1;
    }
    rd_str(p + 10, k, u->name, sizeof(u->name));
    return (int)(10 + k);
}

/* MESSENGERINIT (full) / MESSENGER_UPDATE: 0 ok, -1 "syntax error" */
static int dec_lists(const unsigned char *p, size_t len, int full, d_lists *l)
{
    size_t at = 0;
    unsigned n, i;
    memset(l, 0, sizeof(*l));
    l->maxlen = -1;
    if (len < (size_t)(full ? 8 : 6)) {
        return -1;
    }
    if (full) {
        unsigned k = be16(p);
        if (len < 2 + k) {
            return -1;
        }
        at = 2 + k;
    }
    if (len < at + 2) {
        return -1;
    }
    n = be16(p + at);
    at += 2;
    for (i = 0; i < n; i++) {
        d_user u;
        int k = at < len ? dec_user(p + at, len - at, &u) : -1;
        if (k < 0) {
            return -1;
        }
        if (l->nf < 16) {
            l->friends[l->nf++] = u;
        }
        at += (size_t)k;
    }
    if (len < at + 2) {
        return -1;
    }
    n = be16(p + at);
    at += 2;
    for (i = 0; i < n; i++) {
        d_msg m;
        int k = at < len ? dec_msg(p + at, len - at, &m) : -1;
        if (k < 0) {
            return -1;
        }
        if (l->nm < 40) {
            l->msgs[l->nm++] = m;
        }
        at += (size_t)k;
    }
    if (len < at + 2) {
        return -1;
    }
    n = be16(p + at);
    at += 2;
    for (i = 0; i < n; i++) {
        d_user u;
        int k = at < len ? dec_req(p + at, len - at, &u) : -1;
        if (k < 0) {
            return -1;
        }
        if (l->nr < 16) {
            l->reqs[l->nr++] = u;
        }
        at += (size_t)k;
    }
    if (len >= at + 2) {
        l->maxlen = (int)be16(p + at);
    }
    return 0;
}

/* ---- requests ---- */

static gcs_habbo *h;
static gcs_accounts *acc;
static long t = 1700000000L;
static gcs_habbo_reply rep;
static int peer_a, peer_b, peer_c; /* addresses only: peer keys */

static void post(void *key, const char *servlet, const char *sid, const char *body)
{
    free(rep.body);
    CHECK_EQ(gcs_habbo_http(h, key, servlet, "builder.sulake.com:8080", sid, body, strlen(body), t, &rep), 0);
    CHECK_EQ(rep.status, 200);
}

static int is_text(const char *s)
{
    return rep.len == strlen(s) && !memcmp(rep.body, s, rep.len);
}

static void login_as(void *key, const char *user, const char *pass, char *sid)
{
    char body[200];
    snprintf(body, sizeof(body), "user=%s&pass=%s", user, pass);
    post(key, "/habbo/AuthenticationServlet", "", body);
    CHECK(is_text("0"));
    CHECK(rep.cookie[0] != 0);
    strcpy(sid, rep.cookie);
}

static void init_lists(void *key, const char *sid, int full, d_lists *l)
{
    post(key, "/habbo/MessengerServlet", sid, full ? "m=12" : "m=15");
    if (dec_lists(rep.body, rep.len, full, l) != 0) {
        CHECK(!"answer not decodable");
        fprintf(stderr, "  %s, %u bytes: %.*s\n", full ? "init" : "update", (unsigned)rep.len, (int)rep.len,
                (const char *)rep.body);
    }
}

static void test_console(void)
{
    char sa[48], sb[48], sc[48], body[300];
    d_lists l;
    d_user u;
    unsigned ann_id, bob_id;
    gcs_habbo_stats st;

    remove(ACC);
    remove(HDATA);
    acc = gcs_accounts_create(ACC, quiet_log, NULL);
    CHECK(acc != NULL);
    /* Bob already plays Arena games: IMEI bound account */
    CHECK_EQ(gcs_accounts_login(acc, "Bob", "bobpw", "350000000000021"), GCSA_CREATED);
    CHECK_EQ(gcs_accounts_set(acc, "Bob", "quote", "Hello from the Arena"), 0);
    h = gcs_habbo_create(acc, HDATA, quiet_log, NULL);
    CHECK(h != NULL);

    /* locator: one server, servlets on the same host and port */
    free(rep.body);
    CHECK_EQ(gcs_habbo_http(h, &peer_a, "/test/locator.html", "builder.sulake.com:8080", "", NULL, 0, t, &rep), 0);
    CHECK(is_text("1;PhantomLink;http://builder.sulake.com:8080/habbo/\r\n"));
    free(rep.body);
    CHECK_EQ(gcs_habbo_http(h, &peer_a, "/test/locator.html", "", "", NULL, 0, t, &rep), 0);
    CHECK(is_text("1;PhantomLink;http://builder.sulake.com:8080/habbo/\r\n"));
    free(rep.body);
    CHECK_EQ(gcs_habbo_http(h, &peer_a, "/favicon.ico", "", "", NULL, 0, t, &rep), 0);
    CHECK_EQ(rep.status, 404);

    /* not logged in: "2" (the game logs in and repeats) */
    post(&peer_a, "/habbo/MessengerServlet", "", "m=12");
    CHECK(is_text("2"));
    post(&peer_a, "/habbo/MessengerServlet", "", "m=0");
    CHECK(is_text("0"));

    /* first login creates the shared account; a new password is refused */
    login_as(&peer_a, "Ann", "ann%20pw", sa);
    CHECK_EQ(gcs_accounts_check(acc, "ann", "ann pw"), GCSA_OK);
    post(&peer_c, "/habbo/AuthenticationServlet", "", "user=Ann&pass=wrong");
    CHECK(is_text("1") && !rep.cookie[0]);
    post(&peer_c, "/habbo/AuthenticationServlet", "", "user=&pass=x");
    CHECK(is_text("1"));
    /* the Arena account works with its password, without the IMEI */
    post(&peer_b, "/habbo/AuthenticationServlet", "", "user=bob&pass=wrong");
    CHECK(is_text("1"));
    login_as(&peer_b, "bob", "bobpw", sb);

    /* empty lists, max message length at the end */
    init_lists(&peer_a, sa, 1, &l);
    CHECK_EQ(l.nf, 0);
    CHECK_EQ(l.nm, 0);
    CHECK_EQ(l.nr, 0);
    CHECK_EQ(l.maxlen, GCSH_MAX_TEXT);

    /* find: by account name, any spelling; unknown -> "1" */
    post(&peer_a, "/habbo/MessengerServlet", sa, "m=41&p1=BOB");
    CHECK(dec_user(rep.body, rep.len, &u) == (int)rep.len);
    CHECK(!strcmp(u.name, "Bob"));
    CHECK(!strcmp(u.text, "Hello from the Arena"));
    CHECK(u.online);
    bob_id = u.id;
    post(&peer_a, "/habbo/MessengerServlet", sa, "m=41&p1=nobody");
    CHECK(is_text("1"));

    /* messages need a friend */
    snprintf(body, sizeof(body), "m=33&p1=%u&p2=hi", bob_id);
    post(&peer_a, "/habbo/MessengerServlet", sa, body);
    CHECK(is_text("1"));

    /* friend request Ann -> Bob; asking oneself fails */
    snprintf(body, sizeof(body), "m=39&p1=%u", bob_id);
    post(&peer_a, "/habbo/MessengerServlet", sa, body);
    CHECK(is_text("0"));
    post(&peer_a, "/habbo/MessengerServlet", sa, body); /* again: still one request */
    CHECK(is_text("0"));
    init_lists(&peer_b, sb, 0, &l); /* Bob's update shows it once */
    CHECK_EQ(l.nr, 1);
    CHECK(!strcmp(l.reqs[0].name, "Ann"));
    ann_id = l.reqs[0].id;
    init_lists(&peer_b, sb, 0, &l);
    CHECK_EQ(l.nr, 0);
    init_lists(&peer_b, sb, 1, &l); /* a new init lists it again */
    CHECK_EQ(l.nr, 1);
    snprintf(body, sizeof(body), "m=39&p1=%u", ann_id);
    post(&peer_a, "/habbo/MessengerServlet", sa, body);
    CHECK(is_text("1"));

    /* accept: the answer is Ann's friend record */
    snprintf(body, sizeof(body), "m=37&p1=%u", ann_id);
    post(&peer_b, "/habbo/MessengerServlet", sb, body);
    CHECK(dec_user(rep.body, rep.len, &u) == (int)rep.len);
    CHECK_EQ(u.id, ann_id);
    CHECK(!strcmp(u.name, "Ann") && u.online);
    post(&peer_b, "/habbo/MessengerServlet", sb, body); /* no request any more */
    CHECK(is_text("1"));
    init_lists(&peer_a, sa, 1, &l);
    CHECK_EQ(l.nf, 1);
    CHECK(!strcmp(l.friends[0].name, "Bob"));
    CHECK_EQ(l.nr, 0);

    /* message Ann -> Bob, UTF-8 and URL encoded */
    snprintf(body, sizeof(body), "m=33&p1=%u&p2=Gr%%C3%%BC%%C3%%9Fe+aus+den+Inseln%%21", bob_id);
    post(&peer_a, "/habbo/MessengerServlet", sa, body);
    CHECK(is_text("0"));
    init_lists(&peer_b, sb, 0, &l);
    CHECK_EQ(l.nm, 1);
    CHECK_EQ(l.msgs[0].from, ann_id);
    CHECK(!strcmp(l.msgs[0].name, "Ann"));
    CHECK(!strcmp(l.msgs[0].text, "Gr\xc3\xbc\xc3\x9f"
                                  "e aus den Inseln!"));
    CHECK(strlen(l.msgs[0].when) == 16 && l.msgs[0].when[2] == '-');
    init_lists(&peer_b, sb, 0, &l); /* delivered once per session */
    CHECK_EQ(l.nm, 0);

    /* Ann logs out: offline with her last visit */
    post(&peer_a, "/habbo/MessengerServlet", sa, "m=0");
    CHECK(is_text("0"));
    post(&peer_a, "/habbo/MessengerServlet", sa, "m=12");
    CHECK(is_text("2"));
    init_lists(&peer_b, sb, 1, &l);
    CHECK_EQ(l.nf, 1);
    CHECK(!l.friends[0].online);
    CHECK(strlen(l.friends[0].last) == 16);
    CHECK_EQ(l.nm, 1); /* still unread */

    /* long message: cut at a character boundary */
    login_as(&peer_a, "Ann", "ann%20pw", sa);
    {
        char longmsg[2000];
        int i;
        size_t k = (size_t)snprintf(longmsg, sizeof(longmsg), "m=33&p1=%u&p2=", bob_id);
        for (i = 0; i < 200; i++) {
            k += (size_t)snprintf(longmsg + k, sizeof(longmsg) - k, "%%C3%%A4");
        }
        post(&peer_a, "/habbo/MessengerServlet", sa, longmsg);
        CHECK(is_text("0"));
    }

    /* the data survives a restart */
    gcs_habbo_get_stats(h, &st);
    CHECK_EQ(st.users, 2);
    CHECK_EQ(st.friends, 1);
    CHECK_EQ(st.messages, 2);
    gcs_habbo_destroy(h);
    h = gcs_habbo_create(acc, HDATA, quiet_log, NULL);
    gcs_habbo_get_stats(h, &st);
    CHECK_EQ(st.users, 2);
    CHECK_EQ(st.friends, 1);
    CHECK_EQ(st.messages, 2);
    CHECK_EQ(st.sessions, 0);

    /* old cookie: log in again ("2"); without a cookie the peer's latest login */
    post(&peer_b, "/habbo/MessengerServlet", sb, "m=12");
    CHECK(is_text("2"));
    login_as(&peer_b, "Bob", "bobpw", sb);
    init_lists(&peer_b, "", 1, &l);
    CHECK_EQ(l.nm, 2);
    CHECK_EQ(strlen(l.msgs[1].text), 254); /* 127 two-byte characters */
    CHECK(!l.friends[0].online);

    /* mark read: several p1 values; others' messages are not touched */
    snprintf(body, sizeof(body), "m=32&p1=%u&p1=%u", l.msgs[0].id, l.msgs[1].id);
    post(&peer_c, "/habbo/MessengerServlet", "", body); /* peer_c: no login */
    CHECK(is_text("2"));
    post(&peer_b, "/habbo/MessengerServlet", sb, body);
    CHECK(is_text("0"));
    init_lists(&peer_b, sb, 1, &l);
    CHECK_EQ(l.nm, 0);

    /* a request declined, a friend removed */
    CHECK_EQ(gcs_accounts_login(acc, "Cy", "cypw", NULL), GCSA_CREATED);
    login_as(&peer_c, "Cy", "cypw", sc);
    post(&peer_c, "/habbo/MessengerServlet", sc, "m=41&p1=bob");
    snprintf(body, sizeof(body), "m=39&p1=%u", bob_id);
    post(&peer_c, "/habbo/MessengerServlet", sc, body);
    CHECK(is_text("0"));
    init_lists(&peer_b, sb, 0, &l);
    CHECK_EQ(l.nr, 1);
    snprintf(body, sizeof(body), "m=38&p1=%u", l.reqs[0].id);
    post(&peer_b, "/habbo/MessengerServlet", sb, body);
    CHECK(is_text("0"));
    init_lists(&peer_b, sb, 1, &l);
    CHECK_EQ(l.nr, 0);
    CHECK_EQ(l.nf, 1);
    snprintf(body, sizeof(body), "m=40&p1=%u", ann_id);
    post(&peer_b, "/habbo/MessengerServlet", sb, body);
    CHECK(is_text("0"));
    login_as(&peer_a, "Ann", "ann%20pw", sa); /* her session ended with the restart */
    init_lists(&peer_a, sa, 1, &l);
    CHECK_EQ(l.nf, 0);

    post(&peer_b, "/habbo/MessengerServlet", sb, "m=99");
    CHECK(is_text("1"));

    free(rep.body);
    rep.body = NULL;
    gcs_habbo_destroy(h);
    gcs_accounts_destroy(acc);
    remove(ACC);
    remove(HDATA);
}

/* ---- over TCP through gcs_direct and gcs_native ---- */

typedef struct
{
    char rx[8192];
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

static const char *tcp_request(gcs_direct *d, fake_sock *s, const char *req)
{
    static const gc_u8 ip[4] = { 203, 0, 113, 50 };
    memset(s, 0, sizeof(*s));
    CHECK_EQ(gcs_direct_tcp_open(d, s, ip, 2000, GCSD_HABBO, 1000), 0);
    gcs_direct_tcp_input(d, s, (const gc_u8 *)req, 20, 1000);
    gcs_direct_tcp_input(d, s, (const gc_u8 *)req + 20, strlen(req) - 20, 1000);
    CHECK(s->fin);
    gcs_direct_tcp_eof(d, s, 1000);
    gcs_direct_tcp_gone(d, s);
    return s->rx;
}

static void test_over_tcp(void)
{
    gcs_config cfg;
    gcs_io sio;
    gcs_direct_io dio;
    gcs_core *core;
    gcs_direct *d;
    fake_sock s;
    const char *r, *c;
    char cookie[64], req[600];
    size_t k;

    remove(ACC);
    remove(HDATA);
    gcs_default_config(&cfg);
    strcpy(cfg.arena_accounts, ACC);
    strcpy(cfg.habbo_data, HDATA);
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

    /* as the game's HTTP stack sends them */
    r = tcp_request(d, &s,
                    "GET /test/locator.html HTTP/1.1\r\nHost: builder.sulake.com:8080\r\n"
                    "User-Agent: Habbo Console (1.0)\r\nAccept: */*\r\n\r\n");
    CHECK(!strncmp(r, "HTTP/1.1 200 OK\r\n", 17));
    CHECK(strstr(r, "\r\n\r\n1;PhantomLink;http://builder.sulake.com:8080/habbo/\r\n") != NULL);

    r = tcp_request(d, &s,
                    "POST /habbo/AuthenticationServlet HTTP/1.1\r\nHost: builder.sulake.com:8080\r\n"
                    "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: 19\r\n\r\nuser=Dee&pass=deepw");
    CHECK(strstr(r, "Set-Cookie: JSESSIONID=PLH") != NULL);
    CHECK(strstr(r, "\r\n\r\n0") != NULL);
    c = strstr(r, "JSESSIONID=");
    k = c ? strcspn(c + 11, ";\r") : 0;
    CHECK(k > 0 && k < sizeof(cookie));
    memcpy(cookie, c ? c + 11 : "", k);
    cookie[k] = 0;

    snprintf(req, sizeof(req),
             "POST /habbo/MessengerServlet HTTP/1.1\r\nHost: builder.sulake.com:8080\r\nCookie: JSESSIONID=%s\r\n"
             "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: 4\r\n\r\nm=12",
             cookie);
    r = tcp_request(d, &s, req);
    CHECK(strstr(r, "Content-Type: application/octet-stream") != NULL);
    {
        const char *b = strstr(r, "\r\n\r\n");
        d_lists l;
        CHECK(b != NULL);
        if (b) {
            b += 4;
            CHECK_EQ(dec_lists((const unsigned char *)b, s.len - (size_t)(b - s.rx), 1, &l), 0);
            CHECK_EQ(l.maxlen, GCSH_MAX_TEXT);
        }
    }
    /* the shared account exists now */
    CHECK_EQ(gcs_accounts_check(gcs_native_accounts(gcs_core_native(core)), "dee", "deepw"), GCSA_OK);

    gcs_direct_destroy(d);
    gcs_core_destroy(core);
    remove(ACC);
    remove(HDATA);
}

int main(void)
{
    RUN(test_console);
    RUN(test_over_tcp);
    TEST_MAIN_END();
}
