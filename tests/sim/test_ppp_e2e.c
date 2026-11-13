/* End to end test of the PPP mode (retail Arena games), all production code
 * except the N-Gage:
 *
 *   [N-Gage: gc_ppp + test TCP client] --serial-- [bridge_core] --TCP-- [gcs_core + gcs_native]
 *
 * Covers PPP set-up, DNS, the Community SOAP service with profile data kept
 * in a file, the device pages, the Jabber login, UDP, a refused port and
 * the clean-up when the N-Gage goes away.
 *
 * SPDX-License-Identifier: MIT */
#include "bridge_core.h"
#include "gc_ip.h"
#include "gc_ppp.h"
#include "gc_test.h"
#include "gcs_accounts.h"
#include "gcs_core.h"
#include "gcs_native.h"

#include <stdio.h>

typedef struct
{
    gc_u8 d[256 * 1024];
    size_t n;
} pipe_t;

static pipe_t to_bridge, to_ngage, to_server, to_bridge_srv;
static gc_u32 now;
static bridge br;
static gcs_core *core;
static gcs_conn conn;
static int srv_connect_req, srv_open, verbose;
static gc_ppp ng; /* the N-Gage's PPP */

static void put(pipe_t *p, const gc_u8 *d, size_t n)
{
    CHECK(p->n + n <= sizeof(p->d));
    if (p->n + n <= sizeof(p->d)) {
        memcpy(p->d + p->n, d, n);
        p->n += n;
    }
}

/* ---- bridge and server glue ---- */

static void b_dev_write(void *c, const gc_u8 *d, size_t n)
{
    (void)c;
    put(&to_ngage, d, n);
}

static void b_srv_write(void *c, const gc_u8 *d, size_t n)
{
    (void)c;
    put(&to_server, d, n);
}

static void b_srv_connect(void *c)
{
    (void)c;
    srv_connect_req = 1;
}

static void b_srv_disconnect(void *c)
{
    (void)c;
}

static void b_log(void *c, const char *l)
{
    (void)c;
    if (verbose) {
        printf("  bridge: %s\n", l);
    }
}

static void s_write(void *c, gcs_conn *k, const gc_u8 *d, size_t n)
{
    (void)c;
    (void)k;
    put(&to_bridge_srv, d, n);
}

static void s_close(void *c, gcs_conn *k)
{
    (void)c;
    (void)k;
}

static void s_log(void *c, int level, const char *l)
{
    (void)c;
    if (verbose || level == 0) {
        printf("  server: %s\n", l);
    }
}

/* ---- the N-Gage: PPP plus a tiny TCP/UDP client ---- */

static const gc_u8 NG_IP[4] = { 10, 64, 0, 2 };
static gc_u8 g_dns_ans[4];
static int g_dns_rcode = -1;
static gc_u8 g_udp_rx[512];
static size_t g_udp_len;

typedef struct
{
    int state; /* 0 idle, 1 syn sent, 2 open, 3 closed by peer, 4 reset */
    gc_u8 dst[4];
    gc_u16 sport, dport;
    gc_u32 snd_nxt, rcv_nxt;
    char rx[65536];
    size_t rx_len;
} tcl;

static tcl cl;

static void ng_send_ip(gc_u8 proto, const gc_u8 *dst, const gc_u8 *pl, size_t n)
{
    gc_u8 b[1600];
    memset(b, 0, 20);
    b[0] = 0x45;
    b[2] = (gc_u8)((20 + n) >> 8);
    b[3] = (gc_u8)(20 + n);
    b[8] = 64;
    b[9] = proto;
    memcpy(b + 12, NG_IP, 4);
    memcpy(b + 16, dst, 4);
    {
        gc_u16 cs = gc_ip_fold(gc_ip_sum(0, b, 20));
        b[10] = (gc_u8)(cs >> 8);
        b[11] = (gc_u8)cs;
    }
    memcpy(b + 20, pl, n);
    CHECK_EQ(gc_ppp_send_ip(&ng, b, 20 + n), 0);
}

