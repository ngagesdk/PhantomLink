/** @file gc_ppp.c
 *
 *  Minimal PPP peer, see gc_ppp.h.
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#include "gc_ppp.h"

#include <string.h>

/* control protocol codes */
#define CP_CONF_REQ  1
#define CP_CONF_ACK  2
#define CP_CONF_NAK  3
#define CP_CONF_REJ  4
#define CP_TERM_REQ  5
#define CP_TERM_ACK  6
#define CP_CODE_REJ  7
#define CP_PROTO_REJ 8 /* LCP only */
#define CP_ECHO_REQ  9
#define CP_ECHO_REP  10
#define CP_DISC_REQ  11

/* LCP options */
#define LCP_MRU   1
#define LCP_ACCM  2
#define LCP_AUTH  3
#define LCP_MAGIC 5
#define LCP_PFC   7
#define LCP_ACFC  8

/* IPCP options */
#define IPCP_ADDR 3
#define IPCP_DNS1 129
#define IPCP_DNS2 131

/* control packets are small; longer ones (echo data, rejected packets)
 * are truncated, which the RFC allows, to keep the stack use low (ESP32) */
#define CP_MAX 300

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

static void plog(gc_ppp *p, const char *s)
{
    if (p->ops.log) {
        p->ops.log(p->ctx, s);
    }
}

gc_u16 gc_ppp_fcs(gc_u16 fcs, const gc_u8 *data, size_t len)
{
    size_t i;
    int b;
    for (i = 0; i < len; i++) {
        fcs ^= data[i];
        for (b = 0; b < 8; b++) {
            fcs = (gc_u16)((fcs & 1) ? ((fcs >> 1) ^ 0x8408) : (fcs >> 1));
        }
    }
    return fcs;
}

static size_t put_esc(gc_u8 *out, size_t n, size_t cap, gc_u8 c, gc_u32 accm)
{
    if (c == 0x7E || c == 0x7D || (c < 0x20 && (accm & ((gc_u32)1 << c)))) {
        if (n + 2 > cap) {
            return 0;
        }
        out[n++] = 0x7D;
        out[n++] = (gc_u8)(c ^ 0x20);
    } else {
        if (n + 1 > cap) {
            return 0;
        }
        out[n++] = c;
    }
    return n;
}

size_t gc_ppp_frame(gc_u16 proto, const gc_u8 *info, size_t len, gc_u32 accm, gc_u8 *out, size_t cap)
{
    gc_u8 hdr[4];
    gc_u16 fcs;
    size_t n = 0, i;
    if (cap < 1) {
        return 0;
    }
    hdr[0] = 0xFF;
    hdr[1] = 0x03;
    wr16(hdr + 2, proto);
    fcs = gc_ppp_fcs(GC_PPP_FCS_INIT, hdr, 4);
    fcs = gc_ppp_fcs(fcs, info, len);
    fcs = (gc_u16)~fcs;
    out[n++] = 0x7E;
    for (i = 0; i < 4 && n; i++) {
        n = put_esc(out, n, cap, hdr[i], accm);
    }
    for (i = 0; i < len && n; i++) {
        n = put_esc(out, n, cap, info[i], accm);
    }
    if (n) {
        n = put_esc(out, n, cap, (gc_u8)(fcs & 0xFF), accm);
    }
    if (n) {
        n = put_esc(out, n, cap, (gc_u8)(fcs >> 8), accm);
    }
    if (!n || n + 1 > cap) {
        return 0;
    }
    out[n++] = 0x7E;
    return n;
}

static void send_frame(gc_ppp *p, gc_u16 proto, const gc_u8 *info, size_t len)
{
    /* LCP always with every control character escaped (RFC 1662 7.1) */
    gc_u32 accm = (proto == GC_PPP_LCP || p->lcp.state != GC_PPP_ST_OPENED) ? 0xFFFFFFFFu : p->tx_accm;
    size_t n = gc_ppp_frame(proto, info, len, accm, p->tx, sizeof(p->tx));
    if (n) {
        p->st_tx_frames++;
        p->ops.write(p->ctx, p->tx, n);
    }
}

static void send_cp(gc_ppp *p, gc_u16 proto, gc_u8 code, gc_u8 id, const gc_u8 *data, size_t len)
{
    gc_u8 pkt[CP_MAX];
    if (len + 4 > sizeof(pkt)) {
        len = sizeof(pkt) - 4;
    }
    pkt[0] = code;
    pkt[1] = id;
    wr16(pkt + 2, (gc_u16)(len + 4));
    if (len) {
        memcpy(pkt + 4, data, len);
    }
    send_frame(p, proto, pkt, len + 4);
}

