/** @file test_queue_storage.c
 *
 *  Message storage of the link queue (gc_link.c) and the bridge FIFOs
 *  (bridge_core.c). Built twice: with the default sizing (DLL, server,
 *  pcbridge) and with the ESP32 sizing from firmware/platformio.ini.
 *
 *  - The message windows are the same for both: GC_LINK_QUEUE messages per
 *    link and BRIDGE_FIFO_LEN per FIFO for typical game messages.
 *  - With the default sizing only the message count limits a queue, as
 *    before byte storage was introduced.
 *  - With less storage, a queue is never reported full while there is
 *    clearly room, and messages of every size arrive intact and in order
 *    across many wrap-arounds of the ring.
 *
 *  SPDX-License-Identifier: MIT
 */
#include "../../protocol/tests/gc_test.h"
#include "bridge_core.h"

#define DEFAULT_SIZING (GC_LINK_QBUF == (GC_LINK_QUEUE + 1) * GC_LINK_MAX_MSG)

/* ------------------------------------------------------------------ */
/* byte queues between the parties                                     */
/* ------------------------------------------------------------------ */

#define BQ_CAP (1024 * 1024)
typedef struct
{
    gc_u8 buf[BQ_CAP];
    size_t len;
} bq;

static void bq_put(bq *q, const gc_u8 *d, size_t n)
{
    CHECK(q->len + n <= BQ_CAP);
    if (q->len + n <= BQ_CAP) {
        memcpy(q->buf + q->len, d, n);
        q->len += n;
    }
}

/* message n has a length and content derived from n */
static size_t msg_len(gc_u32 n, size_t max)
{
    static const size_t sizes[] = { 1, 7, 48, 200, 600, 1000, GC_LINK_MAX_MSG, 3, 64, 333 };
    size_t l = sizes[(n * 7u + n / 3u) % (sizeof(sizes) / sizeof(sizes[0]))];
    return l > max ? max : l;
}

static void msg_fill(gc_u32 n, gc_u8 *p, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        p[i] = (gc_u8)(n * 131u + i * 7u + (i >> 8));
    }
    if (len >= 4) {
        p[0] = (gc_u8)(n >> 24);
        p[1] = (gc_u8)(n >> 16);
        p[2] = (gc_u8)(n >> 8);
        p[3] = (gc_u8)n;
    }
}

static int msg_check(gc_u32 n, const gc_u8 *p, size_t len, size_t max)
{
    gc_u8 want[GC_LINK_MAX_MSG];
    if (len != msg_len(n, max)) {
        return 0;
    }
    msg_fill(n, want, len);
    return memcmp(want, p, len) == 0;
}

/* ------------------------------------------------------------------ */
/* link queue                                                          */
/* ------------------------------------------------------------------ */

static gc_link LA, LB;
static bq qa, qb; /* A -> B, B -> A */
static gc_u32 rx_next, rx_bad;
static size_t rx_max;

static void a_write(void *c, const gc_u8 *d, size_t n)
{
    (void)c;
    bq_put(&qa, d, n);
}
static void b_write(void *c, const gc_u8 *d, size_t n)
{
    (void)c;
    bq_put(&qb, d, n);
}
static void b_msg(void *c, const gc_u8 *d, size_t n)
{
    (void)c;
    if (!msg_check(rx_next, d, n, rx_max)) {
        rx_bad++;
    }
    rx_next++;
}

static void links_setup(gc_u32 now)
{
    gc_link_cfg ca, cb;
    gc_link_ops oa, ob;
    memset(&oa, 0, sizeof(oa));
    memset(&ob, 0, sizeof(ob));
    qa.len = qb.len = 0;
    rx_next = rx_bad = 0;
    gc_link_default_cfg(&ca, GC_ROLE_DEVICE, 1);
    gc_link_default_cfg(&cb, GC_ROLE_BRIDGE, 0);
    ca.dead_ms = cb.dead_ms = 60000;
    oa.write = a_write;
    ob.write = b_write;
    ob.on_msg = b_msg;
    gc_link_init(&LA, &ca, &oa, NULL);
    gc_link_init(&LB, &cb, &ob, NULL);
    gc_link_start(&LA, 0x1234, now);
    gc_link_input(&LB, qa.buf, qa.len, now);
    qa.len = 0;
    gc_link_input(&LA, qb.buf, qb.len, now);
    qb.len = 0;
}

