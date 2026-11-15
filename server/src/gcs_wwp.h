/** @file gcs_wwp.h
 *
 *  Worms World Party (6R39, N-Gage Arena game class 1104).
 *
 *  Worms World Party plays its online games through the N-Gage Arena
 *  "Play Now" screens of ArenaFramework: the player picks Freestyle or
 *  Ranked, a lobby, then creates or finds a game room.  The game itself
 *  (a Mophun program, worms.mpn) sends its turns as game data to the
 *  other members of the room (vArenaSendGameData and friends), so there
 *  is no game server: gcs_snap.c passes the data on.  After a game it
 *  reports the result and, in ranked games, its skill points to the
 *  SegaChat reporter (gcs_scores.c), and it reads them back from the
 *  retrieval service ("Points Total", "New Ranked Position").
 *
 *  This module only registers the game class and its lobbies with the
 *  SNAP server; docs/ARENA.md, "Worms World Party", has the findings.
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#ifndef GCS_WWP_H
#define GCS_WWP_H

#include "gcs_snap.h"

#ifdef __cplusplus
extern "C" {
#endif

/* GameClassId (key 0x2007 of game.rsc; arena.amr names the SegaChat
 * services retrieval1104 and reporter1104) */
#define GCS_WWP_GAME_CLASS 1104
/* game.rsc names no lobby: the Play Now lobby list comes from the
 * server. One lobby per matchmaking mode the game offers. */
#define GCS_WWP_LOBBIES "Freestyle,Ranked"

/** The registration to attach with gcs_snap_add_game() (static). */
const gcs_snap_game *gcs_wwp_game(void);

#ifdef __cplusplus
}
#endif

#endif /* GCS_WWP_H */
