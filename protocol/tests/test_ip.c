/* Host tests for gc_ip (IPv4/DNS/UDP/TCP end point). SPDX-License-Identifier: MIT */
#include "gc_ip.h"
#include "gc_test.h"

static const gc_u8 NG[4] = { 10, 64, 0, 2 };  /* the N-Gage */
static const gc_u8 BR[4] = { 10, 64, 0, 1 };  /* the bridge */
static const gc_u8 EXT[4] = { 192, 0, 2, 7 }; /* some other address */

#define MAXOUT 256
typedef struct
{
    gc_u8 d[1600];
    size_t n;
} pkt;

static pkt g_out[MAXOUT];
static int g_nout;
static gc_ip g_ip;
static gc_u8 g_rx[65536];
static size_t g_rx_len;
static int g_accept = 1, g_take = 1, g_eof, g_gone, g_gone_reset, g_writable, g_nudp;
static gc_u8 g_udp_dst[4];
static gc_u16 g_udp_dport;

static void o_send(void *ctx, const gc_u8 *d, size_t n)
{
    (void)ctx;
    CHECK(n <= GC_IP_MTU);
    if (g_nout < MAXOUT) {
        memcpy(g_out[g_nout].d, d, n);
        g_out[g_nout].n = n;
        g_nout++;
    }
}

static int o_lookup(void *ctx, const char *name, gc_u8 addr[4])
{
    (void)ctx;
    if (!strcmp(name, "arena.n-gage.com")) {
        memcpy(addr, BR, 4);
        return 1;
    }
    return 0;
}

static void o_udp(void *ctx, const gc_u8 dst[4], gc_u16 dport, gc_u16 sport, const gc_u8 *d, size_t n)
{
    (void)ctx;
    (void)sport;
    (void)d;
    (void)n;
    memcpy(g_udp_dst, dst, 4);
    g_udp_dport = dport;
    g_nudp++;
}

static int o_accept(void *ctx, int conn, const gc_u8 dst[4], gc_u16 dport, gc_u16 sport)
{
    (void)ctx;
    (void)conn;
    (void)dst;
    (void)sport;
    return g_accept && dport != 23;
}

static int o_data(void *ctx, int conn, const gc_u8 *d, size_t n)
{
    (void)ctx;
    (void)conn;
    if (!g_take) {
        return 0;
    }
    memcpy(g_rx + g_rx_len, d, n);
    g_rx_len += n;
    return 1;
}

static void o_eof(void *ctx, int conn)
{
    (void)ctx;
    (void)conn;
    g_eof++;
}

static void o_gone(void *ctx, int conn, int reset)
{
    (void)ctx;
    (void)conn;
    g_gone++;
    g_gone_reset += reset;
}

static void o_writable(void *ctx, int conn)
{
    (void)ctx;
    (void)conn;
    g_writable++;
}

static void setup(void)
{
    gc_ip_ops ops = { o_send, o_lookup, o_udp, o_accept, o_data, o_eof, o_gone, o_writable, NULL };
    gc_ip_init(&g_ip, &ops, NULL, 42);
    g_nout = 0;
    g_rx_len = 0;
    g_accept = g_take = 1;
    g_eof = g_gone = g_gone_reset = g_writable = g_nudp = 0;
}

static gc_u16 r16(const gc_u8 *p)
{
    return (gc_u16)((p[0] << 8) | p[1]);
}

static gc_u32 r32(const gc_u8 *p)
{
    return ((gc_u32)p[0] << 24) | ((gc_u32)p[1] << 16) | ((gc_u32)p[2] << 8) | p[3];
}

static void w16(gc_u8 *p, gc_u16 v)
{
    p[0] = (gc_u8)(v >> 8);
    p[1] = (gc_u8)v;
}

static void w32(gc_u8 *p, gc_u32 v)
{
    w16(p, (gc_u16)(v >> 16));
    w16(p + 2, (gc_u16)v);
}

static gc_u32 pseudo(const gc_u8 *s, const gc_u8 *d, gc_u8 proto, size_t len)
{
    return gc_ip_sum(gc_ip_sum(0, s, 4), d, 4) + proto + (gc_u32)len;
}