static size_t queued_bytes(const gc_link *l)
{
    size_t sum = 0;
    int i;
    for (i = 0; i < l->q_count; i++) {
        size_t n = l->q[(l->q_head + i) % GC_LINK_QUEUE].len;
        sum += n ? n : 1;
    }
    return sum;
}

/* 16 typical game messages fit without any acknowledgement */
static void test_link_window_typical_messages(void)
{
    gc_u8 m[48];
    int i;
    links_setup(1000);
    CHECK_EQ(LA.state, GC_LS_UP);
    memset(m, 0x5A, sizeof(m));
    for (i = 0; i < GC_LINK_QUEUE; i++) {
        CHECK_EQ(gc_link_send(&LA, m, sizeof(m), 1000), GC_LINK_OK);
    }
    CHECK_EQ(gc_link_send(&LA, m, sizeof(m), 1000), GC_LINK_E_FULL); /* window, not bytes */
    CHECK_EQ(LA.q_count, GC_LINK_QUEUE);
}

/* the largest message always fits into an empty queue */
static void test_link_largest_message(void)
{
    gc_u8 m[GC_LINK_MAX_MSG];
    gc_u32 n = 0;
    links_setup(1000);
    rx_max = GC_LINK_MAX_MSG;
    while (msg_len(n, GC_LINK_MAX_MSG) != GC_LINK_MAX_MSG) {
        n++;
    }
    rx_next = n;
    msg_fill(n, m, sizeof(m));
    CHECK_EQ(gc_link_send(&LA, m, sizeof(m), 1000), GC_LINK_OK);
    gc_link_input(&LB, qa.buf, qa.len, 1000);
    CHECK_EQ(rx_next, n + 1);
    CHECK_EQ(rx_bad, 0);
}

/* Mixed sizes, acknowledgements arriving late and in bursts: everything
 * arrives intact; FULL only when the window is used up or the bytes are
 * really short. */
static void test_link_ring_stress(void)
{
    gc_u32 now = 1000, next = 0, full_bytes = 0, full_count = 0;
    int step;
    links_setup(now);
    rx_max = GC_LINK_MAX_MSG;
    for (step = 0; step < 20000; step++) {
        int r = GC_LINK_OK, burst;
        /* the application sends until the queue reports full */
        for (burst = 0; burst < 4 && r == GC_LINK_OK; burst++) {
            gc_u8 m[GC_LINK_MAX_MSG];
            size_t len = msg_len(next, GC_LINK_MAX_MSG);
            size_t used = queued_bytes(&LA);
            int count = LA.q_count;
            msg_fill(next, m, len);
            r = gc_link_send(&LA, m, len, now);
            if (r == GC_LINK_OK) {
                next++;
            } else {
                CHECK_EQ(r, GC_LINK_E_FULL);
                if (count < GC_LINK_QUEUE) {
                    full_bytes++;
                    /* with the default sizing the count is the only limit */
                    CHECK(!DEFAULT_SIZING);
                    /* never "full" while there is clearly room */
                    CHECK(used + len + GC_LINK_MAX_MSG > GC_LINK_QBUF);
                } else {
                    full_count++;
                }
            }
        }
        /* deliver data every step, acknowledgements only now and then */
        if (qa.len) {
            gc_link_input(&LB, qa.buf, qa.len, now);
            qa.len = 0;
        }
        if (step % 5 == 4) {
            gc_link_input(&LA, qb.buf, qb.len, now);
            qb.len = 0;
        }
        now += 3;
    }
    CHECK(next > 5000);
    CHECK_EQ(rx_bad, 0);
    CHECK(rx_next + (gc_u32)LA.q_count >= next);
    CHECK(full_count + full_bytes > 0); /* the queue did fill up */
    printf("    sent %lu, full: window %lu, bytes %lu\n", (unsigned long)next, (unsigned long)full_count,
           (unsigned long)full_bytes);
}

