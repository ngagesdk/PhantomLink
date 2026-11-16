/** @file gcs_pages.h
 *
 *  Generated Arena device pages of the Play menu: rankings, tournaments,
 *  activities and the game list (docs/ARENA.md "Device pages").
 *
 *  ArenaFramework (cafmplay.dll) asks for lists as XML and shows pages as
 *  XHTML.  A list (*_x.jsp, parser cafmplay.dll 0x36c0) is
 *
 *      <list><currentStart value="0"/><listTotal value="N"/>
 *        <itemNode index="0" contentId="7" gameId="1104"
 *                  username="bob" mediaPath="">label</itemNode> ...
 *      </list>
 *
 *  index, contentId and gameId are numbers, username and mediaPath text,
 *  the element text is the label shown in the menu.  When an item is
 *  chosen, the framework opens the page of that screen with
 *  contentId=, gameId= or username= added to its address (together with
 *  locale=, start=, disp= and platform=device); a search adds cmd=search
 *  and username=.
 *
 *  Pages generated here (a file in www_root with the same path is served
 *  instead, so operators can replace any of them):
 *
 *    community/ranking_list_x.jsp             games with rankings; with
 *                                             cmd=search: players
 *    global/play/rankings/post_game.jsp       a game's rankings (gameId)
 *    global/play/rankings/post_player.jsp     a player's ranks (username,
 *                                             default: the user)
 *    global/play/rankings/spotlight.jsp       leaders of every ranking
 *    community/tournament_list_x.jsp          tournaments
 *    global/play/tournaments/post_tournaments.jsp  one tournament (contentId)
 *    global/play/tournaments/news.jsp         running and upcoming
 *    global/play/tournaments/winners.jsp      finished, with winners
 *    community/activity_list_x.jsp            latest activities
 *    global/play/activities/post_activity.jsp one activity (contentId)
 *    global/play/activities/news.jsp          latest activities
 *    global/play/activities/winners.jsp       = tournaments/winners.jsp
 *    community/game_list_x.jsp                Arena games
 *    global/play/games/post_game.jsp          one game (gameId)
 *
 *  (paths below /n-gage/device/)
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#ifndef GCS_PAGES_H
#define GCS_PAGES_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct gcs_scores;
struct gcs_tourney;
struct gcs_accounts;

typedef struct
{
    struct gcs_scores *scores;
    struct gcs_tourney *tourney;   /* may be NULL */
    struct gcs_accounts *accounts; /* player search; may be NULL */
} gcs_pages;

#define GCSP_LIST_TYPE "text/xml; charset=utf-8"
#define GCSP_PAGE_TYPE "application/xhtml+xml; charset=utf-8"

/** path: without query ("/n-gage/device/..."); query: the part after
 *  '?' (may be NULL); user: the logged in user ("" if unknown); now:
 *  seconds since 1970.  Returns the page (malloc'ed, *len bytes, *type
 *  set), or NULL when the path is not a generated page. */
char *gcs_pages_render(const gcs_pages *p, const char *path, const char *query, const char *user, long now,
                       size_t *len, const char **type);

/** Display name of a game class ("Ashen" for 42318), or NULL. */
const char *gcs_pages_game_name(const char *game_class);

#ifdef __cplusplus
}
#endif

#endif /* GCS_PAGES_H */
