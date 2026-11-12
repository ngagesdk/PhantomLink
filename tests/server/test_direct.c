/* Direct Arena clients (GPRS, gcs_direct.h) against the real server core:
 * Community login over a direct TCP connection, two N-Gages behind one
 * carrier address, SNAP login over UDP, datagrams from unknown addresses,
 * a refused port, Jabber shutdown and forgetting idle addresses.
 *
 * SPDX-License-Identifier: MIT */
#include "gc_test.h"
#include "gcs_core.h"
#include "gcs_direct.h"
#include "gcs_native.h"

#include <stdio.h>

#define ACC "test_direct_accounts.txt"

typedef struct
{
    char rx[16384];
    size_t len;
    int fin;
    int reset;
} fake_sock;

static fake_sock socks[8];
static gc_u8 udp_rx[2048];
static size_t udp_len;
static gc_u16 udp_service, udp_port;
static gc_u8 udp_ip[4];

static void io_tcp_send(void *ctx, void *sock, const gc_u8 *d, size_t len)
{
    fake_sock *s = (fake_sock *)sock;
    (void)ctx;
    CHECK(!s->fin && !s->reset);
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
    (void)ctx;
    udp_service = service;
    memcpy(udp_ip, ip, 4);
    udp_port = port;
    udp_len = len < sizeof(udp_rx) ? len : sizeof(udp_rx);
    memcpy(udp_rx, d, udp_len);
}

static void io_log(void *ctx, int level, const char *line)
{
    (void)ctx;
    if (level <= 1) {
        printf("  log %d: %s\n", level, line);
    }
}

static void s_write(void *c, gcs_conn *k, const gc_u8 *d, size_t n)
{
    (void)c, (void)k, (void)d, (void)n;
}
static void s_close(void *c, gcs_conn *k)
{
    (void)c, (void)k;
}

static gcs_direct *d;
static gc_u32 now = 1000;

static const gc_u8 IP_A[4] = { 203, 0, 113, 7 };  /* carrier NAT, two N-Gages */
static const gc_u8 IP_B[4] = { 198, 51, 100, 9 }; /* never connected over TCP */

/* one HTTP request on a fresh connection; returns the answer */
static const char *http(fake_sock *s, const gc_u8 ip[4], gc_u16 port, const char *op, const char *inner)
{
    static char req[4096];
    char body[2048];
    memset(s, 0, sizeof(*s));
    snprintf(body, sizeof(body),
             "<?xml version='1.0' encoding='UTF-8'?>\n<soapenv:Envelope "
             "xmlns:soapenv=\"http://schemas.xmlsoap.org/soap/envelope/\">\n<soapenv:Body>\n<ns1:%s "
             "xmlns:ns1=\"urn:CommunityApp\">\n%s</ns1:%s>\n</soapenv:Body>\n</soapenv:Envelope>\n\n",
             op, inner, op);
    snprintf(req, sizeof(req),
             "POST /n-gage/axis/services/Community HTTP/1.1\r\nHost: arena.n-gage.com\r\nSOAPAction: \"\"\r\n"
             "Content-Type: text/xml\r\nContent-Length: %u\r\n\r\n%s",
             (unsigned)strlen(body), body);
    CHECK_EQ(gcs_direct_tcp_open(d, s, ip, port, GCSD_HTTP, now), 0);
    /* in two pieces, as TCP may deliver it */
    gcs_direct_tcp_input(d, s, (const gc_u8 *)req, 100, now);
    gcs_direct_tcp_input(d, s, (const gc_u8 *)req + 100, strlen(req) - 100, now);
    CHECK(s->fin); /* answered, then FIN */
    gcs_direct_tcp_eof(d, s, now);
    gcs_direct_tcp_gone(d, s);
    return s->rx;
}

#define LOGIN(s, ip, port, u, p, i)                                                                 \
    http(s, ip, port, "authenticateUser2",                                                          \
         "<username xsi:type=\"xsd:string\">" u "</username>\n<password xsi:type=\"xsd:string\">" p \
         "</password>\n<imei xsi:type=\"xsd:string\">" i "</imei>\n")

/* SNAP login datagram for user */
static size_t snap_login(gc_u8 *u, const char *user)
{
    size_t ul = 16 + 120 + 4;
    memset(u, 0, ul);
    u[0] = 0x30; /* flags 0x3000 */
    u[1] = 136;  /* length */
    u[3] = 44;   /* login */
    memcpy(u + 16, user, strlen(user));
    u[ul - 4] = 0xBA;
    u[ul - 3] = 0x47;
    u[ul - 2] = 0x66;
    u[ul - 1] = 0x11;
    return ul;
}