/* ---- option sets ---- */

static int rejected(const gc_ppp_fsm *f, gc_u8 type)
{
    return (f->rej_opts[(type & 63) >> 3] >> (type & 7)) & 1;
}

static void set_rejected(gc_ppp_fsm *f, gc_u8 type)
{
    f->rej_opts[(type & 63) >> 3] |= (gc_u8)(1 << (type & 7));
}

static size_t our_options(gc_ppp *p, gc_u16 proto, gc_u8 *o)
{
    size_t n = 0;
    if (proto == GC_PPP_LCP) {
        if (!rejected(&p->lcp, LCP_ACCM)) {
            o[n++] = LCP_ACCM;
            o[n++] = 6;
            wr32(o + n, 0);
            n += 4;
        }
        if (!rejected(&p->lcp, LCP_MAGIC)) {
            o[n++] = LCP_MAGIC;
            o[n++] = 6;
            wr32(o + n, p->cfg.magic);
            n += 4;
        }
    } else {
        if (!rejected(&p->ipcp, IPCP_ADDR)) {
            o[n++] = IPCP_ADDR;
            o[n++] = 6;
            memcpy(o + n, p->cfg.local, 4);
            n += 4;
        }
    }
    return n;
}

static gc_ppp_fsm *fsm_of(gc_ppp *p, gc_u16 proto)
{
    return proto == GC_PPP_LCP ? &p->lcp : &p->ipcp;
}

static void send_conf_req(gc_ppp *p, gc_u16 proto)
{
    gc_ppp_fsm *f = fsm_of(p, proto);
    gc_u8 o[32];
    size_t n = our_options(p, proto, o);
    f->id++;
    send_cp(p, proto, CP_CONF_REQ, f->id, o, n);
    f->t_restart = p->now + p->cfg.restart_ms;
    f->timer = 1;
}

static void report_up(gc_ppp *p)
{
    if (!p->up) {
        p->up = 1;
        plog(p, "ppp: IPCP up");
        if (p->ops.on_state) {
            p->ops.on_state(p->ctx, GC_PPP_EV_UP);
        }
    }
}

static void fsm_start(gc_ppp *p, gc_u16 proto)
{
    gc_ppp_fsm *f = fsm_of(p, proto);
    f->retries = p->cfg.max_configure;
    memset(f->rej_opts, 0, sizeof(f->rej_opts));
    send_conf_req(p, proto);
    f->state = GC_PPP_ST_REQSENT;
}

static void layer_up(gc_ppp *p, gc_u16 proto)
{
    gc_ppp_fsm *f = fsm_of(p, proto);
    f->timer = 0;
    if (proto == GC_PPP_LCP) {
        plog(p, "ppp: LCP up");
        if (p->ipcp.state == GC_PPP_ST_INITIAL || p->ipcp.state == GC_PPP_ST_STOPPED) {
            fsm_start(p, GC_PPP_IPCP);
        }
    } else {
        report_up(p);
    }
}

static void layer_down(gc_ppp *p, gc_u16 proto)
{
    if (proto == GC_PPP_LCP) {
        p->ipcp.state = GC_PPP_ST_INITIAL;
        p->ipcp.timer = 0;
        p->tx_accm = 0xFFFFFFFFu;
    }
    if (p->up) {
        p->up = 0;
        plog(p, "ppp: IPCP down");
        if (p->ops.on_state) {
            p->ops.on_state(p->ctx, GC_PPP_EV_DOWN);
        }
    }
}

static void finished(gc_ppp *p, gc_u16 proto)
{
    gc_ppp_fsm *f = fsm_of(p, proto);
    f->state = GC_PPP_ST_STOPPED;
    f->timer = 0;
    if (proto == GC_PPP_LCP) {
        layer_down(p, GC_PPP_LCP);
        p->ipcp.state = GC_PPP_ST_STOPPED;
        plog(p, "ppp: LCP finished");
        if (p->ops.on_state) {
            p->ops.on_state(p->ctx, GC_PPP_EV_DEAD);
        }
    } else {
        layer_down(p, GC_PPP_IPCP);
    }
}

/* ---- peer Configure-Request ---- */

