/** @file gcs_arena.c
 *
 *  N-Gage Arena back end emulation (see gcs_arena.h).
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#include "gcs_arena.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct gcsa_lobby gcsa_lobby;
typedef struct gcsa_room gcsa_room;

typedef struct
{
    void *key; /* NULL: free entry */
    int logged_in;
    gc_u32 id;
    gc_u32 game; /* game class id from LOGIN */
    char name[GCA_MAX_NAME + 1];
    gc_u32 status; /* user status value (STATUS kind USER) */
    int present;   /* sent PRESENCE available */
    gcsa_lobby *lobby;
    gcsa_room *room;
    /* HTTP request being assembled */
    gc_u16 http_req;
    int http_active;
    gc_u8 http_method;
    char http_url[GCA_MAX_TEXT + 1];
    char http_action[GCA_MAX_TEXT + 1];
    size_t http_len;
    char http_body[GCSA_MAX_HTTP + 1];
} gcsa_user;

struct gcsa_lobby
{
    int used;
    gc_u32 id;
    gc_u32 game;
    char name[GCA_MAX_NAME + 1];
};

struct gcsa_room
{
    int used;
    gc_u32 id;
    gcsa_lobby *lobby;
    char name[GCA_MAX_NAME + 1];
    gc_u16 max_users;
    gc_u32 status;
    gcsa_user *owner;
    gcsa_user *m[GCSA_MAX_MEMBERS];
};

struct gcs_arena
{
    gcs_arena_io io;
    void *ctx;
    gc_u32 next_id;
    gcsa_user users[GCSA_MAX_USERS];
    gcsa_lobby lobbies[GCSA_MAX_LOBBIES];
    gcsa_room rooms[GCSA_MAX_ROOMS];
    gcs_arena_game games[GCSA_MAX_GAMES];
    int n_games;
};

/* ------------------------------------------------------------------ */

static void logf_(gcs_arena *a, int level, const char *fmt, ...)
{
    char line[512];
    va_list ap;
    size_t i;

    if (!a->io.log) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    for (i = 0; line[i]; i++) {
        if ((unsigned char)line[i] < 0x20 || line[i] == 0x7f) {
            line[i] = '?';
        }
    }
    a->io.log(a->ctx, level, line);
}

#define LOG_W(...) logf_(a, 1, "arena: " __VA_ARGS__)
#define LOG_I(...) logf_(a, 2, "arena: " __VA_ARGS__)
#define LOG_D(...) logf_(a, 3, "arena: " __VA_ARGS__)

