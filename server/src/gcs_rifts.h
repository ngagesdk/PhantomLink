/** @file gcs_rifts.h
 *
 *  Arena game server of Rifts: Promise of Power (game class 8053).
 *
 *  Rifts plays "play by e-mail" style battles over the N-Gage Arena: a
 *  player creates a challenge with a party, others find and accept it,
 *  and the players then upload and download their turns, each when it
 *  suits them.  The game talks to its game server through the SNAP game
 *  room (gcs_snap.c, SendGameDataToServer / GameDataReceived) with its
 *  own messages:
 *
 *    MSG_SERVER          a chunk (<= 384 bytes) of a request or answer
 *                        stream, keyed by object id (the client's action)
 *    MSG_SERVER_CANCEL   an action failed or was cancelled
 *    MSG_FLOW_EXTEND_CREDIT  flow control: more bytes the peer may send
 *    MSG_PING            answered with MSG_PING
 *
 *  The streams are packed with gcs_rcodec (zlib, CRC-32, Blowfish).  The
 *  requests ("actions") and the stored data are described in
 *  docs/ARENA.md, "Rifts".  This is an independent implementation from
 *  that description; the data (battles, players, characters, turns, save
 *  slots, friends) is kept in memory and written to a file.
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#ifndef GCS_RIFTS_H
#define GCS_RIFTS_H

#include "gcs_snap.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GCS_RIFTS_GAME_CLASS 8053

typedef struct gcs_rifts gcs_rifts;

/** file: where the game data is kept (NULL or "": memory only). */
gcs_rifts *gcs_rifts_create(const char *file, void (*log)(void *ctx, int level, const char *line), void *ctx);
void gcs_rifts_destroy(gcs_rifts *r);

/** The game server to attach with gcs_snap_add_game(). */
const gcs_snap_game *gcs_rifts_game(gcs_rifts *r);

typedef struct
{
    int players;     /* users in a Rifts game room   */
    int battles;     /* challenges and battles kept  */
    gc_u32 requests; /* actions answered             */
    gc_u32 failed;   /* actions cancelled            */
} gcs_rifts_stats;
void gcs_rifts_get_stats(const gcs_rifts *r, gcs_rifts_stats *st);

/* action types (object type of MSG_SERVER) */
#define GCSR_DOWNLOAD_TURNS            0
#define GCSR_TURN_COUNT                1
#define GCSR_UPLOAD_TURNS              2
#define GCSR_UPLOAD_CHARACTERS         3
#define GCSR_CREATE_CHALLENGE          4
#define GCSR_SERVER_CONFIG             5
#define GCSR_NAME_VALIDATION           6
#define GCSR_FIND_CHALLENGE            7
#define GCSR_ACCEPT_CHALLENGE          8
#define GCSR_CHALLENGE_STATUS          9
#define GCSR_DOWNLOAD_CHALLENGE_UNUSED 10 /* reserved in the game enum; not implemented by its server */
#define GCSR_DOWNLOAD_CHARACTERS       11
#define GCSR_END_GAME                  12
#define GCSR_GAME_UPKEEP               13
#define GCSR_UPDATE_FRIENDS            14

/* game messages */
#define GCSR_MSG_SERVER        4
#define GCSR_MSG_SERVER_CANCEL 5
#define GCSR_MSG_END_TURN      18
#define GCSR_MSG_CREDIT        25
#define GCSR_MSG_PING          26

#define GCSR_CHUNK  384  /* data bytes per MSG_SERVER          */
#define GCSR_CREDIT 8192 /* initial flow control credit         */

#ifdef __cplusplus
}
#endif

#endif /* GCS_RIFTS_H */
