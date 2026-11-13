/** @file gcs_native.h
 *
 *  N-Gage Arena back ends in their native protocols, for retail games in
 *  PPP mode and for direct clients over GPRS (gcs_direct.h; docs/ARENA.md).
 *
 *  The bridges terminate the game's TCP/IP traffic (gc_ppp, gc_ip) and
 *  tunnel each TCP connection and UDP datagram to the server in GCN
 *  messages (gc_net.h).  This module is the other end of those tunnels:
 *
 *   - TCP 80: the Community web service (SOAP over HTTP 1.1): login with
 *     the shared accounts (gcs_accounts.h: created on the first login,
 *     bound to the N-Gage's IMEI, the same for every game), the account
 *     and alert operations with per user profile data, the device pages
 *     (profile view generated, *_x.jsp lists empty, other pages a stub),
 *   - TCP 5222: SegaChat, just enough Jabber (jabber:iq:auth checked
 *     against the same accounts, roster, presence) for the client to log
 *     in,
 *   - UDP 9090: the SNAP lobby and game room service (gcs_snap.h) with
 *     the game servers behind it (Rifts: Promise of Power, gcs_rifts.h);
 *     other datagrams are logged.
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

/** accounts_file: where the shared Arena accounts and their profile data
 *  are kept (gcs_accounts.h); rifts_file: the Rifts game server's data
 *  (gcs_rifts.h).  NULL or "": memory only. */
gcs_native *gcs_native_create(const gcs_native_io *io, void *ctx, const char *accounts_file, const char *rifts_file);
void gcs_native_destroy(gcs_native *n);
/** Folder of the Arena device pages (news, support, lists): a request for
 *  /n-gage/... is answered with DIR/<game class>/n-gage/... when it exists
 *  (game class: of the user's SNAP login), else DIR/n-gage/..., else the
 *  built-in placeholder. NULL or "": placeholders only. */
void gcs_native_set_www_root(gcs_native *n, const char *dir);

/** A GCN message from a registered peer (key: the core's peer). */
void gcs_native_input(gcs_native *n, void *key, const gc_u8 *data, size_t len);
/** The peer is gone: drop its connections. */
void gcs_native_peer_gone(gcs_native *n, void *key);
/** Timers (SNAP retransmissions); now in milliseconds. */
void gcs_native_poll(gcs_native *n, gc_u32 now);

typedef struct
{
    int conns;
    int users;
    gc_u32 http_requests;
    gc_u32 xmpp_logins;
    gc_u32 udp_datagrams;
} gcs_native_stats;
void gcs_native_get_stats(const gcs_native *n, gcs_native_stats *st);

/** The shared account store (gcs_accounts.h). */
struct gcs_accounts *gcs_native_accounts(gcs_native *n);
/** The SNAP game server (gcs_snap.h) and the Rifts game server (gcs_rifts.h). */
struct gcs_snap *gcs_native_snap(gcs_native *n);
struct gcs_rifts *gcs_native_rifts(gcs_native *n);

#ifdef __cplusplus
}
#endif

#endif /* GCS_NATIVE_H */