/* checks an outgoing packet's IP and L4 checksums */
static void check_out(const pkt *p)
{
    size_t hl = (size_t)(p->d[0] & 15) * 4;
    CHECK_EQ(p->d[0] >> 4, 4);
    CHECK_EQ(r16(p->d + 2), p->n);
    CHECK_EQ(gc_ip_fold(gc_ip_sum(0, p->d, hl)), 0);
    if (p->d[9] == 6 || p->d[9] == 17) {
        CHECK_EQ(gc_ip_fold(gc_ip_sum(pseudo(p->d + 12, p->d + 16, p->d[9], p->n - hl), p->d + hl, p->n - hl)), 0);
    }
}

static void send_ip(const gc_u8 *dst, gc_u8 proto, const gc_u8 *pl, size_t n, gc_u32 now)
{
    gc_u8 b[1600];
    memset(b, 0, 20);
    b[0] = 0x45;
    w16(b + 2, (gc_u16)(20 + n));
    b[8] = 64;
    b[9] = proto;
    memcpy(b + 12, NG, 4);
    memcpy(b + 16, dst, 4);
    w16(b + 10, gc_ip_fold(gc_ip_sum(0, b, 20)));
    memcpy(b + 20, pl, n);
    gc_ip_input(&g_ip, b, 20 + n, now);
}

static void send_tcp(const gc_u8 *dst, gc_u16 sport, gc_u16 dport, gc_u32 seq, gc_u32 ack, gc_u8 fl, gc_u16 wnd,
                     const gc_u8 *d, size_t n, gc_u32 now)
{
    gc_u8 t[1600];
    size_t hl = (fl & 2) ? 24 : 20;
    memset(t, 0, hl);
    w16(t, sport);
    w16(t + 2, dport);
    w32(t + 4, seq);
    w32(t + 8, ack);
    t[12] = (gc_u8)((hl / 4) << 4);
    t[13] = fl;
    w16(t + 14, wnd);
    if (fl & 2) {
        t[20] = 2;
        t[21] = 4;
        w16(t + 22, 1000);
    }
    if (n) {
        memcpy(t + hl, d, n);
    }
    w16(t + 16, gc_ip_fold(gc_ip_sum(pseudo(NG, dst, 6, hl + n), t, hl + n)));
    send_ip(dst, 6, t, hl + n, now);
}

/* fields of an outgoing TCP segment */
typedef struct
{
    gc_u16 sport, dport, wnd;
    gc_u32 seq, ack;
    gc_u8 fl;
    const gc_u8 *d;
    size_t n;
    gc_u8 src[4];
} seg;

static seg tseg(int i)
{
    seg s;
    const gc_u8 *p = g_out[i].d;
    size_t hl = (size_t)(p[0] & 15) * 4, th;
    const gc_u8 *t = p + hl;
    memset(&s, 0, sizeof(s));
    check_out(&g_out[i]);
    CHECK_EQ(p[9], 6);
    th = (size_t)(t[12] >> 4) * 4;
    s.sport = r16(t);
    s.dport = r16(t + 2);
    s.seq = r32(t + 4);
    s.ack = r32(t + 8);
    s.fl = t[13];
    s.wnd = r16(t + 14);
    s.d = t + th;
    s.n = g_out[i].n - hl - th;
    memcpy(s.src, p + 12, 4);
    return s;
}

static void test_icmp_echo(void)
{
    gc_u8 e[12] = { 8, 0, 0, 0, 0x12, 0x34, 0, 1, 'a', 'b', 'c', 'd' };
    setup();
    w16(e + 2, gc_ip_fold(gc_ip_sum(0, e, sizeof(e))));
    send_ip(EXT, 1, e, sizeof(e), 0);
    CHECK_EQ(g_nout, 1);
    check_out(&g_out[0]);
    CHECK(!memcmp(g_out[0].d + 12, EXT, 4)); /* answered from the address pinged */
    CHECK(!memcmp(g_out[0].d + 16, NG, 4));
    CHECK_EQ(g_out[0].d[20], 0);
    CHECK(!memcmp(g_out[0].d + 24, e + 4, 8));
    CHECK_EQ(gc_ip_fold(gc_ip_sum(0, g_out[0].d + 20, 12)), 0);
}