static gc_u32 pseudo(const gc_u8 *s, const gc_u8 *d, gc_u8 proto, size_t len)
{
    return gc_ip_sum(gc_ip_sum(0, s, 4), d, 4) + proto + (gc_u32)len;
}

static void tcp_seg(gc_u8 fl, const void *d, size_t n)
{
    gc_u8 t[1600];
    gc_u16 cs;
    memset(t, 0, 20);
    t[0] = (gc_u8)(cl.sport >> 8);
    t[1] = (gc_u8)cl.sport;
    t[2] = (gc_u8)(cl.dport >> 8);
    t[3] = (gc_u8)cl.dport;
    t[4] = (gc_u8)(cl.snd_nxt >> 24);
    t[5] = (gc_u8)(cl.snd_nxt >> 16);
    t[6] = (gc_u8)(cl.snd_nxt >> 8);
    t[7] = (gc_u8)cl.snd_nxt;
    t[8] = (gc_u8)(cl.rcv_nxt >> 24);
    t[9] = (gc_u8)(cl.rcv_nxt >> 16);
    t[10] = (gc_u8)(cl.rcv_nxt >> 8);
    t[11] = (gc_u8)cl.rcv_nxt;
    t[12] = 0x50;
    t[13] = fl;
    t[14] = 0x20; /* window 8192 */
    if (n) {
        memcpy(t + 20, d, n);
    }
    cs = gc_ip_fold(gc_ip_sum(pseudo(NG_IP, cl.dst, 6, 20 + n), t, 20 + n));
    t[16] = (gc_u8)(cs >> 8);
    t[17] = (gc_u8)cs;
    ng_send_ip(6, cl.dst, t, 20 + n);
    cl.snd_nxt += (gc_u32)n + ((fl & 0x03) ? 1u : 0u);
}

static gc_u32 r32(const gc_u8 *p)
{
    return ((gc_u32)p[0] << 24) | ((gc_u32)p[1] << 16) | ((gc_u32)p[2] << 8) | p[3];
}

static void ng_on_ip(void *ctx, const gc_u8 *p, size_t n)
{
    size_t hl = (size_t)(p[0] & 15) * 4;
    const gc_u8 *l4 = p + hl;
    size_t ln = n - hl;
    (void)ctx;
    CHECK(!memcmp(p + 16, NG_IP, 4));
    if (p[9] == 17) {
        gc_u16 sport = (gc_u16)((l4[0] << 8) | l4[1]);
        CHECK_EQ(gc_ip_fold(gc_ip_sum(pseudo(p + 12, p + 16, 17, ln), l4, ln)), 0);
        if (sport == 53) {
            const gc_u8 *a = l4 + 8;
            g_dns_rcode = a[3] & 15;
            if (((a[6] << 8) | a[7]) == 1) {
                memcpy(g_dns_ans, l4 + ln - 4, 4);
            }
        } else {
            g_udp_len = ln - 8;
            memcpy(g_udp_rx, l4 + 8, g_udp_len);
        }
        return;
    }
    if (p[9] == 6) {
        size_t th = (size_t)(l4[12] >> 4) * 4, dl = ln - th;
        gc_u8 fl = l4[13];
        CHECK_EQ(gc_ip_fold(gc_ip_sum(pseudo(p + 12, p + 16, 6, ln), l4, ln)), 0);
        if (fl & 0x04) {
            cl.state = 4;
            return;
        }
        if (cl.state == 1 && (fl & 0x12) == 0x12) {
            cl.rcv_nxt = r32(l4 + 4) + 1;
            cl.state = 2;
            tcp_seg(0x10, NULL, 0);
            return;
        }
        if (r32(l4 + 4) != cl.rcv_nxt) {
            return;
        }
        if (dl) {
            memcpy(cl.rx + cl.rx_len, l4 + th, dl);
            cl.rx_len += dl;
            cl.rx[cl.rx_len] = 0;
            cl.rcv_nxt += (gc_u32)dl;
        }
        if (fl & 0x01) {
            cl.rcv_nxt++;
            cl.state = 3;
        }
        if (dl || (fl & 0x01)) {
            tcp_seg(0x10, NULL, 0);
        }
    }
}

