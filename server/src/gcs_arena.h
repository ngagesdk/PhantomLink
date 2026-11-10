/** @file gcs_arena.h
 *
 *  N-Gage Arena back end emulation (GCA messages, see gc_arena.h).
 *
 *  Peers that registered with API family GC_API_ARENA send GCA messages;
 *  gcs_core hands them to this module.  It emulates the three Arena
 *  back ends in a simplified form:
 *
 *   - the Community web service (SOAP over HTTP): authenticateUser2,
 *     checkImei and createUser always succeed; other URLs get 404,
 *   - the SNAP game server: login, lobbies and rooms per game class id
 *     (created on demand by name), member events, room status values,
 *     room and private data relay,
 *   - SegaChat: messages, group chat in the current room, presence;
 *     service requests (retrieval/reporter rankings) are logged and
 *     answered with an empty result.
 *
 *  Like gcs_core it is transport independent and owns no sockets.
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#ifndef GCS_ARENA_H
#define GCS_ARENA_H

#include "gc_arena.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gcs_arena gcs_arena;

typedef struct
{
    /** send one GCA message to the peer identified by key */
    void (*send)(void *ctx, void *key, const gc_u8 *data, size_t len);
    /** log line (level 0 errors .. 3 debug) */
    void (*log)(void *ctx, int level, const char *line);
} gcs_arena_io;

#define GCSA_MAX_LOBBIES 32   /* per server            */
#define GCSA_MAX_ROOMS   128  /* per server            */
#define GCSA_MAX_MEMBERS 16   /* per room              */
#define GCSA_MAX_USERS   256  /* logged in at once     */
#define GCSA_MAX_HTTP    8192 /* request body buffer   */

gcs_arena *gcs_arena_create(const gcs_arena_io *io, void *ctx);
void gcs_arena_destroy(gcs_arena *a);

/** A GCA message from a registered Arena peer.  key identifies the peer
 *  (gcs_core passes its gcs_peer pointer), name is its registered name. */
void gcs_arena_input(gcs_arena *a, void *key, const char *name, const gc_u8 *data, size_t len);

/** The peer is gone for good: leave rooms and lobbies, drop the user. */
void gcs_arena_peer_gone(gcs_arena *a, void *key);

/* ---- game modules ----------------------------------------------------
 *
 * Everything above is generic: any N-Gage title built on the Arena client
 * stack gets login, lobbies, rooms, data relay, chat and the Community
 * web service, separated by its game class id.  Game specific server
 * logic (for example Pocket Kingdom's persistent kingdoms and auctions)
 * plugs in here.  A hook returns 1 if it consumed the message, 0 to let
 * the generic handling continue. */

typedef struct gcs_arena_game
{
    gc_u32 game;      /* game class id sent in LOGIN */
    const char *name; /* for log lines */
    int (*on_login)(void *ctx, gcs_arena *a, gc_u32 user, const char *name);
    int (*on_data)(void *ctx, gcs_arena *a, gc_u32 user, const gca_msg *m);
    int (*on_service)(void *ctx, gcs_arena *a, gc_u32 user, const gca_msg *m);
    void (*on_logout)(void *ctx, gcs_arena *a, gc_u32 user);
    void *ctx;
} gcs_arena_game;

#define GCSA_MAX_GAMES 16

/** Register a game module (copied).  Returns 0 if the table is full or
 *  the game class id is already registered. */
int gcs_arena_add_game(gcs_arena *a, const gcs_arena_game *g);

/** For game modules: send a message to a logged in user.  Returns 0 if
 *  the user is unknown. */
int gcs_arena_send_to(gcs_arena *a, gc_u32 user, const gca_msg *m);

/** Counters for tests and status output. */
typedef struct
{
    int users;
    int lobbies;
    int rooms;
} gcs_arena_stats;
void gcs_arena_get_stats(const gcs_arena *a, gcs_arena_stats *st);

#ifdef __cplusplus
}
#endif

#endif /* GCS_ARENA_H */