static size_t dns_query(gc_u8 *u, const char *name, gc_u16 qtype)
{
    gc_u8 *q = u + 8;
    size_t n = 12;
    const char *s = name;
    memset(u, 0, 20);
    w16(u, 1234);
    w16(u + 2, 53);
    w16(q, 0xBEEF);
    q[2] = 1; /* RD */
    w16(q + 4, 1);
    while (*s) {
        const char *e = strchr(s, '.');
        size_t l = e ? (size_t)(e - s) : strlen(s);
        q[n++] = (gc_u8)l;
        memcpy(q + n, s, l);
        n += l;
        s += l + (e ? 1 : 0);
    }
    q[n++] = 0;
    w16(q + n, qtype);
    w16(q + n + 2, 1);
    n += 4;
    w16(u + 4, (gc_u16)(n + 8));
    w16(u + 6, 0);
    w16(u + 6, gc_ip_fold(gc_ip_sum(pseudo(NG, BR, 17, n + 8), u, n + 8)));
    return n + 8;
}

static void test_dns(void)
{
    gc_u8 u[300];
    size_t n;
    const gc_u8 *a;
    setup();
    n = dns_query(u, "ARENA.n-gage.com", 1);
    send_ip(BR, 17, u, n, 0);
    CHECK_EQ(g_nout, 1);
    check_out(&g_out[0]);
    a = g_out[0].d + 28;
    CHECK_EQ(r16(g_out[0].d + 20), 53);
    CHECK_EQ(r16(g_out[0].d + 22), 1234);
    CHECK_EQ(r16(a), 0xBEEF);
    CHECK_EQ(a[3] & 15, 0);
    CHECK_EQ(r16(a + 6), 1);
    CHECK(!memcmp(g_out[0].d + g_out[0].n - 4, BR, 4));

    n = dns_query(u, "example.org", 1);
    send_ip(BR, 17, u, n, 1);
    CHECK_EQ(g_nout, 2);
    CHECK_EQ(g_out[1].d[28 + 3] & 15, 3); /* NXDOMAIN */
    CHECK_EQ(r16(g_out[1].d + 28 + 6), 0);

    n = dns_query(u, "arena.n-gage.com", 28); /* AAAA: no answer, no error */
    send_ip(BR, 17, u, n, 2);
    CHECK_EQ(g_out[2].d[28 + 3] & 15, 0);
    CHECK_EQ(r16(g_out[2].d + 28 + 6), 0);

    /* corrupt checksum: dropped */
    n = dns_query(u, "arena.n-gage.com", 1);
    u[9] ^= 0x40;
    send_ip(BR, 17, u, n, 3);
    CHECK_EQ(g_nout, 3);
    CHECK_EQ(g_nudp, 0);
}

static void test_udp(void)
{
    gc_u8 u[16] = { 0x04, 0xD2, 0x1F, 0x90, 0, 16, 0, 0, 'h', 'e', 'l', 'l', 'o', '!', '!', '!' };
    gc_u8 reply[3] = { 'a', 'b', 'c' };
    setup();
    CHECK_EQ(gc_ip_udp_send(&g_ip, EXT, 8080, 1234, reply, 3), -1); /* peer unknown */
    send_ip(EXT, 17, u, sizeof(u), 0);                              /* checksum 0: not checked */
    CHECK_EQ(g_nudp, 1);
    CHECK_EQ(g_udp_dport, 8080);
    CHECK(!memcmp(g_udp_dst, EXT, 4));
    CHECK_EQ(gc_ip_udp_send(&g_ip, EXT, 8080, 1234, reply, 3), 0);
    CHECK_EQ(g_nout, 1);
    check_out(&g_out[0]);
    CHECK(!memcmp(g_out[0].d + 12, EXT, 4));
    CHECK(!memcmp(g_out[0].d + 16, NG, 4));
    CHECK(!memcmp(g_out[0].d + 28, "abc", 3));
}

