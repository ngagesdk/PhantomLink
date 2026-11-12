/** @file gcs_direct.h
 *
 *  Direct Arena clients: N-Gages that reach the server over their own
 *  internet connection (GPRS) instead of through a PhantomLink bridge
 *  (docs/ARENA.md, "Arena over GPRS").
 *
 *  The PhantomLink app maps the Arena host names to the server's public
 *  IPv4 address in C:\System\Data\hosts, so the game's TCP connections
 *  (Community web service on port 80, SegaChat on 5222) and its SNAP
 *  datagrams (UDP 9090) arrive at the server itself.  This module turns
 *  them into the GCN messages a bridge would tunnel (gc_net.h) and feeds
 *  them to the same gcs_native instance, so GPRS players and bridge
 *  players share accounts, lobbies and game servers.
 *
 *  Each client address is one gcs_native peer.  Datagrams are only taken
 *  from addresses that have a TCP connection open or had one recently
 *  (a TCP handshake cannot be spoofed), and a SNAP login needs a
 *  Community login from the same address (gcs_native.c, snap_auth).
 *
 *  Transport independent like gcs_native: the caller owns the sockets and
 *  reports their events; sock is the caller's handle of a TCP connection.
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#ifndef GCS_DIRECT_H
#define GCS_DIRECT_H

#include "gc_types.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct gcs_core;
typedef struct gcs_direct gcs_direct;

#define GCSD_MAX_CLIENTS 128     /* client addresses                       */
#define GCSD_MAX_CONNS   128     /* TCP connections, all clients (< 256)   */
#define GCSD_IDLE_MS     600000u /* forget an address this long after its \
                                  * last connection or datagram            */

/* the services, as the game sees them (the listening ports may differ,
 * e.g. behind a port forward) */
#define GCSD_HTTP 80
#define GCSD_XMPP 5222
#define GCSD_SNAP 9090

typedef struct
{
    /** bytes for TCP connection sock */
    void (*tcp_send)(void *ctx, void *sock, const gc_u8 *d, size_t len);
    /** end TCP connection sock: reset 0 = after the queued bytes, shut
     *  down the sending side (FIN); 1 = close at once.  Never called
     *  for a sock after gcs_direct_tcp_gone. */
    void (*tcp_close)(void *ctx, void *sock, int reset);
    /** a datagram from our UDP service port (GCSD_SNAP ...) to ip:port */
    void (*udp_send)(void *ctx, gc_u16 service, const gc_u8 ip[4], gc_u16 port, const gc_u8 *d, size_t len);
    /** log line (level 0 errors .. 3 debug), may be NULL */
    void (*log)(void *ctx, int level, const char *line);
} gcs_direct_io;

/** Attaches to the core's gcs_native (gcs_core_set_direct). */
gcs_direct *gcs_direct_create(struct gcs_core *core, const gcs_direct_io *io, void *ctx);
void gcs_direct_destroy(gcs_direct *d);

/** A TCP connection from ip:port to service (GCSD_HTTP, GCSD_XMPP) was
 *  accepted.  0: taken; -1: refused (no room), the caller closes sock
 *  without calling gcs_direct_tcp_gone. */
int gcs_direct_tcp_open(gcs_direct *d, void *sock, const gc_u8 ip[4], gc_u16 port, gc_u16 service, gc_u32 now);
void gcs_direct_tcp_input(gcs_direct *d, void *sock, const gc_u8 *data, size_t len, gc_u32 now);
/** The client shut down its sending side (FIN). */
void gcs_direct_tcp_eof(gcs_direct *d, void *sock, gc_u32 now);
/** The connection is closed (by either side, or failed); sock is free. */
void gcs_direct_tcp_gone(gcs_direct *d, void *sock);
/** A datagram from ip:port to service.  Ignored unless the address is a
 *  known client (see above). */
void gcs_direct_udp_input(gcs_direct *d, gc_u16 service, const gc_u8 ip[4], gc_u16 port, const gc_u8 *data,
                          size_t len, gc_u32 now);
/** Timers: forgets idle addresses. */
void gcs_direct_poll(gcs_direct *d, gc_u32 now);

typedef struct
{
    int clients;
    int conns;
    gc_u32 tcp_accepted;
    gc_u32 udp_in;
    gc_u32 udp_dropped; /* from unknown addresses */
} gcs_direct_stats;
void gcs_direct_get_stats(const gcs_direct *d, gcs_direct_stats *st);

#ifdef __cplusplus
}
#endif

#endif /* GCS_DIRECT_H */
