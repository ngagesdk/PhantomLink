/** @file gc_ip.c
 *
 *  Minimal IPv4/ICMP/UDP/DNS/TCP end point, see gc_ip.h.
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#include "gc_ip.h"

#include <string.h>

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

#define RTO_INIT      1000
#define RTO_MAX       16000
#define MAX_RETRIES   8
#define FIN_WAIT_2_MS 30000

/* ---- helpers ---- */

static gc_u16 rd16(const gc_u8 *p)
{
    return (gc_u16)((p[0] << 8) | p[1]);
}

static gc_u32 rd32(const gc_u8 *p)
{
    return ((gc_u32)p[0] << 24) | ((gc_u32)p[1] << 16) | ((gc_u32)p[2] << 8) | (gc_u32)p[3];
}

static void wr16(gc_u8 *p, gc_u16 v)
{
    p[0] = (gc_u8)(v >> 8);
    p[1] = (gc_u8)v;
}

static void wr32(gc_u8 *p, gc_u32 v)
{
    p[0] = (gc_u8)(v >> 24);
    p[1] = (gc_u8)(v >> 16);
    p[2] = (gc_u8)(v >> 8);
    p[3] = (gc_u8)v;
}

#define SEQ_LT(a, b)  ((gc_i32)((a) - (b)) < 0)
#define SEQ_LEQ(a, b) ((gc_i32)((a) - (b)) <= 0)

gc_u32 gc_ip_sum(gc_u32 sum, const gc_u8 *d, size_t n)
{
    size_t i;
    for (i = 0; i + 1 < n; i += 2) {
        sum += (gc_u32)((d[i] << 8) | d[i + 1]);
    }
    if (n & 1) {
        sum += (gc_u32)(d[n - 1] << 8);
    }
    return sum;
}

gc_u16 gc_ip_fold(gc_u32 sum)
{
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (gc_u16)~sum;
}

static gc_u32 pseudo(const gc_u8 *src, const gc_u8 *dst, gc_u8 proto, size_t len)
{
    gc_u32 s = gc_ip_sum(0, src, 4);
    s = gc_ip_sum(s, dst, 4);
    return s + proto + (gc_u32)len;
}

static void ilog(gc_ip *ip, const char *s)
{
    if (ip->ops.log) {
        ip->ops.log(ip->ctx, s);
    }
}

/* Completes the IPv4 header in ip->out (payload already at out + 20) and
 * sends it. */
static void ip_send(gc_ip *ip, const gc_u8 *src, const gc_u8 *dst, gc_u8 proto, size_t plen)
{
    gc_u8 *h = ip->out;
    size_t len = 20 + plen;
    h[0] = 0x45;
    h[1] = 0;
    wr16(h + 2, (gc_u16)len);
    wr16(h + 4, ++ip->ip_id);
    wr16(h + 6, 0x4000); /* DF */
    h[8] = 64;
    h[9] = proto;
    wr16(h + 10, 0);
    memcpy(h + 12, src, 4);
    memcpy(h + 16, dst, 4);
    wr16(h + 10, gc_ip_fold(gc_ip_sum(0, h, 20)));
    ip->st_tx++;
    ip->ops.send(ip->ctx, h, len);
}

/* ---- ICMP ---- */

static void icmp_input(gc_ip *ip, const gc_u8 *src, const gc_u8 *dst, const gc_u8 *d, size_t n)
{
    gc_u8 *o = ip->out + 20;
    if (n < 8 || n > sizeof(ip->out) - 20 || gc_ip_fold(gc_ip_sum(0, d, n)) != 0 || d[0] != 8) {
        return;
    }
    memcpy(o, d, n);
    o[0] = 0; /* echo reply */
    wr16(o + 2, 0);
    wr16(o + 2, gc_ip_fold(gc_ip_sum(0, o, n)));
    ip_send(ip, dst, src, GC_IP_PROTO_ICMP, n);
}

/* ---- UDP and DNS ---- */