/* handshake; returns our ISS + 1 */
static gc_u32 open_conn(gc_u16 sport, gc_u16 dport, gc_u32 iss, gc_u32 now)
{
    seg s;
    int k = g_nout;
    send_tcp(BR, sport, dport, iss, 0, 0x02, 8000, NULL, 0, now);
    CHECK_EQ(g_nout, k + 1);
    s = tseg(k);
    CHECK_EQ(s.fl, 0x12);
    CHECK_EQ(s.ack, iss + 1);
    CHECK_EQ(s.dport, sport);
    send_tcp(BR, sport, dport, iss + 1, s.seq + 1, 0x10, 8000, NULL, 0, now);
    CHECK_EQ(g_ip.tcp[0].state == GC_TCP_ESTAB || g_ip.tcp[1].state == GC_TCP_ESTAB, 1);
    return s.seq + 1;
}

static void test_tcp_refused(void)
{
    seg s;
    setup();
    send_tcp(BR, 1000, 23, 500, 0, 0x02, 8000, NULL, 0, 0);
    CHECK_EQ(g_nout, 1);
    s = tseg(0);
    CHECK_EQ(s.fl, 0x14); /* RST|ACK */
    CHECK_EQ(s.ack, 501);
    /* stray segment without connection */
    send_tcp(BR, 1000, 80, 500, 777, 0x10, 8000, NULL, 0, 0);
    s = tseg(1);
    CHECK_EQ(s.fl, 0x04);
    CHECK_EQ(s.seq, 777);
}

static void test_tcp_exchange(void)
{
    gc_u32 ours, theirs = 1000;
    seg s;
    int k;
    const char *req = "POST /n-gage/axis/services/Community HTTP/1.1\r\n\r\n";
    setup();
    ours = open_conn(1757, 80, theirs, 0);
    theirs++;
    k = g_nout;
    send_tcp(BR, 1757, 80, theirs, ours, 0x18, 8000, (const gc_u8 *)req, strlen(req), 10);
    theirs += (gc_u32)strlen(req);
    CHECK_EQ(g_rx_len, strlen(req));
    CHECK(!memcmp(g_rx, req, g_rx_len));
    s = tseg(k);
    CHECK_EQ(s.ack, theirs);
    /* duplicate: re-acked, not delivered twice */
    send_tcp(BR, 1757, 80, theirs - (gc_u32)strlen(req), ours, 0x18, 8000, (const gc_u8 *)req, strlen(req), 11);
    CHECK_EQ(g_rx_len, strlen(req));
    CHECK_EQ(tseg(g_nout - 1).ack, theirs);

    /* answer: 3000 bytes, peer MSS 1000 -> 3 segments */
    {
        gc_u8 big[3000];
        int i;
        for (i = 0; i < 3000; i++) {
            big[i] = (gc_u8)i;
        }
        k = g_nout;
        CHECK_EQ(gc_ip_tcp_send(&g_ip, 0, big, sizeof(big), 20), 3000);
        CHECK_EQ(g_nout, k + 3);
        CHECK_EQ(tseg(k).n, 1000);
        CHECK_EQ(tseg(k + 2).seq, ours + 2000);
        CHECK(!memcmp(tseg(k + 1).d, big + 1000, 1000));
    }
    gc_ip_tcp_close(&g_ip, 0, 21);
    s = tseg(g_nout - 1);
    CHECK_EQ(s.fl & 1, 1);
    CHECK_EQ(s.seq, ours + 3000);
    CHECK_EQ(g_ip.tcp[0].state, GC_TCP_FIN_WAIT_1);
    /* peer acks everything and its FIN */
    send_tcp(BR, 1757, 80, theirs, ours + 3001, 0x11, 8000, NULL, 0, 30);
    CHECK_EQ(g_eof, 1);
    CHECK_EQ(g_gone, 1);
    CHECK_EQ(g_gone_reset, 0);
    CHECK_EQ(g_ip.tcp[0].state, GC_TCP_FREE);
    CHECK_EQ(tseg(g_nout - 1).ack, theirs + 1);
}

