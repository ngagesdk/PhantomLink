/** @file gcs_ptg.h
 *
 *  Pathway to Glory: Ikusa Islands (6R82, N-Gage Arena game class 2955)
 *  and Pathway to Glory (6R72, game class 43520), game server.
 *
 *  Ikusa Islands plays its GPRS games through ArenaFramework's Play Now
 *  screens (game rooms of up to six players) and a game server of its
 *  own behind the room (arena.amr: GameServerFQDN
 *  bs01.ptg2.redlynx.sf.yav4.com).  All game traffic goes to that server
 *  (CArenaFramework slot 0x3c, SNAP op 17) and comes back from it as
 *  room data (op 15).  Findings: docs/ARENA.md, "Pathway to Glory:
 *  Ikusa Islands".
 *
 *  The game's network layer ("Pathway To Glory Network", 6R82.APP
 *  0x83fd4) frames every message as
 *
 *     u16 LE header   bits 0..6 message type (0..54)
 *                     bit 7     the server restarts its sync counters
 *                     bits 8..14 tag, echoed in the acknowledgement
 *                     bit 15    zlib compressed
 *     u16 LE length   of the data (compressed: of the zlib stream)
 *     data
 *
 *  and keeps one receive queue per type.  The game code above it is the
 *  same as for Bluetooth play, where the host's phone is the hub: it
 *  takes every message of its clients and passes on what the others need
 *  (commands, chat, Field Radio).  Over GPRS there is one connection, to
 *  this server, so the server does the hub's passing on, and it packs
 *  the streams the game expects only from the Arena server:
 *
 *   - commands (type 16) of every player go to all others as bundles
 *     (type 32, {u16 length, data} entries); the game executes a command
 *     on the other phones when it arrives, which is what the "blue ring"
 *     shows; the game hides the GPRS round trip by executing locally
 *     first and correcting afterwards (rollback),
 *   - the host's synchronisation records (type 21, end marker 35) go to
 *     the clients as one bundle (type 34); the game makes the end marker
 *     itself after the bundle,
 *   - the host's permanent damage data (type 23, end marker 41) goes on
 *     unchanged; the game makes the end marker itself,
 *   - the Field Radio (type 43, 260 bytes: player, chunk, chunk count,
 *     256 bytes of the recorded message) goes to the other players,
 *   - start: after ArenaFramework's game start the game sends nothing
 *     until the server sends something, so every phone of a room in
 *     progress (STAT bit 31) gets pings (type 18) until it sends,
 *   - keep alive: the game sends type 19 (4 bytes) every 20 s and gives
 *     up after 30 s without type 19 back,
 *   - type 33 acknowledges every 23, 32 and 34,
 *   - type 50 {u16 code} ends the game on the phone (3: the host left,
 *     8: no opponents left; table at 6R82.APP 0x6927c).
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#ifndef GCS_PTG_H
#define GCS_PTG_H

#include "gcs_snap.h"

#ifdef __cplusplus
extern "C" {
#endif

/* GameClassId (key 0x2007 of game.rsc; arena.amr names the SegaChat
 * services retrieval2955 and reporter2955) */
#define GCS_PTG_GAME_CLASS 2955
/* Pathway to Glory (6R72, v1.03; ArenaFramework 2.01.054): the same
 * network layer without the restart bit and the tag in the header;
 * arena.amr: im.ptg.redlynx.sf.yav4.com, snap.ptg.redlynx.sf.yav4.com,
 * retrieval43520 / reporter43520 */
#define GCS_PTG1_GAME_CLASS 43520
/* game.rsc names no lobby; one per matchmaking mode (resources 0x2018
 * and 0x201a: a free game and a ranked one) */
#define GCS_PTG_LOBBIES "Freestyle,Ranked"

#define GCS_PTG_MAX_GAMES   32
#define GCS_PTG_MAX_PLAYERS 8 /* game.rsc 0x200d: 6 */

/* message types (6R82.APP; names from what the game does with them) */
#define GCS_PTG_COMMAND   16 /* a soldier's order, executed on every phone */
#define GCS_PTG_PING      18 /* server -> game: answered with 19 */
#define GCS_PTG_KEEPALIVE 19 /* game -> server every 20 s, 4 bytes, echoed */
#define GCS_PTG_SYNC      21 /* host: synchronisation record */
#define GCS_PTG_PERM      23 /* host: permanent damage data */
#define GCS_PTG_COMMANDS  32 /* server: bundle of commands */
#define GCS_PTG_ACK       33 /* game: acknowledges 23, 32 and 34 */
#define GCS_PTG_SYNCS     34 /* server: bundle of synchronisation records */
#define GCS_PTG_SYNC_END  35 /* end of the synchronisation records */
#define GCS_PTG_PERM_END  41 /* end of the permanent damage data */
#define GCS_PTG_RADIO     43 /* Field Radio chunk */
#define GCS_PTG_STATUS    50 /* server: {u16 code} */
#define GCS_PTG_ALL       52 /* client: passed on to all players by the host */

/* GCS_PTG_STATUS codes */
#define GCS_PTG_ST_HOST_LEFT    3
#define GCS_PTG_ST_TOO_MANY     5
#define GCS_PTG_ST_NO_OPPONENTS 8

#define GCS_PTG_FRAG       596    /* data bytes per message on the wire */
#define GCS_PTG_MAX_UNPACK 200000 /* the game refuses larger compressed messages */

typedef struct gcs_ptg gcs_ptg;

gcs_ptg *gcs_ptg_create(void (*log)(void *ctx, int level, const char *line), void *ctx);
void gcs_ptg_destroy(gcs_ptg *p);

/** The registrations to attach with gcs_snap_add_game(): Ikusa Islands
 *  and the first Pathway to Glory. */
const gcs_snap_game *gcs_ptg_game(gcs_ptg *p);
const gcs_snap_game *gcs_ptg1_game(gcs_ptg *p);

typedef struct
{
    int games;
    gc_u32 messages_in;
    gc_u32 messages_out;
    gc_u32 commands;
    gc_u32 radio_chunks;
    gc_u32 duplicates;
} gcs_ptg_stats;
void gcs_ptg_get_stats(const gcs_ptg *p, gcs_ptg_stats *st);

#ifdef __cplusplus
}
#endif

#endif /* GCS_PTG_H */
