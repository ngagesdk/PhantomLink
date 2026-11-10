/** @file gc_arena.c
 *
 *  GCA - N-Gage Arena messages (see gc_arena.h).
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#include "gc_arena.h"
#include <string.h>

static void w_str(gc_writer *w, const char *s, size_t max)
{
    gc_w_str8(w, s, max);
}

static void w_data(gc_writer *w, const gca_msg *m)
{
    if (m->len > GCA_MAX_DATA || (m->len && !m->payload)) {
        w->overflow = 1;
        return;
    }
    gc_w_u16(w, m->len);
    gc_w_bytes(w, m->payload, m->len);
}

static void r_data(gc_reader *r, gca_msg *m)
{
    m->len = gc_r_u16(r);
    if (m->len > GCA_MAX_DATA) {
        r->error = 1;
        m->len = 0;
        return;
    }
    m->payload = gc_r_bytes(r, m->len);
}

size_t gca_encode(const gca_msg *m, gc_u8 *buf, size_t cap)
{
    gc_writer w;
    gc_u8 i;

    gc_w_init(&w, buf, cap);
    gc_w_u8(&w, m->type);
    switch (m->type) {
    case GCA_MT_HTTP_REQ:
        gc_w_u16(&w, m->req);
        gc_w_u8(&w, m->kind);
        gc_w_u8(&w, m->flags);
        w_str(&w, m->text, GCA_MAX_TEXT);
        w_str(&w, m->text2, GCA_MAX_TEXT);
        w_data(&w, m);
        break;
    case GCA_MT_HTTP_RESP:
        gc_w_u16(&w, m->req);
        gc_w_u16(&w, (gc_u16)m->value);
        gc_w_u8(&w, m->flags);
        w_data(&w, m);
        break;
    case GCA_MT_LOGIN:
        gc_w_u16(&w, m->req);
        gc_w_u32(&w, m->id);
        w_str(&w, m->name, GCA_MAX_NAME);
        w_str(&w, m->text, GCA_MAX_TEXT);
        break;
    case GCA_MT_LOGIN_RESULT:
        gc_w_u16(&w, m->req);
        gc_w_u8(&w, (gc_u8)m->result);
        gc_w_u32(&w, m->id);
        w_str(&w, m->name, GCA_MAX_NAME);
        break;
    case GCA_MT_LIST_REQ:
        gc_w_u16(&w, m->req);
        gc_w_u8(&w, m->kind);
        gc_w_u32(&w, m->id);
        break;
    case GCA_MT_LIST:
        if (m->count > GCA_MAX_LIST) {
            return 0;
        }
        gc_w_u16(&w, m->req);
        gc_w_u8(&w, m->kind);
        gc_w_u8(&w, m->flags);
        gc_w_u8(&w, m->count);
        for (i = 0; i < m->count; i++) {
            gc_w_u32(&w, m->e[i].id);
            gc_w_u16(&w, m->e[i].users);
            gc_w_u16(&w, m->e[i].max_users);
            gc_w_u32(&w, m->e[i].status);
            w_str(&w, m->e[i].name, GCA_MAX_NAME);
        }
        break;
    case GCA_MT_ENTER:
        gc_w_u16(&w, m->req);
        gc_w_u8(&w, m->kind);
        gc_w_u8(&w, m->flags);
        gc_w_u32(&w, m->id);
        gc_w_u32(&w, m->value);
        w_str(&w, m->name, GCA_MAX_NAME);
        break;
    case GCA_MT_ENTER_RESULT:
        gc_w_u16(&w, m->req);
        gc_w_u8(&w, (gc_u8)m->result);
        gc_w_u8(&w, m->kind);
        gc_w_u8(&w, m->flags);
        gc_w_u32(&w, m->id);
        w_str(&w, m->name, GCA_MAX_NAME);
        break;
    case GCA_MT_LEAVE:
        gc_w_u16(&w, m->req);
        gc_w_u8(&w, m->kind);
        break;
    case GCA_MT_LOGOUT:
        gc_w_u16(&w, m->req);
        break;
    case GCA_MT_MEMBER:
        gc_w_u8(&w, m->kind);
        gc_w_u8(&w, m->flags);
        gc_w_u32(&w, m->id);
        w_str(&w, m->name, GCA_MAX_NAME);
        break;
    case GCA_MT_DATA:
        gc_w_u8(&w, m->kind);
        gc_w_u8(&w, m->flags);
        gc_w_u32(&w, m->id);
        w_data(&w, m);
        break;
    case GCA_MT_STATUS:
        gc_w_u8(&w, m->kind);
        gc_w_u32(&w, m->id);
        gc_w_u32(&w, m->value);
        gc_w_u32(&w, m->mask);
        break;
    case GCA_MT_CHAT:
        gc_w_u8(&w, m->kind);
        w_str(&w, m->name, GCA_MAX_NAME);
        w_data(&w, m);
        break;
    case GCA_MT_PRESENCE:
        gc_w_u8(&w, m->kind);
        w_str(&w, m->name, GCA_MAX_NAME);
        w_str(&w, m->text, GCA_MAX_TEXT);
        break;
    case GCA_MT_SERVICE:
        gc_w_u16(&w, m->req);
        w_str(&w, m->name, GCA_MAX_NAME);
        w_str(&w, m->text, GCA_MAX_TEXT);
        w_data(&w, m);
        break;
    case GCA_MT_SERVICE_RESP:
        gc_w_u16(&w, m->req);
        gc_w_u8(&w, (gc_u8)m->result);
        w_data(&w, m);
        break;
    default:
        return 0;
    }
    return w.overflow ? 0 : w.len;
}

int gca_decode(const gc_u8 *buf, size_t len, gca_msg *m)
{
    gc_reader r;
    gc_u8 i;

    memset(m, 0, sizeof(*m));
    gc_r_init(&r, buf, len);
    m->type = gc_r_u8(&r);
    switch (m->type) {
    case GCA_MT_HTTP_REQ:
        m->req = gc_r_u16(&r);
        m->kind = gc_r_u8(&r);
        m->flags = gc_r_u8(&r);
        gc_r_str8(&r, m->text, sizeof(m->text));
        gc_r_str8(&r, m->text2, sizeof(m->text2));
        r_data(&r, m);
        break;
    case GCA_MT_HTTP_RESP:
        m->req = gc_r_u16(&r);
        m->value = gc_r_u16(&r);
        m->flags = gc_r_u8(&r);
        r_data(&r, m);
        break;
    case GCA_MT_LOGIN:
        m->req = gc_r_u16(&r);
        m->id = gc_r_u32(&r);
        gc_r_str8(&r, m->name, sizeof(m->name));
        gc_r_str8(&r, m->text, sizeof(m->text));
        break;
    case GCA_MT_LOGIN_RESULT:
        m->req = gc_r_u16(&r);
        m->result = (gc_i8)gc_r_u8(&r);
        m->id = gc_r_u32(&r);
        gc_r_str8(&r, m->name, sizeof(m->name));
        break;
    case GCA_MT_LIST_REQ:
        m->req = gc_r_u16(&r);
        m->kind = gc_r_u8(&r);
        m->id = gc_r_u32(&r);
        break;
    case GCA_MT_LIST:
        m->req = gc_r_u16(&r);
        m->kind = gc_r_u8(&r);
        m->flags = gc_r_u8(&r);
        m->count = gc_r_u8(&r);
        if (m->count > GCA_MAX_LIST) {
            return 0;
        }
        for (i = 0; i < m->count; i++) {
            m->e[i].id = gc_r_u32(&r);
            m->e[i].users = gc_r_u16(&r);
            m->e[i].max_users = gc_r_u16(&r);
            m->e[i].status = gc_r_u32(&r);
            gc_r_str8(&r, m->e[i].name, sizeof(m->e[i].name));
        }
        break;
    case GCA_MT_ENTER:
        m->req = gc_r_u16(&r);
        m->kind = gc_r_u8(&r);
        m->flags = gc_r_u8(&r);
        m->id = gc_r_u32(&r);
        m->value = gc_r_u32(&r);
        gc_r_str8(&r, m->name, sizeof(m->name));
        break;
    case GCA_MT_ENTER_RESULT:
        m->req = gc_r_u16(&r);
        m->result = (gc_i8)gc_r_u8(&r);
        m->kind = gc_r_u8(&r);
        m->flags = gc_r_u8(&r);
        m->id = gc_r_u32(&r);
        gc_r_str8(&r, m->name, sizeof(m->name));
        break;
    case GCA_MT_LEAVE:
        m->req = gc_r_u16(&r);
        m->kind = gc_r_u8(&r);
        break;
    case GCA_MT_LOGOUT:
        m->req = gc_r_u16(&r);
        break;
    case GCA_MT_MEMBER:
        m->kind = gc_r_u8(&r);
        m->flags = gc_r_u8(&r);
        m->id = gc_r_u32(&r);
        gc_r_str8(&r, m->name, sizeof(m->name));
        break;
    case GCA_MT_DATA:
        m->kind = gc_r_u8(&r);
        m->flags = gc_r_u8(&r);
        m->id = gc_r_u32(&r);
        r_data(&r, m);
        break;
    case GCA_MT_STATUS:
        m->kind = gc_r_u8(&r);
        m->id = gc_r_u32(&r);
        m->value = gc_r_u32(&r);
        m->mask = gc_r_u32(&r);
        break;
    case GCA_MT_CHAT:
        m->kind = gc_r_u8(&r);
        gc_r_str8(&r, m->name, sizeof(m->name));
        r_data(&r, m);
        break;
    case GCA_MT_PRESENCE:
        m->kind = gc_r_u8(&r);
        gc_r_str8(&r, m->name, sizeof(m->name));
        gc_r_str8(&r, m->text, sizeof(m->text));
        break;
    case GCA_MT_SERVICE:
        m->req = gc_r_u16(&r);
        gc_r_str8(&r, m->name, sizeof(m->name));
        gc_r_str8(&r, m->text, sizeof(m->text));
        r_data(&r, m);
        break;
    case GCA_MT_SERVICE_RESP:
        m->req = gc_r_u16(&r);
        m->result = (gc_i8)gc_r_u8(&r);
        r_data(&r, m);
        break;
    default:
        return 0;
    }
    return !r.error;
}

const char *gca_name(gc_u8 type)
{
    switch (type) {
    case GCA_MT_HTTP_REQ:
        return "HTTP_REQ";
    case GCA_MT_HTTP_RESP:
        return "HTTP_RESP";
    case GCA_MT_LOGIN:
        return "LOGIN";
    case GCA_MT_LOGIN_RESULT:
        return "LOGIN_RESULT";
    case GCA_MT_LIST_REQ:
        return "LIST_REQ";
    case GCA_MT_LIST:
        return "LIST";
    case GCA_MT_ENTER:
        return "ENTER";
    case GCA_MT_ENTER_RESULT:
        return "ENTER_RESULT";
    case GCA_MT_LEAVE:
        return "LEAVE";
    case GCA_MT_LOGOUT:
        return "LOGOUT";
    case GCA_MT_MEMBER:
        return "MEMBER";
    case GCA_MT_DATA:
        return "ARENA_DATA";
    case GCA_MT_STATUS:
        return "STATUS";
    case GCA_MT_CHAT:
        return "CHAT";
    case GCA_MT_PRESENCE:
        return "PRESENCE";
    case GCA_MT_SERVICE:
        return "SERVICE";
    case GCA_MT_SERVICE_RESP:
        return "SERVICE_RESP";
    default:
        return "?";
    }
}