/* Builds the answer to a Configure-Request into out; returns the code. */
static gc_u8 check_request(gc_ppp *p, gc_u16 proto, const gc_u8 *o, size_t len, gc_u8 *out, size_t *out_len)
{
    gc_u8 rej[CP_MAX - 4], nak[64];
    size_t nrej = 0, nnak = 0, i = 0;
    gc_u32 accm = 0xFFFFFFFFu;
    gc_u16 mru = 1500;
    int have_accm = 0;

    while (i + 2 <= len) {
        gc_u8 t = o[i], l = o[i + 1];
        int ok = 0, nk = 0;
        gc_u8 want[4];
        if (l < 2 || i + l > len) {
            break; /* malformed: ack what was fine so far, ignore the rest */
        }
        if (proto == GC_PPP_LCP) {
            if (t == LCP_MRU && l == 4) {
                mru = rd16(o + i + 2);
                ok = 1;
            } else if (t == LCP_ACCM && l == 6) {
                accm = rd32(o + i + 2);
                have_accm = 1;
                ok = 1;
            } else if (t == LCP_MAGIC && l == 6) {
                if (rd32(o + i + 2) == p->cfg.magic && p->cfg.magic) {
                    nk = 1; /* looped back? ask for another number */
                    wr32(want, p->cfg.magic ^ 0x5A5A5A5Au);
                } else {
                    p->peer_magic = rd32(o + i + 2);
                    ok = 1;
                }
            } else if ((t == LCP_PFC || t == LCP_ACFC) && l == 2) {
                ok = 1;
            }
        } else {
            if (t == IPCP_ADDR && l == 6) {
                if (!memcmp(o + i + 2, p->cfg.remote, 4)) {
                    ok = 1;
                } else {
                    nk = 1;
                    memcpy(want, p->cfg.remote, 4);
                }
            } else if ((t == IPCP_DNS1 || t == IPCP_DNS2) && l == 6) {
                if (!memcmp(o + i + 2, p->cfg.dns, 4)) {
                    ok = 1;
                } else {
                    nk = 1;
                    memcpy(want, p->cfg.dns, 4);
                }
            }
        }
        if (!ok && !nk) {
            if (nrej + l <= sizeof(rej)) {
                memcpy(rej + nrej, o + i, l);
                nrej += l;
            }
        } else if (nk && nnak + 6 <= sizeof(nak)) {
            nak[nnak] = t;
            nak[nnak + 1] = 6;
            memcpy(nak + nnak + 2, want, 4);
            nnak += 6;
        }
        i += l;
    }
    if (nrej) {
        memcpy(out, rej, nrej);
        *out_len = nrej;
        return CP_CONF_REJ;
    }
    if (nnak) {
        memcpy(out, nak, nnak);
        *out_len = nnak;
        return CP_CONF_NAK;
    }
    if (proto == GC_PPP_LCP) {
        p->peer_mru = mru;
        p->tx_accm = have_accm ? accm : 0xFFFFFFFFu;
    }
    if (len > CP_MAX - 4) {
        len = CP_MAX - 4; /* cannot happen with the options we accept */
    }
    if (len) {
        memcpy(out, o, len);
    }
    *out_len = len;
    return CP_CONF_ACK;
}

static void rcv_conf_req(gc_ppp *p, gc_u16 proto, gc_u8 id, const gc_u8 *o, size_t len)
{
    gc_ppp_fsm *f = fsm_of(p, proto);
    gc_u8 out[CP_MAX - 4];
    size_t out_len = 0;
    gc_u8 code;

    if (f->state == GC_PPP_ST_CLOSING) {
        return;
    }
    if (f->state == GC_PPP_ST_INITIAL || f->state == GC_PPP_ST_STOPPED) {
        f->retries = p->cfg.max_configure;
        memset(f->rej_opts, 0, sizeof(f->rej_opts));
        send_conf_req(p, proto);
        f->state = GC_PPP_ST_REQSENT;
    } else if (f->state == GC_PPP_ST_OPENED) {
        layer_down(p, proto); /* renegotiation */
        send_conf_req(p, proto);
        f->state = GC_PPP_ST_REQSENT;
    }
    code = check_request(p, proto, o, len, out, &out_len);
    send_cp(p, proto, code, id, out, out_len);
    if (code == CP_CONF_ACK) {
        if (f->state == GC_PPP_ST_ACKRCVD) {
            f->state = GC_PPP_ST_OPENED;
            layer_up(p, proto);
        } else {
            f->state = GC_PPP_ST_ACKSENT;
        }
    } else if (f->state == GC_PPP_ST_ACKSENT) {
        f->state = GC_PPP_ST_REQSENT;
    }
}