static void ng_write(void *ctx, const gc_u8 *d, size_t n)
{
    (void)ctx;
    put(&to_bridge, d, n);
}

/* ---- the world ---- */

static void step(void)
{
    gc_u8 tmp[256 * 1024];
    size_t n;
    now += 10;
    if (srv_connect_req) {
        srv_connect_req = 0;
        if (!srv_open) {
            memset(&conn, 0, sizeof(conn));
            gcs_core_conn_open(core, &conn, now);
            srv_open = 1;
        }
        bridge_srv_connected(&br, now);
    }
    n = to_bridge.n;
    memcpy(tmp, to_bridge.d, n);
    to_bridge.n = 0;
    if (n) {
        bridge_dev_input(&br, tmp, n, now);
    }
    n = to_ngage.n;
    memcpy(tmp, to_ngage.d, n);
    to_ngage.n = 0;
    if (n) {
        gc_ppp_input(&ng, tmp, n, now);
    }
    n = to_server.n;
    memcpy(tmp, to_server.d, n);
    to_server.n = 0;
    if (n) {
        gcs_core_conn_input(core, &conn, tmp, n, now);
    }
    n = to_bridge_srv.n;
    memcpy(tmp, to_bridge_srv.d, n);
    to_bridge_srv.n = 0;
    if (n) {
        bridge_srv_input(&br, tmp, n, now);
    }
    bridge_poll(&br, now);
    gcs_core_poll(core, now);
    gc_ppp_poll(&ng, now);
}

static void run(int ms)
{
    int i;
    for (i = 0; i < ms / 10; i++) {
        step();
    }
}

static const gc_u8 BR_IP[4] = { 10, 64, 0, 1 };

/* one TCP exchange: connect, send request, wait until the server closes */
static const char *tcp_request(gc_u16 port, const char *req, int expect_close)
{
    static gc_u16 next_port = 1700;
    int i;
    memset(&cl, 0, sizeof(cl));
    memcpy(cl.dst, BR_IP, 4);
    cl.sport = next_port++;
    cl.dport = port;
    cl.snd_nxt = 1000u * cl.sport;
    cl.state = 1;
    tcp_seg(0x02, NULL, 0);
    for (i = 0; i < 100 && cl.state == 1; i++) {
        step();
    }
    if (cl.state != 2) {
        return NULL;
    }
    if (req) {
        size_t off, len = strlen(req);
        for (off = 0; off < len; off += 1000) { /* below the MSS */
            tcp_seg(0x18, req + off, len - off > 1000 ? 1000 : len - off);
            step();
        }
    }
    for (i = 0; i < 300 && (expect_close ? cl.state == 2 : cl.rx_len == 0); i++) {
        step();
    }
    if (cl.state == 3) {
        tcp_seg(0x11, NULL, 0); /* our FIN */
        run(100);
    }
    return cl.rx;
}

static const char *soap(const char *op, const char *inner, const char *cookie)
{
    static char req[4096];
    char body[2048], ck[200] = "";
    snprintf(body, sizeof(body),
             "<?xml version='1.0' encoding='UTF-8'?>\n<soapenv:Envelope "
             "xmlns:soapenv=\"http://schemas.xmlsoap.org/soap/envelope/\">\n<soapenv:Body>\n<ns1:%s "
             "xmlns:ns1=\"urn:CommunityApp\">\n%s</ns1:%s>\n</soapenv:Body>\n</soapenv:Envelope>\n\n",
             op, inner, op);
    if (cookie) {
        snprintf(ck, sizeof(ck), "Cookie: JSESSIONID=%s\r\n", cookie);
    }
    snprintf(req, sizeof(req),
             "POST /n-gage/axis/services/Community HTTP/1.1\r\nHost: arena.n-gage.com\r\nSOAPAction: \"\"\r\n%s"
             "Content-Type: text/xml\r\nContent-Length: %u\r\n\r\n%s",
             ck, (unsigned)strlen(body), body);
    return tcp_request(80, req, 1);
}

