/** @file gcs_direct.c
 *
 *  Direct Arena clients (GPRS), see gcs_direct.h.
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#include "gcs_direct.h"
#include "gc_link.h"
#include "gc_net.h"
#include "gcs_core.h"
#include "gcs_native.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    int used;
    gc_u8 ip[4];
    int conns;     /* open TCP connections */
    gc_u32 t_last; /* last connection or datagram */
} client;

typedef struct
{
    int used;
    void *sock;
    client *cl;
    gc_u16 id; /* GCN connection id: slot | generation << 8 */
} dconn;

struct gcs_direct
{
    gcs_direct_io io;
    void *ctx;
    gcs_core *core;
    gcs_native *native;
    client clients[GCSD_MAX_CLIENTS];
    dconn conns[GCSD_MAX_CONNS];
    gc_u8 gen[GCSD_MAX_CONNS];
    gcs_direct_stats st;
};

static void dlog(gcs_direct *d, int level, const char *fmt, ...)
{
    char line[256];
    va_list ap;
    if (!d->io.log) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    d->io.log(d->ctx, level, line);
}

#define IPF(ip) (ip)[0], (ip)[1], (ip)[2], (ip)[3]

/* ---- clients and connections ---- */

static client *client_find(gcs_direct *d, const gc_u8 ip[4])
{
    int i;
    for (i = 0; i < GCSD_MAX_CLIENTS; i++) {
        if (d->clients[i].used && !memcmp(d->clients[i].ip, ip, 4)) {
            return &d->clients[i];
        }
    }
    return NULL;
}

static void client_forget(gcs_direct *d, client *c)
{
    gcs_native_peer_gone(d->native, c);
    dlog(d, 3, "direct: %u.%u.%u.%u forgotten", IPF(c->ip));
    memset(c, 0, sizeof(*c));
}

static client *client_get(gcs_direct *d, const gc_u8 ip[4], gc_u32 now)
{
    int i;
    client *c = client_find(d, ip), *idle = NULL;
    if (c) {
        return c;
    }
    for (i = 0; i < GCSD_MAX_CLIENTS; i++) {
        if (!d->clients[i].used) {
            c = &d->clients[i];
            break;
        }
        /* full: the address idle the longest gives way */
        if (!d->clients[i].conns && (!idle || GC_TIME_BEFORE(d->clients[i].t_last, idle->t_last))) {
            idle = &d->clients[i];
        }
    }
    if (!c && idle) {
        client_forget(d, idle);
        c = idle;
    }
    if (!c) {
        return NULL;
    }
    memset(c, 0, sizeof(*c));
    c->used = 1;
    memcpy(c->ip, ip, 4);
    c->t_last = now;
    dlog(d, 3, "direct: new client %u.%u.%u.%u", IPF(ip));
    return c;
}

static dconn *conn_by_sock(gcs_direct *d, void *sock)
{
    int i;
    for (i = 0; i < GCSD_MAX_CONNS; i++) {
        if (d->conns[i].used && d->conns[i].sock == sock) {
            return &d->conns[i];
        }
    }
    return NULL;
}

static dconn *conn_by_id(gcs_direct *d, const client *cl, gc_u16 id)
{
    int slot = id & 0xFF;
    if (slot < GCSD_MAX_CONNS && d->conns[slot].used && d->conns[slot].cl == cl && d->conns[slot].id == id) {
        return &d->conns[slot];
    }
    return NULL;
}

static void conn_free(dconn *c)
{
    c->cl->conns--;
    memset(c, 0, sizeof(*c));
}

/* ---- to gcs_native ---- */

static void to_native(gcs_direct *d, client *cl, const gcn_msg *m)
{
    gc_u8 buf[GC_LINK_MAX_MSG];
    size_t k = gcn_encode(m, buf, sizeof(buf));
    if (k) {
        gcs_native_input(d->native, cl, buf, k);
    }
}

static void native_close(gcs_direct *d, dconn *c, int how)
{
    gcn_msg m;
    memset(&m, 0, sizeof(m));
    m.type = GCN_MT_TCP_CLOSE;
    m.id = c->id;
    m.how = (gc_u8)how;
    to_native(d, c->cl, &m);
}

/* ---- from gcs_native (gcs_core hook) ---- */

static int hook_owns(void *ctx, void *key)
{
    gcs_direct *d = (gcs_direct *)ctx;
    const client *c = (const client *)key;
    return c >= d->clients && c < d->clients + GCSD_MAX_CLIENTS;
}

static void hook_send(void *ctx, void *key, const gc_u8 *data, size_t len)
{
    gcs_direct *d = (gcs_direct *)ctx;
    client *cl = (client *)key;
    gcn_msg m;
    dconn *c;

    if (!cl->used || !gcn_decode(data, len, &m)) {
        return;
    }
    switch (m.type) {
    case GCN_MT_TCP_DATA:
        c = conn_by_id(d, cl, m.id);
        if (c && m.len) {
            d->io.tcp_send(d->ctx, c->sock, m.data, m.len);
        }
        break;
    case GCN_MT_TCP_CLOSE:
        c = conn_by_id(d, cl, m.id);
        if (!c) {
            break;
        }
        if (m.how == GCN_CLOSE_RESET) {
            void *sock = c->sock;
            conn_free(c); /* gcs_native has forgotten it already */
            d->io.tcp_close(d->ctx, sock, 1);
        } else {
            d->io.tcp_close(d->ctx, c->sock, 0);
        }
        break;
    case GCN_MT_UDP:
        d->io.udp_send(d->ctx, m.port, cl->ip, m.peer_port, m.data, m.len);
        break;
    default:
        break;
    }
}