static void copy_str(char *dst, const char *src, size_t cap)
{
    size_t n = strlen(src);
    if (n > cap - 1) {
        n = cap - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int name_eq(const char *a, const char *b)
{
    for (;;) {
        char x = *a++, y = *b++;
        if (x >= 'A' && x <= 'Z') {
            x = (char)(x - 'A' + 'a');
        }
        if (y >= 'A' && y <= 'Z') {
            y = (char)(y - 'A' + 'a');
        }
        if (x != y) {
            return 0;
        }
        if (!x) {
            return 1;
        }
    }
}

static void send_msg(gcs_arena *a, gcsa_user *u, const gca_msg *m)
{
    gc_u8 buf[1100];
    size_t n = gca_encode(m, buf, sizeof(buf));
    if (!n) {
        LOG_W("cannot encode %s for '%s'", gca_name(m->type), u->name);
        return;
    }
    a->io.send(a->ctx, u->key, buf, n);
}

static gcsa_user *user_find(gcs_arena *a, void *key)
{
    int i;
    for (i = 0; i < GCSA_MAX_USERS; i++) {
        if (a->users[i].key == key) {
            return &a->users[i];
        }
    }
    return NULL;
}

static gcsa_user *user_get(gcs_arena *a, void *key, const char *name)
{
    gcsa_user *u = user_find(a, key);
    if (!u) {
        u = user_find(a, NULL);
        if (!u) {
            return NULL;
        }
        memset(u, 0, sizeof(*u));
        u->key = key;
        copy_str(u->name, name && name[0] ? name : "N-Gage", sizeof(u->name));
    }
    return u;
}

static gcsa_user *user_by_name(gcs_arena *a, gc_u32 game, const char *name)
{
    int i;
    for (i = 0; i < GCSA_MAX_USERS; i++) {
        gcsa_user *u = &a->users[i];
        if (u->key && u->logged_in && u->game == game && name_eq(u->name, name)) {
            return u;
        }
    }
    return NULL;
}

static gcsa_user *user_by_id(gcs_arena *a, gc_u32 id)
{
    int i;
    for (i = 0; i < GCSA_MAX_USERS; i++) {
        if (a->users[i].key && a->users[i].logged_in && a->users[i].id == id) {
            return &a->users[i];
        }
    }
    return NULL;
}

static int room_count(const gcsa_room *r)
{
    int i, n = 0;
    for (i = 0; i < GCSA_MAX_MEMBERS; i++) {
        n += r->m[i] != NULL;
    }
    return n;
}

static int lobby_users(gcs_arena *a, const gcsa_lobby *l)
{
    int i, n = 0;
    for (i = 0; i < GCSA_MAX_USERS; i++) {
        n += a->users[i].key && a->users[i].lobby == l;
    }
    return n;
}

static void member_event(gcs_arena *a, gcsa_user *to, gc_u8 kind, const gcsa_user *who, int joined, int host)
{
    gca_msg m;
    memset(&m, 0, sizeof(m));
    m.type = GCA_MT_MEMBER;
    m.kind = kind;
    m.flags = (gc_u8)((joined ? GCA_F_JOINED : 0) | (host ? GCA_F_HOST : 0));
    m.id = who->id;
    copy_str(m.name, who->name, sizeof(m.name));
    send_msg(a, to, &m);
}

static const gcs_arena_game *game_of(const gcs_arena *a, gc_u32 game)
{
    int i;
    for (i = 0; i < a->n_games; i++) {
        if (a->games[i].game == game) {
            return &a->games[i];
        }
    }
    return NULL;
}

/* ---- rooms ---------------------------------------------------------- */

static void room_leave(gcs_arena *a, gcsa_user *u)
{
    gcsa_room *r = u->room;
    int i;

    if (!r) {
        return;
    }
    u->room = NULL;
    for (i = 0; i < GCSA_MAX_MEMBERS; i++) {
        if (r->m[i] == u) {
            r->m[i] = NULL;
        }
    }
    if (r->owner == u) {
        r->owner = NULL;
        for (i = 0; i < GCSA_MAX_MEMBERS && !r->owner; i++) {
            r->owner = r->m[i];
        }
    }
    for (i = 0; i < GCSA_MAX_MEMBERS; i++) {
        if (r->m[i]) {
            member_event(a, r->m[i], GCA_K_ROOM, u, 0, 0);
            if (r->owner) {
                member_event(a, r->m[i], GCA_K_ROOM, r->owner, 1, 1);
            }
        }
    }
    if (!room_count(r)) {
        LOG_I("room %08x '%s' closed", r->id, r->name);
        r->used = 0;
    }
}

static void lobby_leave(gcs_arena *a, gcsa_user *u)
{
    gcsa_lobby *l = u->lobby;
    int i;

    room_leave(a, u);
    if (!l) {
        return;
    }
    u->lobby = NULL;
    for (i = 0; i < GCSA_MAX_USERS; i++) {
        if (a->users[i].key && a->users[i].lobby == l) {
            member_event(a, &a->users[i], GCA_K_LOBBY, u, 0, 0);
        }
    }
}

static gcsa_lobby *lobby_find(gcs_arena *a, gc_u32 game, gc_u32 id, const char *name)
{
    int i;
    for (i = 0; i < GCSA_MAX_LOBBIES; i++) {
        gcsa_lobby *l = &a->lobbies[i];
        if (l->used && l->game == game && (id ? l->id == id : name_eq(l->name, name))) {
            return l;
        }
    }
    return NULL;
}

static gcsa_lobby *lobby_create(gcs_arena *a, gc_u32 game, const char *name)
{
    int i;
    for (i = 0; i < GCSA_MAX_LOBBIES; i++) {
        gcsa_lobby *l = &a->lobbies[i];
        if (!l->used) {
            memset(l, 0, sizeof(*l));
            l->used = 1;
            l->id = ++a->next_id;
            l->game = game;
            copy_str(l->name, name[0] ? name : "Lobby", sizeof(l->name));
            LOG_I("lobby %08x '%s' created for game %08x", l->id, l->name, game);
            return l;
        }
    }
    return NULL;
}

static gcsa_room *room_find(gcs_arena *a, gcsa_lobby *l, gc_u32 id, const char *name)
{
    int i;
    for (i = 0; i < GCSA_MAX_ROOMS; i++) {
        gcsa_room *r = &a->rooms[i];
        if (r->used && r->lobby == l && (id ? r->id == id : name_eq(r->name, name))) {
            return r;
        }
    }
    return NULL;
}

/* ---- handlers ------------------------------------------------------- */

static void reply_result(gcs_arena *a, gcsa_user *u, gc_u8 type, gc_u16 req, gc_i8 result)
{
    gca_msg m;
    memset(&m, 0, sizeof(m));
    m.type = type;
    m.req = req;
    m.result = result;
    send_msg(a, u, &m);
}

static void h_login(gcs_arena *a, gcsa_user *u, const gca_msg *in)
{
    gca_msg m;
    const char *name = in->name[0] ? in->name : u->name;
    gcsa_user *other = user_by_name(a, in->id, name);

    memset(&m, 0, sizeof(m));
    m.type = GCA_MT_LOGIN_RESULT;
    m.req = in->req;
    if (other && other != u) {
        m.result = -5; /* GC_R_REJECTED: name in use */
        send_msg(a, u, &m);
        return;
    }
    if (u->logged_in && u->game != in->id) {
        lobby_leave(a, u);
    }
    if (!u->logged_in) {
        u->id = ++a->next_id;
    }
    u->logged_in = 1;
    u->game = in->id;
    copy_str(u->name, name, sizeof(u->name));
    {
        const gcs_arena_game *g = game_of(a, u->game);
        LOG_I("user %08x '%s' logged in (game class %08x%s%s)", u->id, u->name, u->game, g ? ", " : "",
              g ? g->name : "");
        m.id = u->id;
        copy_str(m.name, u->name, sizeof(m.name));
        send_msg(a, u, &m);
        if (g && g->on_login) {
            g->on_login(g->ctx, a, u->id, u->name);
        }
    }
}

static void h_list(gcs_arena *a, gcsa_user *u, const gca_msg *in)
{
    gca_msg m;
    int i;
    gcsa_lobby *l = NULL;

    memset(&m, 0, sizeof(m));
    m.type = GCA_MT_LIST;
    m.req = in->req;
    m.kind = in->kind;
    if (in->kind == GCA_K_ROOM) {
        l = in->id ? lobby_find(a, u->game, in->id, "") : u->lobby;
        if (!l) {
            send_msg(a, u, &m); /* empty */
            return;
        }
    }
    for (i = 0; i < (in->kind == GCA_K_ROOM ? GCSA_MAX_ROOMS : GCSA_MAX_LOBBIES); i++) {
        gca_entry *e;
        if (m.count == GCA_MAX_LIST) {
            m.flags = GCA_F_MORE;
            send_msg(a, u, &m);
            m.count = 0;
            m.flags = 0;
        }
        e = &m.e[m.count];
        if (in->kind == GCA_K_ROOM) {
            gcsa_room *r = &a->rooms[i];
            if (!r->used || r->lobby != l) {
                continue;
            }
            e->id = r->id;
            e->users = (gc_u16)room_count(r);
            e->max_users = r->max_users;
            e->status = r->status;
            copy_str(e->name, r->name, sizeof(e->name));
        } else {
            gcsa_lobby *lb = &a->lobbies[i];
            if (!lb->used || lb->game != u->game) {
                continue;
            }
            e->id = lb->id;
            e->users = (gc_u16)lobby_users(a, lb);
            e->max_users = GCSA_MAX_USERS;
            copy_str(e->name, lb->name, sizeof(e->name));
        }
        m.count++;
    }
    send_msg(a, u, &m);
}

static void enter_result(gcs_arena *a, gcsa_user *u, const gca_msg *in, gc_i8 res, gc_u32 id, const char *name,
                         int host)
{
    gca_msg m;
    memset(&m, 0, sizeof(m));
    m.type = GCA_MT_ENTER_RESULT;
    m.req = in->req;
    m.kind = in->kind;
    m.result = res;
    m.id = id;
    m.flags = (gc_u8)(host ? GCA_F_HOST : 0);
    if (name) {
        copy_str(m.name, name, sizeof(m.name));
    }
    send_msg(a, u, &m);
}

static void h_enter_lobby(gcs_arena *a, gcsa_user *u, const gca_msg *in)
{
    gcsa_lobby *l = lobby_find(a, u->game, in->id, in->name);
    int i;

    if (!l && !in->id) {
        l = lobby_create(a, u->game, in->name);
    }
    if (!l) {
        enter_result(a, u, in, in->id ? -1 : -11, 0, NULL, 0);
        return;
    }
    if (u->lobby != l) {
        lobby_leave(a, u);
        u->lobby = l;
        for (i = 0; i < GCSA_MAX_USERS; i++) {
            gcsa_user *o = &a->users[i];
            if (o->key && o != u && o->lobby == l) {
                member_event(a, o, GCA_K_LOBBY, u, 1, 0);
            }
        }
    }
    enter_result(a, u, in, 0, l->id, l->name, 0);
    for (i = 0; i < GCSA_MAX_USERS; i++) {
        gcsa_user *o = &a->users[i];
        if (o->key && o != u && o->lobby == l) {
            member_event(a, u, GCA_K_LOBBY, o, 1, 0);
        }
    }
}

static void h_enter_room(gcs_arena *a, gcsa_user *u, const gca_msg *in)
{
    gcsa_room *r;
    int i, slot = -1;

    if (!u->lobby) {
        enter_result(a, u, in, -10, 0, NULL, 0); /* GC_R_NOT_MEMBER */
        return;
    }
    r = room_find(a, u->lobby, in->id, in->name);
    if (!r && !in->id && (in->flags & GCA_F_CREATE) && in->name[0]) {
        for (i = 0; i < GCSA_MAX_ROOMS && !r; i++) {
            if (!a->rooms[i].used) {
                r = &a->rooms[i];
                memset(r, 0, sizeof(*r));
                r->used = 1;
                r->id = ++a->next_id;
                r->lobby = u->lobby;
                copy_str(r->name, in->name, sizeof(r->name));
                r->max_users = (gc_u16)(in->value && in->value <= GCSA_MAX_MEMBERS ? in->value : GCSA_MAX_MEMBERS);
                LOG_I("room %08x '%s' created by '%s' (max %u)", r->id, r->name, u->name, r->max_users);
            }
        }
        if (!r) {
            enter_result(a, u, in, -11, 0, NULL, 0); /* GC_R_LIMIT */
            return;
        }
    }
    if (!r) {
        enter_result(a, u, in, -1, 0, NULL, 0); /* GC_R_NOT_FOUND */
        return;
    }
    if (u->room == r) {
        enter_result(a, u, in, 0, r->id, r->name, r->owner == u);
        return;
    }
    for (i = 0; i < GCSA_MAX_MEMBERS; i++) {
        if (!r->m[i] && slot < 0 && i < r->max_users) {
            slot = i;
        }
    }
    if (slot < 0 || room_count(r) >= r->max_users) {
        enter_result(a, u, in, -2, r->id, r->name, 0); /* GC_R_FULL */
        return;
    }
    room_leave(a, u);
    r->m[slot] = u;
    u->room = r;
    if (!r->owner) {
        r->owner = u;
    }
    enter_result(a, u, in, 0, r->id, r->name, r->owner == u);
    for (i = 0; i < GCSA_MAX_MEMBERS; i++) {
        gcsa_user *o = r->m[i];
        if (o && o != u) {
            member_event(a, o, GCA_K_ROOM, u, 1, 0);
            member_event(a, u, GCA_K_ROOM, o, 1, r->owner == o);
        }
    }
    if (r->status) {
        gca_msg m;
        memset(&m, 0, sizeof(m));
        m.type = GCA_MT_STATUS;
        m.kind = GCA_K_ROOM;
        m.id = r->id;
        m.value = r->status;
        m.mask = 0xFFFFFFFFu;
        send_msg(a, u, &m);
    }
}

static void h_data(gcs_arena *a, gcsa_user *u, const gca_msg *in)
{
    gca_msg m = *in;
    int i;
    const gcs_arena_game *g = game_of(a, u->game);

    if (g && g->on_data && g->on_data(g->ctx, a, u->id, in)) {
        return;
    }

    m.id = u->id;
    if (in->kind == GCA_K_USER) {
        gcsa_user *to = user_by_id(a, in->id);
        if (!to || to->game != u->game) {
            LOG_D("data from '%s' to unknown user %08x dropped", u->name, in->id);
            return;
        }
        send_msg(a, to, &m);
        return;
    }
    if (!u->room) {
        return;
    }
    for (i = 0; i < GCSA_MAX_MEMBERS; i++) {
        gcsa_user *o = u->room->m[i];
        if (o && o != u && (!in->id || o->id == in->id)) {
            send_msg(a, o, &m);
        }
    }
}

static void h_status(gcs_arena *a, gcsa_user *u, const gca_msg *in)
{
    gca_msg m = *in;
    gcsa_room *r = u->room;
    int i;

    if (in->kind == GCA_K_ROOM) {
        if (!r || r->owner != u) {
            return;
        }
        r->status = (r->status & ~in->mask) | (in->value & in->mask);
        m.id = r->id;
        m.value = r->status;
    } else {
        u->status = (u->status & ~in->mask) | (in->value & in->mask);
        m.kind = GCA_K_USER;
        m.id = u->id;
        m.value = u->status;
    }
    if (!r) {
        return;
    }
    for (i = 0; i < GCSA_MAX_MEMBERS; i++) {
        if (r->m[i] && r->m[i] != u) {
            send_msg(a, r->m[i], &m);
        }
    }
}

static void h_chat(gcs_arena *a, gcsa_user *u, const gca_msg *in)
{
    gca_msg m = *in;
    int i;

    copy_str(m.name, u->name, sizeof(m.name));
    if (in->kind == GCA_CHAT_GROUPCHAT) {
        if (!u->room) {
            return;
        }
        for (i = 0; i < GCSA_MAX_MEMBERS; i++) {
            if (u->room->m[i] && u->room->m[i] != u) {
                send_msg(a, u->room->m[i], &m);
            }
        }
    } else {
        gcsa_user *to = user_by_name(a, u->game, in->name);
        if (to) {
            send_msg(a, to, &m);
        } else {
            LOG_D("chat from '%s' to unknown '%s' dropped", u->name, in->name);
        }
    }
}

static void h_presence(gcs_arena *a, gcsa_user *u, const gca_msg *in)
{
    gca_msg m;
    int i;

    memset(&m, 0, sizeof(m));
    m.type = GCA_MT_PRESENCE;
    if (in->kind == GCA_PRES_SUBSCRIBE) {
        /* no rosters yet: every subscription is accepted at once */
        gcsa_user *to = user_by_name(a, u->game, in->name);
        m.kind = GCA_PRES_SUBSCRIBED;
        copy_str(m.name, in->name, sizeof(m.name));
        send_msg(a, u, &m);
        if (to && to->present) {
            m.kind = GCA_PRES_AVAILABLE;
            send_msg(a, u, &m);
        }
        return;
    }
    if (in->kind != GCA_PRES_AVAILABLE && in->kind != GCA_PRES_UNAVAILABLE) {
        return;
    }
    m.kind = in->kind;
    copy_str(m.name, u->name, sizeof(m.name));
    copy_str(m.text, in->text, sizeof(m.text));
    for (i = 0; i < GCSA_MAX_USERS; i++) {
        gcsa_user *o = &a->users[i];
        if (o->key && o != u && o->logged_in && o->game == u->game) {
            send_msg(a, o, &m);
        }
    }
    if (in->kind == GCA_PRES_AVAILABLE && !u->present) {
        /* tell the newcomer who is online */
        for (i = 0; i < GCSA_MAX_USERS; i++) {
            gcsa_user *o = &a->users[i];
            if (o->key && o != u && o->present && o->game == u->game) {
                gca_msg p;
                memset(&p, 0, sizeof(p));
                p.type = GCA_MT_PRESENCE;
                p.kind = GCA_PRES_AVAILABLE;
                copy_str(p.name, o->name, sizeof(p.name));
                send_msg(a, u, &p);
            }
        }
    }
    u->present = in->kind == GCA_PRES_AVAILABLE;
}

static void h_service(gcs_arena *a, gcsa_user *u, const gca_msg *in)
{
    char xml[GCA_MAX_DATA + 1];
    const gcs_arena_game *g = game_of(a, u->game);

    if (g && g->on_service && g->on_service(g->ctx, a, u->id, in)) {
        return;
    }
    memcpy(xml, in->payload, in->len);
    xml[in->len] = '\0';
    LOG_I("service request from '%s' to '%s' (%s): %s", u->name, in->name, in->text, xml);
    reply_result(a, u, GCA_MT_SERVICE_RESP, in->req, 0);
}

/* ---- Community web service ----------------------------------------- */

static void http_reply(gcs_arena *a, gcsa_user *u, gc_u16 req, gc_u16 status, const char *body)
{
    gca_msg m;
    size_t len = strlen(body), off = 0;

    do {
        size_t n = len - off > GCA_HTTP_CHUNK ? GCA_HTTP_CHUNK : len - off;
        memset(&m, 0, sizeof(m));
        m.type = GCA_MT_HTTP_RESP;
        m.req = req;
        m.value = status;
        m.flags = (gc_u8)(off + n < len ? GCA_F_MORE : 0);
        m.payload = (const gc_u8 *)body + off;
        m.len = (gc_u16)n;
        send_msg(a, u, &m);
        off += n;
    } while (off < len);
}

static const char *find_op(const char *body, const char *action)
{
    static const char *const ops[] = { "authenticateUser2", "checkImei", "createUser" };
    size_t i;
    for (i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
        if (strstr(action, ops[i]) || strstr(body, ops[i])) {
            return ops[i];
        }
    }
    return NULL;
}

static void http_complete(gcs_arena *a, gcsa_user *u)
{
    char out[1024];
    const char *op;

    u->http_body[u->http_len] = '\0';
    LOG_I("http %s %s from '%s' (%u bytes, SOAPAction '%s')", u->http_method == GCA_HTTP_POST ? "POST" : "GET",
          u->http_url, u->name, (unsigned)u->http_len, u->http_action);
    LOG_D("http body: %s", u->http_body);
    op = strstr(u->http_url, "/services/Community") ? find_op(u->http_body, u->http_action) : NULL;
    if (!op) {
        http_reply(a, u, u->http_req, 404, "");
        return;
    }
    /* The response layout follows Apache Axis 1.x rpc/encoded; the
     * return types are inferred from the strings in arenabase.dll
     * (authenticateUser2Return, checkImeiReturn, xsd:boolean). */
    snprintf(out, sizeof(out),
             "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
             "<soapenv:Envelope xmlns:soapenv=\"http://schemas.xmlsoap.org/soap/envelope/\" "
             "xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\" "
             "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">"
             "<soapenv:Body><ns1:%sResponse "
             "soapenv:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\" xmlns:ns1=\"Community\">"
             "<%sReturn xsi:type=\"xsd:boolean\">true</%sReturn>"
             "</ns1:%sResponse></soapenv:Body></soapenv:Envelope>",
             op, op, op, op);
    http_reply(a, u, u->http_req, 200, out);
}

static void h_http(gcs_arena *a, gcsa_user *u, const gca_msg *in)
{
    if (!u->http_active || u->http_req != in->req) {
        u->http_active = 1;
        u->http_req = in->req;
        u->http_method = in->kind;
        u->http_len = 0;
        copy_str(u->http_url, in->text, sizeof(u->http_url));
        copy_str(u->http_action, in->text2, sizeof(u->http_action));
    }
    if (u->http_len + in->len > GCSA_MAX_HTTP) {
        u->http_active = 0;
        http_reply(a, u, in->req, 413, "");
        return;
    }
    memcpy(u->http_body + u->http_len, in->payload, in->len);
    u->http_len += in->len;
    if (!(in->flags & GCA_F_MORE)) {
        u->http_active = 0;
        http_complete(a, u);
    }
}

/* ------------------------------------------------------------------ */

gcs_arena *gcs_arena_create(const gcs_arena_io *io, void *ctx)
{
    gcs_arena *a = (gcs_arena *)calloc(1, sizeof(*a));
    if (a) {
        a->io = *io;
        a->ctx = ctx;
        a->next_id = 0x1000;
    }
    return a;
}

void gcs_arena_destroy(gcs_arena *a)
{
    free(a);
}

void gcs_arena_input(gcs_arena *a, void *key, const char *name, const gc_u8 *data, size_t len)
{
    gca_msg in;
    gcsa_user *u;

    if (!gca_decode(data, len, &in)) {
        LOG_W("malformed GCA message (type %02x, %u bytes)", len ? data[0] : 0, (unsigned)len);
        return;
    }
    u = user_get(a, key, name);
    if (!u) {
        LOG_W("too many Arena users");
        return;
    }
    LOG_D("'%s' -> %s", u->name, gca_name(in.type));
    if (in.type == GCA_MT_HTTP_REQ) {
        h_http(a, u, &in);
        return;
    }
    if (in.type == GCA_MT_LOGIN) {
        h_login(a, u, &in);
        return;
    }
    if (!u->logged_in) {
        reply_result(a, u, in.type == GCA_MT_SERVICE ? GCA_MT_SERVICE_RESP : GCA_MT_LOGIN_RESULT, in.req, -6);
        return;
    }
    switch (in.type) {
    case GCA_MT_LIST_REQ:
        h_list(a, u, &in);
        break;
    case GCA_MT_ENTER:
        if (in.kind == GCA_K_LOBBY) {
            h_enter_lobby(a, u, &in);
        } else {
            h_enter_room(a, u, &in);
        }
        break;
    case GCA_MT_LEAVE:
        if (in.kind == GCA_K_LOBBY) {
            lobby_leave(a, u);
        } else {
            room_leave(a, u);
        }
        break;
    case GCA_MT_LOGOUT:
    {
        const gcs_arena_game *g = game_of(a, u->game);
        lobby_leave(a, u);
        u->logged_in = 0;
        if (g && g->on_logout) {
            g->on_logout(g->ctx, a, u->id);
        }
        break;
    }
    case GCA_MT_DATA:
        h_data(a, u, &in);
        break;
    case GCA_MT_STATUS:
        h_status(a, u, &in);
        break;
    case GCA_MT_CHAT:
        h_chat(a, u, &in);
        break;
    case GCA_MT_PRESENCE:
        h_presence(a, u, &in);
        break;
    case GCA_MT_SERVICE:
        h_service(a, u, &in);
        break;
    default:
        LOG_W("unexpected %s from '%s'", gca_name(in.type), u->name);
        break;
    }
}

void gcs_arena_peer_gone(gcs_arena *a, void *key)
{
    gcsa_user *u = user_find(a, key);
    int i;

    if (!u || !key) {
        return;
    }
    if (u->present) {
        for (i = 0; i < GCSA_MAX_USERS; i++) {
            gcsa_user *o = &a->users[i];
            if (o->key && o != u && o->logged_in && o->game == u->game) {
                gca_msg m;
                memset(&m, 0, sizeof(m));
                m.type = GCA_MT_PRESENCE;
                m.kind = GCA_PRES_UNAVAILABLE;
                copy_str(m.name, u->name, sizeof(m.name));
                send_msg(a, o, &m);
            }
        }
    }
    lobby_leave(a, u);
    if (u->logged_in) {
        const gcs_arena_game *g = game_of(a, u->game);
        if (g && g->on_logout) {
            g->on_logout(g->ctx, a, u->id);
        }
    }
    LOG_I("user '%s' gone", u->name);
    memset(u, 0, sizeof(*u));
}

int gcs_arena_add_game(gcs_arena *a, const gcs_arena_game *g)
{
    if (a->n_games >= GCSA_MAX_GAMES || game_of(a, g->game)) {
        return 0;
    }
    a->games[a->n_games++] = *g;
    return 1;
}

int gcs_arena_send_to(gcs_arena *a, gc_u32 user, const gca_msg *m)
{
    gcsa_user *u = user_by_id(a, user);
    if (!u) {
        return 0;
    }
    send_msg(a, u, m);
    return 1;
}

void gcs_arena_get_stats(const gcs_arena *a, gcs_arena_stats *st)
{
    int i;
    memset(st, 0, sizeof(*st));
    for (i = 0; i < GCSA_MAX_USERS; i++) {
        st->users += a->users[i].key != NULL;
    }
    for (i = 0; i < GCSA_MAX_LOBBIES; i++) {
        st->lobbies += a->lobbies[i].used;
    }
    for (i = 0; i < GCSA_MAX_ROOMS; i++) {
        st->rooms += a->rooms[i].used;
    }
}
