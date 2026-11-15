/** @file gcs_habbo.h
 *
 *  Habbo Console back end for Habbo Islands (docs/HABBO.md).
 *
 *  Habbo Islands does not use N-Gage Arena.  Its only online feature is
 *  the Habbo Console: friends, messages and friend requests of a Habbo
 *  account.  The game talks HTTP 1.1 to port 8080 over an access point
 *  of the phone:
 *
 *   - GET  .../locator.html          server list, lines "id;name;base URL"
 *   - POST <base>AuthenticationServlet  user=NAME&pass=PASSWORD -> "0" ok
 *   - POST <base>MessengerServlet       m=ID[&p1=..][&p2=..]
 *
 *  The m values are the FUSE client message IDs of Habbo Hotel's own
 *  messenger (12 init, 15 update, 32 mark read, 33 send, 37 accept,
 *  38 decline, 39 request, 40 remove, 41 find; 0 is logout).  Short
 *  answers are "0" (done), "1" (failed) and "2" (not logged in: the game
 *  logs in again and repeats the request); the others are binary, Java
 *  DataOutputStream style: big-endian u16 counts, 64-bit ids and strings
 *  as u16 length + UTF-8 bytes.
 *
 *  Logins use the shared PhantomLink accounts (gcs_accounts.h): the
 *  same name and password as in the Arena games, created on the first
 *  login.  The game sends no IMEI, so a Habbo login neither checks nor
 *  makes the IMEI binding.  Users find each other by account name.
 *
 *  The game resolves builder.sulake.com, which the PhantomLink app maps
 *  to the server (or to the bridge) in C:\System\Data\hosts.  The
 *  locator answer points the game back to the same host and port.
 *
 *  Transport independent: gcs_native feeds complete HTTP requests of
 *  port GCSH_PORT (bridges and direct clients alike).
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#ifndef GCS_HABBO_H
#define GCS_HABBO_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct gcs_accounts;
typedef struct gcs_habbo gcs_habbo;

/** log line (level 0 errors .. 3 debug) */
typedef void (*gcs_habbo_log_fn)(void *ctx, int level, const char *line);

#define GCSH_PORT          8080 /* the game's TCP port                       */
#define GCSH_MAX_USERS     1024 /* console users (= accounts)                */
#define GCSH_MAX_FRIENDS   8192 /* friendships, all users                    */
#define GCSH_MAX_REQUESTS  2048 /* open friend requests, all users           */
#define GCSH_MAX_MESSAGES  4096 /* unread messages, all users                */
#define GCSH_MAX_INBOX     100  /* unread messages per user                  */
#define GCSH_MAX_TEXT      255  /* message text in bytes                     */
#define GCSH_MAX_LIST      200  /* friends per user                          */
#define GCSH_MAX_PER_REPLY 30   /* messages per init/update answer           */
#define GCSH_SESSIONS      256  /* logged in consoles                        */
#define GCSH_SESSION_S     3600 /* a session ends after this idle time (s)   */
#define GCSH_ONLINE_S      900  /* shown as online this long after a request */

/** acc: the shared accounts (not owned).  file: friends, requests and
 *  unread messages (NULL or "": memory only). */
gcs_habbo *gcs_habbo_create(struct gcs_accounts *acc, const char *file, gcs_habbo_log_fn log, void *ctx);
void gcs_habbo_destroy(gcs_habbo *h);

typedef struct
{
    int status;          /* 200, or 404 for unknown paths                  */
    const char *ctype;   /* Content-Type                                   */
    unsigned char *body; /* malloc'd (free it), NULL when empty            */
    size_t len;
    char cookie[48]; /* new session: value of Set-Cookie JSESSIONID    */
} gcs_habbo_reply;

/** One complete request.  key: the peer (bridge or direct client
 *  address), used when a request carries no session cookie; path: the
 *  request target; host: the Host header ("" none); sid: the JSESSIONID
 *  cookie ("" none); now: seconds since 1970.  Returns 0 and fills r
 *  (free r->body), -1 when out of memory. */
int gcs_habbo_http(gcs_habbo *h, void *key, const char *path, const char *host, const char *sid, const char *body,
                   size_t blen, long now, gcs_habbo_reply *r);

/** The peer is gone: its sessions stay valid (the cookie), but they are
 *  no longer the fallback for requests without one. */
void gcs_habbo_peer_gone(gcs_habbo *h, void *key);

typedef struct
{
    int users;    /* users that used the console          */
    int friends;  /* friendships                          */
    int requests; /* open friend requests                 */
    int messages; /* unread messages                      */
    int sessions; /* logged in consoles                   */
    unsigned long logins;
    unsigned long sent;
} gcs_habbo_stats;
void gcs_habbo_get_stats(const gcs_habbo *h, gcs_habbo_stats *st);

#ifdef __cplusplus
}
#endif

#endif /* GCS_HABBO_H */
