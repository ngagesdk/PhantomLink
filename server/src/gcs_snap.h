/** @file gcs_snap.h
 *
 *  SNAP game server for retail N-Gage Arena games in PPP mode.
 *
 *  SNAP is the lobby and game room service of the N-Gage Arena (Nokia's
 *  version of SEGA's SNAP, snapcomm.dll "kkClient Ver 2.1").  The game
 *  talks to it over UDP, port 9090 for the login.  This module answers
 *  it: login (checked against the shared accounts), lobbies, game rooms,
 *  data between the members of a room, and data to and from the room's
 *  game server, which a game module implements (Rifts: gcs_rifts.c).
 *
 *  Wire format (docs/ARENA.md, "SNAP"): a datagram holds one or more
 *  messages and ends with the trailer BA 47 66 xx.  A message starts with
 *  a 16 byte big endian header:
 *
 *     u16 flags | length    0x8000 reliable, 0x4000 carries an ack,
 *                           0x2000 set by the client, 0x1000 lobby
 *                           (else game room), 0x0800 more messages
 *                           follow, 0x0400 game settings; the low 10
 *                           bits are the length including the header
 *     u8  command sequence  echoed in the answer
 *     u8  opcode
 *     u32 user id           assigned by the login answer
 *     u32 sequence          reliable or unreliable sequence number
 *     u32 ack               reliable sequence number acknowledged
 *
 *  Transport independent like gcs_native: the caller passes datagrams in
 *  and sends what comes out through io.send.
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#ifndef GCS_SNAP_H
#define GCS_SNAP_H

#include "gc_types.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gcs_snap gcs_snap;

#define GCSS_PORT         9090 /* SNAP login port of snapcomm.dll   */
#define GCSS_MAX_SESSIONS 64
#define GCSS_MAX_LOBBIES  16
#define GCSS_MAX_ROOMS    64
#define GCSS_MAX_NAME     32
#define GCSS_RESEND_MS    1000
#define GCSS_MAX_TRIES    15
#define GCSS_IDLE_MS      180000 /* drop a silent session          */

typedef struct
{
    /** send one datagram to the game: from addr:port (the server side
     *  end point the game used) to its peer_port, through peer key */
    void (*send)(void *ctx, void *key, const gc_u8 addr[4], gc_u16 port, gc_u16 peer_port, const gc_u8 *d,
                 size_t n);
    /** log line (level 0 errors .. 3 debug) */
    void (*log)(void *ctx, int level, const char *line);
    /** May user log in through peer key?  Copies the account's spelling
     *  of the name to canon and returns 1, or returns 0. */
    int (*auth)(void *ctx, void *key, const char *user, char canon[GCSS_MAX_NAME + 1]);
} gcs_snap_io;

/** A game server, attached to the game rooms of one game class. */
typedef struct
{
    gc_u32 game_class;
    const char *name;
    /** the lobby the game looks for (LobbyToJoin in its game.rsc), listed
     *  by op 71 even before anybody entered it; may be NULL */
    const char *lobby;
    void *ctx;
    /** a user entered a room of this game (the server learns its name) */
    void (*joined)(void *ctx, gcs_snap *s, gc_u32 uid, const char *user);
    /** the user left the room or logged out */
    void (*left)(void *ctx, gcs_snap *s, gc_u32 uid);
    /** game data for the game server (SendGameDataToServer) */
    void (*data)(void *ctx, gcs_snap *s, gc_u32 uid, const gc_u8 *d, size_t n);
    /** called from gcs_snap_poll */
    void (*poll)(void *ctx, gcs_snap *s, gc_u32 now);
} gcs_snap_game;

gcs_snap *gcs_snap_create(const gcs_snap_io *io, void *ctx);
void gcs_snap_destroy(gcs_snap *s);

/** Attach a game server (kept by pointer, must outlive s).  0 ok. */
int gcs_snap_add_game(gcs_snap *s, const gcs_snap_game *g);

/** A datagram from the game.  addr/port: where the game sent it (the
 *  server side), peer_port: the game's port. */
void gcs_snap_input(gcs_snap *s, void *key, const gc_u8 addr[4], gc_u16 port, gc_u16 peer_port, const gc_u8 *d,
                    size_t n, gc_u32 now);
/** Retransmissions and timeouts. */
void gcs_snap_poll(gcs_snap *s, gc_u32 now);
/** The peer is gone: drop its sessions. */
void gcs_snap_peer_gone(gcs_snap *s, void *key);

/** Game class of user's SNAP session (the game he plays), 0 if none. */
gc_u32 gcs_snap_game_class_of(const gcs_snap *s, const char *user);

/** Game server -> user: reliable game data (arrives as GameDataReceived).
 *  0 ok, -1 unknown user or queue full. */
int gcs_snap_send_game_data(gcs_snap *s, gc_u32 uid, const gc_u8 *d, size_t n);

typedef struct
{
    int sessions;
    int rooms;
    gc_u32 datagrams;
    gc_u32 bad_datagrams;
    gc_u32 logins;
    gc_u32 resends;
} gcs_snap_stats;
void gcs_snap_get_stats(const gcs_snap *s, gcs_snap_stats *st);

/* opcodes found in snapcomm.dll (client side names in docs/ARENA.md) */
#define GCSS_OP_ACK          0
#define GCSS_OP_LOGOUT       2
#define GCSS_OP_LOBBY_INFO   3 /* query lobby by name / lobby info     */
#define GCSS_OP_CREATE_ROOM  4
#define GCSS_OP_REMOVED      5 /* user/lobby gone (notification)       */
#define GCSS_OP_JOIN         6 /* lobby (0x1000) or room               */
#define GCSS_OP_LEAVE        7
#define GCSS_OP_ATTR         8 /* change attributes                    */
#define GCSS_OP_ATTR2        9
#define GCSS_OP_USERS        10 /* query users                          */
#define GCSS_OP_ROOMS        11 /* search game rooms                    */
#define GCSS_OP_LOBBIES      14 /* lobby list                           */
#define GCSS_OP_DATA         15 /* game data to the room (broadcast)    */
#define GCSS_OP_PDATA        16 /* game data to some users (private)    */
#define GCSS_OP_TO_SERVER    17 /* game data to the room's game server  */
#define GCSS_OP_FAIL         39 /* result: failure {cmd, reason}         */
#define GCSS_OP_OK           40 /* result: success {cmd, value, ...}     */
#define GCSS_OP_LOGIN_OK     41 /* login result {port, -, user id}        */
#define GCSS_OP_GAME_LOBBIES 71 /* lobbies of the login's game class   */
#define GCSS_OP_LOGIN        44

#ifdef __cplusplus
}
#endif

#endif /* GCS_SNAP_H */
