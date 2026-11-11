/* Host tests for gc_ppp (PPP peer of the bridges). SPDX-License-Identifier: MIT */
#include "gc_ppp.h"
#include "gc_test.h"

/* ---- capture of what the PPP peer writes, deframed into packets ---- */

#define MAXPK 64
typedef struct
{
    gc_u16 proto;
    gc_u8 d[1600];
    size_t n;
} pk;

typedef struct
{
    gc_u8 raw[65536];
    size_t raw_len;
    pk pk[MAXPK];
    int npk;
    int ev[16];
    int nev;
    gc_u8 ip[1600];
    size_t ip_len;
    int nip;
} cap;

static void deframe(cap *c)
{
    size_t i;
    gc_u8 f[1700];
    size_t n = 0;
    int esc = 0;
    c->npk = 0;
    for (i = 0; i < c->raw_len; i++) {
        gc_u8 b = c->raw[i];
        if (b == 0x7E) {
            if (n >= 6 && c->npk < MAXPK) {
                pk *k = &c->pk[c->npk++];
                CHECK_EQ(gc_ppp_fcs(GC_PPP_FCS_INIT, f, n), GC_PPP_FCS_GOOD);
                CHECK_EQ(f[0], 0xFF);
                CHECK_EQ(f[1], 0x03);
                k->proto = (gc_u16)((f[2] << 8) | f[3]);
                k->n = n - 6;
                memcpy(k->d, f + 4, k->n);
            }
            n = 0;
            esc = 0;
        } else if (b == 0x7D) {
            esc = 1;
        } else {
            if (esc) {
                b ^= 0x20;
                esc = 0;
            }
            if (n < sizeof(f)) {
                f[n++] = b;
            }
        }
    }
}

static void w(void *ctx, const gc_u8 *d, size_t n)
{
    cap *c = (cap *)ctx;
    if (c->raw_len + n <= sizeof(c->raw)) {
        memcpy(c->raw + c->raw_len, d, n);
        c->raw_len += n;
    }
}

static void on_ip(void *ctx, const gc_u8 *d, size_t n)
{
    cap *c = (cap *)ctx;
    memcpy(c->ip, d, n);
    c->ip_len = n;
    c->nip++;
}

static void on_state(void *ctx, int ev)
{
    cap *c = (cap *)ctx;
    if (c->nev < 16) {
        c->ev[c->nev++] = ev;
    }
}

static gc_ppp g_p;
static cap g_c;

static void setup(void)
{
    gc_ppp_cfg cfg;
    gc_ppp_ops ops = { w, on_ip, on_state, NULL };
    gc_ppp_default_cfg(&cfg);
    cfg.magic = 0x11223344u;
    memset(&g_c, 0, sizeof(g_c));
    gc_ppp_init(&g_p, &cfg, &ops, &g_c);
}

/* send a packet to the PPP peer as the N-Gage would (default ACCM, ACFC
 * not used) */
static void feed(gc_u16 proto, const gc_u8 *d, size_t n, gc_u32 now)
{
    gc_u8 f[4000];
    size_t len = gc_ppp_frame(proto, d, n, 0xFFFFFFFFu, f, sizeof(f));
    CHECK(len > 0);
    g_c.raw_len = 0;
    gc_ppp_input(&g_p, f, len, now);
    deframe(&g_c);
}

static const pk *find(gc_u16 proto, gc_u8 code)
{
    int i;
    for (i = 0; i < g_c.npk; i++) {
        if (g_c.pk[i].proto == proto && g_c.pk[i].n >= 4 && g_c.pk[i].d[0] == code) {
            return &g_c.pk[i];
        }
    }
    return NULL;
}

static void test_fcs(void)
{
    /* RFC 1662: FCS over data plus its own FCS gives the good value */
    gc_u8 d[6] = { 0xFF, 0x03, 0xC0, 0x21, 0, 0 };
    gc_u16 f = (gc_u16)~gc_ppp_fcs(GC_PPP_FCS_INIT, d, 4);
    d[4] = (gc_u8)f;
    d[5] = (gc_u8)(f >> 8);
    CHECK_EQ(gc_ppp_fcs(GC_PPP_FCS_INIT, d, 6), GC_PPP_FCS_GOOD);
    CHECK(gc_ppp_detect(d, 0) == 0);
    d[0] = 0x7E;
    CHECK(gc_ppp_detect(d, 1));
}

