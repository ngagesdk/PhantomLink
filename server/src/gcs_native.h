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
 *     (profile view, rankings, tournaments, activities and games generated
 *     by gcs_pages.h, other *_x.jsp lists empty, other pages a stub),
 *   - TCP 5222: SegaChat (Jabber): jabber:iq:auth checked against the
 *     same accounts, friends (roster, friend requests, presence with the
 *     Play Now ext_pres data, gcs_roster.h), messages between users (kept
 *     while the receiver is offline) and the score service of games with
 *     online leaderboards (reporter@ / retrieval@ngage-auth, gcs_scores.h),
 *   - UDP 9090: the SNAP lobby and game room service (gcs_snap.h) with
 *     the game servers behind it (Rifts: Promise of Power, gcs_rifts.h);
 *     other datagrams are logged,
 *   - TCP 8080: not Arena, the Habbo Console of Habbo Islands
 *     (gcs_habbo.h), with the same accounts.
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
 *  (gcs_rifts.h); habbo_file: Habbo Console friends and messages
 *  (gcs_habbo.h); scores_file: the leaderboards (gcs_scores.h); pk_file:
 *  Pocket Kingdom's kingdoms and Global Auction (gcs_pk.h).
 *  NULL or "": memory only. */
gcs_native *gcs_native_create(const gcs_native_io *io, void *ctx, const char *accounts_file, const char *rifts_file,
                              const char *habbo_file, const char *scores_file, const char *pk_file);
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
    gc_u32 score_reports; /* SegaChat score reports (gcs_scores.h) */
    gc_u32 score_queries; /* SegaChat leaderboard queries */
    gc_u32 chat_messages; /* SegaChat messages between users */
} gcs_native_stats;
void gcs_native_get_stats(const gcs_native *n, gcs_native_stats *st);

/** The shared account store (gcs_accounts.h). */
struct gcs_accounts *gcs_native_accounts(gcs_native *n);
/** The SNAP game server (gcs_snap.h) and the Rifts game server (gcs_rifts.h). */
struct gcs_snap *gcs_native_snap(gcs_native *n);
struct gcs_rifts *gcs_native_rifts(gcs_native *n);
/** The Habbo Console back end (gcs_habbo.h). */
struct gcs_habbo *gcs_native_habbo(gcs_native *n);
/** The Pocket Kingdom game server (gcs_pk.h). */
struct gcs_pk *gcs_native_pk(gcs_native *n);

/** SegaChat friends, requests and offline messages (gcs_roster.h):
 *  file NULL or "" keeps them in memory only.  Call before the first
 *  client; returns -1 when out of memory. */
int gcs_native_set_roster_file(gcs_native *n, const char *file);
struct gcs_roster *gcs_native_roster(gcs_native *n);

/** Tournaments (gcs_tourney.h): the operator's list and the standings
 *  file (NULL or "": none / memory only).  Call before the first client;
 *  returns -1 when out of memory. */
int gcs_native_set_tournament_files(gcs_native *n, const char *defs_file, const char *data_file);
struct gcs_tourney *gcs_native_tourney(gcs_native *n);

/** The SegaChat leaderboards (gcs_scores.h). */
struct gcs_scores *gcs_native_scores(gcs_native *n);

#ifdef __cplusplus
}
#endif

#endif /* GCS_NATIVE_H */
