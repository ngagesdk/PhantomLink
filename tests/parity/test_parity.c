/** @file test_parity.c
 *
 *  Bridge parity: the game must not be able to tell the ESP32 bridge from
 *  pcbridge.
 *
 *    [N-Gage engine] --Bluetooth-- [bridge profile] --TCP-- [server]
 *
 *  The N-Gage engine (dll/core) and the server core are the production
 *  code with production sizing. The bridge is either profile from
 *  bridge_profile.h. Unlike tests/sim, the links have latency, limited
 *  bandwidth and (optionally) Wi-Fi modem-sleep delivery, and time
 *  advances in 1 ms steps, so window and buffer sizes matter.
 *
 *  Every scenario is run with both profiles on identical links; what the
 *  game observes (send results, callbacks, received data) must match and
 *  must be free of errors.
 *
 *  SPDX-License-Identifier: MIT
 */
#include "../../protocol/tests/gc_test.h"
#include "bridge_profile.h"
#include "gc_dev.h"
#include "gc_link.h"
#include "gcs_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_NODES 4
#define UID       0x10005B8B

/* ------------------------------------------------------------------ */
/* timed byte pipe                                                     */
/* ------------------------------------------------------------------ */

#define PIPE_CAP (512 * 1024)

typedef struct
{
    gc_u8 *buf;
    gc_u32 *at; /* delivery time per byte */
    size_t head, len;
    gc_u32 tx_free; /* sender busy until */
    gc_u32 last_at;
    int bytes_per_s; /* 0 = unlimited */
    int latency_ms;
    int quantum_ms; /* deliver only at multiples of this (modem sleep) */
    int up;
} pipe_t;

static void pipe_init(pipe_t *p, int bps, int lat, int quantum)
{
    memset(p, 0, sizeof(*p));
    p->buf = (gc_u8 *)malloc(PIPE_CAP);
    p->at = (gc_u32 *)malloc(PIPE_CAP * sizeof(gc_u32));
    p->bytes_per_s = bps;
    p->latency_ms = lat;
    p->quantum_ms = quantum;
}

static void pipe_free(pipe_t *p)
{
    free(p->buf);
    free(p->at);
}

static void pipe_reset(pipe_t *p, int up)
{
    p->head = p->len = 0;
    p->up = up;
}

static void pipe_put(pipe_t *p, const gc_u8 *d, size_t n, gc_u32 now)
{
    gc_u32 start, at;
    size_t i;
    if (!p->up || !n) {
        return;
    }
    if (p->head + p->len + n > PIPE_CAP) {
        memmove(p->buf, p->buf + p->head, p->len);
        memmove(p->at, p->at + p->head, p->len * sizeof(gc_u32));
        p->head = 0;
    }
    if (p->len + n > PIPE_CAP) {
        CHECK(!"pipe overflow");
        return;
    }
    start = GC_TIME_BEFORE(p->tx_free, now) ? now : p->tx_free;
    p->tx_free = start + (p->bytes_per_s ? (gc_u32)((n * 1000 + p->bytes_per_s - 1) / p->bytes_per_s) : 0);
    at = p->tx_free + (gc_u32)p->latency_ms;
    if (p->quantum_ms) {
        at = (at + (gc_u32)p->quantum_ms - 1) / (gc_u32)p->quantum_ms * (gc_u32)p->quantum_ms;
    }
    if (GC_TIME_BEFORE(at, p->last_at)) {
        at = p->last_at;
    }
    p->last_at = at;
    for (i = 0; i < n; i++) {
        p->buf[p->head + p->len + i] = d[i];
        p->at[p->head + p->len + i] = at;
    }
    p->len += n;
}

static size_t pipe_ready(const pipe_t *p, gc_u32 now)
{
    size_t n = 0;
    while (n < p->len && !GC_TIME_BEFORE(now, p->at[p->head + n])) {
        n++;
    }
    return n;
}

static void pipe_take(pipe_t *p, size_t n)
{
    p->head += n;
    p->len -= n;
}

static int pipe_busy(const pipe_t *p, gc_u32 now)
{
    return GC_TIME_BEFORE(now, p->tx_free);
}

/* ------------------------------------------------------------------ */
/* world                                                               */
/* ------------------------------------------------------------------ */

typedef struct
{
    int bt_bps;       /* Bluetooth bytes per second, each direction */
    int bt_latency;   /* one way, ms */
    int net_latency;  /* bridge <-> server one way, ms */
    int net_bps;      /* 0 = unlimited */
    int wifi_quantum; /* server -> bridge delivery granularity (modem sleep) */
    int players;      /* 2..4 */
    int period_ms;    /* game message interval per player */
    int payload;      /* game message size */
    int duration_ms;  /* of the game phase */
} scenario;