static void test_tcp_peer_closes_first(void)
{
    gc_u32 ours, theirs = 5000;
    seg s;
    setup();
    ours = open_conn(1800, 5222, theirs, 0);
    theirs++;
    send_tcp(BR, 1800, 5222, theirs, ours, 0x19, 8000, (const gc_u8 *)"bye", 3, 5);
    CHECK_EQ(g_rx_len, 3);
    CHECK_EQ(g_eof, 1);
    CHECK_EQ(g_ip.tcp[0].state, GC_TCP_CLOSE_WAIT);
    CHECK_EQ(tseg(g_nout - 1).ack, theirs + 4);
    CHECK_EQ(gc_ip_tcp_send(&g_ip, 0, (const gc_u8 *)"ok", 2, 6), 2);
    gc_ip_tcp_close(&g_ip, 0, 6);
    s = tseg(g_nout - 1);
    CHECK_EQ(s.fl & 1, 1);
    CHECK_EQ(g_ip.tcp[0].state, GC_TCP_LAST_ACK);
    send_tcp(BR, 1800, 5222, theirs + 4, ours + 3, 0x10, 8000, NULL, 0, 7);
    CHECK_EQ(g_ip.tcp[0].state, GC_TCP_FREE);
    CHECK_EQ(g_gone, 1);
}

static void test_tcp_fin_wait_2_timeout(void)
{
    gc_u32 ours;
    setup();
    ours = open_conn(2500, 80, 1, 0);
    gc_ip_tcp_close(&g_ip, 0, 10);
    send_tcp(BR, 2500, 80, 2, ours + 1, 0x10, 8000, NULL, 0, 20); /* FIN acked, no FIN from the peer */
    CHECK_EQ(g_ip.tcp[0].state, GC_TCP_FIN_WAIT_2);
    gc_ip_poll(&g_ip, 20000);
    CHECK_EQ(g_ip.tcp[0].state, GC_TCP_FIN_WAIT_2);
    gc_ip_poll(&g_ip, 31000);
    CHECK_EQ(g_ip.tcp[0].state, GC_TCP_FREE);
    CHECK_EQ(g_gone, 1);
    CHECK_EQ(g_gone_reset, 0);
}

static void test_tcp_retransmit_and_timeout(void)
{
    gc_u32 ours, theirs = 9000;
    int k, i;
    setup();
    ours = open_conn(2000, 80, theirs, 0);
    k = g_nout;
    CHECK_EQ(gc_ip_tcp_send(&g_ip, 0, (const gc_u8 *)"hello", 5, 100), 5);
    CHECK_EQ(g_nout, k + 1);
    gc_ip_poll(&g_ip, 600);
    CHECK_EQ(g_nout, k + 1);
    gc_ip_poll(&g_ip, 1200); /* RTO 1 s */
    CHECK_EQ(g_nout, k + 2);
    CHECK_EQ(tseg(k + 1).seq, ours);
    CHECK_EQ(tseg(k + 1).n, 5);
    /* acked now: no more retransmissions */
    send_tcp(BR, 2000, 80, theirs + 1, ours + 5, 0x10, 8000, NULL, 0, 1300);
    CHECK(g_writable >= 1);
    gc_ip_poll(&g_ip, 10000);
    CHECK_EQ(g_nout, k + 2);
    /* never acked: reset after the retries */
    CHECK_EQ(gc_ip_tcp_send(&g_ip, 0, (const gc_u8 *)"x", 1, 20000), 1);
    for (i = 0; i < 40; i++) {
        gc_ip_poll(&g_ip, 20000 + (gc_u32)i * 16000);
    }
    CHECK_EQ(g_gone, 1);
    CHECK_EQ(g_gone_reset, 1);
    CHECK_EQ(tseg(g_nout - 1).fl & 4, 4);
}