static int udp_out(gc_ip *ip, const gc_u8 *src, const gc_u8 *dst, gc_u16 sport, gc_u16 dport, const gc_u8 *data,
                   size_t len)
{
    gc_u8 *u = ip->out + 20;
    gc_u16 cs;
    if (len + 28 > sizeof(ip->out)) {
        return -1;
    }
    wr16(u, sport);
    wr16(u + 2, dport);
    wr16(u + 4, (gc_u16)(len + 8));
    wr16(u + 6, 0);
    if (data != u + 8 && len) {
        memcpy(u + 8, data, len);
    }
    cs = gc_ip_fold(gc_ip_sum(pseudo(src, dst, GC_IP_PROTO_UDP, len + 8), u, len + 8));
    wr16(u + 6, cs ? cs : 0xFFFF);
    ip_send(ip, src, dst, GC_IP_PROTO_UDP, len + 8);
    return 0;
}

int gc_ip_udp_send(gc_ip *ip, const gc_u8 src[4], gc_u16 sport, gc_u16 dport, const gc_u8 *data, size_t len)
{
    if (!ip->have_peer) {
        return -1;
    }
    return udp_out(ip, src, ip->peer, sport, dport, data, len);
}

/* Answers a DNS query into ip->out (+28); returns the answer length or 0. */
static size_t dns_answer(gc_ip *ip, const gc_u8 *q, size_t n)
{
    gc_u8 *a = ip->out + 28;
    size_t cap = sizeof(ip->out) - 28, i = 12, qend, o;
    char name[256];
    size_t nl = 0;
    gc_u16 qtype, qclass;
    gc_u8 addr[4];
    int found;
    if (n < 17 || (q[2] & 0x80) || rd16(q + 4) != 1) {
        return 0; /* responses and multi question queries are ignored */
    }
    while (i < n && q[i]) {
        gc_u8 l = q[i];
        size_t k;
        if ((l & 0xC0) || i + 1 + l > n || nl + l + 1 >= sizeof(name)) {
            return 0;
        }
        if (nl) {
            name[nl++] = '.';
        }
        for (k = 0; k < l; k++) {
            char c = (char)q[i + 1 + k];
            name[nl++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
        }
        i += 1 + l;
    }
    name[nl] = 0;
    qend = i + 5;
    if (qend > n || qend + 16 > cap) {
        return 0;
    }
    qtype = rd16(q + i + 1);
    qclass = rd16(q + i + 3);
    found = ip->ops.lookup ? ip->ops.lookup(ip->ctx, name, addr) : 0;

    memcpy(a, q, qend);
    a[2] = (gc_u8)(0x80 | (q[2] & 0x79) | 0x04); /* QR, opcode, AA, RD copied */
    a[3] = (gc_u8)(0x80 | (found ? 0 : 3));      /* RA, NOERROR / NXDOMAIN */
    wr16(a + 6, 0);
    wr16(a + 8, 0);
    wr16(a + 10, 0);
    o = qend;
    if (found && qtype == 1 && qclass == 1) {
        wr16(a + 6, 1);
        a[o++] = 0xC0; /* pointer to the question name */
        a[o++] = 0x0C;
        wr16(a + o, 1);
        wr16(a + o + 2, 1);
        wr32(a + o + 4, 60);
        wr16(a + o + 8, 4);
        memcpy(a + o + 10, addr, 4);
        o += 14;
    }
    if (ip->ops.log) {
        char line[300];
        size_t k = 0;
        const char *pre = found ? "dns: " : "dns: unknown ";
        while (*pre && k < sizeof(line) - 1) {
            line[k++] = *pre++;
        }
        for (i = 0; name[i] && k < sizeof(line) - 1; i++) {
            line[k++] = name[i];
        }
        line[k] = 0;
        ilog(ip, line);
    }
    return o;
}

static void udp_input(gc_ip *ip, const gc_u8 *src, const gc_u8 *dst, const gc_u8 *d, size_t n)
{
    gc_u16 sport, dport, ulen;
    if (n < 8) {
        return;
    }
    ulen = rd16(d + 4);
    if (ulen < 8 || ulen > n) {
        return;
    }
    if (rd16(d + 6) && gc_ip_fold(gc_ip_sum(pseudo(src, dst, GC_IP_PROTO_UDP, ulen), d, ulen)) != 0) {
        ip->st_rx_bad++;
        return;
    }
    sport = rd16(d);
    dport = rd16(d + 2);
    if (dport == 53) {
        size_t al = dns_answer(ip, d + 8, ulen - 8u);
        if (al) {
            udp_out(ip, dst, src, 53, sport, ip->out + 28, al);
        }
        return;
    }
    if (ip->ops.udp) {
        ip->ops.udp(ip->ctx, dst, dport, sport, d + 8, ulen - 8u);
    }
}

/* ---- TCP ---- */

static void tcp_out(gc_ip *ip, gc_tcp_conn *c, gc_u32 seq, gc_u8 flags, const gc_u8 *data, size_t len)
{
    gc_u8 *t = ip->out + 20;
    size_t hl = (flags & TCP_SYN) ? 24 : 20;
    gc_u32 wnd = GC_TCP_WND;
    wr16(t, c->lport);
    wr16(t + 2, c->rport);
    wr32(t + 4, seq);
    wr32(t + 8, (flags & TCP_ACK) ? c->rcv_nxt : 0);
    t[12] = (gc_u8)((hl / 4) << 4);
    t[13] = flags;
    wr16(t + 14, (gc_u16)(wnd > 65535 ? 65535 : wnd));
    wr16(t + 16, 0);
    wr16(t + 18, 0);
    if (flags & TCP_SYN) {
        t[20] = 2; /* MSS */
        t[21] = 4;
        wr16(t + 22, ip->mss);
    }
    if (len) {
        memcpy(t + hl, data, len);
    }
    wr16(t + 16, gc_ip_fold(gc_ip_sum(pseudo(c->laddr, c->raddr, GC_IP_PROTO_TCP, hl + len), t, hl + len)));
    ip_send(ip, c->laddr, c->raddr, GC_IP_PROTO_TCP, hl + len);
}

/* reset in answer to a segment without a connection (RFC 793 p.36) */
static void tcp_rst_reply(gc_ip *ip, const gc_u8 *src, const gc_u8 *dst, const gc_u8 *t, size_t seglen)
{
    gc_tcp_conn c;
    gc_u8 fl = t[13];
    memset(&c, 0, sizeof(c));
    memcpy(c.laddr, dst, 4);
    memcpy(c.raddr, src, 4);
    c.lport = rd16(t + 2);
    c.rport = rd16(t);
    if (fl & TCP_RST) {
        return;
    }
    if (fl & TCP_ACK) {
        tcp_out(ip, &c, rd32(t + 8), TCP_RST, NULL, 0);
    } else {
        c.rcv_nxt = rd32(t + 4) + (gc_u32)seglen + ((fl & TCP_SYN) ? 1u : 0u) + ((fl & TCP_FIN) ? 1u : 0u);
        tcp_out(ip, &c, 0, TCP_RST | TCP_ACK, NULL, 0);
    }
}

static void conn_free(gc_ip *ip, int i, int reset)
{
    gc_tcp_conn *c = &ip->tcp[i];
    int was = c->state;
    c->state = GC_TCP_FREE;
    c->timer = 0;
    if (was != GC_TCP_FREE && ip->ops.tcp_gone) {
        ip->ops.tcp_gone(ip->ctx, i, reset || was == GC_TCP_SYN_RCVD);
    }
}

static void arm(gc_ip *ip, gc_tcp_conn *c)
{
    c->t_rto = ip->now + c->rto;
    c->timer = 1;
}

/* Sends what the window allows from snd_nxt on, and the FIN. */
static void tcp_output(gc_ip *ip, gc_tcp_conn *c)
{
    gc_u32 off;
    if (c->state == GC_TCP_SYN_RCVD) {
        return;
    }
    for (;;) {
        gc_u32 inflight = c->snd_nxt - c->snd_una - (c->fin_sent ? 1u : 0u);
        size_t unsent, n, k, pos;
        gc_u32 wnd = c->snd_wnd;
        gc_u8 *seg = ip->seg;
        off = inflight;
        unsent = c->tx_len > off ? c->tx_len - off : 0;
        if (!unsent || c->fin_sent) {
            break;
        }
        if (wnd <= inflight) {
            if (!c->timer) {
                arm(ip, c); /* persist: probe the window later */
            }
            break;
        }
        n = unsent;
        if (n > wnd - inflight) {
            n = wnd - inflight;
        }
        if (n > c->mss) {
            n = c->mss;
        }
        pos = (c->tx_head + off) % GC_TCP_TXBUF;
        for (k = 0; k < n; k++) {
            seg[k] = c->tx[(pos + k) % GC_TCP_TXBUF];
        }
        tcp_out(ip, c, c->snd_nxt, (gc_u8)(TCP_ACK | TCP_PSH), seg, n);
        c->snd_nxt += (gc_u32)n;
        if (!c->timer) {
            arm(ip, c);
        }
    }
    if (c->fin_queued && !c->fin_sent && c->snd_nxt - c->snd_una == c->tx_len) {
        tcp_out(ip, c, c->snd_nxt, TCP_FIN | TCP_ACK, NULL, 0);
        c->snd_nxt++;
        c->fin_sent = 1;
        if (c->state == GC_TCP_ESTAB) {
            c->state = GC_TCP_FIN_WAIT_1;
        } else if (c->state == GC_TCP_CLOSE_WAIT) {
            c->state = GC_TCP_LAST_ACK;
        }
        if (!c->timer) {
            arm(ip, c);
        }
    }
}

static void tcp_syn(gc_ip *ip, const gc_u8 *src, const gc_u8 *dst, const gc_u8 *t, size_t hl)
{
    int i, slot = -1;
    gc_tcp_conn *c;
    size_t o;
    for (i = 0; i < GC_TCP_CONNS; i++) {
        if (ip->tcp[i].state == GC_TCP_FREE) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        ilog(ip, "tcp: no free connection");
        tcp_rst_reply(ip, src, dst, t, 0);
        return;
    }
    c = &ip->tcp[slot];
    memset(c, 0, sizeof(*c));
    memcpy(c->laddr, dst, 4);
    memcpy(c->raddr, src, 4);
    c->lport = rd16(t + 2);
    c->rport = rd16(t);
    c->rcv_nxt = rd32(t + 4) + 1;
    c->snd_wnd = rd16(t + 14);
    c->mss = 536;
    for (o = 20; o < hl;) {
        gc_u8 k = t[o];
        if (k == 0) {
            break;
        }
        if (k == 1) {
            o++;
            continue;
        }
        if (o + 1 >= hl || t[o + 1] < 2 || o + t[o + 1] > hl) {
            break;
        }
        if (k == 2 && t[o + 1] == 4) {
            c->mss = rd16(t + o + 2);
        }
        o += t[o + 1];
    }
    if (c->mss > ip->mss) {
        c->mss = ip->mss;
    }
    if (c->mss < 64) {
        c->mss = 64;
    }
    if (!ip->ops.tcp_accept || !ip->ops.tcp_accept(ip->ctx, slot, dst, c->lport, c->rport)) {
        tcp_rst_reply(ip, src, dst, t, 0);
        return;
    }
    ip->iss_seed = ip->iss_seed * 1103515245u + 12345u + ip->now;
    c->snd_una = ip->iss_seed;
    c->snd_nxt = c->snd_una + 1;
    c->state = GC_TCP_SYN_RCVD;
    c->rto = RTO_INIT;
    tcp_out(ip, c, c->snd_una, TCP_SYN | TCP_ACK, NULL, 0);
    arm(ip, c);
}

static void tcp_input(gc_ip *ip, const gc_u8 *src, const gc_u8 *dst, const gc_u8 *t, size_t n)
{
    size_t hl, dlen;
    gc_u8 fl;
    gc_u32 seq, ack;
    gc_tcp_conn *c = NULL;
    int i, idx = -1;
    const gc_u8 *data;

    if (n < 20) {
        return;
    }
    hl = (size_t)(t[12] >> 4) * 4;
    if (hl < 20 || hl > n || gc_ip_fold(gc_ip_sum(pseudo(src, dst, GC_IP_PROTO_TCP, n), t, n)) != 0) {
        ip->st_rx_bad++;
        return;
    }
    fl = t[13];
    seq = rd32(t + 4);
    ack = rd32(t + 8);
    data = t + hl;
    dlen = n - hl;

    for (i = 0; i < GC_TCP_CONNS; i++) {
        gc_tcp_conn *k = &ip->tcp[i];
        if (k->state != GC_TCP_FREE && k->lport == rd16(t + 2) && k->rport == rd16(t) && !memcmp(k->laddr, dst, 4) &&
            !memcmp(k->raddr, src, 4)) {
            c = k;
            idx = i;
            break;
        }
    }
    if (!c) {
        if ((fl & (TCP_SYN | TCP_ACK | TCP_RST)) == TCP_SYN) {
            tcp_syn(ip, src, dst, t, hl);
        } else {
            tcp_rst_reply(ip, src, dst, t, dlen);
        }
        return;
    }
    if (fl & TCP_RST) {
        conn_free(ip, idx, 1);
        return;
    }
    if (fl & TCP_SYN) {
        if (c->state == GC_TCP_SYN_RCVD && seq + 1 == c->rcv_nxt) {
            tcp_out(ip, c, c->snd_una, TCP_SYN | TCP_ACK, NULL, 0); /* our SYN-ACK was lost */
        } else {
            tcp_out(ip, c, c->snd_nxt, TCP_ACK, NULL, 0);
        }
        return;
    }
    if (!(fl & TCP_ACK)) {
        return;
    }

    /* acknowledgement */
    if (SEQ_LT(c->snd_una, ack) && SEQ_LEQ(ack, c->snd_nxt)) {
        gc_u32 acked = ack - c->snd_una;
        if (c->state == GC_TCP_SYN_RCVD) {
            acked--; /* our SYN */
            c->state = GC_TCP_ESTAB;
        }
        if (c->fin_sent && ack == c->snd_nxt) {
            acked--; /* our FIN */
        }
        if (acked > c->tx_len) {
            acked = (gc_u32)c->tx_len;
        }
        c->tx_head = (c->tx_head + acked) % GC_TCP_TXBUF;
        c->tx_len -= acked;
        c->snd_una = ack;
        c->retries = 0;
        c->rto = RTO_INIT;
        c->timer = 0;
        if (c->snd_una != c->snd_nxt) {
            arm(ip, c);
        }
        if (acked) {
            c->writable_pending = 1;
        }
        if (c->fin_sent && ack == c->snd_nxt) {
            if (c->state == GC_TCP_FIN_WAIT_1) {
                c->state = GC_TCP_FIN_WAIT_2;
                c->t_rto = ip->now + FIN_WAIT_2_MS; /* the peer may never close */
                c->timer = 1;
            } else if (c->state == GC_TCP_CLOSING || c->state == GC_TCP_LAST_ACK) {
                conn_free(ip, idx, 0);
                return;
            }
        }
    } else if (c->state == GC_TCP_SYN_RCVD) {
        tcp_rst_reply(ip, src, dst, t, dlen);
        return;
    }
    c->snd_wnd = rd16(t + 14);

    /* data and FIN, in order only */
    if (dlen || (fl & TCP_FIN)) {
        int can_take = c->state == GC_TCP_ESTAB || c->state == GC_TCP_FIN_WAIT_1 || c->state == GC_TCP_FIN_WAIT_2;
        if (seq != c->rcv_nxt) {
            /* old (retransmitted) or out of order: repeat our ACK; take
             * the new tail of a partially old segment */
            gc_u32 skip = c->rcv_nxt - seq;
            if (SEQ_LT(seq, c->rcv_nxt) && skip < dlen && can_take) {
                data += skip;
                dlen -= skip;
                seq = c->rcv_nxt;
            } else {
                tcp_out(ip, c, c->snd_nxt, TCP_ACK, NULL, 0);
                return;
            }
        }
        if (dlen && can_take) {
            if (ip->ops.tcp_data && !ip->ops.tcp_data(ip->ctx, idx, data, dlen)) {
                return; /* owner is full: not acknowledged */
            }
            c->rcv_nxt += (gc_u32)dlen;
        }
        if ((fl & TCP_FIN) && c->state != GC_TCP_FREE) {
            c->rcv_nxt++;
            if (c->state == GC_TCP_ESTAB) {
                c->state = GC_TCP_CLOSE_WAIT;
            } else if (c->state == GC_TCP_FIN_WAIT_1) {
                c->state = GC_TCP_CLOSING;
            }
            if (!c->eof_reported) {
                c->eof_reported = 1;
                if (ip->ops.tcp_eof) {
                    ip->ops.tcp_eof(ip->ctx, idx);
                }
            }
            if (c->state == GC_TCP_FIN_WAIT_2) {
                tcp_out(ip, c, c->snd_nxt, TCP_ACK, NULL, 0);
                conn_free(ip, idx, 0); /* no TIME_WAIT: the link is point to point */
                return;
            }
        }
        if (c->state != GC_TCP_FREE) {
            tcp_out(ip, c, c->snd_nxt, TCP_ACK, NULL, 0);
        }
    }
    if (c->state != GC_TCP_FREE) {
        tcp_output(ip, c);
        if (c->writable_pending) {
            c->writable_pending = 0;
            if (ip->ops.tcp_writable) {
                ip->ops.tcp_writable(ip->ctx, idx);
            }
        }
    }
}

/* ---- IPv4 ---- */

void gc_ip_init(gc_ip *ip, const gc_ip_ops *ops, void *ctx, gc_u32 seed)
{
    memset(ip, 0, sizeof(*ip));
    ip->ops = *ops;
    ip->ctx = ctx;
    ip->iss_seed = seed;
    ip->mss = GC_IP_MTU - 40;
}

void gc_ip_set_mss(gc_ip *ip, gc_u16 mss)
{
    if (mss >= 64 && mss <= GC_IP_MTU - 40) {
        ip->mss = mss;
    }
}

void gc_ip_input(gc_ip *ip, const gc_u8 *pkt, size_t len, gc_u32 now)
{
    size_t hl, tl;
    gc_u8 src[4], dst[4];
    ip->now = now;
    if (len < 20 || (pkt[0] >> 4) != 4) {
        ip->st_rx_bad++;
        return;
    }
    hl = (size_t)(pkt[0] & 15) * 4;
    tl = rd16(pkt + 2);
    if (hl < 20 || tl < hl || tl > len || gc_ip_fold(gc_ip_sum(0, pkt, hl)) != 0) {
        ip->st_rx_bad++;
        return;
    }
    if (rd16(pkt + 6) & 0x3FFF) {
        ilog(ip, "ip: fragment dropped");
        return;
    }
    ip->st_rx++;
    memcpy(src, pkt + 12, 4);
    memcpy(ip->peer, src, 4); /* the N-Gage (point to point link) */
    ip->have_peer = 1;
    memcpy(dst, pkt + 16, 4);
    switch (pkt[9]) {
    case GC_IP_PROTO_ICMP:
        icmp_input(ip, src, dst, pkt + hl, tl - hl);
        break;
    case GC_IP_PROTO_UDP:
        udp_input(ip, src, dst, pkt + hl, tl - hl);
        break;
    case GC_IP_PROTO_TCP:
        tcp_input(ip, src, dst, pkt + hl, tl - hl);
        break;
    default:
        break;
    }
}

void gc_ip_poll(gc_ip *ip, gc_u32 now)
{
    int i;
    ip->now = now;
    for (i = 0; i < GC_TCP_CONNS; i++) {
        gc_tcp_conn *c = &ip->tcp[i];
        if (c->state == GC_TCP_FREE || !c->timer || GC_TIME_BEFORE(now, c->t_rto)) {
            continue;
        }
        c->timer = 0;
        if (c->state == GC_TCP_FIN_WAIT_2) {
            conn_free(ip, i, 0);
            continue;
        }
        if (c->snd_una == c->snd_nxt) {
            /* zero window persist probe: one byte past the window */
            if (c->tx_len && c->snd_wnd == 0) {
                gc_u8 b = c->tx[c->tx_head];
                tcp_out(ip, c, c->snd_una, TCP_ACK, &b, 1);
                c->snd_nxt++;
                c->rto = c->rto * 2 > RTO_MAX ? RTO_MAX : c->rto * 2;
                arm(ip, c);
            }
            continue;
        }
        if (++c->retries > MAX_RETRIES) {
            ilog(ip, "tcp: retransmissions exhausted");
            tcp_out(ip, c, c->snd_nxt, TCP_RST | TCP_ACK, NULL, 0);
            conn_free(ip, i, 1);
            continue;
        }
        c->rto = c->rto * 2 > RTO_MAX ? RTO_MAX : c->rto * 2;
        if (c->state == GC_TCP_SYN_RCVD) {
            tcp_out(ip, c, c->snd_una, TCP_SYN | TCP_ACK, NULL, 0);
            arm(ip, c);
            continue;
        }
        /* go back to the oldest unacknowledged byte */
        if (c->fin_sent) {
            c->fin_sent = 0;
            if (c->state == GC_TCP_FIN_WAIT_1) {
                c->state = GC_TCP_ESTAB;
            } else if (c->state == GC_TCP_LAST_ACK) {
                c->state = GC_TCP_CLOSE_WAIT;
            } else if (c->state == GC_TCP_CLOSING) {
                c->state = GC_TCP_CLOSE_WAIT; /* peer's FIN is already in */
            }
        }
        c->snd_nxt = c->snd_una;
        if (c->snd_wnd == 0 && c->tx_len) {
            c->snd_wnd = 1; /* probe */
        }
        tcp_output(ip, c);
        if (!c->timer) {
            arm(ip, c);
        }
    }
}

void gc_ip_reset(gc_ip *ip)
{
    int i;
    for (i = 0; i < GC_TCP_CONNS; i++) {
        if (ip->tcp[i].state != GC_TCP_FREE) {
            conn_free(ip, i, 1);
        }
    }
}

int gc_ip_tcp_send(gc_ip *ip, int conn, const gc_u8 *data, size_t len, gc_u32 now)
{
    gc_tcp_conn *c;
    size_t n, k, pos;
    if (conn < 0 || conn >= GC_TCP_CONNS) {
        return -1;
    }
    c = &ip->tcp[conn];
    if (c->fin_queued || (c->state != GC_TCP_SYN_RCVD && c->state != GC_TCP_ESTAB && c->state != GC_TCP_CLOSE_WAIT)) {
        return -1;
    }
    ip->now = now;
    n = GC_TCP_TXBUF - c->tx_len;
    if (n > len) {
        n = len;
    }
    pos = (c->tx_head + c->tx_len) % GC_TCP_TXBUF;
    for (k = 0; k < n; k++) {
        c->tx[(pos + k) % GC_TCP_TXBUF] = data[k];
    }
    c->tx_len += n;
    tcp_output(ip, c);
    return (int)n;
}

size_t gc_ip_tcp_space(const gc_ip *ip, int conn)
{
    if (conn < 0 || conn >= GC_TCP_CONNS || ip->tcp[conn].state == GC_TCP_FREE) {
        return 0;
    }
    return GC_TCP_TXBUF - ip->tcp[conn].tx_len;
}

void gc_ip_tcp_close(gc_ip *ip, int conn, gc_u32 now)
{
    gc_tcp_conn *c;
    if (conn < 0 || conn >= GC_TCP_CONNS) {
        return;
    }
    c = &ip->tcp[conn];
    if (c->state == GC_TCP_FREE || c->fin_queued) {
        return;
    }
    ip->now = now;
    c->fin_queued = 1;
    tcp_output(ip, c);
}

void gc_ip_tcp_abort(gc_ip *ip, int conn)
{
    gc_tcp_conn *c;
    if (conn < 0 || conn >= GC_TCP_CONNS || ip->tcp[conn].state == GC_TCP_FREE) {
        return;
    }
    c = &ip->tcp[conn];
    tcp_out(ip, c, c->snd_nxt, TCP_RST | TCP_ACK, NULL, 0);
    conn_free(ip, conn, 1);
}
