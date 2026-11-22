/** @file gcs_ssx.h
 *
 *  Online service of SSX Out of Bounds (6R65, UID 0x101fd402), docs/SSX.md.
 *
 *  SSX belongs to the first generation of N-Gage Arena games (2003/04).
 *  It has none of the later Arena DLLs: its menu code talks over IPC to
 *  AIRPLAYSERVER.EXE (Ideaworks3D, on the game card), which speaks its own
 *  reliable, encrypted UDP protocol:
 *
 *   - discovery.cng.n-gage.com, UDP port 41001: every datagram is one
 *     message, 8-byte header (u32 sequence, u8 type, u8 "no ack", u16
 *     0x00ff | 0x8000 when encrypted), little endian integers, strings as
 *     u32 length + bytes.  Reliable messages are acknowledged (type 0xfa
 *     + u32 sequence) and resent after 2 s.
 *   - key exchange: 18 -> 133 (RSA modulus and exponent as decimal
 *     strings) -> 19 (128-bit session key, RSA PKCS#1 v1.5) -> 134.  After
 *     that messages are two-key triple DES CBC: IV (u32 8 + 8 bytes),
 *     u32 plain length, cipher text of plain + 8 bytes MD5 or SHA-1 +
 *     zero padding + one block.
 *   - discovery (50 -> 150): the game sends the MD5 of
 *     C:\System\Libs\abtesrv.dll, the "billing DLL".  When it differs from
 *     PhantomLink's, the server sends its own (51 -> 121 chunks); the game
 *     loads it with RLibrary and asks it for the game server's host and
 *     port.  PhantomLink's abtesrv.dll (dll/ssx) names this same server.
 *   - accounts: logon (0), create (16), change password (13), question
 *     (14), password (15) and user name by IMEI (17), on the shared
 *     PhantomLink accounts (gcs_accounts.h).
 *   - an object tree the game browses (9 -> 124): the game knows the menu
 *     ids (2000100 root, 2000109 Shadow Racing, 2000110/120/130 the peaks,
 *     2000150 My Career, 2000160 View Career, ...) and draws the lists
 *     from the entries (name, value, date).
 *   - uploads (6 -> 122, chunks 8 -> 123): code 10121 career (the
 *     0xdc-byte career record; values: completion, earnings), 10122
 *     shadow (an *_best.ghz file; values: time in ms or score, track).
 *   - downloads (3 -> 121 chunks): objects of type 10122 + track (a
 *     shadow) and 1001 (a career).
 *
 *  Unlocking the Fresh Gear items needs nothing from the server: the game
 *  checks its own records once the Arena menu is reached.
 *
 *  Transport independent: gcs_native feeds the datagrams of UDP port
 *  GCSX_PORT (bridges and direct clients alike).
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#ifndef GCS_SSX_H
#define GCS_SSX_H

#include "gc_types.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct gcs_accounts;
typedef struct gcs_ssx gcs_ssx;

#define GCSX_PORT       41001  /* discovery and game server, UDP            */
#define GCSX_SESSIONS   64     /* game sessions (address + port)            */
#define GCSX_TRACKS     15     /* shadow racing tracks                      */
#define GCSX_MAX_FILE   262144 /* largest upload (shadow or career)         */
#define GCSX_MAX_PLUGIN 262144 /* largest billing DLL                       */
#define GCSX_LIST_MAX   50     /* entries of a ranking list                 */
#define GCSX_IDLE_MS    600000 /* a session is forgotten after this         */
#define GCSX_RESEND_MS  2500   /* reliable messages are resent after this   */
#define GCSX_TRIES      12     /* ... this often, then the session ends     */
#define GCSX_WINDOW     8      /* reliable messages in flight per session   */
#define GCSX_RSA_BITS   1024

typedef struct
{
    /** send a datagram to addr:port (the game's server side end point is
     *  addr:port, its own port peer_port) */
    void (*send)(void *ctx, void *key, const gc_u8 addr[4], gc_u16 port, gc_u16 peer_port, const gc_u8 *d,
                 size_t len);
    /** log line (level 0 errors .. 3 debug) */
    void (*log)(void *ctx, int level, const char *line);
    /** 32 random bits (keys, IVs); NULL: a weak built-in generator */
    gc_u32 (*random)(void *ctx);
} gcs_ssx_io;

/** acc: the shared accounts (not owned).  file: uploaded shadows and
 *  careers (NULL or "": memory only). */
gcs_ssx *gcs_ssx_create(const gcs_ssx_io *io, void *ctx, struct gcs_accounts *acc, const char *file);
void gcs_ssx_destroy(gcs_ssx *s);

/** The billing DLL handed to the games (PhantomLink's abtesrv.dll):
 *  returns 0, or -1 when the file cannot be read (then games keep the one
 *  they have).  NULL or "": none. */
int gcs_ssx_set_plugin(gcs_ssx *s, const char *path);
/** Same from memory (tests). */
int gcs_ssx_set_plugin_data(gcs_ssx *s, const gc_u8 *data, size_t len);

/** A datagram from a game to addr:port (port GCSX_PORT) from its port
 *  peer_port; key identifies the bridge or direct client. */
void gcs_ssx_input(gcs_ssx *s, void *key, const gc_u8 addr[4], gc_u16 port, gc_u16 peer_port, const gc_u8 *d,
                   size_t len, gc_u32 now);
/** Resends and time outs; call often (every 100 ms is plenty). */
void gcs_ssx_poll(gcs_ssx *s, gc_u32 now);
/** The bridge or direct client key is gone. */
void gcs_ssx_peer_gone(gcs_ssx *s, void *key);

/** Fixed wall clock (seconds since 1970) for the tests; 0: real time. */
void gcs_ssx_set_time(gcs_ssx *s, long t);

typedef struct
{
    int sessions;
    int shadows; /* stored shadows, all tracks */
    int careers;
    gc_u32 datagrams;
    gc_u32 bad_datagrams; /* too short, not decryptable */
} gcs_ssx_stats;
void gcs_ssx_get_stats(const gcs_ssx *s, gcs_ssx_stats *st);

#ifdef __cplusplus
}
#endif

#endif /* GCS_SSX_H */