/* ---- public ---- */

gcs_direct *gcs_direct_create(gcs_core *core, const gcs_direct_io *io, void *ctx)
{
    gcs_direct *d;
    gcs_direct_hook hook;
    if (!core || !io || !io->tcp_send || !io->tcp_close || !io->udp_send || !gcs_core_native(core)) {
        return NULL;
    }
    d = (gcs_direct *)calloc(1, sizeof(*d));
    if (!d) {
        return NULL;
    }
    d->io = *io;
    d->ctx = ctx;
    d->core = core;
    d->native = gcs_core_native(core);
    hook.owns = hook_owns;
    hook.send = hook_send;
    gcs_core_set_direct(core, &hook, d);
    return d;
}

void gcs_direct_destroy(gcs_direct *d)
{
    int i;
    if (!d) {
        return;
    }
    gcs_core_set_direct(d->core, NULL, NULL);
    for (i = 0; i < GCSD_MAX_CLIENTS; i++) {
        if (d->clients[i].used) {
            gcs_native_peer_gone(d->native, &d->clients[i]);
        }
    }
    free(d);
}

int gcs_direct_tcp_open(gcs_direct *d, void *sock, const gc_u8 ip[4], gc_u16 port, gc_u16 service, gc_u32 now)
{
    int i;
    client *cl;
    dconn *c = NULL;
    gcn_msg m;

    for (i = 0; i < GCSD_MAX_CONNS; i++) {
        if (!d->conns[i].used) {
            c = &d->conns[i];
            break;
        }
    }
    cl = c ? client_get(d, ip, now) : NULL;
    if (!cl) {
        dlog(d, 1, "direct: no room for %u.%u.%u.%u", IPF(ip));
        return -1;
    }
    d->gen[i]++;
    c->used = 1;
    c->sock = sock;
    c->cl = cl;
    c->id = (gc_u16)(i | (d->gen[i] << 8));
    cl->conns++;
    cl->t_last = now;
    d->st.tcp_accepted++;
    dlog(d, 3, "direct: %u.%u.%u.%u:%u -> port %u (id %04x)", IPF(ip), port, service, c->id);

    memset(&m, 0, sizeof(m));
    m.type = GCN_MT_TCP_OPEN;
    m.id = c->id;
    m.port = service;
    m.peer_port = port;
    to_native(d, cl, &m); /* may answer with a reset at once */
    return 0;
}

void gcs_direct_tcp_input(gcs_direct *d, void *sock, const gc_u8 *data, size_t len, gc_u32 now)
{
    const size_t chunk = GC_LINK_MAX_MSG - GCN_HDR_DATA;
    size_t off;
    gcn_msg m;
    dconn *c = conn_by_sock(d, sock);
    if (!c) {
        return;
    }
    c->cl->t_last = now;
    memset(&m, 0, sizeof(m));
    m.type = GCN_MT_TCP_DATA;
    m.id = c->id;
    for (off = 0; off < len; off += chunk) {
        client *cl = c->cl;
        m.data = data + off;
        m.len = len - off > chunk ? chunk : len - off;
        to_native(d, cl, &m);
        if (conn_by_id(d, cl, m.id) != c) {
            return; /* reset by gcs_native */
        }
    }
}

void gcs_direct_tcp_eof(gcs_direct *d, void *sock, gc_u32 now)
{
    dconn *c = conn_by_sock(d, sock);
    if (!c) {
        return;
    }
    c->cl->t_last = now;
    native_close(d, c, GCN_CLOSE_FIN);
}

void gcs_direct_tcp_gone(gcs_direct *d, void *sock)
{
    dconn *c = conn_by_sock(d, sock);
    if (!c) {
        return;
    }
    native_close(d, c, GCN_CLOSE_RESET); /* frees gcs_native's side */
    c = conn_by_sock(d, sock);           /* (no callback frees it, but be safe) */
    if (c) {
        conn_free(c);
    }
}

void gcs_direct_udp_input(gcs_direct *d, gc_u16 service, const gc_u8 ip[4], gc_u16 port, const gc_u8 *data,
                          size_t len, gc_u32 now)
{
    gcn_msg m;
    client *cl = client_find(d, ip);
    if (!cl) {
        d->st.udp_dropped++;
        dlog(d, 3, "direct: datagram from unknown %u.%u.%u.%u:%u dropped", IPF(ip), port);
        return;
    }
    if (len > GC_LINK_MAX_MSG - GCN_HDR_UDP) {
        dlog(d, 1, "direct: datagram from %u.%u.%u.%u too large (%u bytes)", IPF(ip), (unsigned)len);
        return;
    }
    cl->t_last = now;
    d->st.udp_in++;
    memset(&m, 0, sizeof(m));
    m.type = GCN_MT_UDP;
    m.port = service;
    m.peer_port = port;
    m.data = data;
    m.len = len;
    to_native(d, cl, &m);
}

void gcs_direct_poll(gcs_direct *d, gc_u32 now)
{
    int i;
    for (i = 0; i < GCSD_MAX_CLIENTS; i++) {
        client *c = &d->clients[i];
        if (c->used && !c->conns && (gc_u32)(now - c->t_last) >= GCSD_IDLE_MS) {
            client_forget(d, c);
        }
    }
}

void gcs_direct_get_stats(const gcs_direct *d, gcs_direct_stats *st)
{
    int i;
    *st = d->st;
    st->clients = 0;
    st->conns = 0;
    for (i = 0; i < GCSD_MAX_CLIENTS; i++) {
        st->clients += d->clients[i].used;
    }
    for (i = 0; i < GCSD_MAX_CONNS; i++) {
        st->conns += d->conns[i].used;
    }
}
