/** @file pla_core.c
 *
 *  PhantomLink Arena core (see pla_core.h).
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#include "pla_core.h"
#include <string.h>

#define PLA_NOT_READY (-100)
#define PLA_ENCODE    (-101)

int pla_service_of(gc_u8 type)
{
    if (type == GCA_MT_HTTP_REQ || type == GCA_MT_HTTP_RESP) {
        return PLA_SVC_HTTP;
    }
    if (type >= GCA_MT_LOGIN && type <= GCA_MT_LOGOUT) {
        return PLA_SVC_SNAP;
    }
    if (type >= GCA_MT_CHAT && type <= GCA_MT_SERVICE_RESP) {
        return PLA_SVC_CHAT;
    }
    return -1;
}

static void set_state(pla_core *c, int st)
{
    int i;
    if (c->state == st) {
        return;
    }
    c->state = st;
    for (i = 0; i < PLA_SVC_COUNT; i++) {
        if (c->on_state[i]) {
            c->on_state[i](c->listen_ctx[i], st);
        }
    }
}

static void send_register(pla_core *c)
{
    gc_msg m;
    gc_u8 buf[GC_LINK_MAX_MSG];
    size_t n;

    memset(&m, 0, sizeof(m));
    m.type = GC_MT_REGISTER;
    m.u.reg.api = GC_API_ARENA;
    memcpy(m.u.reg.device_id, c->device_id, GC_DEVICE_ID_LEN);
    memcpy(m.u.reg.name, c->name, sizeof(m.u.reg.name));
    n = gc_msg_encode(&m, buf, sizeof(buf));
    if (n && gc_link_send(&c->link, buf, n, c->now) == GC_LINK_OK) {
        c->tx_msgs++;
        set_state(c, PLA_ST_REGISTERING);
    }
}

static void l_write(void *ctx, const gc_u8 *data, size_t len)
{
    pla_core *c = (pla_core *)ctx;
    c->io.write(c->io.ctx, data, len);
}

static void l_msg(void *ctx, const gc_u8 *data, size_t len)
{
    pla_core *c = (pla_core *)ctx;
    gca_msg a;
    gc_msg m;
    int svc;

    if (!len) {
        return;
    }
    c->rx_msgs++;
    if (GCA_IS_ARENA(data[0])) {
        if (!gca_decode(data, len, &a)) {
            return;
        }
        svc = pla_service_of(a.type);
        if (svc >= 0 && c->listen[svc]) {
            c->listen[svc](c->listen_ctx[svc], &a);
        }
        return;
    }
    if (!gc_msg_decode(data, len, &m)) {
        return;
    }
    if (m.type == GC_MT_REGISTERED) {
        set_state(c, m.u.registered.result == GC_R_OK ? PLA_ST_READY : PLA_ST_LINKING);
    }
}

static void l_state(void *ctx, int ev, int reason)
{
    pla_core *c = (pla_core *)ctx;
    (void)reason;
    switch (ev) {
    case GC_LE_UP_FRESH:
        /* the server lost our state: register again */
        send_register(c);
        break;
    case GC_LE_UP_RESUMED:
        if (c->state < PLA_ST_REGISTERING) {
            send_register(c);
        }
        break;
    case GC_LE_DOWN:
    case GC_LE_CLOSED:
        set_state(c, PLA_ST_DOWN);
        break;
    default:
        break;
    }
}

void pla_init(pla_core *c, const pla_io *io, const char *name, const gc_u8 *device_id, gc_u32 link_id)
{
    gc_link_cfg cfg;
    gc_link_ops ops;
    size_t n = strlen(name);

    memset(c, 0, sizeof(*c));
    c->io = *io;
    c->link_id = link_id;
    if (n > GC_MAX_NAME) {
        n = GC_MAX_NAME;
    }
    memcpy(c->name, name, n);
    memcpy(c->device_id, device_id, GC_DEVICE_ID_LEN);
    gc_link_default_cfg(&cfg, GC_ROLE_DEVICE, 1);
    ops.write = l_write;
    ops.on_msg = l_msg;
    ops.on_state = l_state;
    ops.on_hello = NULL;
    ops.can_accept = NULL;
    gc_link_init(&c->link, &cfg, &ops, c);
}

void pla_set_listener(pla_core *c, int svc, pla_listener fn, pla_state_fn st, void *ctx)
{
    if (svc < 0 || svc >= PLA_SVC_COUNT) {
        return;
    }
    c->listen[svc] = fn;
    c->on_state[svc] = st;
    c->listen_ctx[svc] = ctx;
}

void pla_transport_up(pla_core *c, gc_u32 now)
{
    c->now = now;
    set_state(c, PLA_ST_LINKING);
    gc_link_start(&c->link, c->link_id, now);
}

void pla_transport_down(pla_core *c, gc_u32 now)
{
    gc_link_transport_lost(&c->link, now);
    set_state(c, PLA_ST_DOWN);
}

void pla_input(pla_core *c, const gc_u8 *data, size_t len, gc_u32 now)
{
    c->now = now;
    gc_link_input(&c->link, data, len, now);
}

void pla_poll(pla_core *c, gc_u32 now)
{
    c->now = now;
    gc_link_poll(&c->link, now);
}

static int needs_req(gc_u8 t)
{
    return t == GCA_MT_HTTP_REQ || t == GCA_MT_LOGIN || t == GCA_MT_LIST_REQ || t == GCA_MT_ENTER ||
           t == GCA_MT_LEAVE || t == GCA_MT_LOGOUT || t == GCA_MT_SERVICE;
}

int pla_send(pla_core *c, gca_msg *m, gc_u32 now)
{
    gc_u8 buf[GC_LINK_MAX_MSG];
    size_t n;
    int r;

    if (c->state != PLA_ST_READY) {
        return PLA_NOT_READY;
    }
    if (!m->req && needs_req(m->type)) {
        if (++c->next_req == 0) {
            c->next_req = 1;
        }
        m->req = c->next_req;
    }
    n = gca_encode(m, buf, sizeof(buf));
    if (!n) {
        return PLA_ENCODE;
    }
    r = gc_link_send(&c->link, buf, n, now);
    if (r == GC_LINK_OK) {
        c->tx_msgs++;
    }
    return r;
}