static void test_framing_escapes(void)
{
    gc_u8 info[3] = { 0x7E, 0x7D, 0x01 }, out[64];
    size_t n = gc_ppp_frame(0x0021, info, 3, 0, out, sizeof(out));
    size_t i;
    int flags = 0;
    CHECK(n > 0);
    for (i = 0; i < n; i++) {
        flags += out[i] == 0x7E;
    }
    CHECK_EQ(flags, 2); /* only the delimiters */
    /* with ACCM 0, 0x01 stays raw; with all set it is escaped */
    CHECK(memchr(out, 0x01, n) != NULL);
    n = gc_ppp_frame(0x0021, info, 3, 0xFFFFFFFFu, out, sizeof(out));
    CHECK(memchr(out, 0x01, n) == NULL);
    CHECK_EQ(gc_ppp_frame(0x0021, info, 3, 0, out, 5), 0);
}

/* the LCP request of a Symbian 6.1 client: MRU, ACCM 0, magic, PFC, ACFC */
static const gc_u8 lcp_req[] = { 1, 1, 0, 24, 1, 4, 0x05, 0xDC, 2, 6, 0, 0, 0, 0, 5, 6, 0xAB, 0xCD, 0xEF, 0x01, 7, 2, 8, 2 };

static void lcp_up(void)
{
    const pk *k;
    gc_u8 ack[4];
    feed(GC_PPP_LCP, lcp_req, sizeof(lcp_req), 0);
    k = find(GC_PPP_LCP, 1);
    CHECK(k != NULL); /* our own request */
    CHECK(find(GC_PPP_LCP, 2) != NULL);
    CHECK_EQ(g_p.lcp.state, GC_PPP_ST_ACKSENT);
    if (!k) {
        return;
    }
    ack[0] = 2;
    ack[1] = k->d[1];
    ack[2] = 0;
    ack[3] = 4; /* a short Ack is enough for the id check */
    feed(GC_PPP_LCP, ack, 4, 10);
    CHECK_EQ(g_p.lcp.state, GC_PPP_ST_OPENED);
    CHECK_EQ(g_p.tx_accm, 0);
    CHECK_EQ(g_p.peer_magic, 0xABCDEF01u);
    /* IPCP request of ours went out right away */
    CHECK(find(GC_PPP_IPCP, 1) != NULL);
}

static void test_lcp_open(void)
{
    const pk *a;
    setup();
    feed(GC_PPP_LCP, lcp_req, sizeof(lcp_req), 0);
    a = find(GC_PPP_LCP, 2);
    CHECK(a != NULL);
    if (a) {
        CHECK_EQ(a->n, sizeof(lcp_req));
        CHECK(!memcmp(a->d + 4, lcp_req + 4, sizeof(lcp_req) - 4));
    }
    setup();
    lcp_up();
}

static void test_lcp_reject_auth_and_unknown(void)
{
    static const gc_u8 req[] = { 1, 7, 0, 17, 3, 4, 0xC0, 0x23, 13, 3, 6, 5, 6, 1, 2, 3, 4 };
    const pk *r;
    setup();
    feed(GC_PPP_LCP, req, sizeof(req), 0);
    r = find(GC_PPP_LCP, 4);
    CHECK(r != NULL);
    if (r) {
        CHECK_EQ(r->d[1], 7);
        CHECK_EQ(r->n, 4 + 4 + 3); /* auth + callback rejected, magic not */
        CHECK_EQ(r->d[4], 3);
        CHECK_EQ(r->d[8], 13);
    }
    CHECK(find(GC_PPP_LCP, 2) == NULL);
    CHECK_EQ(g_p.lcp.state, GC_PPP_ST_REQSENT);
}

static void test_lcp_magic_loop(void)
{
    static const gc_u8 req[] = { 1, 2, 0, 10, 5, 6, 0x11, 0x22, 0x33, 0x44 };
    const pk *n;
    setup();
    feed(GC_PPP_LCP, req, sizeof(req), 0);
    n = find(GC_PPP_LCP, 3);
    CHECK(n != NULL);
    if (n) {
        CHECK_EQ(n->d[4], 5);
        CHECK(memcmp(n->d + 6, "\x11\x22\x33\x44", 4) != 0);
    }
}