struct world;

typedef struct
{
    struct world *w;
    int idx;
    gcd_dev dev;
    void *br;
    gcs_conn conn;
    int conn_open;
    gc_u32 connect_at; /* pending TCP connect completes at */
    pipe_t bt_up, bt_down, tcp_up, tcp_down;
    /* what the game observed */
    unsigned long sent, send_err, last_send_err;
    unsigned long rx_data, rx_order_err;
    gc_u32 rx_next[MAX_NODES];
    gc_u32 tx_seq;
    int disconnected; /* GCD_EV2_DISCONNECTED */
    int connected;    /* GCD_EV2_CONNECTED */
    int transport;    /* GCD_EV_TRANSPORT (not shown to games, diagnostic) */
} node;

typedef struct world
{
    const bp_profile *prof;
    scenario sc;
    gcs_core *core;
    node n[MAX_NODES];
    gc_u32 now;
} world;

void parity_log(void *ctx, const char *who, const char *line)
{
    node *s = (node *)ctx;
    printf("      %6lu [%s bridge %d] %s\n", (unsigned long)s->w->now, who, s->idx, line);
}

static void dev_write(void *ctx, const gc_u8 *d, size_t n)
{
    node *s = (node *)ctx;
    pipe_put(&s->bt_up, d, n, s->w->now);
}
static void dev_log(void *ctx, const char *l)
{
    node *s = (node *)ctx;
    if (getenv("PARITY_LOG")) {
        printf("      %6lu [dev %d] %s\n", (unsigned long)s->w->now, s->idx, l);
    }
}
static void dev_wake(void *ctx) { (void)ctx; }

static void io_bt_write(void *ctx, const gc_u8 *d, size_t n)
{
    node *s = (node *)ctx;
    pipe_put(&s->bt_down, d, n, s->w->now);
}
static int io_bt_busy(void *ctx)
{
    node *s = (node *)ctx;
    return pipe_busy(&s->bt_down, s->w->now);
}
static void io_tcp_write(void *ctx, const gc_u8 *d, size_t n)
{
    node *s = (node *)ctx;
    pipe_put(&s->tcp_up, d, n, s->w->now);
}
static void io_tcp_connect(void *ctx)
{
    node *s = (node *)ctx;
    s->connect_at = (s->w->now + 2u * (gc_u32)s->w->sc.net_latency) | 1u;
}
static void world_tcp_close(world *w, node *s)
{
    if (s->conn_open) {
        gcs_core_conn_closed(w->core, &s->conn, w->now);
        s->conn_open = 0;
    }
    s->connect_at = 0;
    pipe_reset(&s->tcp_up, 0);
    pipe_reset(&s->tcp_down, 0);
}
static void io_tcp_close(void *ctx)
{
    node *s = (node *)ctx;
    world_tcp_close(s->w, s);
}

static void srv_write(void *ctx, gcs_conn *c, const gc_u8 *d, size_t n)
{
    world *w = (world *)ctx;
    node *s = (node *)c->net;
    pipe_put(&s->tcp_down, d, n, w->now);
}
static void srv_close(void *ctx, gcs_conn *c)
{
    world *w = (world *)ctx;
    node *s = (node *)c->net;
    world_tcp_close(w, s);
    w->prof->tcp_lost(s->br, w->now);
}
static void srv_log(void *ctx, int lvl, const char *l)
{
    world *w = (world *)ctx;
    (void)lvl;
    if (getenv("PARITY_LOG")) {
        printf("      %6lu [server] %s\n", (unsigned long)w->now, l);
    }
}