static void test_direct(void)
{
    gcs_config sc;
    gcs_io sio;
    gcs_direct_io dio;
    gcs_core *core;
    gcs_direct_stats st;
    gcs_native_stats ns;
    gc_u8 u[200];
    size_t ul;
    const char *r;

    remove(ACC);
    gcs_default_config(&sc);
    strcpy(sc.arena_accounts, ACC);
    memset(&sio, 0, sizeof(sio));
    sio.write = s_write;
    sio.close = s_close;
    core = gcs_core_create(&sc, &sio, NULL, 1);
    CHECK(core != NULL);
    memset(&dio, 0, sizeof(dio));
    dio.tcp_send = io_tcp_send;
    dio.tcp_close = io_tcp_close;
    dio.udp_send = io_udp_send;
    dio.log = io_log;
    d = gcs_direct_create(core, &dio, NULL);
    CHECK(d != NULL);

    /* a datagram before any TCP connection from that address: dropped */
    ul = snap_login(u, "alice");
    udp_len = 0;
    gcs_direct_udp_input(d, GCSD_SNAP, IP_A, 40001, u, ul, now);
    CHECK_EQ(udp_len, 0);

    /* two N-Gages behind the same carrier address log in */
    r = LOGIN(&socks[0], IP_A, 1500, "alice", "pw1", "350000000000011");
    CHECK(strstr(r, "HTTP/1.1 200 OK") && strstr(r, "xsd:boolean\">true<") && strstr(r, "Set-Cookie: JSESSIONID=PL"));
    r = LOGIN(&socks[1], IP_A, 1501, "bob", "pw2", "350000000000012");
    CHECK(strstr(r, "xsd:boolean\">true<"));
    /* wrong password: refused */
    r = LOGIN(&socks[2], IP_A, 1502, "alice", "nope", "350000000000011");
    CHECK(strstr(r, "xsd:boolean\">false<") && !strstr(r, "Set-Cookie"));

    /* both can log in to SNAP from that address, the earlier login too */
    ul = snap_login(u, "alice");
    udp_len = 0;
    gcs_direct_udp_input(d, GCSD_SNAP, IP_A, 40001, u, ul, now);
    CHECK_EQ(udp_len, 16 + 12 + 4);
    CHECK_EQ(udp_rx[3], 41); /* login accepted */
    CHECK_EQ(udp_service, GCSD_SNAP);
    CHECK(!memcmp(udp_ip, IP_A, 4));
    CHECK_EQ(udp_port, 40001);
    ul = snap_login(u, "bob");
    udp_len = 0;
    gcs_direct_udp_input(d, GCSD_SNAP, IP_A, 40002, u, ul, now);
    CHECK_EQ(udp_len, 16 + 12 + 4);
    CHECK_EQ(udp_rx[3], 41);
    CHECK_EQ(udp_port, 40002);
    /* a name that never logged in from there: refused */
    r = LOGIN(&socks[2], IP_B, 1600, "carol", "pw3", "350000000000013");
    CHECK(strstr(r, "xsd:boolean\">true<"));
    ul = snap_login(u, "carol");
    udp_len = 0;
    gcs_direct_udp_input(d, GCSD_SNAP, IP_A, 40003, u, ul, now);
    CHECK(udp_len == 0 || udp_rx[3] != 41);

    /* a port nobody serves: reset at once */
    memset(&socks[3], 0, sizeof(socks[3]));
    CHECK_EQ(gcs_direct_tcp_open(d, &socks[3], IP_A, 1503, 1234, now), 0);
    CHECK(socks[3].reset);
    gcs_direct_tcp_gone(d, &socks[3]); /* the caller's clean-up: harmless */

    /* Jabber: the client's FIN is answered with ours */
    memset(&socks[4], 0, sizeof(socks[4]));
    CHECK_EQ(gcs_direct_tcp_open(d, &socks[4], IP_A, 1504, GCSD_XMPP, now), 0);
    gcs_direct_tcp_eof(d, &socks[4], now);
    CHECK(socks[4].fin);
    gcs_direct_tcp_gone(d, &socks[4]);

    /* an open connection whose socket fails: gcs_native forgets it */
    memset(&socks[5], 0, sizeof(socks[5]));
    CHECK_EQ(gcs_direct_tcp_open(d, &socks[5], IP_A, 1505, GCSD_XMPP, now), 0);
    gcs_native_get_stats(gcs_core_native(core), &ns);
    CHECK_EQ(ns.conns, 1);
    gcs_direct_tcp_gone(d, &socks[5]);
    gcs_native_get_stats(gcs_core_native(core), &ns);
    CHECK_EQ(ns.conns, 0);

    gcs_direct_get_stats(d, &st);
    CHECK_EQ(st.clients, 2);
    CHECK_EQ(st.conns, 0);
    CHECK_EQ(st.udp_dropped, 1);

    /* idle addresses are forgotten, then their datagrams dropped again */
    now += GCSD_IDLE_MS + 1;
    gcs_direct_poll(d, now);
    gcs_direct_get_stats(d, &st);
    CHECK_EQ(st.clients, 0);
    udp_len = 0;
    gcs_direct_udp_input(d, GCSD_SNAP, IP_A, 40001, u, ul, now);
    CHECK_EQ(udp_len, 0);

    gcs_direct_destroy(d);
    gcs_core_destroy(core);
    remove(ACC);
}

int main(void)
{
    RUN(test_direct);
    TEST_MAIN_END();
}
