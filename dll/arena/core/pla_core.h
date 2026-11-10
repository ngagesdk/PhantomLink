/** @file pla_core.h
 *
 *  PhantomLink Arena core: the one bridge link shared by the three Arena
 *  plug-ins (SnapComm.dll, sc_lib.dll, HttpDllRequest.dll).
 *
 *  Portable C89 (no writable static data, no heap), like gc_dev.c: the
 *  Symbian adapter (dll/arena/symbian/PlArena.cpp) owns the Bluetooth
 *  connection and the timer and feeds bytes and the time in; the plug-ins
 *  send GCA messages (gc_arena.h) through pla_send() and receive the
 *  answers through the listener registered for their service.
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#ifndef PLA_CORE_H
#define PLA_CORE_H

#include "gc_arena.h"
#include "gc_link.h"
#include "gc_msg.h"

#ifdef __cplusplus
extern "C" {
#endif

/* services: which plug-in a GCA message belongs to */
#define PLA_SVC_HTTP  0 /* HttpDllRequest.dll: HTTP_*                     */
#define PLA_SVC_SNAP  1 /* SnapComm.dll: LOGIN .. LOGOUT                   */
#define PLA_SVC_CHAT  2 /* sc_lib.dll: CHAT, PRESENCE, SERVICE*            */
#define PLA_SVC_COUNT 3

/* connection states */
#define PLA_ST_DOWN        0 /* no link to the bridge                     */
#define PLA_ST_LINKING     1 /* transport up, link handshake              */
#define PLA_ST_REGISTERING 2
#define PLA_ST_READY       3 /* registered with the server                */

typedef void (*pla_listener)(void *ctx, const gca_msg *m);
/** connection state changes, delivered to every listener's state hook */
typedef void (*pla_state_fn)(void *ctx, int state);

typedef struct
{
    /** write raw bytes to the Bluetooth transport */
    void (*write)(void *ctx, const gc_u8 *data, size_t len);
    void *ctx;
} pla_io;

typedef struct
{
    pla_io io;
    gc_link link;
    int state;
    gc_u16 next_req;
    gc_u32 link_id;
    char name[GC_MAX_NAME + 1];
    gc_u8 device_id[GC_DEVICE_ID_LEN];
    pla_listener listen[PLA_SVC_COUNT];
    pla_state_fn on_state[PLA_SVC_COUNT];
    void *listen_ctx[PLA_SVC_COUNT];
    gc_u32 now; /* last time passed in */
    gc_u32 rx_msgs;
    gc_u32 tx_msgs;
} pla_core;

/** link_id must come from a secure random source (see PROTOCOL.md). */
void pla_init(pla_core *c, const pla_io *io, const char *name, const gc_u8 *device_id, gc_u32 link_id);

void pla_set_listener(pla_core *c, int svc, pla_listener fn, pla_state_fn st, void *ctx);

/** Transport events from the adapter. */
void pla_transport_up(pla_core *c, gc_u32 now);
void pla_transport_down(pla_core *c, gc_u32 now);
void pla_input(pla_core *c, const gc_u8 *data, size_t len, gc_u32 now);
void pla_poll(pla_core *c, gc_u32 now);

/** Send a GCA message.  If m->req is 0 for a request type, a fresh
 *  request id is assigned and returned in m->req.  Returns GC_LINK_OK,
 *  or a negative value (not ready, queue full, encoding error). */
int pla_send(pla_core *c, gca_msg *m, gc_u32 now);

/** Which service a GCA type belongs to (-1: none). */
int pla_service_of(gc_u8 type);

#ifdef __cplusplus
}
#endif

#endif /* PLA_CORE_H */