static void world_init(world *w, const bp_profile *prof, const scenario *sc)
{
    gcs_config cfg;
    gcs_io io;
    bp_io bio;
    int i;

    memset(w, 0, sizeof(*w));
    w->prof = prof;
    w->sc = *sc;
    w->now = 100000;
    gcs_default_config(&cfg);
    strcpy(cfg.board_file, "parity-board-test.dat");
    cfg.log_level = 3;
    io.write = srv_write;
    io.close = srv_close;
    io.log = srv_log;
    w->core = gcs_core_create(&cfg, &io, w, 7);

    bio.bt_write = io_bt_write;
    bio.bt_busy = io_bt_busy;
    bio.tcp_write = io_tcp_write;
    bio.tcp_connect = io_tcp_connect;
    bio.tcp_close = io_tcp_close;

    for (i = 0; i < sc->players; i++) {
        node *s = &w->n[i];
        gcd_config dc;
        gcd_ops dops;
        gc_u8 addr[6] = { 0x00, 0x0e, 0x6d, 0x11, 0x22, 0 };
        s->w = w;
        s->idx = i;
        pipe_init(&s->bt_up, sc->bt_bps, sc->bt_latency, 0);
        pipe_init(&s->bt_down, sc->bt_bps, sc->bt_latency, 0);
        pipe_init(&s->tcp_up, sc->net_bps, sc->net_latency, 0);
        pipe_init(&s->tcp_down, sc->net_bps, sc->net_latency, sc->wifi_quantum);
        pipe_reset(&s->bt_up, 1);
        pipe_reset(&s->bt_down, 1);

        gcd_default_config(&dc);
        dc.api = GC_API_V2;
        dc.variant = 2; /* Tony Hawk's Pro Skater */
        dc.game_uid = UID;
        sprintf(dc.name, "ngage%d", i);
        dc.link_id = 0x1000 + (gc_u32)i;
        dops.write = dev_write;
        dops.log = dev_log;
        dops.wake = dev_wake;
        gcd_init(&s->dev, &dc, &dops, s);

        s->br = prof->create(&bio, s, 0x2000 + (gc_u32)i, w->now);
        addr[5] = (gc_u8)i;
        prof->dev_connected(s->br, addr, w->now);
        gcd_transport_up(&s->dev, w->now);
    }
}

static void world_free(world *w)
{
    int i;
    for (i = 0; i < w->sc.players; i++) {
        node *s = &w->n[i];
        w->prof->destroy(s->br);
        pipe_free(&s->bt_up);
        pipe_free(&s->bt_down);
        pipe_free(&s->tcp_up);
        pipe_free(&s->tcp_down);
    }
    gcs_core_destroy(w->core);
    remove("parity-board-test.dat");
}

static void on_event(node *s, const gcd_event *e)
{
    if (getenv("PARITY_LOG") && e->type != GCD_EV2_DATA) {
        printf("      %6lu [dev %d] EVENT %d id %d err %d\n", (unsigned long)s->w->now, s->idx, e->type, e->id, e->err);
    }
    switch (e->type) {
    case GCD_EV2_CONNECTED:
        if (e->err == 0) {
            s->connected++;
        }
        break;
    case GCD_EV2_DISCONNECTED:
        s->disconnected++;
        break;
    case GCD_EV_TRANSPORT:
        s->transport++;
        break;
    case GCD_EV2_DATA:
        if (e->len >= 4 && e->id >= 0 && e->id < MAX_NODES) {
            gc_u32 seq = ((gc_u32)e->data[0] << 24) | ((gc_u32)e->data[1] << 16) | ((gc_u32)e->data[2] << 8) | e->data[3];
            if (seq != s->rx_next[e->id]) {
                s->rx_order_err++;
            }
            s->rx_next[e->id] = seq + 1;
            s->rx_data++;
        }
        break;
    default:
        break;
    }
}

static void game_send(node *s)
{
    gc_u8 p[GC_MAX_DATA];
    int r, k;
    for (k = 4; k < s->w->sc.payload; k++) {
        p[k] = (gc_u8)(k * 31 + s->tx_seq);
    }
    p[0] = (gc_u8)(s->tx_seq >> 24);
    p[1] = (gc_u8)(s->tx_seq >> 16);
    p[2] = (gc_u8)(s->tx_seq >> 8);
    p[3] = (gc_u8)s->tx_seq;
    /* host: to everybody (THPS broadcasts its state); client: to host */
    r = gcd_v2_send(&s->dev, s->idx == 0 ? GCD_ALL : GC_SLOT_HOST, p, (size_t)s->w->sc.payload, s->w->now);
    if (r == GCD_KErrNone) {
        s->tx_seq++;
        s->sent++;
    } else {
        s->send_err++;
        s->last_send_err = (unsigned long)(-r);
    }
}

