/** @file gc_net.h
 *
 *  GCN - network tunnel messages for the PPP mode (docs/ARENA.md).
 *
 *  A retail Arena game talks TCP/IP over PPP to the bridge (gc_ppp,
 *  gc_ip).  The bridge terminates TCP there and carries each connection
 *  and every UDP datagram in these messages over its normal link to the
 *  server, which runs the Arena services (server/src/gcs_native.c):
 *
 *    TCP_OPEN   bridge -> server   new connection: id, address and port
 *                                  the game connected to, its own port
 *    TCP_DATA   both               bytes of connection id
 *    TCP_CLOSE  both               end of connection id: FIN (no more
 *                                  bytes from this side) or reset (gone)
 *    UDP        both               a datagram; addr/port is the server
 *                                  side end point, peer_port the game's
 *
 *  Before tunnelling, the bridge registers with API family GC_API_ARENA
 *  and the N-Gage's Bluetooth address as device id, like the Arena
 *  plug-ins do.  Connection ids carry a generation in the high byte so
 *  that a late message for a closed connection never reaches its
 *  successor.  Types 0xA0..0xAF are reserved for GCN.  Integers are big
 *  endian; the payload runs to the end of the message.
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#ifndef GC_NET_H
#define GC_NET_H

#include "gc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GCN_MT_FIRST     0xA0
#define GCN_MT_LAST      0xAF
#define GCN_IS_NET(t)    ((t) >= GCN_MT_FIRST && (t) <= GCN_MT_LAST)
#define GCN_MT_TCP_OPEN  0xA0
#define GCN_MT_TCP_DATA  0xA1
#define GCN_MT_TCP_CLOSE 0xA2
#define GCN_MT_UDP       0xA3

#define GCN_CLOSE_FIN   0
#define GCN_CLOSE_RESET 1

#define GCN_HDR_DATA 3 /* type + id */
/* largest TCP_DATA payload the server sends: a bridge only takes a message
 * when its TCP send buffer has room for all of it (GC_TCP_TXBUF >= this) */
#define GCN_SRV_CHUNK 512
#define GCN_HDR_UDP   9 /* type + addr + port + peer_port */

typedef struct
{
    gc_u8 type;
    gc_u16 id;         /* TCP_*: connection                         */
    gc_u8 addr[4];     /* TCP_OPEN, UDP: server side address          */
    gc_u16 port;       /* TCP_OPEN, UDP: server side port             */
    gc_u16 peer_port;  /* TCP_OPEN, UDP: the game's port            */
    gc_u8 how;         /* TCP_CLOSE: GCN_CLOSE_*                      */
    const gc_u8 *data; /* TCP_DATA, UDP: points into the message   */
    size_t len;
} gcn_msg;

/** Encode into out; returns the length or 0 if it does not fit. */
size_t gcn_encode(const gcn_msg *m, gc_u8 *out, size_t cap);
/** Decode; 0 if malformed or not a GCN message. data points into msg. */
int gcn_decode(const gc_u8 *msg, size_t len, gcn_msg *m);

/** True for the host names of the N-Gage Arena back ends (arena.n-gage.com,
 *  *.n-gage.com, *.yav4.com, ngage-auth), which the bridges resolve to
 *  themselves in PPP mode. name is lower case. */
int gcn_arena_name(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* GC_NET_H */
