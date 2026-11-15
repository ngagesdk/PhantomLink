/** @file gcs_wwp.c
 *
 *  Worms World Party, see gcs_wwp.h.
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#include "gcs_wwp.h"

#include <stddef.h>

/* No game server: game data goes from member to member of the room
 * (gcs_snap.c, ops 15 and 16), so only the class and the lobbies. */
static const gcs_snap_game wwp_game = {
    GCS_WWP_GAME_CLASS,
    "Worms World Party",
    GCS_WWP_LOBBIES,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
};

const gcs_snap_game *gcs_wwp_game(void)
{
    return &wwp_game;
}