static void world_run(world *w, int ms, int game)
{
    int t, i;
    for (t = 0; t < ms; t++) {
        w->now++;
        for (i = 0; i < w->sc.players; i++) {
            node *s = &w->n[i];
            gcd_event e;
            size_t n;
            if (s->connect_at && !GC_TIME_BEFORE(w->now, s->connect_at)) {
                s->connect_at = 0;
                pipe_reset(&s->tcp_up, 1);
                pipe_reset(&s->tcp_down, 1);
                s->conn.net = s;
                s->conn.peer = NULL;
                gcs_core_conn_open(w->core, &s->conn, w->now);
                s->conn_open = 1;
                w->prof->tcp_connected(s->br, w->now);
            }
            /* N-Gage */
            if ((n = pipe_ready(&s->bt_down, w->now)) > 0) {
                gcd_transport_input(&s->dev, s->bt_down.buf + s->bt_down.head, n, w->now);
                pipe_take(&s->bt_down, n);
            }
            /* bridge */
            if ((n = pipe_ready(&s->bt_up, w->now)) > 0) {
                pipe_take(&s->bt_up, w->prof->dev_rx(s->br, s->bt_up.buf + s->bt_up.head, n, w->now));
            }
            if ((n = pipe_ready(&s->tcp_down, w->now)) > 0) {
                pipe_take(&s->tcp_down, w->prof->tcp_rx(s->br, s->tcp_down.buf + s->tcp_down.head, n, w->now));
            }
            /* server */
            if (s->conn_open && (n = pipe_ready(&s->tcp_up, w->now)) > 0) {
                size_t k = n;
                pipe_take(&s->tcp_up, 0);
                gcs_core_conn_input(w->core, &s->conn, s->tcp_up.buf + s->tcp_up.head, k, w->now);
                if (s->tcp_up.up) {
                    pipe_take(&s->tcp_up, k);
                }
            }
            w->prof->loop(s->br, w->now);
            gcd_poll(&s->dev, w->now);
            while (gcd_pop_event(&s->dev, &e)) {
                on_event(s, &e);
            }
            if (game && (w->now + (gc_u32)i * 7u) % (gc_u32)w->sc.period_ms == 0) {
                game_send(s);
            }
        }
        gcs_core_poll(w->core, w->now);
    }
}

/* 2.x session as THPS sets it up: host accepts all, clients declare the
 * host and connect, host connects. */
static int world_connect_session(world *w)
{
    int i;
    world_run(w, 3000, 0);
    gcd_v2_start(&w->n[0].dev, 1, 0, w->now);
    gcd_v2_set_accept_mode(&w->n[0].dev, GCD_ACCEPT_ALL, w->now);
    world_run(w, 2000, 0);
    for (i = 1; i < w->sc.players; i++) {
        gcd_device host;
        memset(&host, 0, sizeof(host));
        host.session_id = w->n[0].dev.session_id;
        host.slot = GC_SLOT_HOST;
        strcpy(host.name, "ngage0");
        gcd_v2_start(&w->n[i].dev, 0, 0, w->now);
        gcd_v2_declare(&w->n[i].dev, &host, w->now);
        gcd_v2_connect(&w->n[i].dev, GCD_ALL, w->now);
        world_run(w, 2000, 0);
    }
    gcd_v2_connect(&w->n[0].dev, GCD_ALL, w->now);
    world_run(w, 3000, 0);
    for (i = 0; i < w->sc.players; i++) {
        if (gcd_connect_state(&w->n[i].dev) != GCD_CS_CONNECTED) {
            return 0;
        }
    }
    return 1;
}

typedef struct
{
    int session_ok;
    unsigned long sent, send_err, rx, rx_expected, order_err;
    int disconnected, transport;
    bp_stats br;
} outcome;

static void run_scenario(const bp_profile *prof, const scenario *sc, outcome *o)
{
    static world w;
    int i;
    memset(o, 0, sizeof(*o));
    world_init(&w, prof, sc);
    o->session_ok = world_connect_session(&w);
    if (o->session_ok) {
        world_run(&w, sc->duration_ms, 1);
        world_run(&w, 5000, 0); /* drain */
    }
    for (i = 0; i < sc->players; i++) {
        node *s = &w.n[i];
        bp_stats b;
        o->sent += s->sent;
        o->send_err += s->send_err;
        o->rx += s->rx_data;
        o->order_err += s->rx_order_err;
        o->disconnected += s->disconnected;
        o->transport += s->transport;
        /* host messages reach every client, client messages the host */
        o->rx_expected += i == 0 ? s->sent * (unsigned long)(sc->players - 1) : s->sent;
        prof->stats(s->br, &b);
        o->br.dev_backpressure += b.dev_backpressure;
        o->br.srv_backpressure += b.srv_backpressure;
        o->br.dev_retx += b.dev_retx;
        o->br.srv_retx += b.srv_retx;
        o->br.bt_teardowns += b.bt_teardowns;
        o->br.tcp_teardowns += b.tcp_teardowns;
        o->br.srv_fresh += b.srv_fresh;
    }
    world_free(&w);
}