static void ipcp_up(void)
{
    /* Symbian: address 0, both DNS 0, NBNS, VJ compression */
    static const gc_u8 r1[] = { 1, 1, 0, 40, 2, 6, 0, 0x2D, 0x0F, 1, 3, 6, 0, 0, 0, 0, 129, 6, 0, 0, 0, 0,
                                130, 6, 0, 0, 0, 0, 131, 6, 0, 0, 0, 0, 132, 6, 0, 0, 0, 0 };
    static const gc_u8 r2[] = { 1, 2, 0, 22, 3, 6, 0, 0, 0, 0, 129, 6, 0, 0, 0, 0, 131, 6, 0, 0, 0, 0 };
    static const gc_u8 r3[] = { 1, 3, 0, 22, 3, 6, 10, 64, 0, 2, 129, 6, 10, 64, 0, 1, 131, 6, 10, 64, 0, 1 };
    const pk *k, *our;
    gc_u8 ack[4];
    our = find(GC_PPP_IPCP, 1);
    CHECK(our != NULL);
    if (!our) {
        return;
    }
    ack[0] = 2;
    ack[1] = our->d[1];
    ack[2] = 0;
    ack[3] = 4;
    CHECK_EQ(our->d[4], 3);
    CHECK(!memcmp(our->d + 6, "\x0a\x40\x00\x01", 4));

    feed(GC_PPP_IPCP, r1, sizeof(r1), 20);
    k = find(GC_PPP_IPCP, 4);
    CHECK(k != NULL);
    if (k) {
        CHECK_EQ(k->n, 4 + 6 + 6 + 6); /* VJ, both NBNS */
    }
    feed(GC_PPP_IPCP, r2, sizeof(r2), 30);
    k = find(GC_PPP_IPCP, 3);
    CHECK(k != NULL);
    if (k) {
        CHECK_EQ(k->n, 22);
        CHECK(!memcmp(k->d + 4, r3 + 4, 18)); /* the Nak carries our values */
    }
    feed(GC_PPP_IPCP, r3, sizeof(r3), 40);
    CHECK(find(GC_PPP_IPCP, 2) != NULL);
    CHECK_EQ(g_p.ipcp.state, GC_PPP_ST_ACKSENT);
    CHECK_EQ(g_c.nev, 0);
    feed(GC_PPP_IPCP, ack, 4, 50);
    CHECK_EQ(g_p.ipcp.state, GC_PPP_ST_OPENED);
    CHECK(gc_ppp_is_up(&g_p));
    CHECK_EQ(g_c.nev, 1);
    CHECK_EQ(g_c.ev[0], GC_PPP_EV_UP);
}

static void test_ipcp_and_ip(void)
{
    gc_u8 pkt[40];
    setup();
    CHECK_EQ(gc_ppp_send_ip(&g_p, pkt, 20), -1);
    lcp_up();
    ipcp_up();
    memset(pkt, 0x7E, sizeof(pkt));
    feed(GC_PPP_IP, pkt, sizeof(pkt), 60);
    CHECK_EQ(g_c.nip, 1);
    CHECK_EQ(g_c.ip_len, sizeof(pkt));
    g_c.raw_len = 0;
    CHECK_EQ(gc_ppp_send_ip(&g_p, pkt, sizeof(pkt)), 0);
    deframe(&g_c);
    CHECK_EQ(g_c.npk, 1);
    CHECK_EQ(g_c.pk[0].proto, GC_PPP_IP);
    CHECK_EQ(g_c.pk[0].n, sizeof(pkt));
}

static void test_compressed_fields(void)
{
    /* PFC + ACFC: IP packet as 0x21 + payload, no FF 03 */
    gc_u8 raw[64], info[21];
    size_t n = 0, i;
    gc_u16 fcs;
    setup();
    lcp_up();
    ipcp_up();
    info[0] = 0x21;
    for (i = 1; i < sizeof(info); i++) {
        info[i] = (gc_u8)i;
    }
    fcs = (gc_u16)~gc_ppp_fcs(GC_PPP_FCS_INIT, info, sizeof(info));
    raw[n++] = 0x7E;
    for (i = 0; i < sizeof(info) + 2; i++) {
        gc_u8 b = i < sizeof(info) ? info[i] : (gc_u8)(i == sizeof(info) ? fcs : fcs >> 8);
        if (b == 0x7E || b == 0x7D || b < 0x20) {
            raw[n++] = 0x7D;
            b ^= 0x20;
        }
        raw[n++] = b;
    }
    raw[n++] = 0x7E;
    gc_ppp_input(&g_p, raw, n, 70);
    CHECK_EQ(g_c.nip, 1);
    CHECK_EQ(g_c.ip_len, 20);
    /* corrupt FCS is dropped */
    raw[3] ^= 1;
    gc_ppp_input(&g_p, raw, n, 71);
    CHECK_EQ(g_c.nip, 1);
    CHECK_EQ(g_p.st_rx_bad_fcs, 1);
}