/* ------------------------------------------------------------------ */
/* bridge FIFOs                                                        */
/* ------------------------------------------------------------------ */

static bridge BR;
static gc_link DEV, SRV; /* the N-Gage's and the server's link */
static bq dev2br, br2dev, srv2br, br2srv;
static int dev_stalled; /* N-Gage does not read (bytes are held) */
static int fifo_dropped;
static gc_u32 dev_next, dev_bad;

static void br_dev_write(void *c, const gc_u8 *d, size_t n)
{
    (void)c;
    bq_put(&br2dev, d, n);
}
static void br_srv_write(void *c, const gc_u8 *d, size_t n)
{
    (void)c;
    bq_put(&br2srv, d, n);
}
static void br_srv_connect(void *c) { (void)c; }
static void br_log(void *c, const char *l)
{
    (void)c;
    if (strstr(l, "queue full")) {
        fifo_dropped++;
    }
}
static void dev_write(void *c, const gc_u8 *d, size_t n)
{
    (void)c;
    bq_put(&dev2br, d, n);
}
static void srv_write(void *c, const gc_u8 *d, size_t n)
{
    (void)c;
    bq_put(&srv2br, d, n);
}
static void dev_msg(void *c, const gc_u8 *d, size_t n)
{
    (void)c;
    if (n && d[0] == GC_MT_BRIDGE_STATUS) {
        return;
    }
    /* server messages carry the counter after a GC_MT_DATA type byte */
    if (n < 1 || d[0] != GC_MT_DATA || !msg_check(dev_next, d + 1, n - 1, GC_LINK_MAX_MSG - 1)) {
        dev_bad++;
    }
    dev_next++;
}

static void step(gc_u32 now)
{
    if (dev2br.len) {
        size_t n = dev2br.len;
        dev2br.len = 0;
        bridge_dev_input(&BR, dev2br.buf, n, now);
    }
    if (!dev_stalled && br2dev.len) {
        size_t n = br2dev.len;
        br2dev.len = 0;
        gc_link_input(&DEV, br2dev.buf, n, now);
    }
    if (srv2br.len) {
        size_t n = srv2br.len;
        srv2br.len = 0;
        bridge_srv_input(&BR, srv2br.buf, n, now);
    }
    if (br2srv.len) {
        size_t n = br2srv.len;
        br2srv.len = 0;
        gc_link_input(&SRV, br2srv.buf, n, now);
    }
    gc_link_poll(&DEV, now);
    gc_link_poll(&SRV, now);
    bridge_poll(&BR, now);
}

static void bridge_setup(gc_u32 now)
{
    bridge_cfg bc;
    bridge_io io;
    gc_link_cfg c;
    gc_link_ops o;
    gc_u8 addr[6] = { 0, 0x0e, 0x6d, 1, 2, 3 };
    dev2br.len = br2dev.len = srv2br.len = br2srv.len = 0;
    dev_stalled = fifo_dropped = 0;
    dev_next = dev_bad = 0;
    memset(&io, 0, sizeof(io));
    io.dev_write = br_dev_write;
    io.srv_write = br_srv_write;
    io.srv_connect = br_srv_connect;
    io.log = br_log;
    bridge_default_cfg(&bc);
    bridge_init(&BR, &bc, &io, NULL);

    memset(&o, 0, sizeof(o));
    gc_link_default_cfg(&c, GC_ROLE_DEVICE, 1);
    c.dead_ms = 60000;
    o.write = dev_write;
    o.on_msg = dev_msg;
    gc_link_init(&DEV, &c, &o, NULL);
    gc_link_default_cfg(&c, GC_ROLE_SERVER, 0);
    c.dead_ms = 60000;
    o.write = srv_write;
    o.on_msg = NULL;
    gc_link_init(&SRV, &c, &o, NULL);

    bridge_dev_connected(&BR, addr, now);
    gc_link_start(&DEV, 0x4E47, now);
    bridge_set_wifi(&BR, 1, -40, now);
    bridge_poll(&BR, now);
    bridge_srv_connected(&BR, now);
    gc_link_listen(&SRV, now);
    step(now);
    step(now);
    step(now);
}