static void print_outcome(const char *label, const char *prof, const outcome *o)
{
    printf("    %-28s %-6s session %d sent %lu send_err %lu rx %lu/%lu order %lu disc %d transport %d | "
           "bp dev %lu srv %lu retx dev %lu srv %lu teardown bt %lu tcp %lu fresh %lu\n",
           label, prof, o->session_ok, o->sent, o->send_err, o->rx, o->rx_expected, o->order_err, o->disconnected,
           o->transport, o->br.dev_backpressure, o->br.srv_backpressure, o->br.dev_retx, o->br.srv_retx,
           o->br.bt_teardowns, o->br.tcp_teardowns, o->br.srv_fresh);
}

/* What the game observes must be clean, and identical for both bridges. */
static void check_parity(const char *label, const scenario *sc)
{
    outcome pc, esp;
    run_scenario(&bp_profile_pc, sc, &pc);
    run_scenario(&bp_profile_esp32, sc, &esp);
    print_outcome(label, "pc", &pc);
    print_outcome(label, "esp32", &esp);

    CHECK_EQ(pc.session_ok, 1);
    CHECK_EQ(pc.send_err, 0);
    CHECK_EQ(pc.disconnected, 0);
    CHECK_EQ(pc.order_err, 0);
    CHECK_EQ(pc.rx, pc.rx_expected);

    CHECK_EQ(esp.session_ok, pc.session_ok);
    CHECK_EQ(esp.send_err, pc.send_err);
    CHECK_EQ(esp.disconnected, pc.disconnected);
    CHECK_EQ(esp.transport, pc.transport);
    CHECK_EQ(esp.sent, pc.sent);
    CHECK_EQ(esp.order_err, 0);
    CHECK_EQ(esp.rx, esp.rx_expected);
    CHECK_EQ(esp.br.bt_teardowns, 0);
    CHECK_EQ(esp.br.tcp_teardowns, 0);
    CHECK_EQ(esp.br.srv_fresh, pc.br.srv_fresh);
}

/* ------------------------------------------------------------------ */
/* scenarios                                                           */
/* ------------------------------------------------------------------ */

static scenario base(void)
{
    scenario s;
    s.bt_bps = 20000; /* Bluetooth 1.1 SPP as reached by the N-Gage */
    s.bt_latency = 15;
    s.net_latency = 25;
    s.net_bps = 0;
    s.wifi_quantum = 0;
    s.players = 2;
    s.period_ms = 50;
    s.payload = 48;
    s.duration_ms = 30000;
    return s;
}

static void test_sizing(void)
{
    bp_sizing pc, esp;
    bp_profile_pc.sizing(&pc);
    bp_profile_esp32.sizing(&esp);
    printf("    pc    : link queue %d (%lu bytes), fifo %d (%lu bytes), bridge %lu bytes\n", pc.link_queue,
           (unsigned long)pc.link_qbuf, pc.fifo_len, (unsigned long)pc.fifo_bytes, (unsigned long)pc.bridge_size);
    printf("    esp32 : link queue %d (%lu bytes), fifo %d (%lu bytes), bridge %lu bytes\n", esp.link_queue,
           (unsigned long)esp.link_qbuf, esp.fifo_len, (unsigned long)esp.fifo_bytes, (unsigned long)esp.bridge_size);
}

static void test_two_players_lan(void)
{
    scenario s = base();
    check_parity("2 players, LAN", &s);
}

static void test_two_players_modem_sleep(void)
{
    scenario s = base();
    s.net_latency = 30;
    s.wifi_quantum = 102; /* ESP32 Wi-Fi must stay in modem sleep with Bluetooth on */
    check_parity("2 players, modem sleep", &s);
}

static void test_four_players_internet(void)
{
    scenario s = base();
    s.players = 4;
    s.period_ms = 66;
    s.net_latency = 60;
    s.wifi_quantum = 102;
    check_parity("4 players, internet", &s);
}

/* the upstream ESP32 sizing (4 messages per link and FIFO) failed here
 * with send errors while pcbridge was clean */
static void test_two_players_fast_internet(void)
{
    scenario s = base();
    s.period_ms = 33;
    s.net_latency = 60;
    s.wifi_quantum = 102;
    check_parity("2 players 30 Hz, internet", &s);
}

/* large messages: the ESP32 queues run out of bytes before messages */
static void test_large_messages(void)
{
    scenario s = base();
    s.period_ms = 66;
    s.payload = 600;
    s.net_latency = 30;
    s.wifi_quantum = 102;
    check_parity("2 players, 600 byte messages", &s);
}

int main(void)
{
    RUN(test_sizing);
    RUN(test_two_players_lan);
    RUN(test_two_players_modem_sleep);
    RUN(test_two_players_fast_internet);
    RUN(test_four_players_internet);
    RUN(test_large_messages);
    TEST_MAIN_END();
}
