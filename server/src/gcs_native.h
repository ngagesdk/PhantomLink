/** @file gcs_native.h
 *
 *  N-Gage Arena back ends in their native protocols, for retail games in
 *  PPP mode (docs/ARENA.md).
 *
 *  The bridges terminate the game's TCP/IP traffic (gc_ppp, gc_ip) and
 *  tunnel each TCP connection and UDP datagram to the server in GCN
 *  messages (gc_net.h).  This module is the other end of those tunnels:
 *
 *   - TCP 80: the Community web service (SOAP over HTTP 1.1): login, the
 *     account and alert operations with per user profile data kept in a
 *     file, the device pages (*_x.jsp lists empty, other pages a stub),
 *   - TCP 5222: SegaChat, just enough Jabber (jabber:iq:auth, roster,
 *     presence) for the client to log in,
 *   - UDP: the SNAP game server; datagrams are logged (protocol still
 *     unknown).
 *
 *  It replaces tools/arena/arena_native.py.  Transport independent like
 *  gcs_core and gcs_arena: the core passes GCN messages in and sends the
 *  answers to the peer identified by key.
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#ifndef GCS_NATIVE_H
#define GCS_NATIVE_H

#include "gc_net.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gcs_native gcs_native;

typedef struct
{
    /** send one GCN message to the peer identified by key */
    void (*send)(void *ctx, void *key, const gc_u8 *data, size_t len);
    /** log line (level 0 errors .. 3 debug) */
    void (*log)(void *ctx, int level, const char *line);
} gcs_native_io;

#define GCSN_MAX_CONNS   256   /* tunnelled TCP connections, all peers */
#define GCSN_MAX_REQUEST 16384 /* one HTTP request / pending XMPP input */
#define GCSN_MAX_USERS   1024  /* accounts kept in the accounts file   */

/** accounts_file: where profile data is kept (NULL or "": memory only). */
gcs_native *gcs_native_create(const gcs_native_io *io, void *ctx, const char *accounts_file);
void gcs_native_destroy(gcs_native *n);

/** A GCN message from a registered peer (key: the core's peer). */
void gcs_native_input(gcs_native *n, void *key, const gc_u8 *data, size_t len);
/** The peer is gone: drop its connections. */
void gcs_native_peer_gone(gcs_native *n, void *key);

typedef struct
{
    int conns;
    int users;
    gc_u32 http_requests;
    gc_u32 xmpp_logins;
    gc_u32 udp_datagrams;
} gcs_native_stats;
void gcs_native_get_stats(const gcs_native *n, gcs_native_stats *st);

#ifdef __cplusplus
}
#endif

#endif /* GCS_NATIVE_H */
