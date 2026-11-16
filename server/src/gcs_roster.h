/** @file gcs_roster.h
 *
 *  SegaChat friends: the roster (friend list) of every Arena account,
 *  open friend requests and messages kept for users who are offline.
 *  Used by the SegaChat service in gcs_native.c (Communicate > Friends,
 *  chat, whispers and Play Now friends of every ArenaFramework game).
 *
 *  sc_lib.dll is a libjabber client and uses plain Jabber for this:
 *
 *   - roster: <iq type='get' id='segachat_get_roster'><query
 *     xmlns='jabber:iq:roster'/></iq>; a friend is removed with an iq set
 *     of <item jid='x' subscription='remove'/>.
 *   - friend request: <presence to='x@ngage-auth' type='subscribe'
 *     id='segachat_buddy_req'>, answered by the other user with type
 *     'subscribed' (Authorise) or 'unsubscribed' (Do not authorise), id
 *     segachat_buddy_accrej.
 *   - presence: <presence> with <ext_pres .../> children (game class,
 *     lobby and room of Play Now friends), type 'unavailable' when the
 *     user leaves.
 *
 *  Friendship here is always mutual: an accepted request makes both
 *  users friends (subscription 'both'), and removing a friend removes
 *  the friendship on both sides.  This matches the Arena menus, which
 *  only know friends and open requests.
 *
 *  Data file (one record per line, '|' separated, names without '|'):
 *
 *      F|alice|bob             alice and bob are friends (stored once)
 *      R|alice|bob|status      alice asked bob, not answered yet
 *      M|bob|alice|time|xml    message for bob from alice (the stanza,
 *                              newlines escaped as character references)
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#ifndef GCS_ROSTER_H
#define GCS_ROSTER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gcs_roster gcs_roster;

typedef void (*gcs_roster_log_fn)(void *ctx, int level, const char *line);

#define GCSR_MAX_NAME     32    /* user name, characters                */
#define GCSR_MAX_FRIENDS  200   /* friends per user                     */
#define GCSR_MAX_PAIRS    16384 /* friendships, all users               */
#define GCSR_MAX_REQUESTS 4096  /* open requests, all users             */
#define GCSR_MAX_STATUS   127   /* text of a request                    */
#define GCSR_MAX_MSGS     4096  /* offline messages, all users          */
#define GCSR_MAX_USERMSG  100   /* offline messages per user            */
#define GCSR_MAX_STANZA   1500  /* one offline message, bytes           */

/** file: NULL or "" keeps everything in memory only. */
gcs_roster *gcs_roster_create(const char *file, gcs_roster_log_fn log, void *ctx);
void gcs_roster_destroy(gcs_roster *r);

/** 1 if a and b are friends (names compare without case). */
int gcs_roster_are_friends(const gcs_roster *r, const char *a, const char *b);

/** Friends of user: calls fn for each (in the order they were added).
 *  Returns the number of friends. */
int gcs_roster_friends(const gcs_roster *r, const char *user, void (*fn)(void *ctx, const char *friend_name),
                       void *ctx);

/** Makes a and b friends; removes open requests between them.
 *  Returns 1 when new, 0 when they were friends already, -1 when a list
 *  or the store is full or a name is unusable. */
int gcs_roster_add_friends(gcs_roster *r, const char *a, const char *b);

/** Removes the friendship and every open request between a and b.
 *  Returns 1 if anything was removed. */
int gcs_roster_remove(gcs_roster *r, const char *a, const char *b);

/** from asks to: returns 1 for a new request, 0 when it was open
 *  already (status updated), -1 when full or unusable. */
int gcs_roster_request(gcs_roster *r, const char *from, const char *to, const char *status);

/** 1 if from has an open request to to; its status goes to status
 *  (cap >= GCSR_MAX_STATUS + 1) when not NULL. */
int gcs_roster_has_request(const gcs_roster *r, const char *from, const char *to, char *status);

/** Removes the request from -> to; 1 if it existed. */
int gcs_roster_drop_request(gcs_roster *r, const char *from, const char *to);

/** Open requests: to_user 1 lists the requests to user (fn gets the
 *  sender), 0 the requests user sent (fn gets the receiver). */
int gcs_roster_requests(const gcs_roster *r, const char *user, int to_user,
                        void (*fn)(void *ctx, const char *other, const char *status), void *ctx);

/** Keeps a message for to (offline).  stanza: the complete stanza as it
 *  will be delivered.  Returns 0, or -1 when full or too large (the
 *  oldest message of the user is dropped first when only his limit is
 *  reached). */
int gcs_roster_store_message(gcs_roster *r, const char *to, const char *from, long now, const char *stanza);

/** Hands the stored messages of user to fn (oldest first) and removes
 *  them.  Returns the number of messages. */
int gcs_roster_take_messages(gcs_roster *r, const char *user,
                             void (*fn)(void *ctx, const char *from, long time, const char *stanza), void *ctx);

/** Forgets everything about user (account deleted). */
void gcs_roster_forget(gcs_roster *r, const char *user);

typedef struct
{
    int friendships;
    int requests;
    int messages;
} gcs_roster_stats;
void gcs_roster_get_stats(const gcs_roster *r, gcs_roster_stats *st);

#ifdef __cplusplus
}
#endif

#endif /* GCS_ROSTER_H */
