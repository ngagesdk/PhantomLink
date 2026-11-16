/** @file gcs_tourney.h
 *
 *  N-Gage Arena tournaments and the activity feed of the Play menu
 *  (Tournaments, Activities, Winners; docs/ARENA.md "Tournaments").
 *
 *  The original Arena ran its tournaments on the server: a game, a time
 *  window and a ranking, shown on the device pages under Play >
 *  Tournaments.  Here the server operator defines them in a text file
 *  (`tournaments`), one per line:
 *
 *      # id|game class|statistic|mode|start|end|name|description
 *      1|42318|game_total|best|2026-12-01|2027-01-01|Ashen December Cup|Best total score of December.
 *      2|1104|$ps_points|sum|2026-12-01 18:00|2026-12-08 18:00|Worms week|Most skill points in ranked games.
 *      3|1104|$wins|sum|2026-12-01|2026-12-31|Worms winners|Most ranked games won.
 *
 *   - id: a number > 0, unique; the device pages show it as contentId.
 *   - game class: as in the SegaChat score reports (Ashen 42318, Worms
 *     World Party 1104, Pocket Kingdom 1).
 *   - statistic: an item of the game's score reports (game_total,
 *     level_1, ...), "$ps_points" for the skill points of ranked games or
 *     "$wins" for won ranked games.
 *   - mode: best (highest single value counts), low (lowest single value,
 *     e.g. times), sum (values add up: skill points, wins).
 *   - start, end: "YYYY-MM-DD" or "YYYY-MM-DD HH:MM", UTC; the end is
 *     excluded.
 *
 *  Every score report during a tournament's window updates its standings
 *  (gcs_tourney_report, fed by gcs_scores' listener).  The file is read
 *  again when it changes; standings of removed tournaments are dropped
 *  when the data file is next written.
 *
 *  Activities: new personal bests and won ranked games, the latest
 *  GCST_MAX_ACTIVITIES of them.
 *
 *  Data file (`tournament_data`, written by the server):
 *
 *      S|id|user|value|count|time     standing of user in tournament id
 *      A|time|class|user|stat|value   activity
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#ifndef GCS_TOURNEY_H
#define GCS_TOURNEY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gcs_tourney gcs_tourney;

typedef void (*gcs_tourney_log_fn)(void *ctx, int level, const char *line);

#define GCST_MAX_TOURNEYS   64
#define GCST_MAX_STANDINGS  8192 /* entries, all tournaments */
#define GCST_MAX_ACTIVITIES 64
#define GCST_MAX_CLASS      15
#define GCST_MAX_STAT       31
#define GCST_MAX_NAME       32
#define GCST_MAX_TITLE      63
#define GCST_MAX_TEXT       255

#define GCST_BEST 0
#define GCST_LOW  1
#define GCST_SUM  2

#define GCST_UPCOMING 0
#define GCST_RUNNING  1
#define GCST_FINISHED 2

typedef struct
{
    int id;
    char game_class[GCST_MAX_CLASS + 1];
    char stat[GCST_MAX_STAT + 1];
    int mode;
    long start, end; /* seconds since 1970, UTC */
    char name[GCST_MAX_TITLE + 1];
    char text[GCST_MAX_TEXT + 1];
} gcs_tourney_def;

typedef struct
{
    char user[GCST_MAX_NAME + 1];
    int rank;
    long value;
    long count; /* reports that counted */
    long time;  /* last change */
} gcs_tourney_entry;

typedef struct
{
    long time;
    char game_class[GCST_MAX_CLASS + 1];
    char user[GCST_MAX_NAME + 1];
    char stat[GCST_MAX_STAT + 1]; /* "$wins": won a ranked game */
    long value;
} gcs_tourney_activity;

/** defs_file: the operator's tournament list (may be missing);
 *  data_file: standings and activities (NULL or "": memory only). */
gcs_tourney *gcs_tourney_create(const char *defs_file, const char *data_file, gcs_tourney_log_fn log, void *ctx);
void gcs_tourney_destroy(gcs_tourney *t);

/** Reads the tournament list again if the file changed. */
void gcs_tourney_refresh(gcs_tourney *t);

/** A reported value (the arguments of gcs_scores_listener). */
void gcs_tourney_report(gcs_tourney *t, const char *game_class, const char *user, const char *stat, long value,
                        int delta, int new_best, long now);

/** Number of tournaments and the i-th (sorted: running, upcoming, finished;
 *  within each by start), NULL when out of range. */
int gcs_tourney_count(const gcs_tourney *t);
const gcs_tourney_def *gcs_tourney_at(const gcs_tourney *t, int i, long now);
const gcs_tourney_def *gcs_tourney_find(const gcs_tourney *t, int id);

/** GCST_UPCOMING, GCST_RUNNING or GCST_FINISHED. */
int gcs_tourney_state(const gcs_tourney_def *d, long now);

/** Standings of tournament id: the first max entries; total gets the
 *  number of players.  Returns the number written. */
int gcs_tourney_standings(const gcs_tourney *t, int id, gcs_tourney_entry *out, int max, int *total);

/** Rank of user in tournament id (0: not in it). */
int gcs_tourney_rank(const gcs_tourney *t, int id, const char *user, gcs_tourney_entry *out);

/** Latest activities, newest first.  Returns the number written. */
int gcs_tourney_activities(const gcs_tourney *t, gcs_tourney_activity *out, int max);

/** "YYYY-MM-DD[ HH:MM]" (UTC) to seconds since 1970; -1 if malformed. */
long gcs_tourney_parse_time(const char *s);

#ifdef __cplusplus
}
#endif

#endif /* GCS_TOURNEY_H */