static gc_u32 server_send(gc_u32 n, gc_u32 now)
{
    gc_u8 m[GC_LINK_MAX_MSG];
    size_t len = msg_len(n, GC_LINK_MAX_MSG - 1);
    m[0] = GC_MT_DATA;
    msg_fill(n, m + 1, len);
    return gc_link_send(&SRV, m, len + 1, now) == GC_LINK_OK;
}

/* while the N-Gage does not read, the bridge holds a full link window plus
 * a full FIFO of typical messages before it applies backpressure */
static void test_bridge_window_typical_messages(void)
{
    gc_u32 now = 5000;
    gc_u8 m[49];
    int i;
    bridge_setup(now);
    CHECK_EQ(BR.u.gc.dev.state, GC_LS_UP);
    CHECK_EQ(BR.srv.state, GC_LS_UP);
    dev_stalled = 1;
    m[0] = GC_MT_DATA;
    memset(m + 1, 0x33, sizeof(m) - 1);
    for (i = 0; i < GC_LINK_QUEUE + BRIDGE_FIFO_LEN; i++) {
        CHECK_EQ(gc_link_send(&SRV, m, sizeof(m), now), GC_LINK_OK);
        step(now);
    }
    CHECK_EQ(BR.u.gc.dev.q_count, GC_LINK_QUEUE);
    CHECK_EQ(BR.u.gc.to_dev.count, BRIDGE_FIFO_LEN);
    CHECK_EQ(BR.srv.st_backpressure, 0);
    CHECK_EQ(SRV.q_count, 0); /* the bridge acknowledged all of them */
    CHECK_EQ(gc_link_send(&SRV, m, sizeof(m), now), GC_LINK_OK);
    step(now);
    CHECK_EQ(BR.srv.st_backpressure, 1); /* the next one waits at the server */
    CHECK_EQ(fifo_dropped, 0);
}

/* mixed sizes through a stalled and released N-Gage: nothing is lost,
 * duplicated or reordered, the FIFO never drops */
static void test_bridge_fifo_stress(void)
{
    gc_u32 now = 5000, sent = 0;
    int round, k;
    bridge_setup(now);
    for (round = 0; round < 60; round++) {
        dev_stalled = round % 2 == 0;
        for (k = 0; k < 400; k++) {
            while (SRV.q_count < GC_LINK_QUEUE && server_send(sent, now)) {
                sent++;
            }
            step(now);
            now += 5;
        }
    }
    dev_stalled = 0;
    for (k = 0; k < 4000 && dev_next < sent; k++) {
        step(now);
        now += 5;
    }
    CHECK(sent > 1000);
    CHECK_EQ(dev_next, sent);
    CHECK_EQ(dev_bad, 0);
    CHECK_EQ(fifo_dropped, 0);
    CHECK(BR.srv.st_backpressure > 0); /* the FIFO was full at times */
    printf("    forwarded %lu, backpressure %lu\n", (unsigned long)dev_next, (unsigned long)BR.srv.st_backpressure);
}

/* ESP32 sizing: PPP mode's state fits in the GameComms device link it
 * replaces, so Arena support costs the bridge no memory */
static void test_ppp_fits_device_link(void)
{
    bridge b;
    printf("    ppp state %u bytes, device link + queue %u bytes\n", (unsigned)sizeof(b.u.pp), (unsigned)sizeof(b.u.gc));
    if (!DEFAULT_SIZING) {
        CHECK(sizeof(b.u.pp) <= sizeof(b.u.gc));
    }
}

int main(void)
{
    printf("    sizing: link %d messages / %d bytes, fifo %d messages / %d bytes%s\n", GC_LINK_QUEUE, GC_LINK_QBUF,
           BRIDGE_FIFO_LEN, BRIDGE_FIFO_BYTES, DEFAULT_SIZING ? " (default)" : "");
    RUN(test_ppp_fits_device_link);
    RUN(test_link_window_typical_messages);
    RUN(test_link_largest_message);
    RUN(test_link_ring_stress);
    RUN(test_bridge_window_typical_messages);
    RUN(test_bridge_fifo_stress);
    TEST_MAIN_END();
}