static void rcv_conf_ack(gc_ppp *p, gc_u16 proto, gc_u8 id)
{
    gc_ppp_fsm *f = fsm_of(p, proto);
    if (id != f->id) {
        return;
    }
    switch (f->state) {
    case GC_PPP_ST_REQSENT:
        f->state = GC_PPP_ST_ACKRCVD;
        f->retries = p->cfg.max_configure;
        break;
    case GC_PPP_ST_ACKSENT:
        f->state = GC_PPP_ST_OPENED;
        layer_up(p, proto);
        break;
    case GC_PPP_ST_ACKRCVD:
    case GC_PPP_ST_OPENED:
        if (f->state == GC_PPP_ST_OPENED) {
            layer_down(p, proto);
        }
        send_conf_req(p, proto);
        f->state = GC_PPP_ST_REQSENT;
        break;
    default:
        break;
    }
}

static void rcv_conf_nak_rej(gc_ppp *p, gc_u16 proto, gc_u8 code, gc_u8 id, const gc_u8 *o, size_t len)
{
    gc_ppp_fsm *f = fsm_of(p, proto);
    size_t i = 0;
    if (id != f->id || f->state == GC_PPP_ST_INITIAL || f->state == GC_PPP_ST_STOPPED ||
        f->state == GC_PPP_ST_CLOSING) {
        return;
    }
    while (i + 2 <= len && o[i + 1] >= 2 && i + o[i + 1] <= len) {
        gc_u8 t = o[i];
        if (code == CP_CONF_REJ) {
            set_rejected(f, t);
        } else if (proto == GC_PPP_LCP && t == LCP_MAGIC && o[i + 1] == 6) {
            p->cfg.magic = (p->cfg.magic * 1103515245u + 12345u) | 1u;
        } else if (proto == GC_PPP_LCP && t == LCP_ACCM) {
            set_rejected(f, t); /* use the default instead */
        }
        /* a Nak for our IP address is ignored: we keep ours */
        i += o[i + 1];
    }
    if (f->state == GC_PPP_ST_OPENED) {
        layer_down(p, proto);
    }
    send_conf_req(p, proto);
    if (f->state != GC_PPP_ST_ACKSENT) {
        f->state = GC_PPP_ST_REQSENT;
    }
}

/* ---- packet dispatch ---- */

static void rcv_cp(gc_ppp *p, gc_u16 proto, const gc_u8 *pkt, size_t len)
{
    gc_ppp_fsm *f = fsm_of(p, proto);
    gc_u8 code, id;
    size_t plen;
    if (len < 4) {
        return;
    }
    code = pkt[0];
    id = pkt[1];
    plen = rd16(pkt + 2);
    if (plen < 4 || plen > len) {
        return;
    }
    if (proto == GC_PPP_IPCP && p->lcp.state != GC_PPP_ST_OPENED) {
        return; /* RFC 1661: discard NCP packets until LCP is open */
    }
    if (f->state == GC_PPP_ST_INITIAL && code != CP_CONF_REQ && proto == GC_PPP_LCP) {
        fsm_start(p, proto);
    }
    switch (code) {
    case CP_CONF_REQ:
        rcv_conf_req(p, proto, id, pkt + 4, plen - 4);
        break;
    case CP_CONF_ACK:
        rcv_conf_ack(p, proto, id);
        break;
    case CP_CONF_NAK:
    case CP_CONF_REJ:
        rcv_conf_nak_rej(p, proto, code, id, pkt + 4, plen - 4);
        break;
    case CP_TERM_REQ:
        send_cp(p, proto, CP_TERM_ACK, id, NULL, 0);
        plog(p, proto == GC_PPP_LCP ? "ppp: LCP Terminate-Request" : "ppp: IPCP Terminate-Request");
        finished(p, proto);
        break;
    case CP_TERM_ACK:
        if (f->state == GC_PPP_ST_CLOSING) {
            finished(p, proto);
        } else if (f->state == GC_PPP_ST_OPENED) {
            layer_down(p, proto);
            send_conf_req(p, proto);
            f->state = GC_PPP_ST_REQSENT;
        }
        break;
    case CP_CODE_REJ:
    case CP_PROTO_REJ:
        break; /* nothing we send is optional enough to care */
    case CP_ECHO_REQ:
        if (proto == GC_PPP_LCP && f->state == GC_PPP_ST_OPENED && plen >= 8) {
            gc_u8 d[CP_MAX - 4];
            size_t n = plen - 4 > sizeof(d) ? sizeof(d) : plen - 4;
            memcpy(d, pkt + 4, n);
            wr32(d, rejected(&p->lcp, LCP_MAGIC) ? 0 : p->cfg.magic);
            send_cp(p, proto, CP_ECHO_REP, id, d, n);
        } else if (proto != GC_PPP_LCP) {
            send_cp(p, proto, CP_CODE_REJ, ++f->id, pkt, plen > CP_MAX - 8 ? CP_MAX - 8 : plen);
        }
        break;
    case CP_ECHO_REP:
    case CP_DISC_REQ:
        if (proto != GC_PPP_LCP) {
            send_cp(p, proto, CP_CODE_REJ, ++f->id, pkt, plen > CP_MAX - 8 ? CP_MAX - 8 : plen);
        }
        break;
    default:
        send_cp(p, proto, CP_CODE_REJ, ++f->id, pkt, plen > CP_MAX - 8 ? CP_MAX - 8 : plen);
        break;
    }
}