static void test_echo_and_protocol_reject(void)
{
    static const gc_u8 echo[] = { 9, 5, 0, 12, 0xAB, 0xCD, 0xEF, 0x01, 'p', 'i', 'n', 'g' };
    static const gc_u8 v6[] = { 1, 1, 0, 14, 1, 10, 0, 0, 0, 0, 0, 0, 0, 1 };
    const pk *k;
    setup();
    lcp_up();
    feed(GC_PPP_LCP, echo, sizeof(echo), 20);
    k = find(GC_PPP_LCP, 10);
    CHECK(k != NULL);
    if (k) {
        CHECK_EQ(k->d[1], 5);
        CHECK(!memcmp(k->d + 4, "\x11\x22\x33\x44ping", 8));
    }
    feed(0x8057, v6, sizeof(v6), 30); /* IPV6CP */
    k = find(GC_PPP_LCP, 8);
    CHECK(k != NULL);
    if (k) {
        CHECK_EQ(k->d[4], 0x80);
        CHECK_EQ(k->d[5], 0x57);
        CHECK(!memcmp(k->d + 6, v6, sizeof(v6)));
    }
}

static void test_terminate(void)
{
    static const gc_u8 tr[] = { 5, 9, 0, 4 };
    setup();
    lcp_up();
    ipcp_up();
    feed(GC_PPP_LCP, tr, sizeof(tr), 100);
    CHECK(find(GC_PPP_LCP, 6) != NULL);
    CHECK(!gc_ppp_is_up(&g_p));
    CHECK_EQ(g_c.nev, 3);
    CHECK_EQ(g_c.ev[1], GC_PPP_EV_DOWN);
    CHECK_EQ(g_c.ev[2], GC_PPP_EV_DEAD);
    CHECK_EQ(g_p.lcp.state, GC_PPP_ST_STOPPED);
}

static void test_close_and_timeouts(void)
{
    int i;
    setup();
    gc_ppp_open(&g_p, 0);
    CHECK_EQ(g_p.lcp.state, GC_PPP_ST_REQSENT);
    /* no answer: 10 retries 3 s apart, then dead */
    for (i = 1; i <= 12; i++) {
        g_c.raw_len = 0;
        gc_ppp_poll(&g_p, (gc_u32)i * 3000);
    }
    CHECK_EQ(g_p.lcp.state, GC_PPP_ST_STOPPED);
    CHECK_EQ(g_c.nev, 1);
    CHECK_EQ(g_c.ev[0], GC_PPP_EV_DEAD);

    setup();
    lcp_up();
    gc_ppp_close(&g_p, 1000);
    deframe(&g_c);
    CHECK(find(GC_PPP_LCP, 5) != NULL);
    CHECK_EQ(g_p.lcp.state, GC_PPP_ST_CLOSING);
    {
        gc_u8 ta[4];
        ta[0] = 6;
        ta[1] = g_p.lcp.id;
        ta[2] = 0;
        ta[3] = 4;
        feed(GC_PPP_LCP, ta, 4, 1100);
    }
    CHECK_EQ(g_p.lcp.state, GC_PPP_ST_STOPPED);
    CHECK_EQ(g_c.ev[g_c.nev - 1], GC_PPP_EV_DEAD);
}

/* two of our peers back to back must reach IPCP opened on both sides */
static gc_ppp g_a, g_b;
static cap g_ca, g_cb;
static gc_u8 q_ab[65536], q_ba[65536];
static size_t n_ab, n_ba;

static void wa(void *ctx, const gc_u8 *d, size_t n)
{
    (void)ctx;
    memcpy(q_ab + n_ab, d, n);
    n_ab += n;
}

static void wb(void *ctx, const gc_u8 *d, size_t n)
{
    (void)ctx;
    memcpy(q_ba + n_ba, d, n);
    n_ba += n;
}