static void dns_query(const char *name)
{
    gc_u8 u[300], *q = u + 8;
    size_t n = 12;
    const char *s = name;
    gc_u16 cs;
    memset(u, 0, sizeof(u));
    u[0] = 0x04;
    u[1] = 0x00; /* port 1024 */
    u[3] = 53;
    q[0] = 0x55;
    q[2] = 1;
    q[5] = 1;
    while (*s) {
        const char *e = strchr(s, '.');
        size_t l = e ? (size_t)(e - s) : strlen(s);
        q[n++] = (gc_u8)l;
        memcpy(q + n, s, l);
        n += l;
        s += l + (e ? 1 : 0);
    }
    q[n++] = 0;
    q[n + 1] = 1;
    q[n + 3] = 1;
    n += 4;
    u[4] = (gc_u8)((n + 8) >> 8);
    u[5] = (gc_u8)(n + 8);
    cs = gc_ip_fold(gc_ip_sum(pseudo(NG_IP, BR_IP, 17, n + 8), u, n + 8));
    u[6] = (gc_u8)(cs >> 8);
    u[7] = (gc_u8)cs;
    g_dns_rcode = -1;
    ng_send_ip(17, BR_IP, u, n + 8);
    run(100);
}

static void test_ppp_mode(void)
{
    static const char *ACC = "test_ppp_e2e_accounts.txt";
    gcs_config sc;
    gcs_io sio = { s_write, s_close, s_log };
    bridge_cfg bc;
    bridge_io bio;
    gc_ppp_cfg pc;
    gc_ppp_ops po;
    gcs_native_stats ns;
    const char *r;
    gc_u8 addr[6] = { 0x00, 0x0E, 0x6D, 0x01, 0x02, 0x03 };
    char cookie[64];
    int i;

    remove(ACC);
    gcs_default_config(&sc);
    strcpy(sc.arena_accounts, ACC);
    sc.log_level = verbose ? 3 : 0;
    core = gcs_core_create(&sc, &sio, NULL, 1);
    CHECK(core != NULL);

    bridge_default_cfg(&bc);
    memset(&bio, 0, sizeof(bio));
    bio.dev_write = b_dev_write;
    bio.srv_write = b_srv_write;
    bio.srv_connect = b_srv_connect;
    bio.srv_disconnect = b_srv_disconnect;
    bio.log = b_log;
    bridge_init(&br, &bc, &bio, NULL);
    bridge_set_wifi(&br, 1, -50, now);
    run(500);
    CHECK_EQ(br.srv_state, GC_BS_UP);

    /* the N-Gage connects and starts PPP (it requests 0.0.0.0 like Symbian
     * would not, but the address the bridge assigns is the same) */
    bridge_dev_connected(&br, addr, now);
    gc_ppp_default_cfg(&pc);
    memcpy(pc.local, NG_IP, 4);
    memcpy(pc.remote, BR_IP, 4);
    pc.magic = 0x1234567u;
    memset(&po, 0, sizeof(po));
    po.write = ng_write;
    po.on_ip = ng_on_ip;
    gc_ppp_init(&ng, &pc, &po, NULL);
    gc_ppp_open(&ng, now);
    for (i = 0; i < 200 && !(gc_ppp_is_up(&ng) && br.mode == BRIDGE_MODE_PPP && gc_ppp_is_up(&br.u.pp.ppp)); i++) {
        step();
    }
    CHECK(gc_ppp_is_up(&ng));
    CHECK_EQ(br.mode, BRIDGE_MODE_PPP);
    run(200);
    CHECK(br.registered);

    dns_query("arena.n-gage.com");
    CHECK_EQ(g_dns_rcode, 0);
    CHECK(!memcmp(g_dns_ans, BR_IP, 4));
    dns_query("im.prod.pk.nokia.sf.yav4.com");
    CHECK(!memcmp(g_dns_ans, BR_IP, 4));
    dns_query("www.example.org");
    CHECK_EQ(g_dns_rcode, 3);

    /* login: the user name comes back as xsd:string after the boolean; the
     * first login creates the account and binds it to the IMEI */
#define LOGIN(u, p, i)                                                                                       \
    soap("authenticateUser2", "<username xsi:type=\"xsd:string\">" u "</username>\n<password xsi:type=\""    \
                              "xsd:string\">" p "</password>\n<imei xsi:type=\"xsd:string\">" i "</imei>\n", \
         NULL)
    r = LOGIN("tester", "pw", "350000000000001");
    CHECK(r && strstr(r, "HTTP/1.1 200 OK"));
    CHECK(r && strstr(r, "<authenticateUser2Return xsi:type=\"xsd:boolean\">true</authenticateUser2Return>"
                         "<username xsi:type=\"xsd:string\">tester</username>"));
    cookie[0] = 0;
    if (r && strstr(r, "Set-Cookie: JSESSIONID=PL")) {
        const char *ck = strstr(r, "JSESSIONID=") + 11;
        size_t k = strcspn(ck, ";\r\n");
        if (k < sizeof(cookie)) {
            memcpy(cookie, ck, k);
            cookie[k] = 0;
        }
    }
    CHECK_EQ(strlen(cookie), 34);
    /* the same account later: same password and N-Gage, any case of the name */
    r = LOGIN("Tester", "pw", "350000000000001");
    CHECK(r && strstr(r, "xsd:boolean\">true<") && strstr(r, "<username xsi:type=\"xsd:string\">tester</username>"));
    /* refused: wrong password, empty password, another N-Gage, a second account on this N-Gage */
    r = LOGIN("tester", "nope", "350000000000001");
    CHECK(r && strstr(r, "xsd:boolean\">false<") && !strstr(r, "Set-Cookie"));
    r = LOGIN("tester", "", "350000000000001");
    CHECK(r && strstr(r, "xsd:boolean\">false<"));
    r = LOGIN("tester", "pw", "350000000000002");
    CHECK(r && strstr(r, "xsd:boolean\">false<"));
    r = LOGIN("other", "pw2", "350000000000001");
    CHECK(r && strstr(r, "xsd:boolean\">false<"));
    /* another N-Gage can create its own account */
    r = LOGIN("other", "pw2", "350000000000002");
    CHECK(r && strstr(r, "xsd:boolean\">true<"));
    CHECK(gcs_accounts_count(gcs_native_accounts(gcs_core_native(core))) == 2);
    /* a forged or stale cookie is not a login: the request counts for the
     * user logged in on this N-Gage ("other"), not for "tester" */
    r = soap("editAccount", "<firstName xsi:type=\"xsd:string\">Evil</firstName>\n", "PL746573746572");
    CHECK(r && strstr(r, "<errorCode xsi:type=\"xsd:int\">0</errorCode>"));
    CHECK(!strcmp(gcs_accounts_get(gcs_native_accounts(gcs_core_native(core)), "other", "firstName"), "Evil"));
    CHECK(!gcs_accounts_get(gcs_native_accounts(gcs_core_native(core)), "tester", "firstName")[0]);
    /* back to tester on this bridge connection */
    r = LOGIN("tester", "pw", "350000000000001");
#undef LOGIN

    /* profile edits are kept */
    r = soap("editAccount",
             "<firstName xsi:type=\"xsd:string\">Ann</firstName>\n<lastName xsi:type=\"xsd:string\">B &amp; "
             "C</lastName>\n<emailAddress xsi:type=\"xsd:string\">a@b.c</emailAddress>\n",
             cookie);
    CHECK(r && strstr(r, "<errorCode xsi:type=\"xsd:int\">0</errorCode>"));
    r = soap("viewAccount", "", cookie);
    CHECK(r && strstr(r, "<viewAccountReturn><item>Ann</item><item>B &amp; C</item><item>a@b.c</item>"));
    /* also without a cookie: the bridge connection remembers the user */
    r = soap("editAlertSubscriptions", "<gameReleaseNews xsi:type=\"xsd:boolean\">true</gameReleaseNews>\n", NULL);
    r = soap("viewAlertSubscriptions", "", NULL);
    CHECK(r && strstr(r, "<item>true</item><item>false</item><item>false</item><item>false</item><item>false</item>"
                         "<item>false</item>"));
    r = soap("getNewAlertStatus", "", cookie);
    CHECK(r && strstr(r, "<getNewAlertStatusReturn>false</getNewAlertStatusReturn>"));
    {
        FILE *f = fopen(ACC, "r");
        char buf[2048] = "";
        size_t n = 0;
        CHECK(f != NULL);
        if (f) {
            n = fread(buf, 1, sizeof(buf) - 1, f);
            fclose(f);
        }
        buf[n] = 0;
        CHECK(strstr(buf, "tester\tlastName\tB & C\n") != NULL);
        CHECK(strstr(buf, "tester\tgameReleaseNews\ttrue\n") != NULL);
        CHECK(strstr(buf, "tester\tpwsha1\t1a91d62f7ca67399625a4368a6ab5d4a3baa6073\n") != NULL);
        CHECK(strstr(buf, "tester\timei\t350000000000001\n") != NULL);
        CHECK(strstr(buf, "\tpassword\t") == NULL && strstr(buf, "tester\tfirstName\tEvil") == NULL);
    }

    /* The profile page uses the requested account, not the most recent login. */
    r = soap("editDisplayOptions", "<displayFirstName>true</displayFirstName><displayLastName>false</displayLastName>", cookie);
    CHECK(r && strstr(r, "<errorCode xsi:type=\"xsd:int\">0</errorCode>"));
    r = soap("editFavorites", "<favGame>Rifts &amp; more</favGame>", cookie);
    CHECK(r && strstr(r, "<errorCode xsi:type=\"xsd:int\">0</errorCode>"));
    r = tcp_request(80, "GET /n-gage/device/global/customize/profile/profile_view.jsp;jsessionid=PL0?locale=1&username=%74ester&%C4%84 HTTP/1.1\r\n\r\n", 1);
    CHECK(r && strstr(r, "200 OK") && strstr(r, "application/xhtml+xml"));
    CHECK(r && strstr(r, "Username:</b> tester") && strstr(r, "First name:</b> Ann"));
    CHECK(r && strstr(r, "Favorite game:</b> Rifts &amp; more") && !strstr(r, "B &amp; C") && !strstr(r, "a@b.c"));
    r = tcp_request(80, "GET /n-gage/device/global/customize/profile/profile_view.jsp?username=other HTTP/1.1\r\n\r\n", 1);
    CHECK(r && strstr(r, "Username:</b> other") && !strstr(r, "Evil") && !strstr(r, "Ann"));
    r = tcp_request(80, "GET /n-gage/device/global/customize/profile/profile_view.jsp?username=missing HTTP/1.1\r\n\r\n", 1);
    CHECK(r && strstr(r, "404 Not Found"));
    r = tcp_request(80, "GET /n-gage/device/global/customize/profile/profile_view.jsp?username=tester%00other HTTP/1.1\r\n\r\n", 1);
    CHECK(r && strstr(r, "404 Not Found"));
    r = tcp_request(80, "GET /n-gage/device/global/customize/profile/profile_view.jsp?username=tester&username=other HTTP/1.1\r\n\r\n", 1);
    CHECK(r && strstr(r, "404 Not Found"));
    r = tcp_request(80, "GET /n-gage/device/global/customize/profile/profile_view.jsp HTTP/1.1\r\n\r\n", 1);
    CHECK(r && strstr(r, "Username:</b> tester"));
    CHECK_EQ(gcs_accounts_login(gcs_native_accounts(gcs_core_native(core)), "Joe & Jane", "pw", ""), GCSA_CREATED);
    r = tcp_request(80, "GET /n-gage/device/global/customize/profile/profile_view.jsp?username=Joe+%26+Jane HTTP/1.1\r\n\r\n", 1);
    CHECK(r && strstr(r, "Username:</b> Joe &amp; Jane"));

    /* device pages */
    r = tcp_request(80,
                    "GET /n-gage/device/community/ranking_list_x.jsp;jsessionid=PL746573746572?locale=1&username=tester "
                    "HTTP/1.1\r\nHost: arena.n-gage.com\r\n\r\n",
                    1);
    CHECK(r && strstr(r, "<currentStart value=\"0\"/><listTotal value=\"0\"/>"));
    r = tcp_request(80, "GET /n-gage/device/global/support/support_main.jsp HTTP/1.1\r\n\r\n", 1);
    CHECK(r && strstr(r, "200 OK") && strstr(r, "xhtml"));
    r = tcp_request(80, "GET /favicon.ico HTTP/1.1\r\n\r\n", 1);
    CHECK(r && strstr(r, "404"));
#ifdef GCS_WWW_DIR
    /* the example pages (server/deploy/www) */
    gcs_native_set_www_root(gcs_core_native(core), GCS_WWW_DIR "/");
    r = tcp_request(80, "GET /n-gage/device/global/customize/profile/profile_view.jsp?username=tester HTTP/1.1\r\n\r\n", 1);
    CHECK(r && strstr(r, "Username:</b> tester") && strstr(r, "First name:</b> Ann"));
    r = tcp_request(80, "GET /n-gage/device/global/support/support_main.jsp HTTP/1.1\r\n\r\n", 1);
    CHECK(r && strstr(r, "200 OK") && strstr(r, "application/xhtml+xml"));
    CHECK(r && strstr(r, "run by a PhantomLink server") && strstr(r, "background-color:#000000"));
    r = tcp_request(80,
                    "GET /n-gage/servlets/msg;jsessionid=PL0?locale=1&start=0&disp=10&platform=device& HTTP/1.1\r\n\r\n",
                    1);
    CHECK(r && strstr(r, "application/xhtml+xml") && strstr(r, "Welcome back") && !strstr(r, "Rifts"));
    r = tcp_request(80, "GET /n-gage/device/global/support/legal_main.jsp?locale=1 HTTP/1.1\r\n\r\n", 1);
    CHECK(r && strstr(r, "<title>About</title>"));
    r = tcp_request(80, "GET /n-gage/device/community/ranking_list_x.jsp HTTP/1.1\r\n\r\n", 1);
    CHECK(r && strstr(r, "<listTotal value=\"0\"/>")); /* no file: built-in list */
    r = tcp_request(80, "GET /n-gage/../README.md HTTP/1.1\r\n\r\n", 1);
    CHECK(r && strstr(r, "404"));
    r = tcp_request(80, "GET /n-gage/device HTTP/1.1\r\n\r\n", 1);
    CHECK(r && strstr(r, "404")); /* a folder */
    gcs_native_set_www_root(gcs_core_native(core), "");
#endif

    /* SegaChat login */
    r = tcp_request(5222,
                    "<?xml version='1.0'?><stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                    "xmlns='jabber:client' to='ngage-auth'><iq type='get' id='segachat_query_username'><query "
                    "xmlns='jabber:iq:auth'><username>tester</username></query></iq>",
                    0);
    run(200);
    CHECK(r && strstr(r, "from='ngage-auth'"));
    CHECK(r && strstr(r, "<iq type='result' id='segachat_query_username'><query xmlns='jabber:iq:auth'>"));
    cl.rx_len = 0;
    {
        static const char *bad = "<iq type='set' id='bad'><query xmlns='jabber:iq:auth'><username>tester</username>"
                                 "<password>0000000000000000000000000000000000000000</password><resource>segachat"
                                 "</resource></query></iq>";
        tcp_seg(0x18, bad, strlen(bad));
    }
    run(300);
    CHECK(strstr(cl.rx, "<iq type='error' id='bad'><error code='401'>") != NULL);
    {
        /* SegaChat sends the SHA-1 of the password */
        static const char *st = "<iq type='set' id='segachat_auth'><query xmlns='jabber:iq:auth'><username>tester"
                                "</username><password>1a91d62f7ca67399625a4368a6ab5d4a3baa6073</password><resource>"
                                "segachat</resource></query></iq><iq "
                                "type='get' id='r'><query xmlns='jabber:iq:roster'/></iq><presence><status>"
                                "available</status></presence>";
        tcp_seg(0x18, st, strlen(st));
    }
    run(300);
    CHECK(strstr(cl.rx, "<iq type='result' id='segachat_auth'/>") != NULL);
    CHECK(strstr(cl.rx, "<iq type='result' id='r'><query xmlns='jabber:iq:roster'/></iq>") != NULL);
    tcp_seg(0x18, "</stream:stream>", 16);
    run(300);
    CHECK(strstr(cl.rx, "</stream:stream>") != NULL);
    CHECK_EQ(cl.state, 3);
    tcp_seg(0x11, NULL, 0);
    run(100);

    /* SNAP datagram is logged by the server */
    {
        gc_u8 u[12] = { 0x04, 0x01, 0x1F, 0x90, 0, 12, 0, 0, 1, 2, 3, 4 };
        ng_send_ip(17, BR_IP, u, sizeof(u));
        run(100);
    }
    /* SNAP login (UDP 9090) of the user logged in above: the answer comes
     * back from the server's address and port */
    {
        gc_u8 u[8 + 16 + 120 + 4];
        size_t ul = sizeof(u);
        memset(u, 0, sizeof(u));
        u[0] = 0x0F;
        u[1] = 0xA0; /* source port 4000 */
        u[2] = 0x23;
        u[3] = 0x82; /* 9090 */
        u[4] = (gc_u8)(ul >> 8);
        u[5] = (gc_u8)ul;
        u[8] = 0x30; /* flags 0x3000, length 136 */
        u[9] = 136;
        u[11] = 44; /* login */
        memcpy(u + 8 + 16, "tester", 6);
        u[ul - 4] = 0xBA;
        u[ul - 3] = 0x47;
        u[ul - 2] = 0x66;
        u[ul - 1] = 0x11;
        g_udp_len = 0;
        ng_send_ip(17, BR_IP, u, ul);
        run(200);
        CHECK_EQ(g_udp_len, 16 + 12 + 4);
        CHECK_EQ(g_udp_rx[3], 41); /* login accepted */
        CHECK(g_udp_rx[g_udp_len - 4] == 0xBA && g_udp_rx[g_udp_len - 3] == 0x47);
    }

    /* a port nobody serves: reset */
    r = tcp_request(1234, NULL, 0);
    CHECK(r == NULL || cl.state == 4);
    run(300);
    CHECK_EQ(cl.state, 4);

    gcs_native_get_stats(gcs_core_native(core), &ns);
    CHECK_EQ(ns.udp_datagrams, 2);
    CHECK_EQ(ns.xmpp_logins, 1);
    CHECK(ns.http_requests >= 9);

    /* open a connection, then the N-Gage goes away: the server drops it */
    memset(&cl, 0, sizeof(cl));
    memcpy(cl.dst, BR_IP, 4);
    cl.sport = 4000;
    cl.dport = 5222;
    cl.state = 1;
    tcp_seg(0x02, NULL, 0);
    run(200);
    CHECK_EQ(cl.state, 2);
    gcs_native_get_stats(gcs_core_native(core), &ns);
    CHECK_EQ(ns.conns, 1);
    bridge_dev_disconnected(&br, now);
    CHECK_EQ(bridge_dev_state(&br), GC_LS_IDLE);
    run(500);
    gcs_native_get_stats(gcs_core_native(core), &ns);
    CHECK_EQ(ns.conns, 0);

    /* a GameComms device next: the device link is set up again in the
     * memory PPP mode used */
    bridge_dev_connected(&br, NULL, now);
    CHECK_EQ(br.mode, BRIDGE_MODE_NONE);
    CHECK_EQ(bridge_dev_state(&br), GC_LS_WAIT_HELLO);
    CHECK_EQ(br.u.gc.to_dev.count, 0);
    bridge_dev_disconnected(&br, now);

    bridge_release(&br);
    gcs_core_destroy(core);
    remove(ACC);
}

int main(int argc, char **argv)
{
    (void)argv;
    verbose = argc > 1;
    RUN(test_ppp_mode);
    TEST_MAIN_END();
}
