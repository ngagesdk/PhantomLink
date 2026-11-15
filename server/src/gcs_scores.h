/** @file gcs_scores.h
 *
 *  SegaChat score service: online leaderboards of N-Gage Arena games
 *  that report scores over SegaChat (Jabber) instead of a game server.
 *  The first such game is Ashen (game class 42318, docs/ARENA.md).
 *
 *  sc_lib.dll sends two Jabber messages for this (no type attribute,
 *  the request kind is in the id attribute):
 *
 *   - to reporter@ngage-auth, id segachat_send_event: a score report.
 *     Attributes event_type ("submit"), game_class_id, match_id,
 *     snap_name (the player), client_time; children
 *     <item name='STAT'><value>N</value></item>, one per statistic.
 *   - to retrieval@ngage-auth, id segachat_retrieve_req: a query.
 *     Attributes event_type ("topn"), game_class_id, snap_name; child
 *     <retrieve><request>ITEMLIST</request></retrieve>, where ITEMLIST is
 *     <itemlist><item name='K' value='V' />...</itemlist> with the keys
 *     queryid, board, stat, offset, limit, periodicity, ordering, format.
 *
 *  The answer to a query is a message with id segachat_retrieve_resp,
 *  the same event_type and game_class_id, and
 *  <retrieve><response>CSV</response></retrieve>.  The CSV (format
 *  "csv") has '|' between fields and '\n' after every line:
 *
 *      0                                   status, 0 = ok
 *      OK                                  message (shown on errors)
 *      queryid|board|stat|ROWS|offset|total|periodicity
 *      name|rank|score|date                ROWS times
 *
 *  Ashen reads the status, the message, field 3 of the header (the
 *  number of rows) and name, rank and score of every row; the other
 *  fields are this server's choice.  Its parser keeps at most 10 rows on
 *  the stack and has no bounds check, so never more than GCSS_MAX_ROWS
 *  rows are sent.  A status other than 0 makes the game show the message.
 *
 *  Every user keeps the best (highest) value per game class and
 *  statistic; values of 0 or less are not kept (Ashen reports all its
 *  levels at once, with 0 for the levels not played yet).  Ordering
 *  "natural" lists the highest values first; "reverse" / "ascending"
 *  the lowest.  Only all-time lists exist; other periodicity values are
 *  answered with the all-time list.
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#ifndef GCS_SCORES_H
#define GCS_SCORES_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gcs_scores gcs_scores;

/** log line (level 0 errors .. 3 debug) */
typedef void (*gcs_scores_log_fn)(void *ctx, int level, const char *line);

#define GCSS_REPORTER    "reporter"  /* node of the score report JID */
#define GCSS_RETRIEVAL   "retrieval" /* node of the query JID        */
#define GCSS_MAX_RECORDS 16384       /* best values, all games       */
#define GCSS_MAX_CLASS   15          /* game class id, characters    */
#define GCSS_MAX_STAT    31          /* statistic name, characters   */
#define GCSS_MAX_NAME    32          /* user name, characters        */
#define GCSS_MAX_ROWS    10          /* rows per answer (Ashen: 10)  */
#define GCSS_MAX_CSV     1000        /* answer size (Ashen: < 1024)  */

/** file: the best values (NULL or "": memory only). */
gcs_scores *gcs_scores_create(const char *file, gcs_scores_log_fn log, void *ctx);
void gcs_scores_destroy(gcs_scores *s);

/** One reported statistic of user (the account name, not the snap_name
 *  the client claims).  value: the decimal text of the item.  Returns 1
 *  for a new best value, 0 when the old one stays, -1 when rejected
 *  (bad name or value, store full).  Call gcs_scores_commit() after the
 *  items of one report. */
int gcs_scores_submit(gcs_scores *s, const char *game_class, const char *user, const char *stat, const char *value,
                      long now);

/** Saves the file if a submit changed something. */
void gcs_scores_commit(gcs_scores *s);

/** Answers a query.  request: the ITEMLIST (XML entities already
 *  decoded); out: the CSV, always terminated (cap >= 64).  Returns the
 *  number of rows, or -1 for an error answer (status 1). */
