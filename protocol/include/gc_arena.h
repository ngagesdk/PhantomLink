/** @file gc_arena.h
 *
 *  GCA - N-Gage Arena messages.
 *
 *  Games built on the N-Gage Arena client (SEGA SNAP), such as Pocket
 *  Kingdom, do not use GameComms.  The PhantomLink Arena plug-ins
 *  (dll/arena: SnapComm.dll, sc_lib.dll, HttpDllRequest.dll) replace the
 *  network clients of that stack and carry their requests in these
 *  messages over the normal link (gc_link.h) to the server, which
 *  emulates the Arena back ends (server/src/gcs_arena.c):
 *
 *    HTTP_*          the Community web service (SOAP over HTTP)
 *    LOGIN, LIST,
 *    ENTER, LEAVE,
 *    MEMBER, DATA,
 *    STATUS          the SNAP game server: lobbies, rooms, room data
 *    CHAT, PRESENCE,
 *    SERVICE         SegaChat (Jabber): messages, presence, the
 *                    retrieval/reporter ranking services
 *
 *  The messages share one header byte (type) with the GCM messages of
 *  gc_msg.h; types 0x80..0x9F are reserved for GCA.  The bridge forwards
 *  them verbatim.  Integers are big endian, str8 is a u8 length plus
 *  UTF-8 bytes, data is a u16 length plus bytes.  Decoders ignore
 *  trailing bytes.
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#ifndef GC_ARENA_H
#define GC_ARENA_H

#include "gc_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* API family announced in REGISTER by the Arena plug-ins */
#define GC_API_ARENA 3

#define GCA_MT_FIRST    0x80
#define GCA_MT_LAST     0x9F
#define GCA_IS_ARENA(t) ((t) >= GCA_MT_FIRST && (t) <= GCA_MT_LAST)

/* Community web service (HTTP tunnel) */
#define GCA_MT_HTTP_REQ  0x80 /* dev -> srv */
#define GCA_MT_HTTP_RESP 0x81 /* srv -> dev */

/* SNAP game server */
#define GCA_MT_LOGIN        0x88 /* dev -> srv */
#define GCA_MT_LOGIN_RESULT 0x89 /* srv -> dev */
#define GCA_MT_LIST_REQ     0x8A /* dev -> srv */
#define GCA_MT_LIST         0x8B /* srv -> dev */
#define GCA_MT_ENTER        0x8C /* dev -> srv */
#define GCA_MT_ENTER_RESULT 0x8D /* srv -> dev */
#define GCA_MT_LEAVE        0x8E /* dev -> srv */
#define GCA_MT_MEMBER       0x8F /* srv -> dev */
#define GCA_MT_DATA         0x90 /* both */
#define GCA_MT_STATUS       0x91 /* both */
#define GCA_MT_LOGOUT       0x92 /* dev -> srv */

/* SegaChat */
#define GCA_MT_CHAT         0x98 /* both */
#define GCA_MT_PRESENCE     0x99 /* both */
#define GCA_MT_SERVICE      0x9A /* dev -> srv */
#define GCA_MT_SERVICE_RESP 0x9B /* srv -> dev */

/* limits */
#define GCA_MAX_NAME 32
#define GCA_MAX_TEXT 255
#define GCA_MAX_LIST 16   /* entries per LIST message */
#define GCA_MAX_DATA 1000 /* payload bytes per message */
/* HTTP bodies are sent in chunks of at most this size (flag MORE), so
 * that URL + SOAPAction + body always fit into one link message. */
#define GCA_HTTP_CHUNK 512

/* kinds */
#define GCA_K_LOBBY 0 /* LIST_REQ/ENTER/LEAVE/MEMBER/STATUS */
#define GCA_K_ROOM  1
#define GCA_K_USER  2 /* STATUS, DATA: private to one user */

#define GCA_HTTP_GET  0
#define GCA_HTTP_POST 1

#define GCA_CHAT_MESSAGE   0
#define GCA_CHAT_GROUPCHAT 1
#define GCA_CHAT_HEADLINE  2

#define GCA_PRES_AVAILABLE    0
#define GCA_PRES_UNAVAILABLE  1
#define GCA_PRES_SUBSCRIBE    2
#define GCA_PRES_SUBSCRIBED   3
#define GCA_PRES_UNSUBSCRIBE  4
#define GCA_PRES_UNSUBSCRIBED 5

/* flags */
#define GCA_F_MORE   0x01 /* HTTP_*, LIST: continued in the next message */
#define GCA_F_CREATE 0x02 /* ENTER: create the room if it does not exist */
#define GCA_F_JOINED 0x04 /* MEMBER: joined (otherwise left) */
#define GCA_F_HOST   0x08 /* ENTER_RESULT/MEMBER: room owner */

typedef struct
{
    gc_u32 id;
    gc_u16 users;
    gc_u16 max_users;
    gc_u32 status;
    char name[GCA_MAX_NAME + 1];
} gca_entry;

/** One GCA message.  Which fields are used depends on the type:
 *
 *  HTTP_REQ      req, kind=method, flags, text=URL, text2=SOAPAction, payload=body
 *  HTTP_RESP     req, value=HTTP status, flags, payload=body
 *  LOGIN         req, id=game class id, name=user, text=password
 *  LOGIN_RESULT  req, result, id=user id, name=user name
 *  LIST_REQ      req, kind, id=lobby (rooms)
 *  LIST          req, kind, flags, count, e[]
 *  ENTER         req, kind, flags, id (0: by name), value=max users, name
 *  ENTER_RESULT  req, result, kind, flags, id, name
 *  LEAVE         req, kind
 *  LOGOUT        req
 *  MEMBER        kind, flags, id=user, name
 *  DATA          kind, flags, id=destination user (dev->srv) or source user (srv->dev), payload
 *  STATUS        kind, id, value, mask
 *  CHAT          kind, name=peer (to or from), payload=text
 *  PRESENCE      kind, name=peer, text=status text
 *  SERVICE       req, name=service, text=request name, payload=XML
 *  SERVICE_RESP  req, result, payload=XML
 */
typedef struct
{
    gc_u8 type;
    gc_u16 req;
    gc_i8 result;
    gc_u8 kind;
    gc_u8 flags;
    gc_u32 id;
    gc_u32 value;
    gc_u32 mask;
    char name[GCA_MAX_NAME + 1];
    char text[GCA_MAX_TEXT + 1];
    char text2[GCA_MAX_TEXT + 1];
    gc_u16 len;
    const gc_u8 *payload; /* decode: points into the input buffer */
    gc_u8 count;
    gca_entry e[GCA_MAX_LIST];
} gca_msg;

/** Encode; returns the length or 0 (unknown type, buffer too small,
 *  payload too large). */
size_t gca_encode(const gca_msg *m, gc_u8 *buf, size_t cap);

/** Decode; returns 1 on success, 0 if malformed or not a GCA type. */
int gca_decode(const gc_u8 *buf, size_t len, gca_msg *m);

const char *gca_name(gc_u8 type);

#ifdef __cplusplus
}
#endif

#endif /* GC_ARENA_H */
