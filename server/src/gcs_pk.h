/** @file gcs_pk.h
 *
 *  Arena game server of Pocket Kingdom: Own the World.
 *
 *  Online, Pocket Kingdom talks to its game server through the SNAP game
 *  room (gcs_snap.c, SendGameDataToServer / GameDataReceived).  Every
 *  message starts with a type byte and, for most types, a sequence byte
 *  that the answer echoes.  Player names are 16 byte fields (at most 15
 *  characters, zero padded); numbers in the auction messages are big
 *  endian.  docs/ARENA.md, "Pocket Kingdom game server", has the layouts.
 *
 *  The server does three jobs:
 *
 *    kingdoms   who is online in which area of the map, and what their
 *               kingdom looks like (castle, transporter, mark, colour);
 *               the search for enemy kingdoms lists up to five of them
 *    relay      scouting, attacks and chat messages go from one player's
 *               game to the other's (the defender's game answers them);
 *               the server finds the receiver by name and answers for
 *               players who are offline or have moved to another area;
 *               chat messages to offline players wait for their next
 *               visit
 *    auction    the Global Auction: exhibit, browse, bid, and collect
 *               won items, sold items' loot and refunds
 *
 *  This is an independent implementation from the client side of the
 *  retail game (6RAH.APP, version 110804); the data is kept in memory
 *  and written to a file.
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#ifndef GCS_PK_H
#define GCS_PK_H

#include "gcs_snap.h"

#ifdef __cplusplus
extern "C" {
#endif

/* GameClassId of Pocket Kingdom as its SegaChat presence carries it on
 * hardware; the SNAP login is expected to carry the same value */
#define GCS_PK_GAME_CLASS 1
/* "Scrimmage": LobbyToJoin in game.rsc; "Auction": the lobby of the
 * Global Auction (the game switches with CArenaFramework slot 0x6c,
 * lobby "Auction", room "Auction") */
#define GCS_PK_LOBBY "Scrimmage,Auction,Ranked"
/* "Scrimmage": RoomToJoin in game.rsc (the Play Now screens go back to
 * the mode selection if the lobby has no room of that name; the game
 * never creates it); "Auction": the Global Auction's room; "Ranked": the
 * room Ranked matchmaking finds (it may not create one,
 * ProhibitRoomCreation, and asks for STAT & 0x30000000 == 0x30000000,
 * fewer than 0x7fff users, ARNA..ARND == 0).  Kingdoms only meet kingdoms
 * of the same room: Scrimmage is the mode without losses. */
#define GCS_PK_ROOMS "Scrimmage@Scrimmage,Auction@Auction,Ranked@Ranked#30000001"

typedef struct gcs_pk gcs_pk;

/** file: where the game data is kept (NULL or "": memory only). */
gcs_pk *gcs_pk_create(const char *file, void (*log)(void *ctx, int level, const char *line), void *ctx);
void gcs_pk_destroy(gcs_pk *p);

/** The game server to attach with gcs_snap_add_game(). */
const gcs_snap_game *gcs_pk_game(gcs_pk *p);

/** Tests: send answers through fn instead of gcs_snap_send_game_data(). */
void gcs_pk_set_output(gcs_pk *p, void (*fn)(void *ctx, gc_u32 uid, const gc_u8 *d, size_t n), void *ctx);
/** Tests: use t (seconds since 1970) as the current time; 0: the clock. */
void gcs_pk_set_time(gcs_pk *p, long t);

typedef struct
{
    int online;      /* players in a Pocket Kingdom game room   */
    int kingdoms;    /* kingdoms known (registered once)        */
    int auctions;    /* auctions kept (running or not collected) */
    int mail;        /* chat messages waiting for offline players */
    gc_u32 messages; /* game messages handled                    */
    gc_u32 relayed;  /* messages passed on to another player     */
} gcs_pk_stats;
void gcs_pk_get_stats(const gcs_pk *p, gcs_pk_stats *st);

/* game -> server message types (first byte) */
#define GCSP_REGISTER 1    /* online: area and kingdom look       -> 129 */
#define GCSP_PING     2    /* keep alive                          -> 130 */
#define GCSP_LOGOUT   3    /* going offline                           */
#define GCSP_MOVE     5    /* moved to another area                   */
#define GCSP_SEARCH   6    /* kingdoms in the area                -> 134 */
#define GCSP_SCOUT    7    /* scout a kingdom (relayed)           -> 135 */
#define GCSP_ATTACK   9    /* attack a kingdom (relayed)          -> 137 */
#define GCSP_ENGAGE   10   /* the battle starts (relayed + echoed)    */
#define GCSP_EXHIBIT  13   /* auction: exhibit an item            -> 141 */
#define GCSP_BROWSE   14   /* auction: search the exhibits        -> 142 */
#define GCSP_BID      15   /* auction: bid                        -> 143 */
#define GCSP_MYLIST   16   /* auction: own exhibits or bids       -> 144 */
#define GCSP_UNUSED17 17   /* built by the game, never sent       -> 145 */
#define GCSP_CHAT     18   /* chat message (relayed)              -> 146 */
#define GCSP_SUMMARY  21   /* kingdom summary (victory points...) -> 149 */
#define GCSP_UNUSED22 22   /* built by the game, never sent       -> 150 */
#define GCSP_ANSWER   0x80 /* answer type = request type | 0x80        */
#define GCSP_FATAL    255  /* the game goes offline when it gets this  */

/* result codes in byte 2 of a scout or attack answer (> 252) */
#define GCSP_NOBODY_HOME 0xff /* sent by the defender's own game */
#define GCSP_MOVED_AREA  0xfe /* sent by the server: offline or elsewhere */
/* 146 [2]: 0 delivered (the game shows nothing); anything else starts a
 * mission (type 11, 6RAH 0x6817c) that ends with trigger037 "Couldn't
 * deliver the message!" (0x70e6c) */
#define GCSP_CHAT_OK     0
#define GCSP_CHAT_FAILED 1

#define GCSP_MAX_NAME     15
#define GCSP_SEARCH_MAX   5  /* kingdoms per search answer          */
#define GCSP_LIST_MAX     10 /* auction entries per answer          */
#define GCSP_MAX_EXHIBITS 10 /* per player (trigger040)             */
#define GCSP_MAX_BIDS     10 /* per player (trigger042)             */
#define GCSP_MAX_LOOT     9999999
#define GCSP_AUCTION_SECS (24L * 3600L) /* how long an auction runs     */
#define GCSP_MAX_MAIL     50            /* waiting chat messages per player     */

#ifdef __cplusplus
}
#endif

#endif /* GCS_PK_H */