int gcs_scores_query(gcs_scores *s, const char *game_class, const char *request, char *out, size_t cap);

/* ---- ArenaFramework games (arenabase.dll), e.g. Worms World Party ----
 *
 *  Games built on ArenaFramework send the same two messages, to
 *  reporter<class>@ / retrieval<class>@ (arena.amr: JabberReportingName,
 *  JabberServicesName) or reporter@ / retrieval@.  Their item lists carry
 *  "$version" and a numeric "queryid", and every request, reports
 *  included, gets an answer: a segachat_retrieve_resp with the request's
 *  event_type and game_class_id (arenabase drops answers whose
 *  game_class_id is not its own class).  The CSV starts with status and
 *  message like above; the rest is checked field by field
 *  (arenabase.dll 0x7774 and the parsers at 0x7b00..0x9500):
 *
 *    submit        one line of 4 fields; field 3 goes to the game
 *    getmatchid    one line of 6 fields; field 1 is the new match id
 *    getplayer     header + rows, topn and proximitylist too: header
 *                  field 1 is a number, field 4 the board name
 *                  ("playerskills", "highscores", "hsfilter"); every row
 *                  has the same number of fields:
 *
 *                  board         getplayer   topn      proximitylist
 *                  playerskills  7 / 6       6 / 7     6 / 7
 *                  highscores    6 / 4       7 / 4     7 / 4
 *                  hsfilter      6 / 3       7 / 3     6 / 3
 *                  (header fields / row fields)
 *
 *  Header: queryid|total|offset|rows|board|periodicity[|name or stat].
 *  Rows (this server's choice where the game reads them, like Ashen's
 *  highscores): name|rank|score|date for highscores, name|rank|score for
 *  hsfilter, name|rank|points|wins|losses|draws[|games] for
 *  playerskills.
 *
 *  Reports: "$ps_points" (with "$ps_skilltype") adds the game's skill
 *  points of one match to the player's total; win_lose and draw (Worms
 *  World Party: also opponent_id, gametime, roundsplayed, checksums)
 *  count won, lost and drawn games; each only once per match_id and
 *  player.  Other numeric items go to the best values (highscores).
 *  getmatchid hands out new match ids. */

/** 1 if an item list (request) or report item name comes from
 *  ArenaFramework ("$version" / "queryid" item). */
int gcs_scores_is_af(const char *request);

typedef struct
{
    const char *name;
    const char *value;
} gcs_scores_item;

/** A report (segachat_send_event) of an ArenaFramework game.  user: the
 *  account; match_id: the message attribute (may be empty).  Writes the
 *  answer CSV to out (cap >= 64).  Returns 0, or -1 for an error answer. */
int gcs_scores_af_submit(gcs_scores *s, const char *game_class, const char *user, const char *match_id,
                         const gcs_scores_item *items, int n, long now, char *out, size_t cap);

/** A query (segachat_retrieve_req) of an ArenaFramework game: event
 *  getmatchid, getplayer, topn or proximitylist.  user: the account
 *  asking (default name for getplayer and proximitylist).  Writes the
 *  answer CSV to out (cap >= 64); returns the number of rows, -1 for an
 *  error answer. */
int gcs_scores_af_query(gcs_scores *s, const char *game_class, const char *event, const char *user,
                        const char *request, char *out, size_t cap);

#define GCSS_MAX_SKILLS  4096 /* players with skill points, all games */
#define GCSS_AF_MAX_ROWS 20   /* rows per ArenaFramework answer       */
#define GCSS_AF_MAX_CSV  2000 /* answer size                           */

typedef struct
{
    int records; /* kept best values */
    unsigned long submits;
    unsigned long queries;
    int skills;            /* players with skill points or results */
    unsigned long matches; /* match ids handed out */
} gcs_scores_stats;
void gcs_scores_get_stats(const gcs_scores *s, gcs_scores_stats *st);

#ifdef __cplusplus
}
#endif

#endif /* GCS_SCORES_H */
