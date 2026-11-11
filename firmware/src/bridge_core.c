/** @file bridge_core.c
 *
 *  ESP32 bridge core.  See bridge_core.h and docs/PROTOCOL.md section
 *  "Bridge behaviour".
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#include "bridge_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- logging ---- */

static void blog(bridge *b, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

#include <stdarg.h>
static void blog(bridge *b, const char *fmt, ...)
{
    char line[160];
    va_list ap;
    if (!b->io.log) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    b->io.log(b->ctx, line);
}

/* ---- fifo ---- */

/* Where n bytes would go in buf, or -1. Same ring as the link queue
 * (gc_link.c, link_alloc): messages are contiguous, released oldest
 * first, and take at least one byte each. */
static long fifo_place(const bridge_fifo *f, size_t n)
{
    size_t need = n ? n : 1;
    size_t head, tail = f->wr;
    if (f->count >= BRIDGE_FIFO_LEN || n > GC_LINK_MAX_MSG) {
        return -1;
    }
    if (f->count == 0) {
        return 0;
    }
    head = f->s[f->head].off;
    if (tail > head) {
        if (BRIDGE_FIFO_BYTES - tail >= need) {
            return (long)tail;
        }
        return head >= need ? 0 : -1;
    }
    return head - tail >= need ? (long)tail : -1;
}

static int fifo_room(const bridge_fifo *f, size_t n)
{
    return fifo_place(f, n) >= 0;
}

static int fifo_push(bridge_fifo *f, const gc_u8 *d, size_t n)
{
    bridge_slot *s;
    long off = fifo_place(f, n);
    if (off < 0) {
        return 0;
    }
    s = &f->s[(f->head + f->count) % BRIDGE_FIFO_LEN];
    s->len = (gc_u16)n;
    s->off = (gc_u16)off;
    memcpy(f->buf + off, d, n);
    f->wr = (gc_u16)(off + (n ? n : 1));
    f->count++;
    return 1;
}

static bridge_slot *fifo_peek(bridge_fifo *f)
{
    return f->count ? &f->s[f->head] : NULL;
}

static void fifo_pop(bridge_fifo *f)
{
    if (f->count) {
        f->head = (f->head + 1) % BRIDGE_FIFO_LEN;
        f->count--;
    }
}

static void fifo_clear(bridge_fifo *f)
{
    f->head = 0;
    f->count = 0;
    f->wr = 0;
}

/* move as much as possible from fifo into link */
static void fifo_drain(bridge *b, bridge_fifo *f, gc_link *l)
{
    bridge_slot *s;
    if (l->state != GC_LS_UP) {
        return;
    }
    while ((s = fifo_peek(f)) != NULL && gc_link_free_slots(l) > 0) {
        if (gc_link_send(l, f->buf + s->off, s->len, b->now) != GC_LINK_OK) {
            break;
        }
        fifo_pop(f);
    }
}

/* ---- device side output ---- */

static void dev_send(bridge *b, const gc_msg *m)
{
    gc_u8 buf[GC_LINK_MAX_MSG];
    size_t n = gc_msg_encode(m, buf, sizeof(buf));
    if (!n) {
        return;
    }
    if (b->dev.state == GC_LS_UP && b->to_dev.count == 0 && gc_link_send(&b->dev, buf, n, b->now) == GC_LINK_OK) {
        return;
    }
    fifo_push(&b->to_dev, buf, n);
}

static void send_status(bridge *b)
{
    gc_msg m;
    if (b->dev.state != GC_LS_UP) {
        return;
    }
    memset(&m, 0, sizeof(m));
    m.type = GC_MT_BRIDGE_STATUS;
    m.u.bridge.wifi = (gc_u8)(b->wifi_up ? 1 : 0);
    m.u.bridge.server = (gc_u8)b->srv_state;
    m.u.bridge.rssi = b->rssi;
    strcpy(m.u.bridge.version, "bridge " BRIDGE_VERSION);
    dev_send(b, &m);
}

static void set_srv_state(bridge *b, int st)
{
    if (b->srv_state != st) {
        b->srv_state = st;
        send_status(b);
    }
}

/* answer a device request locally because the server is unreachable */
static void reply_unavailable(bridge *b, gc_u8 req_type)
{
    gc_msg m;
    memset(&m, 0, sizeof(m));
    switch (req_type) {
    case GC_MT_REGISTER:
        m.type = GC_MT_REGISTERED;
        m.u.registered.result = GC_R_UNAVAILABLE;
        break;
    case GC_MT_HOST_OPEN:
        m.type = GC_MT_HOST_OPENED;
        m.u.host_opened.result = GC_R_UNAVAILABLE;
        break;
    case GC_MT_JOIN:
        m.type = GC_MT_JOIN_RESULT;
        m.u.join_result.result = GC_R_UNAVAILABLE;
        m.u.join_result.slot = GC_SLOT_NONE;
        break;
    case GC_MT_DISCOVER:
        m.type = GC_MT_SESSION_LIST; /* empty list */
        break;
    default:
        m.type = GC_MT_ERROR;
        m.u.error.code = GC_R_UNAVAILABLE;
        m.u.error.ref_type = req_type;
        strcpy(m.u.error.text, "server unreachable");
        break;
    }
    dev_send(b, &m);
}

/* ---- server side output ---- */

static int srv_usable(bridge *b)
{
    /* up, or temporarily down with a resumable session */
    return b->srv.state == GC_LS_UP || (b->srv.has_session && b->srv_state != GC_BS_DOWN);
}

static void srv_queue(bridge *b, const gc_u8 *d, size_t n)
{
    if (b->srv.state == GC_LS_UP && b->to_srv.count == 0 && gc_link_send(&b->srv, d, n, b->now) == GC_LINK_OK) {
        return;
    }
    if (!fifo_push(&b->to_srv, d, n)) {
        blog(b, "server queue full, dropping %s", gc_msg_name(d[0]));
    }
}

/* ---- PPP mode (retail Arena games) ---- */

#if BRIDGE_PPP

GC_STATIC_ASSERT(tcp_txbuf_takes_a_server_chunk, GC_TCP_TXBUF >= GCN_SRV_CHUNK);

/* room for one more message of n bytes towards the server */
static int srv_room(bridge *b, size_t n)
{
    return srv_usable(b) && fifo_room(&b->to_srv, n);
}

static int srv_send_net(bridge *b, const gcn_msg *m)
{
    gc_u8 buf[GC_LINK_MAX_MSG];
    size_t n = gcn_encode(m, buf, sizeof(buf));
    if (!n || !srv_room(b, n)) {
        return 0;
    }
    srv_queue(b, buf, n);
    return 1;
}

static gc_u16 conn_id(bridge *b, int c)
{
    return (gc_u16)((b->pp->conn[c].gen << 8) | (gc_u8)c);
}

/* sends a pending TCP_CLOSE; 1 if nothing is pending any more */
static int conn_flush_close(bridge *b, int c)
{
    gcn_msg m;
    if (!b->pp->conn[c].pend) {
        return 1;
    }
    memset(&m, 0, sizeof(m));
    m.type = GCN_MT_TCP_CLOSE;
    m.id = (gc_u16)((b->pp->conn[c].pend_gen << 8) | (gc_u8)c);
    m.how = (gc_u8)(b->pp->conn[c].pend == 2 ? GCN_CLOSE_RESET : GCN_CLOSE_FIN);
    if (!srv_send_net(b, &m)) {
        return 0;
    }
    b->pp->conn[c].pend = 0;
    return 1;
}

static void conn_close_later(bridge *b, int c, int how)
{
    if (!b->pp->conn[c].open) {
        return;
    }
    if (how == 2) {
        b->pp->conn[c].open = 0;
    }
    if (b->pp->conn[c].pend < how) {
        b->pp->conn[c].pend = (gc_u8)how;
    }
    b->pp->conn[c].pend_gen = b->pp->conn[c].gen;
    conn_flush_close(b, c);
}

static void p_write(void *ctx, const gc_u8 *d, size_t n)
{
    bridge *b = (bridge *)ctx;
    if (b->dev_connected && b->io.dev_write) {
        b->io.dev_write(b->ctx, d, n);
    }
}

static void p_on_ip(void *ctx, const gc_u8 *pkt, size_t len)
{
    bridge *b = (bridge *)ctx;
    gc_ip_input(&b->pp->ip, pkt, len, b->now);
}

static void ppp_register(bridge *b)
{
    gc_msg m;
    size_t n;
    memset(&m, 0, sizeof(m));
    m.type = GC_MT_REGISTER;
    m.u.reg.api = GC_API_ARENA;
    memcpy(m.u.reg.device_id, b->dev_addr, GC_DEVICE_ID_LEN);
    strcpy(m.u.reg.name, "N-Gage (PPP)");
    strcpy(m.u.reg.key, b->cfg.access_key);
    n = gc_msg_encode(&m, b->reg_msg, sizeof(b->reg_msg));
    b->reg_len = (gc_u16)n;
    b->registered = 0;
    if (n && b->srv.state == GC_LS_UP) {
        srv_queue(b, b->reg_msg, n);
    }
}

/* every connection is gone on the server: reset them towards the N-Gage */
static void ppp_abort_all(bridge *b)
{
    int c;
    for (c = 0; c < GC_TCP_CONNS; c++) {
        b->pp->conn[c].open = 0;
        b->pp->conn[c].pend = 0;
        gc_ip_tcp_abort(&b->pp->ip, c);
    }
}

static void p_on_state(void *ctx, int ev)
{
    bridge *b = (bridge *)ctx;
    if (ev == GC_PPP_EV_UP) {
        blog(b, "ppp: up, N-Gage %u.%u.%u.%u", b->cfg.ppp_remote[0], b->cfg.ppp_remote[1], b->cfg.ppp_remote[2],
             b->cfg.ppp_remote[3]);
    } else {
        blog(b, ev == GC_PPP_EV_DEAD ? "ppp: link terminated" : "ppp: IP down");
        gc_ip_reset(&b->pp->ip);
    }
}

static void p_log(void *ctx, const char *line)
{
    blog((bridge *)ctx, "%s", line);
}

static void i_send(void *ctx, const gc_u8 *pkt, size_t len)
{
    gc_ppp_send_ip(&((bridge *)ctx)->pp->ppp, pkt, len);
}

static int i_lookup(void *ctx, const char *name, gc_u8 addr[4])
{
    bridge *b = (bridge *)ctx;
    if (!gcn_arena_name(name)) {
        return 0;
    }
    memcpy(addr, b->cfg.ppp_local, 4);
    return 1;
}

static void i_udp(void *ctx, const gc_u8 dst[4], gc_u16 dport, gc_u16 sport, const gc_u8 *data, size_t len)
{
    bridge *b = (bridge *)ctx;
    gcn_msg m;
    memset(&m, 0, sizeof(m));
    m.type = GCN_MT_UDP;
    memcpy(m.addr, dst, 4);
    m.port = dport;
    m.peer_port = sport;
    m.data = data;
    m.len = len;
    b->st_ppp_udp++;
    if (!srv_send_net(b, &m)) {
        blog(b, "ppp: udp to port %u dropped (server queue)", dport);
    }
}

static int i_accept(void *ctx, int c, const gc_u8 dst[4], gc_u16 dport, gc_u16 sport)
{
    bridge *b = (bridge *)ctx;
    gcn_msg m;
    if (!b->reg_len || !conn_flush_close(b, c)) {
        return 0;
    }
    memset(&m, 0, sizeof(m));
    m.type = GCN_MT_TCP_OPEN;
    b->pp->conn[c].gen++;
    m.id = conn_id(b, c);
    memcpy(m.addr, dst, 4);
    m.port = dport;
    m.peer_port = sport;
    if (!srv_send_net(b, &m)) {
        blog(b, "ppp: tcp to port %u refused (server unreachable)", dport);
        return 0;
    }
    b->pp->conn[c].open = 1;
    b->pp->conn[c].pend = 0;
    b->st_ppp_tcp++;
    blog(b, "ppp: tcp %u.%u.%u.%u:%u from port %u (id %04x)", dst[0], dst[1], dst[2], dst[3], dport, sport, m.id);
    return 1;
}

static int i_data(void *ctx, int c, const gc_u8 *data, size_t len)
{
    bridge *b = (bridge *)ctx;
    const size_t chunk = GC_LINK_MAX_MSG - GCN_HDR_DATA;
    size_t chunks = (len + chunk - 1) / chunk, off;
    gcn_msg m;
    if (!b->pp->conn[c].open) {
        return 1; /* server side is gone: swallow, a reset follows */
    }
    if (!srv_usable(b) || BRIDGE_FIFO_LEN - b->to_srv.count < (int)chunks || !fifo_room(&b->to_srv, len < chunk ? len + GCN_HDR_DATA : GC_LINK_MAX_MSG)) {
        return 0; /* not acknowledged: the N-Gage sends it again */
    }
    memset(&m, 0, sizeof(m));
    m.type = GCN_MT_TCP_DATA;
    m.id = conn_id(b, c);
    for (off = 0; off < len; off += chunk) {
        m.data = data + off;
        m.len = len - off > chunk ? chunk : len - off;
        if (!srv_send_net(b, &m)) {
            blog(b, "ppp: server queue full, data lost (id %04x)", m.id);
            conn_close_later(b, c, 2);
            gc_ip_tcp_abort(&b->pp->ip, c);
            return 1;
        }
    }
    return 1;
}

static void i_eof(void *ctx, int c)
{
    conn_close_later((bridge *)ctx, c, 1);
}

static void i_gone(void *ctx, int c, int reset)
{
    bridge *b = (bridge *)ctx;
    if (b->pp->conn[c].open) {
        conn_close_later(b, c, reset ? 2 : 1);
        b->pp->conn[c].open = 0;
    }
}

static int ppp_start(bridge *b)
{
    gc_ppp_cfg pc;
    gc_ppp_ops po;
    gc_ip_ops io;
    if (!b->pp) {
        b->pp = (struct bridge_ppp *)calloc(1, sizeof(*b->pp));
        if (!b->pp) {
            if (!b->ppp_nomem) {
                blog(b, "device speaks PPP, but %u bytes for PPP mode are not available", (unsigned)sizeof(*b->pp));
            }
            b->ppp_nomem = 1;
            return -1;
        }
        blog(b, "PPP mode: %u bytes allocated", (unsigned)sizeof(*b->pp));
    }
    gc_ppp_default_cfg(&pc);
    memcpy(pc.local, b->cfg.ppp_local, 4);
    memcpy(pc.dns, b->cfg.ppp_local, 4);
    memcpy(pc.remote, b->cfg.ppp_remote, 4);
    pc.magic = b->cfg.server_link_id ^ b->now ^ 0x50504C4Bu;
    memset(&po, 0, sizeof(po));
    po.write = p_write;
    po.on_ip = p_on_ip;
    po.on_state = p_on_state;
    po.log = p_log;
    gc_ppp_init(&b->pp->ppp, &pc, &po, b);
    memset(&io, 0, sizeof(io));
    io.send = i_send;
    io.lookup = i_lookup;
    io.udp = i_udp;
    io.tcp_accept = i_accept;
    io.tcp_data = i_data;
    io.tcp_eof = i_eof;
    io.tcp_gone = i_gone;
    io.log = p_log;
    gc_ip_init(&b->pp->ip, &io, b, b->cfg.server_link_id + b->now);
    gc_ip_set_mss(&b->pp->ip, (gc_u16)(GC_LINK_MAX_MSG - GCN_HDR_DATA));
    memset(b->pp->conn, 0, sizeof(b->pp->conn));
    b->mode = BRIDGE_MODE_PPP;
    blog(b, "device speaks PPP: retail Arena mode");
    ppp_register(b);
    return 0;
}

/* a GCN message from the server */
static void ppp_srv_msg(bridge *b, const gc_u8 *d, size_t n)
{
    gcn_msg m;
    int c;
    if (!gcn_decode(d, n, &m)) {
        return;
    }
    if (m.type == GCN_MT_UDP) {
        gc_ip_udp_send(&b->pp->ip, m.addr, m.port, m.peer_port, m.data, m.len);
        return;
    }
    c = m.id & 0xFF;
    if (c >= GC_TCP_CONNS || (m.id >> 8) != b->pp->conn[c].gen || !b->pp->conn[c].open) {
        return; /* an old connection */
    }
    if (m.type == GCN_MT_TCP_DATA) {
        if (gc_ip_tcp_send(&b->pp->ip, c, m.data, m.len, b->now) != (int)m.len) {
            blog(b, "ppp: tcp send failed (id %04x)", m.id);
        }
    } else if (m.type == GCN_MT_TCP_CLOSE) {
        if (m.how == GCN_CLOSE_RESET) {
            b->pp->conn[c].open = 0;
            gc_ip_tcp_abort(&b->pp->ip, c);
        } else {
            gc_ip_tcp_close(&b->pp->ip, c, b->now);
        }
    }
}

static int ppp_can_accept(bridge *b, const gc_u8 *d, size_t n)
{
    int c;
    if (n < GCN_HDR_DATA || d[0] != GCN_MT_TCP_DATA) {
        return 1;
    }
    c = d[2];
    if (c >= GC_TCP_CONNS || d[1] != b->pp->conn[c].gen || !b->pp->conn[c].open) {
        return 1; /* dropped anyway */
    }
    return gc_ip_tcp_space(&b->pp->ip, c) >= n - GCN_HDR_DATA;
}

static void ppp_poll(bridge *b)
{
    int c;
    gc_ppp_poll(&b->pp->ppp, b->now);
    gc_ip_poll(&b->pp->ip, b->now);
    for (c = 0; c < GC_TCP_CONNS; c++) {
        conn_flush_close(b, c);
    }
}

#endif /* BRIDGE_PPP */

/* ---- device link callbacks ---- */

static void dev_write_cb(void *ctx, const gc_u8 *d, size_t n)
{
    bridge *b = (bridge *)ctx;
    if (b->dev_connected && b->io.dev_write) {
        b->io.dev_write(b->ctx, d, n);
    }
}

static int dev_can_accept(void *ctx, const gc_u8 *msg, size_t len)
{
    bridge *b = (bridge *)ctx;
    /* a REGISTER grows when the device id and the access key are added */
    return fifo_room(&b->to_srv, len && msg[0] == GC_MT_REGISTER ? GC_LINK_MAX_MSG : len);
}

static void dev_msg_cb(void *ctx, const gc_u8 *d, size_t n)
{
    bridge *b = (bridge *)ctx;
    gc_msg m;
    gc_u8 buf[GC_LINK_MAX_MSG];

    b->st_dev_msgs++;
    if (n == 0) {
        return;
    }
    if (d[0] == GC_MT_BRIDGE_QUERY) {
        send_status(b);
        return;
    }
    if (d[0] == GC_MT_REGISTER) {
        /* fill in the device id from the Bluetooth address and add the
         * access key; remember the message for replay */
        if (!gc_msg_decode(d, n, &m)) {
            return;
        }
        if (m.u.reg.device_id[0] == 0 && m.u.reg.device_id[1] == 0 && m.u.reg.device_id[2] == 0 &&
            m.u.reg.device_id[3] == 0 && m.u.reg.device_id[4] == 0 && m.u.reg.device_id[5] == 0) {
            memcpy(m.u.reg.device_id, b->dev_addr, GC_DEVICE_ID_LEN);
        }
        strcpy(m.u.reg.key, b->cfg.access_key);
        n = gc_msg_encode(&m, buf, sizeof(buf));
        if (!n) {
            return;
        }
        memcpy(b->reg_msg, buf, n);
        b->reg_len = (gc_u16)n;
        b->registered = 0;
        d = buf;
        blog(b, "device '%s' registers (api %u, variant %u)", m.u.reg.name, m.u.reg.api, m.u.reg.variant);
    }
    if (!srv_usable(b)) {
        reply_unavailable(b, d[0]);
        return;
    }
    if (d[0] == GC_MT_REGISTER && b->srv.state != GC_LS_UP) {
        return; /* replayed automatically when the link comes up */
    }
    srv_queue(b, d, n);
}

static void dev_state_cb(void *ctx, int ev, int reason)
{
    bridge *b = (bridge *)ctx;
    switch (ev) {
    case GC_LE_UP_FRESH:
        blog(b, "device link up");
        fifo_clear(&b->to_dev);
        b->dev_lost_pending = 0;
        send_status(b);
        break;
    case GC_LE_UP_RESUMED:
        blog(b, "device link resumed");
        b->dev_lost_pending = 0;
        fifo_drain(b, &b->to_dev, &b->dev);
        break;
    case GC_LE_DOWN:
    case GC_LE_CLOSED:
        blog(b, "device link %s (%d)", ev == GC_LE_DOWN ? "down" : "closed", reason);
        if (!b->dev_lost_pending) {
            b->dev_lost_pending = 1;
            b->t_dev_lost = b->now;
        }
        break;
    default:
        break;
    }
}

static void dev_hello_cb(void *ctx, const gc_hello *h)
{
    bridge *b = (bridge *)ctx;
    /* a new link id means the DLL restarted: start a fresh link */
    gc_link_accept(&b->dev, h, 1, b->now);
}

/* ---- server link callbacks ---- */

static void srv_write_cb(void *ctx, const gc_u8 *d, size_t n)
{
    bridge *b = (bridge *)ctx;
    if (b->io.srv_write && b->srv_state != GC_BS_DOWN) {
        b->io.srv_write(b->ctx, d, n);
    }
}

static int srv_can_accept(void *ctx, const gc_u8 *msg, size_t len)
{
    bridge *b = (bridge *)ctx;
#if BRIDGE_PPP
    if (b->mode == BRIDGE_MODE_PPP) {
        return ppp_can_accept(b, msg, len);
    }
#endif
    (void)msg;
    return fifo_room(&b->to_dev, len);
}

static void srv_msg_cb(void *ctx, const gc_u8 *d, size_t n)
{
    bridge *b = (bridge *)ctx;

    b->st_srv_msgs++;
    if (n == 0) {
        return;
    }
    if (d[0] == GC_MT_REGISTERED && n >= 2) {
        b->registered = ((gc_i8)d[1] == GC_R_OK);
    }
#if BRIDGE_PPP
    if (b->mode == BRIDGE_MODE_PPP) {
        if (GCN_IS_NET(d[0])) {
            ppp_srv_msg(b, d, n);
        } else if (d[0] == GC_MT_REGISTERED) {
            blog(b, "ppp: server %s the registration", b->registered ? "accepted" : "refused");
        } else if (d[0] == GC_MT_ERROR) {
            gc_msg m;
            if (gc_msg_decode(d, n, &m)) {
                blog(b, "ppp: server error %d: %s", m.u.error.code, m.u.error.text);
            }
        }
        return;
    }
#endif
    if (b->dev.state == GC_LS_UP && b->to_dev.count == 0 && gc_link_send(&b->dev, d, n, b->now) == GC_LINK_OK) {
        return;
    }
    if (!fifo_push(&b->to_dev, d, n)) {
        blog(b, "device queue full, dropping %s", gc_msg_name(d[0])); /* cannot happen: can_accept */
    }
}

static void srv_state_cb(void *ctx, int ev, int reason)
{
    bridge *b = (bridge *)ctx;
    switch (ev) {
    case GC_LE_UP_FRESH:
        blog(b, "server link up (fresh)");
        b->backoff = b->cfg.reconnect_min_ms;
        set_srv_state(b, GC_BS_UP);
        b->registered = 0;
        /* everything the server knew about us is gone: replay the
         * registration; the DLL sees the unsolicited REGISTERED and
         * knows that any session state was lost */
        fifo_clear(&b->to_srv);
#if BRIDGE_PPP
        if (b->mode == BRIDGE_MODE_PPP) {
            ppp_abort_all(b); /* the server lost the tunnelled connections */
            if (b->reg_len && b->dev_connected) {
                gc_link_send(&b->srv, b->reg_msg, b->reg_len, b->now);
            }
            b->srv_ever_up = 1;
            break;
        }
#endif
        if (b->reg_len && b->dev.state == GC_LS_UP) {
            gc_link_send(&b->srv, b->reg_msg, b->reg_len, b->now);
        }
        b->srv_ever_up = 1;
        break;
    case GC_LE_UP_RESUMED:
        blog(b, "server link resumed");
        b->backoff = b->cfg.reconnect_min_ms;
        set_srv_state(b, GC_BS_UP);
        fifo_drain(b, &b->to_srv, &b->srv);
        break;
    case GC_LE_DOWN:
        blog(b, "server link down (%d)", reason);
        if (b->io.srv_disconnect) {
            b->io.srv_disconnect(b->ctx);
        }
        set_srv_state(b, GC_BS_CONNECTING);
        b->t_reconnect = b->now + b->backoff;
        break;
    case GC_LE_CLOSED:
        blog(b, "server link closed (%d)", reason);
        if (b->io.srv_disconnect) {
            b->io.srv_disconnect(b->ctx);
        }
        fifo_clear(&b->to_srv);
        set_srv_state(b, GC_BS_CONNECTING);
        b->t_reconnect = b->now + b->backoff;
        break;
    default:
        break;
    }
}

/* ---- public ---- */

void bridge_default_cfg(bridge_cfg *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->server_link_id = 0x42524447; /* overwrite with a random value */
    cfg->device_grace_ms = 15000;
    cfg->reconnect_min_ms = 1000;
    cfg->reconnect_max_ms = 15000;
    cfg->ppp = 1;
    cfg->ppp_local[0] = cfg->ppp_remote[0] = 10;
    cfg->ppp_local[1] = cfg->ppp_remote[1] = 64;
    cfg->ppp_local[3] = 1;
    cfg->ppp_remote[3] = 2;
}

void bridge_init(bridge *b, const bridge_cfg *cfg, const bridge_io *io, void *ctx)
{
    gc_link_cfg lc;
    gc_link_ops ops;

    memset(b, 0, sizeof(*b));
    b->cfg = *cfg;
    b->io = *io;
    b->ctx = ctx;
    b->backoff = cfg->reconnect_min_ms;
    b->srv_state = GC_BS_DOWN;

    gc_link_default_cfg(&lc, GC_ROLE_BRIDGE, 0);
    ops.write = dev_write_cb;
    ops.on_msg = dev_msg_cb;
    ops.on_state = dev_state_cb;
    ops.on_hello = dev_hello_cb;
    ops.can_accept = dev_can_accept;
    gc_link_init(&b->dev, &lc, &ops, b);

    gc_link_default_cfg(&lc, GC_ROLE_BRIDGE, 1);
    lc.ka_ms = 5000;
    lc.dead_ms = 20000;
    lc.rto_ms = 1000;
    lc.rto_max_ms = 5000;
    lc.hello_ms = 2000;
    ops.write = srv_write_cb;
    ops.on_msg = srv_msg_cb;
    ops.on_state = srv_state_cb;
    ops.on_hello = NULL;
    ops.can_accept = srv_can_accept;
    gc_link_init(&b->srv, &lc, &ops, b);
}

void bridge_release(bridge *b)
{
#if BRIDGE_PPP
    free(b->pp);
    b->pp = NULL;
    if (b->mode == BRIDGE_MODE_PPP) {
        b->mode = BRIDGE_MODE_NONE;
    }
#else
    (void)b;
#endif
}

void bridge_dev_connected(bridge *b, const gc_u8 *bt_addr, gc_u32 now)
{
    b->now = now;
    b->dev_connected = 1;
    b->mode = BRIDGE_MODE_NONE;
    b->ppp_nomem = 0;
    if (bt_addr) {
        memcpy(b->dev_addr, bt_addr, GC_DEVICE_ID_LEN);
    }
    blog(b, "device connected %02X:%02X:%02X:%02X:%02X:%02X", b->dev_addr[0], b->dev_addr[1], b->dev_addr[2],
         b->dev_addr[3], b->dev_addr[4], b->dev_addr[5]);
    gc_link_listen(&b->dev, now);
}

void bridge_dev_input(bridge *b, const gc_u8 *data, size_t len, gc_u32 now)
{
    b->now = now;
    if (!len) {
        return;
    }
    if (b->mode == BRIDGE_MODE_NONE) {
#if BRIDGE_PPP
        if (b->cfg.ppp && (b->ppp_nomem || gc_ppp_detect(data, len))) {
            if (ppp_start(b) != 0) {
                /* PPP bytes must not reach the GameComms decoder: drop
                 * them and retry with the next ones (the N-Gage repeats
                 * its LCP request) */
                return;
            }
        } else
#endif
        {
            /* name what the device started with: a GameComms frame, or an
             * N-Gage dialling a modem ("AT...") when its access point is
             * not set to direct PPP (Arena) */
            char hx[3 * 8 + 1];
            size_t i, k = 0;
            for (i = 0; i < len && i < 8; i++) {
                k += (size_t)snprintf(hx + k, sizeof(hx) - k, "%02x ", data[i]);
            }
            hx[k ? k - 1 : 0] = 0;
            if (len >= 2 && (data[0] == 'A' || data[0] == 'a') && (data[1] == 'T' || data[1] == 't')) {
                blog(b, "device sends modem commands (%s): set its Arena access point to PPP again", hx);
            } else {
                blog(b, "device speaks GameComms (first bytes %s)", hx);
            }
            b->mode = BRIDGE_MODE_LINK;
        }
    }
#if BRIDGE_PPP
    if (b->mode == BRIDGE_MODE_PPP) {
        gc_ppp_input(&b->pp->ppp, data, len, now);
        fifo_drain(b, &b->to_srv, &b->srv);
        return;
    }
#endif
    gc_link_input(&b->dev, data, len, now);
    fifo_drain(b, &b->to_srv, &b->srv);
}

void bridge_dev_disconnected(bridge *b, gc_u32 now)
{
    b->now = now;
    b->dev_connected = 0;
    blog(b, "device disconnected");
#if BRIDGE_PPP
    if (b->mode == BRIDGE_MODE_PPP) {
        gc_ip_reset(&b->pp->ip); /* tells the server about every open connection */
    }
#endif
    gc_link_transport_lost(&b->dev, now);
    if (!b->dev_lost_pending) {
        b->dev_lost_pending = 1;
        b->t_dev_lost = now;
    }
}

void bridge_set_wifi(bridge *b, int up, gc_i8 rssi, gc_u32 now)
{
    b->now = now;
    b->rssi = rssi;
    if (up == b->wifi_up) {
        return;
    }
    b->wifi_up = up;
    blog(b, "wifi %s", up ? "up" : "down");
    if (up) {
        b->t_reconnect = now;
        set_srv_state(b, GC_BS_CONNECTING);
    } else {
        if (b->io.srv_disconnect) {
            b->io.srv_disconnect(b->ctx);
        }
        gc_link_transport_lost(&b->srv, now);
        set_srv_state(b, b->srv.has_session ? GC_BS_CONNECTING : GC_BS_DOWN);
    }
    send_status(b);
}

void bridge_srv_connected(bridge *b, gc_u32 now)
{
    b->now = now;
    blog(b, "tcp connected");
    gc_link_start(&b->srv, b->cfg.server_link_id, now);
}

void bridge_srv_input(bridge *b, const gc_u8 *data, size_t len, gc_u32 now)
{
    b->now = now;
    gc_link_input(&b->srv, data, len, now);
    fifo_drain(b, &b->to_dev, &b->dev);
}

void bridge_srv_disconnected(bridge *b, gc_u32 now)
{
    b->now = now;
    gc_link_transport_lost(&b->srv, now);
    if (b->srv_state != GC_BS_DOWN || b->wifi_up) {
        set_srv_state(b, b->wifi_up ? GC_BS_CONNECTING : GC_BS_DOWN);
    }
    b->t_reconnect = now + b->backoff;
    b->backoff = b->backoff * 2 > b->cfg.reconnect_max_ms ? b->cfg.reconnect_max_ms : b->backoff * 2;
}

void bridge_poll(bridge *b, gc_u32 now)
{
    b->now = now;
    gc_link_poll(&b->dev, now);
    gc_link_poll(&b->srv, now);
#if BRIDGE_PPP
    if (b->mode == BRIDGE_MODE_PPP) {
        ppp_poll(b);
    }
#endif

    /* (re)connect to the server */
    if (b->wifi_up && b->srv.state != GC_LS_UP && b->srv.state != GC_LS_HELLO_SENT &&
        !GC_TIME_BEFORE(now, b->t_reconnect)) {
        b->t_reconnect = now + b->backoff;
        b->backoff = b->backoff * 2 > b->cfg.reconnect_max_ms ? b->cfg.reconnect_max_ms : b->backoff * 2;
        set_srv_state(b, GC_BS_CONNECTING);
        if (b->io.srv_connect) {
            b->io.srv_connect(b->ctx);
        }
    }
    /* a resumable server session that never comes back is abandoned */
    if (b->srv_state == GC_BS_CONNECTING && !b->wifi_up && b->srv.has_session &&
        (gc_u32)(now - b->srv.t_last_rx) > 60000) {
        gc_link_forget(&b->srv);
        fifo_clear(&b->to_srv);
        set_srv_state(b, GC_BS_DOWN);
    }

    /* device gone for good: release its server side state */
    if (b->dev_lost_pending && b->dev.state != GC_LS_UP && (gc_u32)(now - b->t_dev_lost) >= b->cfg.device_grace_ms) {
        b->dev_lost_pending = 0;
        blog(b, "device did not come back, leaving session");
        fifo_clear(&b->to_dev);
        gc_link_forget(&b->dev);
        if (b->reg_len && b->srv.state == GC_LS_UP) {
            gc_msg m;
            gc_u8 buf[8];
            size_t n;
            memset(&m, 0, sizeof(m));
            m.type = GC_MT_LEAVE;
            m.u.reason.reason = GC_LEFT_LOST;
            n = gc_msg_encode(&m, buf, sizeof(buf));
            srv_queue(b, buf, n);
        }
        b->reg_len = 0;
    }

    fifo_drain(b, &b->to_srv, &b->srv);
    fifo_drain(b, &b->to_dev, &b->dev);
}

gc_u32 bridge_next_timeout(bridge *b, gc_u32 now)
{
    gc_u32 a = gc_link_next_timeout(&b->dev, now);
    gc_u32 c = gc_link_next_timeout(&b->srv, now);
#if BRIDGE_PPP
    if (b->mode == BRIDGE_MODE_PPP && a > 200) {
        a = 200; /* PPP restart and TCP retransmission timers */
    }
#endif
    return a < c ? a : c;
}
