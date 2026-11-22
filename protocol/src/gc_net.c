/** @file gc_net.c
 *
 *  GCN tunnel messages, see gc_net.h.
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#include "gc_net.h"

#include <string.h>

size_t gcn_encode(const gcn_msg *m, gc_u8 *out, size_t cap)
{
    size_t n;
    switch (m->type) {
    case GCN_MT_TCP_OPEN:
        n = 11;
        if (cap < n) {
            return 0;
        }
        out[1] = (gc_u8)(m->id >> 8);
        out[2] = (gc_u8)m->id;
        memcpy(out + 3, m->addr, 4);
        out[7] = (gc_u8)(m->port >> 8);
        out[8] = (gc_u8)m->port;
        out[9] = (gc_u8)(m->peer_port >> 8);
        out[10] = (gc_u8)m->peer_port;
        break;
    case GCN_MT_TCP_DATA:
        n = GCN_HDR_DATA + m->len;
        if (cap < n) {
            return 0;
        }
        out[1] = (gc_u8)(m->id >> 8);
        out[2] = (gc_u8)m->id;
        if (m->len) {
            memcpy(out + GCN_HDR_DATA, m->data, m->len);
        }
        break;
    case GCN_MT_TCP_CLOSE:
        n = 4;
        if (cap < n) {
            return 0;
        }
        out[1] = (gc_u8)(m->id >> 8);
        out[2] = (gc_u8)m->id;
        out[3] = m->how;
        break;
    case GCN_MT_UDP:
        n = GCN_HDR_UDP + m->len;
        if (cap < n) {
            return 0;
        }
        memcpy(out + 1, m->addr, 4);
        out[5] = (gc_u8)(m->port >> 8);
        out[6] = (gc_u8)m->port;
        out[7] = (gc_u8)(m->peer_port >> 8);
        out[8] = (gc_u8)m->peer_port;
        if (m->len) {
            memcpy(out + GCN_HDR_UDP, m->data, m->len);
        }
        break;
    case GCN_MT_UDP_PART:
        n = GCN_HDR_PART + m->len;
        if (cap < n) {
            return 0;
        }
        memcpy(out + 1, m->addr, 4);
        out[5] = (gc_u8)(m->port >> 8);
        out[6] = (gc_u8)m->port;
        out[7] = (gc_u8)(m->peer_port >> 8);
        out[8] = (gc_u8)m->peer_port;
        out[9] = (gc_u8)(m->part_id >> 8);
        out[10] = (gc_u8)m->part_id;
        out[11] = (gc_u8)(m->total >> 8);
        out[12] = (gc_u8)m->total;
        out[13] = (gc_u8)(m->offset >> 8);
        out[14] = (gc_u8)m->offset;
        if (m->len) {
            memcpy(out + GCN_HDR_PART, m->data, m->len);
        }
        break;
    default:
        return 0;
    }
    out[0] = m->type;
    return n;
}

int gcn_decode(const gc_u8 *msg, size_t len, gcn_msg *m)
{
    memset(m, 0, sizeof(*m));
    if (len < 1) {
        return 0;
    }
    m->type = msg[0];
    switch (m->type) {
    case GCN_MT_TCP_OPEN:
        if (len < 11) {
            return 0;
        }
        m->id = (gc_u16)((msg[1] << 8) | msg[2]);
        memcpy(m->addr, msg + 3, 4);
        m->port = (gc_u16)((msg[7] << 8) | msg[8]);
        m->peer_port = (gc_u16)((msg[9] << 8) | msg[10]);
        return 1;
    case GCN_MT_TCP_DATA:
        if (len < GCN_HDR_DATA) {
            return 0;
        }
        m->id = (gc_u16)((msg[1] << 8) | msg[2]);
        m->data = msg + GCN_HDR_DATA;
        m->len = len - GCN_HDR_DATA;
        return 1;
    case GCN_MT_TCP_CLOSE:
        if (len < 4) {
            return 0;
        }
        m->id = (gc_u16)((msg[1] << 8) | msg[2]);
        m->how = msg[3];
        return 1;
    case GCN_MT_UDP_PART:
        if (len < GCN_HDR_PART) {
            return 0;
        }
        memcpy(m->addr, msg + 1, 4);
        m->port = (gc_u16)((msg[5] << 8) | msg[6]);
        m->peer_port = (gc_u16)((msg[7] << 8) | msg[8]);
        m->part_id = (gc_u16)((msg[9] << 8) | msg[10]);
        m->total = (gc_u16)((msg[11] << 8) | msg[12]);
        m->offset = (gc_u16)((msg[13] << 8) | msg[14]);
        m->data = msg + GCN_HDR_PART;
        m->len = len - GCN_HDR_PART;
        return 1;
    case GCN_MT_UDP:
        if (len < GCN_HDR_UDP) {
            return 0;
        }
        memcpy(m->addr, msg + 1, 4);
        m->port = (gc_u16)((msg[5] << 8) | msg[6]);
        m->peer_port = (gc_u16)((msg[7] << 8) | msg[8]);
        m->data = msg + GCN_HDR_UDP;
        m->len = len - GCN_HDR_UDP;
        return 1;
    default:
        return 0;
    }
}

int gcn_send_udp(const gcn_msg *m, size_t cap, gc_u16 id, int (*emit)(void *ctx, const gc_u8 *msg, size_t len),
                 void *ctx)
{
    gc_u8 buf[1200]; /* one link message (GC_LINK_MAX_MSG); small enough for a bridge task stack */
    size_t off = 0, n;
    gcn_msg p;
    if (cap > sizeof(buf)) {
        cap = sizeof(buf);
    }
    if (m->len + GCN_HDR_UDP <= cap) {
        n = gcn_encode(m, buf, cap);
        return n && emit(ctx, buf, n);
    }
    if (m->len > GCN_UDP_MAX || cap <= GCN_HDR_PART) {
        return 0;
    }
    p = *m;
    p.type = GCN_MT_UDP_PART;
    p.part_id = id;
    p.total = (gc_u16)m->len;
    while (off < m->len) {
        size_t k = m->len - off;
        if (k > cap - GCN_HDR_PART) {
            k = cap - GCN_HDR_PART;
        }
        p.offset = (gc_u16)off;
        p.data = m->data + off;
        p.len = k;
        n = gcn_encode(&p, buf, cap);
        if (!n || !emit(ctx, buf, n)) {
            return 0;
        }
        off += k;
    }
    return 1;
}

static int ends_with(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && !strcmp(s + a - b, suffix) && (a == b || s[a - b - 1] == '.');
}

int gcn_arena_name(const char *name)
{
    return !strcmp(name, "ngage-auth") || ends_with(name, "n-gage.com") || ends_with(name, "yav4.com");
}