static void test_back_to_back(void)
{
    gc_ppp_cfg ca, cb;
    gc_ppp_ops oa = { wa, on_ip, on_state, NULL }, ob = { wb, on_ip, on_state, NULL };
    gc_u32 t;
    gc_u8 tmp[65536];
    size_t n;
    gc_ppp_default_cfg(&ca);
    cb = ca;
    memcpy(cb.local, ca.remote, 4);
    memcpy(cb.remote, ca.local, 4);
    cb.magic = 0x99887766u;
    memset(&g_ca, 0, sizeof(g_ca));
    memset(&g_cb, 0, sizeof(g_cb));
    gc_ppp_init(&g_a, &ca, &oa, &g_ca);
    gc_ppp_init(&g_b, &cb, &ob, &g_cb);
    n_ab = n_ba = 0;
    gc_ppp_open(&g_a, 0);
    gc_ppp_open(&g_b, 0);
    for (t = 1; t < 200 && !(gc_ppp_is_up(&g_a) && gc_ppp_is_up(&g_b)); t++) {
        n = n_ab;
        memcpy(tmp, q_ab, n);
        n_ab = 0;
        gc_ppp_input(&g_b, tmp, n, t * 10);
        n = n_ba;
        memcpy(tmp, q_ba, n);
        n_ba = 0;
        gc_ppp_input(&g_a, tmp, n, t * 10);
        gc_ppp_poll(&g_a, t * 10);
        gc_ppp_poll(&g_b, t * 10);
    }
    CHECK(gc_ppp_is_up(&g_a));
    CHECK(gc_ppp_is_up(&g_b));
    CHECK(t < 20);
}

static void test_garbage(void)
{
    int round, i;
    gc_u8 junk[512];
    setup();
    lcp_up();
    ipcp_up();
    for (round = 0; round < 2000; round++) {
        size_t n = gct_rand() % sizeof(junk);
        for (i = 0; i < (int)n; i++) {
            junk[i] = (gc_u8)gct_rand();
        }
        g_c.raw_len = 0;
        gc_ppp_input(&g_p, junk, n, 200 + (gc_u32)round);
    }
    /* still alive enough to answer an echo */
    {
        static const gc_u8 fl[1] = { 0x7E };
        static const gc_u8 echo[] = { 9, 77, 0, 8, 0, 0, 0, 0 };
        gc_ppp_input(&g_p, fl, 1, 5000);
        if (g_p.lcp.state == GC_PPP_ST_OPENED) {
            feed(GC_PPP_LCP, echo, sizeof(echo), 5001);
            CHECK(find(GC_PPP_LCP, 10) != NULL);
        }
    }
}

/* our MRU is asked for when it is below 1500; a Nak drops it. Run with
 * the ESP32 configuration (GC_PPP_MRU 1100, GC_PPP_TXBUF 16), which also
 * checks that frames written in pieces deframe like whole ones. */
static void test_our_mru(void)
{
    const pk *k;
    gc_u8 nak[8];
    size_t i;
    int has_mru = 0;
    setup();
    feed(GC_PPP_LCP, lcp_req, sizeof(lcp_req), 0);
    k = find(GC_PPP_LCP, 1);
    CHECK(k != NULL);
    if (!k) {
        return;
    }
    for (i = 4; i + 2 <= k->n && k->d[i + 1] >= 2; i += k->d[i + 1]) {
        if (k->d[i] == 1) {
            CHECK_EQ(k->d[i + 1], 4);
            CHECK_EQ((k->d[i + 2] << 8) | k->d[i + 3], GC_PPP_MRU);
            has_mru = 1;
        }
    }
    CHECK_EQ(has_mru, GC_PPP_MRU < 1500);
    if (!has_mru) {
        return;
    }
    nak[0] = 3;
    nak[1] = k->d[1];
    nak[2] = 0;
    nak[3] = 8;
    nak[4] = 1;
    nak[5] = 4;
    nak[6] = 0x05;
    nak[7] = 0xDC;
    g_c.raw_len = 0;
    feed(GC_PPP_LCP, nak, sizeof(nak), 10);
    k = find(GC_PPP_LCP, 1);
    CHECK(k != NULL);
    for (i = 4; k && i + 2 <= k->n && k->d[i + 1] >= 2; i += k->d[i + 1]) {
        CHECK(k->d[i] != 1);
    }
}

int main(void)
{
    RUN(test_fcs);
    RUN(test_framing_escapes);
    RUN(test_lcp_open);
    RUN(test_lcp_reject_auth_and_unknown);
    RUN(test_lcp_magic_loop);
    RUN(test_ipcp_and_ip);
    RUN(test_compressed_fields);
    RUN(test_echo_and_protocol_reject);
    RUN(test_terminate);
    RUN(test_close_and_timeouts);
    RUN(test_back_to_back);
    RUN(test_garbage);
    RUN(test_our_mru);
    TEST_MAIN_END();
}