static void protocol_reject(gc_ppp *p, gc_u16 proto, const gc_u8 *info, size_t len)
{
    gc_u8 d[CP_MAX - 4];
    size_t n = len;
    if (p->lcp.state != GC_PPP_ST_OPENED) {
        return;
    }
    if (n > sizeof(d) - 2) {
        n = sizeof(d) - 2;
    }
    wr16(d, proto);
    memcpy(d + 2, info, n);
    send_cp(p, GC_PPP_LCP, CP_PROTO_REJ, ++p->lcp.id, d, n + 2);
}

static void rcv_frame(gc_ppp *p, gc_u8 *f, size_t n)
{
    gc_u16 proto;
    if (n < 4 || gc_ppp_fcs(GC_PPP_FCS_INIT, f, n) != GC_PPP_FCS_GOOD) {
        p->st_rx_bad_fcs++;
        return;
    }
    n -= 2;
    p->st_rx_frames++;
    if (n >= 2 && f[0] == 0xFF && f[1] == 0x03) {
        f += 2;
        n -= 2;
    }
    if (n < 1) {
        return;
    }
    if (f[0] & 1) { /* compressed protocol field */
        proto = f[0];
        f++;
        n--;
    } else {
        if (n < 2) {
            return;
        }
        proto = rd16(f);
        f += 2;
        n -= 2;
    }
    if (proto == GC_PPP_LCP) {
        rcv_cp(p, proto, f, n);
    } else if (p->lcp.state == GC_PPP_ST_INITIAL) {
        fsm_start(p, GC_PPP_LCP); /* peer is ahead of us: start LCP */
    } else if (proto == GC_PPP_IPCP) {
        rcv_cp(p, proto, f, n);
    } else if (proto == GC_PPP_IP) {
        if (p->ipcp.state == GC_PPP_ST_OPENED && p->ops.on_ip) {
            p->ops.on_ip(p->ctx, f, n);
        }
    } else {
        protocol_reject(p, proto, f, n);
    }
}

/* ---- public ---- */

void gc_ppp_default_cfg(gc_ppp_cfg *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->local[0] = 10;
    cfg->local[1] = 64;
    cfg->local[3] = 1;
    memcpy(cfg->remote, cfg->local, 4);
    cfg->remote[3] = 2;
    memcpy(cfg->dns, cfg->local, 4);
    cfg->restart_ms = 3000;
    cfg->max_configure = 10;
    cfg->max_terminate = 2;
    cfg->magic = 0x50484C4Bu; /* "PHLK"; owners should randomise */
}

void gc_ppp_init(gc_ppp *p, const gc_ppp_cfg *cfg, const gc_ppp_ops *ops, void *ctx)
{
    memset(p, 0, sizeof(*p));
    p->cfg = *cfg;
    if (!p->cfg.magic) {
        p->cfg.magic = 0x50484C4Bu;
    }
    p->ops = *ops;
    p->ctx = ctx;
    p->tx_accm = 0xFFFFFFFFu;
    p->peer_mru = 1500;
    p->lcp.state = GC_PPP_ST_INITIAL;
    p->ipcp.state = GC_PPP_ST_INITIAL;
}