static void test_tcp_backpressure_and_window(void)
{
    gc_u32 ours, theirs = 100;
    gc_u8 buf[GC_TCP_TXBUF + 100];
    int k;
    setup();
    ours = open_conn(3000, 80, theirs, 0);
    theirs++;
    g_take = 0;
    send_tcp(BR, 3000, 80, theirs, ours, 0x18, 8000, (const gc_u8 *)"abc", 3, 1);
    CHECK_EQ(g_rx_len, 0);
    g_take = 1;
    send_tcp(BR, 3000, 80, theirs, ours, 0x18, 8000, (const gc_u8 *)"abc", 3, 2);
    CHECK_EQ(g_rx_len, 3);
    theirs += 3;

    /* peer window 0: nothing goes out until it opens */
    send_tcp(BR, 3000, 80, theirs, ours, 0x10, 0, NULL, 0, 3);
    memset(buf, 'z', sizeof(buf));
    k = g_nout;
    CHECK_EQ(gc_ip_tcp_send(&g_ip, 0, buf, sizeof(buf), 4), GC_TCP_TXBUF);
    CHECK_EQ(g_nout, k);
    CHECK_EQ(gc_ip_tcp_space(&g_ip, 0), 0);
    gc_ip_poll(&g_ip, 1100); /* persist probe */
    CHECK_EQ(g_nout, k + 1);
    CHECK_EQ(tseg(k).n, 1);
    send_tcp(BR, 3000, 80, theirs, ours + 1, 0x10, 2500, NULL, 0, 1200);
    CHECK(g_nout >= k + 3); /* 1000 + 1000 + 500 within the window */
    CHECK(gc_ip_tcp_space(&g_ip, 0) >= 1);
}

static void test_tcp_rst_and_reset(void)
{
    gc_u32 ours;
    setup();
    ours = open_conn(4000, 80, 1, 0);
    send_tcp(BR, 4000, 80, 2, ours, 0x04, 0, NULL, 0, 1);
    CHECK_EQ(g_gone, 1);
    CHECK_EQ(g_gone_reset, 1);
    open_conn(4001, 80, 1, 2);
    gc_ip_reset(&g_ip);
    CHECK_EQ(g_gone, 2);
    CHECK_EQ(gc_ip_tcp_send(&g_ip, 0, (const gc_u8 *)"x", 1, 3), -1);
}

static void test_tcp_many_and_lost_synack(void)
{
    int i, k;
    setup();
    for (i = 0; i < GC_TCP_CONNS; i++) {
        send_tcp(BR, (gc_u16)(5000 + i), 80, 10, 0, 0x02, 8000, NULL, 0, 0);
    }
    k = g_nout;
    send_tcp(BR, 6000, 80, 10, 0, 0x02, 8000, NULL, 0, 0); /* table full */
    CHECK_EQ(tseg(k).fl & 4, 4);
    /* repeated SYN: SYN-ACK again */
    send_tcp(BR, 5000, 80, 10, 0, 0x02, 8000, NULL, 0, 1);
    CHECK_EQ(tseg(g_nout - 1).fl, 0x12);
    gc_ip_poll(&g_ip, 1500);
    CHECK(g_nout > k + 2);
}

static void test_garbage(void)
{
    int r, i;
    gc_u8 b[600];
    setup();
    open_conn(7000, 80, 1, 0);
    for (r = 0; r < 5000; r++) {
        size_t n = gct_rand() % sizeof(b);
        for (i = 0; i < (int)n; i++) {
            b[i] = (gc_u8)gct_rand();
        }
        if (n > 20 && (r & 1)) { /* valid header, random payload */
            b[0] = 0x45;
            w16(b + 2, (gc_u16)n);
            w16(b + 6, 0);
            b[9] = (gc_u8)(r % 3 == 0 ? 6 : r % 3 == 1 ? 17
                                                       : 1);
            memcpy(b + 12, NG, 4);
            w16(b + 10, 0);
            w16(b + 10, gc_ip_fold(gc_ip_sum(0, b, 20)));
        }
        g_nout = 0;
        gc_ip_input(&g_ip, b, n, (gc_u32)r);
        gc_ip_poll(&g_ip, (gc_u32)r);
    }
    CHECK(1);
}

int main(void)
{
    RUN(test_icmp_echo);
    RUN(test_dns);
    RUN(test_udp);
    RUN(test_tcp_refused);
    RUN(test_tcp_exchange);
    RUN(test_tcp_peer_closes_first);
    RUN(test_tcp_fin_wait_2_timeout);
    RUN(test_tcp_retransmit_and_timeout);
    RUN(test_tcp_backpressure_and_window);
    RUN(test_tcp_rst_and_reset);
    RUN(test_tcp_many_and_lost_synack);
    RUN(test_garbage);
    TEST_MAIN_END();
}