void gc_ppp_input(gc_ppp *p, const gc_u8 *data, size_t len, gc_u32 now)
{
    size_t i;
    p->now = now;
    for (i = 0; i < len; i++) {
        gc_u8 c = data[i];
        if (c == 0x7E) {
            if (!p->rx_drop && !p->rx_esc && p->rx_len) {
                rcv_frame(p, p->rx, p->rx_len);
            }
            p->rx_len = 0;
            p->rx_esc = 0;
            p->rx_drop = 0;
        } else if (p->rx_drop) {
            continue;
        } else if (c == 0x7D) {
            p->rx_esc = 1;
        } else {
            if (p->rx_esc) {
                c ^= 0x20;
                p->rx_esc = 0;
            }
            if (p->rx_len >= sizeof(p->rx)) {
                p->rx_drop = 1;
                p->rx_len = 0;
            } else {
                p->rx[p->rx_len++] = c;
            }
        }
    }
}

static void fsm_timeout(gc_ppp *p, gc_u16 proto)
{
    gc_ppp_fsm *f = fsm_of(p, proto);
    if (!f->timer || GC_TIME_BEFORE(p->now, f->t_restart)) {
        return;
    }
    f->timer = 0;
    if (f->state == GC_PPP_ST_CLOSING) {
        if (f->retries) {
            f->retries--;
            f->id++;
            send_cp(p, proto, CP_TERM_REQ, f->id, NULL, 0);
            f->t_restart = p->now + p->cfg.restart_ms;
            f->timer = 1;
        } else {
            finished(p, proto);
        }
    } else if (f->state == GC_PPP_ST_REQSENT || f->state == GC_PPP_ST_ACKRCVD || f->state == GC_PPP_ST_ACKSENT) {
        if (f->retries) {
            f->retries--;
            send_conf_req(p, proto);
            if (f->state == GC_PPP_ST_ACKRCVD) {
                f->state = GC_PPP_ST_REQSENT;
            }
        } else {
            plog(p, proto == GC_PPP_LCP ? "ppp: LCP negotiation timed out" : "ppp: IPCP negotiation timed out");
            finished(p, proto == GC_PPP_IPCP ? GC_PPP_LCP : proto);
        }
    }
}

void gc_ppp_poll(gc_ppp *p, gc_u32 now)
{
    p->now = now;
    fsm_timeout(p, GC_PPP_LCP);
    fsm_timeout(p, GC_PPP_IPCP);
}

int gc_ppp_send_ip(gc_ppp *p, const gc_u8 *pkt, size_t len)
{
    if (p->ipcp.state != GC_PPP_ST_OPENED || len > p->peer_mru || len > GC_PPP_MRU) {
        return -1;
    }
    send_frame(p, GC_PPP_IP, pkt, len);
    return 0;
}

void gc_ppp_open(gc_ppp *p, gc_u32 now)
{
    p->now = now;
    if (p->lcp.state == GC_PPP_ST_INITIAL || p->lcp.state == GC_PPP_ST_STOPPED) {
        fsm_start(p, GC_PPP_LCP);
    }
}

void gc_ppp_close(gc_ppp *p, gc_u32 now)
{
    p->now = now;
    if (p->lcp.state == GC_PPP_ST_INITIAL || p->lcp.state == GC_PPP_ST_STOPPED ||
        p->lcp.state == GC_PPP_ST_CLOSING) {
        return;
    }
    layer_down(p, GC_PPP_LCP);
    p->lcp.state = GC_PPP_ST_CLOSING;
    p->lcp.retries = p->cfg.max_terminate;
    p->lcp.id++;
    send_cp(p, GC_PPP_LCP, CP_TERM_REQ, p->lcp.id, NULL, 0);
    p->lcp.t_restart = now + p->cfg.restart_ms;
    p->lcp.timer = 1;
}

int gc_ppp_is_up(const gc_ppp *p)
{
    return p->up;
}

int gc_ppp_detect(const gc_u8 *data, size_t len)
{
    /* Symbian starts with a flag and an LCP frame (7E FF 7D 23 C0 21 ...
     * or 7E FF 03 C0 21 ...); GCL frames never contain 0x7E there */
    return len >= 1 && data[0] == 0x7E;
}
