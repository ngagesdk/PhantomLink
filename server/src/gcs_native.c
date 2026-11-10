/** @file gcs_native.c
 *
 *  N-Gage Arena back ends in their native protocols, see gcs_native.h.
 *  The answer formats come from the parsers in the Arena DLLs
 *  (cafmlogin, cafmmenumanager, cafmcustomize, cafmplay; docs/ARENA.md).
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#include "gcs_native.h"
#include "gc_link.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_NAME  64
#define MAX_VALUE 128
#define MAX_PEERS 64

#define K_OTHER 0
#define K_HTTP  1
#define K_XMPP  2

/* profile fields, the order of the <item>s in the view answers */
static const char *const FIELDS[] = {
    "firstName", "lastName", "emailAddress",                                                  /* 0..2   */
    "displayFirstName", "displayLastName", "displayCity", "displayState", "displayCountry",   /* 3..7   */
    "gameReleaseNews", "tournamentNews", "ladderNews", "downloadNews", "activityNews",        /* 8..12  */
    "ngageNews", "sendEmail", "favGame", "favMusic", "favFood", "hobbies", "zodiac", "quote", /* 13..20 */
    "password", "question", "answer"                                                          /* 21..23 */
};
#define N_FIELDS   ((int)(sizeof(FIELDS) / sizeof(FIELDS[0])))
#define F_PASSWORD 21

typedef struct
{
    char name[MAX_NAME];
    char v[N_FIELDS][MAX_VALUE];
} account;

/* view operation -> edit operation, fields, kind */
#define V_STR   0
#define V_BOOL  1
#define V_MIXED 2 /* bool, string */
#define V_NIL   3 /* typed items */
#define V_QUOTE 4
typedef struct
{
    const char *view;
    const char *edit;
    int first, count, kind;
} view_op;

static const view_op VIEWS[] = {
    { "viewAccount", "editAccount", 0, 3, V_STR },
    { "viewDisplayOptions", "editDisplayOptions", 3, 5, V_BOOL },
    { "viewAlertSubscriptions", "editAlertSubscriptions", 8, 6, V_BOOL },
    { "viewAlertOptions", "editAlertOptions", 14, 2, V_MIXED }, /* sendEmail, emailAddress (field 2) */
    { "viewFavorites", "editFavorites", 15, 5, V_NIL },
    { "viewQuote", "editQuote", 20, 1, V_QUOTE },
};
#define N_VIEWS ((int)(sizeof(VIEWS) / sizeof(VIEWS[0])))

typedef struct
{
    int used;
    void *key;
    gc_u16 id;
    gc_u16 port;
    int kind;
    char *buf; /* pending input */
    size_t len;
    int peer_eof;
    int xmpp_started;
    char xmpp_user[MAX_NAME];
} conn;

typedef struct
{
    void *key;
    char user[MAX_NAME];
} peer_user;

struct gcs_native
{
    gcs_native_io io;
    void *ctx;
    char file[256];
    conn conns[GCSN_MAX_CONNS];
    peer_user peers[MAX_PEERS];
    account *acc;
    int n_acc;
    gcs_native_stats st;
};

/* ---- logging ---- */

static void nlog(gcs_native *n, int level, const char *fmt, ...)
{
    char line[600];
    va_list ap;
    size_t i;
    if (!n->io.log) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    for (i = 0; line[i]; i++) {
        if ((unsigned char)line[i] < 0x20 || line[i] == 0x7f) {
            line[i] = ' ';
        }
    }
    n->io.log(n->ctx, level, line);
}

#define LOG_W(...) nlog(n, 1, "native: " __VA_ARGS__)
#define LOG_I(...) nlog(n, 2, "native: " __VA_ARGS__)
#define LOG_D(...) nlog(n, 3, "native: " __VA_ARGS__)

static void copy_str(char *dst, const char *src, size_t cap)
{
    size_t k = strlen(src);
    if (k >= cap) {
        k = cap - 1;
    }
    memcpy(dst, src, k);
    dst[k] = 0;
}

/* ---- growing output buffer ---- */

typedef struct
{
    char *d;
    size_t len, cap;
    int oom;
} sbuf;

static void sb_add(sbuf *b, const char *s, size_t k)
{
    if (b->oom) {
        return;
    }
    if (b->len + k + 1 > b->cap) {
        size_t nc = (b->len + k + 1) * 2;
        char *nd = (char *)realloc(b->d, nc);
        if (!nd) {
            b->oom = 1;
            return;
        }
        b->d = nd;
        b->cap = nc;
    }
    memcpy(b->d + b->len, s, k);
    b->len += k;
    b->d[b->len] = 0;
}

static void sb_str(sbuf *b, const char *s)
{
    sb_add(b, s, strlen(s));
}

static void sb_fmt(sbuf *b, const char *fmt, ...)
{
    char tmp[1024];
    va_list ap;
    int k;
    va_start(ap, fmt);
    k = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (k > 0) {
        sb_add(b, tmp, (size_t)k < sizeof(tmp) ? (size_t)k : sizeof(tmp) - 1);
    }
}

static void sb_xml(sbuf *b, const char *s)
{
    for (; *s; s++) {
        switch (*s) {
        case '<':
            sb_str(b, "&lt;");
            break;
        case '>':
            sb_str(b, "&gt;");
            break;
        case '&':
            sb_str(b, "&amp;");
            break;
        case '"':
            sb_str(b, "&quot;");
            break;
        case '\'':
            sb_str(b, "&apos;");
            break;
        default:
            sb_add(b, s, 1);
        }
    }
}

/* ---- XML helpers (the client's XML is simple and generated) ---- */

static void xml_unescape(char *s)
{
    static const struct
    {
        const char *e;
        char c;
    } ents[] = { { "&lt;", '<' }, { "&gt;", '>' }, { "&amp;", '&' }, { "&quot;", '"' }, { "&apos;", '\'' } };
    char *w = s;
    while (*s) {
        int i, hit = 0;
        if (*s == '&') {
            for (i = 0; i < 5; i++) {
                size_t k = strlen(ents[i].e);
                if (!strncmp(s, ents[i].e, k)) {
                    *w++ = ents[i].c;
                    s += k;
                    hit = 1;
                    break;
                }
            }
        }
        if (!hit) {
            *w++ = *s++;
        }
    }
    *w = 0;
}

/* Text of the first element <name ...>text</name> (or <name/>) in xml;
 * 1 if found. */
static int xml_field(const char *xml, size_t len, const char *name, char *out, size_t cap)
{
    size_t nl = strlen(name), i;
    for (i = 0; i + nl + 2 <= len; i++) {
        const char *p = xml + i;
        const char *gt, *end, *e;
        char closing[80];
        size_t k;
        if (p[0] != '<' || strncmp(p + 1, name, nl) || (p[nl + 1] != ' ' && p[nl + 1] != '>' && p[nl + 1] != '/')) {
            continue;
        }
        end = xml + len;
        gt = (const char *)memchr(p, '>', (size_t)(end - p));
        if (!gt) {
            return 0;
        }
        if (gt[-1] == '/') {
            out[0] = 0;
            return 1;
        }
        snprintf(closing, sizeof(closing), "</%s>", name);
        for (e = gt + 1; e + strlen(closing) <= end; e++) {
            if (!strncmp(e, closing, strlen(closing))) {
                break;
            }
        }
        if (e + strlen(closing) > end) {
            return 0;
        }
        k = (size_t)(e - gt - 1);
        if (k >= cap) {
            k = cap - 1;
        }
        memcpy(out, gt + 1, k);
        out[k] = 0;
        xml_unescape(out);
        return 1;
    }
    return 0;
}

static const char *find_n(const char *hay, size_t len, const char *needle)
{
    size_t k = strlen(needle), i;
    if (k > len) {
        return NULL;
    }
    for (i = 0; i + k <= len; i++) {
        if (!memcmp(hay + i, needle, k)) {
            return hay + i;
        }
    }
    return NULL;
}

static int ieq_prefix(const char *s, const char *prefix)
{
    for (; *prefix; s++, prefix++) {
        char a = *s, b = *prefix;
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a + 32);
        }
        if (b >= 'A' && b <= 'Z') {
            b = (char)(b + 32);
        }
        if (a != b) {
            return 0;
        }
    }
    return 1;
}

/* ---- accounts ---- */

static void esc_out(FILE *f, const char *s)
{
    for (; *s; s++) {
        if (*s == '\\') {
            fputs("\\\\", f);
        } else if (*s == '\t') {
            fputs("\\t", f);
        } else if (*s == '\n') {
            fputs("\\n", f);
        } else if (*s == '\r') {
            fputs("\\r", f);
        } else {
            fputc(*s, f);
        }
    }
}

static void esc_in(char *s)
{
    char *w = s;
    while (*s) {
        if (*s == '\\' && s[1]) {
            s++;
            *w++ = *s == 't' ? '\t' : *s == 'n' ? '\n'
                                  : *s == 'r'   ? '\r'
                                                : *s;
            s++;
        } else {
            *w++ = *s++;
        }
    }
    *w = 0;
}

static account *acc_find(gcs_native *n, const char *user, int create)
{
    int i;
    account *a;
    for (i = 0; i < n->n_acc; i++) {
        if (!strcmp(n->acc[i].name, user)) {
            return &n->acc[i];
        }
    }
    if (!create || n->n_acc >= GCSN_MAX_USERS) {
        return NULL;
    }
    a = (account *)realloc(n->acc, sizeof(account) * (size_t)(n->n_acc + 1));
    if (!a) {
        return NULL;
    }
    n->acc = a;
    a = &n->acc[n->n_acc++];
    memset(a, 0, sizeof(*a));
    copy_str(a->name, user, sizeof(a->name));
    return a;
}

static void acc_save(gcs_native *n)
{
    char tmp[300];
    FILE *f;
    int i, k;
    if (!n->file[0]) {
        return;
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", n->file);
    f = fopen(tmp, "w");
    if (!f) {
        LOG_W("cannot write %s", tmp);
        return;
    }
    fputs("# PhantomLink Arena accounts: user<TAB>field<TAB>value\n", f);
    for (i = 0; i < n->n_acc; i++) {
        for (k = 0; k < N_FIELDS; k++) {
            if (n->acc[i].v[k][0]) {
                esc_out(f, n->acc[i].name);
                fprintf(f, "\t%s\t", FIELDS[k]);
                esc_out(f, n->acc[i].v[k]);
                fputc('\n', f);
            }
        }
    }
    if (fclose(f) != 0) {
        LOG_W("cannot write %s", tmp);
        return;
    }
    remove(n->file); /* rename does not replace on Windows */
    if (rename(tmp, n->file) != 0) {
        LOG_W("cannot replace %s", n->file);
    }
}

static void acc_load(gcs_native *n)
{
    char line[600];
    FILE *f = fopen(n->file, "r");
    if (!f) {
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        char *t1, *t2, *e;
        int k;
        account *a;
        if (line[0] == '#') {
            continue;
        }
        e = line + strlen(line);
        while (e > line && (e[-1] == '\n' || e[-1] == '\r')) {
            *--e = 0;
        }
        t1 = strchr(line, '\t');
        t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (!t2) {
            continue;
        }
        *t1 = 0;
        *t2 = 0;
        esc_in(line);
        esc_in(t2 + 1);
        for (k = 0; k < N_FIELDS; k++) {
            if (!strcmp(t1 + 1, FIELDS[k])) {
                a = acc_find(n, line, 1);
                if (a) {
                    copy_str(a->v[k], t2 + 1, MAX_VALUE);
                }
                break;
            }
        }
    }
    fclose(f);
    LOG_I("%d accounts loaded from %s", n->n_acc, n->file);
}

static const char *boolstr(const char *v)
{
    return (ieq_prefix(v, "true") || ieq_prefix(v, "yes") || !strcmp(v, "1")) ? "true" : "false";
}

/* ---- users per bridge connection (the client sends the name only at login) ---- */

static const char *peer_user_get(gcs_native *n, void *key)
{
    int i;
    for (i = 0; i < MAX_PEERS; i++) {
        if (n->peers[i].key == key) {
            return n->peers[i].user;
        }
    }
    return "player";
}

static void peer_user_set(gcs_native *n, void *key, const char *user)
{
    int i, free_i = -1;
    for (i = 0; i < MAX_PEERS; i++) {
        if (n->peers[i].key == key) {
            break;
        }
        if (!n->peers[i].key && free_i < 0) {
            free_i = i;
        }
    }
    if (i == MAX_PEERS) {
        i = free_i >= 0 ? free_i : 0;
    }
    n->peers[i].key = key;
    copy_str(n->peers[i].user, user, MAX_NAME);
}

/* session id = "PL" + hex of the user name, so it decodes without state */
static void session_id(const char *user, char *out, size_t cap)
{
    size_t k = 2, i;
    static const char hx[] = "0123456789ABCDEF";
    out[0] = 'P';
    out[1] = 'L';
    for (i = 0; user[i] && k + 3 < cap && i < 40; i++) {
        out[k++] = hx[(unsigned char)user[i] >> 4];
        out[k++] = hx[(unsigned char)user[i] & 15];
    }
    out[k] = 0;
}

static int session_user(const char *sid, char *out, size_t cap)
{
    size_t k = 0;
    if (sid[0] != 'P' || sid[1] != 'L') {
        return 0;
    }
    sid += 2;
    while (sid[0] && sid[1] && k + 1 < cap) {
        int hi, lo;
        hi = (sid[0] >= 'A') ? sid[0] - 'A' + 10 : sid[0] - '0';
        lo = (sid[1] >= 'A') ? sid[1] - 'A' + 10 : sid[1] - '0';
        if (hi < 0 || hi > 15 || lo < 0 || lo > 15) {
            break;
        }
        out[k++] = (char)(hi * 16 + lo);
        sid += 2;
    }
    out[k] = 0;
    return k > 0;
}

/* ---- tunnel output ---- */

static void send_net(gcs_native *n, void *key, const gcn_msg *m)
{
    gc_u8 buf[GC_LINK_MAX_MSG];
    size_t k = gcn_encode(m, buf, sizeof(buf));
    if (k) {
        n->io.send(n->ctx, key, buf, k);
    }
}

static void tcp_write(gcs_native *n, conn *c, const char *d, size_t len)
{
    gcn_msg m;
    size_t off;
    const size_t chunk = GCN_SRV_CHUNK;
    memset(&m, 0, sizeof(m));
    m.type = GCN_MT_TCP_DATA;
    m.id = c->id;
    for (off = 0; off < len; off += chunk) {
        m.data = (const gc_u8 *)d + off;
        m.len = len - off > chunk ? chunk : len - off;
        send_net(n, c->key, &m);
    }
}

static void conn_free(conn *c)
{
    free(c->buf);
    memset(c, 0, sizeof(*c));
}

static void tcp_close(gcs_native *n, conn *c, int how)
{
    gcn_msg m;
    memset(&m, 0, sizeof(m));
    m.type = GCN_MT_TCP_CLOSE;
    m.id = c->id;
    m.how = (gc_u8)how;
    send_net(n, c->key, &m);
    if (how == GCN_CLOSE_RESET || c->peer_eof) {
        conn_free(c);
    } else {
        c->kind = K_OTHER; /* half closed: ignore further input */
    }
}

/* ---- Community web service ---- */

#define SOAP_HEAD                                                                                             \
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"                                                              \
    "<soapenv:Envelope xmlns:soapenv=\"http://schemas.xmlsoap.org/soap/envelope/\" "                          \
    "xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">" \
    "<soapenv:Body><ns1:%sResponse soapenv:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\" "      \
    "xmlns:ns1=\"urn:CommunityApp\">"

static void items(sbuf *b, account *a, const view_op *v)
{
    int i;
    for (i = 0; i < v->count; i++) {
        int f = v->first + i;
        const char *val = a ? a->v[f] : "";
        if (v->kind == V_MIXED && i == 1) {
            val = a ? a->v[2] : ""; /* emailAddress */
        }
        if (v->kind == V_BOOL || (v->kind == V_MIXED && i == 0)) {
            sb_fmt(b, "<item>%s</item>", boolstr(val));
        } else if (v->kind == V_NIL) {
            sb_str(b, "<item xsi:type=\"xsd:string\">");
            sb_xml(b, val);
            sb_str(b, "</item>");
        } else {
            sb_str(b, "<item>");
            sb_xml(b, val);
            sb_str(b, "</item>");
        }
    }
    if (v->kind == V_NIL) {
        sb_str(b, "<item xsi:type=\"xsd:string\"></item>"); /* the parser reads six */
    }
}

static void soap_body(gcs_native *n, sbuf *b, const char *op, const char *user, const char *body, size_t blen)
{
    account *a = acc_find(n, user, 0);
    char val[MAX_VALUE];
    int i, k;

    if (!strcmp(op, "authenticateUser2") || !strcmp(op, "createUser")) {
        /* cafmlogin: "xsd:boolean" + "true", then the first "xsd:string"
         * after it is the Arena user name (Jabber login, URLs) */
        sb_fmt(b, "<%sReturn xsi:type=\"xsd:boolean\">true</%sReturn><username xsi:type=\"xsd:string\">", op, op);
        sb_xml(b, user);
        sb_str(b, "</username>");
        if (xml_field(body, blen, "password", val, sizeof(val))) {
            a = acc_find(n, user, 1);
            if (a && strcmp(a->v[F_PASSWORD], val)) {
                copy_str(a->v[F_PASSWORD], val, MAX_VALUE);
                acc_save(n);
            }
        }
        return;
    }
    if (!strcmp(op, "checkImei")) {
        sb_fmt(b, "<%sReturn xsi:type=\"xsd:boolean\">true</%sReturn>", op, op);
        return;
    }
    if (!strcmp(op, "getNewAlertStatus")) {
        sb_fmt(b, "<%sReturn>false</%sReturn>", op, op);
        return;
    }
    for (i = 0; i < N_VIEWS; i++) {
        const view_op *v = &VIEWS[i];
        if (!strcmp(op, v->view)) {
            if (v->kind == V_QUOTE) {
                sb_str(b, "<viewQuoteReturn xsi:type=\"xsd:string\">");
                sb_xml(b, a ? a->v[v->first] : "");
                sb_str(b, "</viewQuoteReturn>");
                return;
            }
            sb_fmt(b, "<%sReturn>", op);
            if (i == 0 && !a) {
                sb_str(b, "<item>");
                sb_xml(b, user);
                sb_str(b, "</item><item></item><item></item>");
            } else {
                items(b, a, v);
            }
            sb_fmt(b, "</%sReturn>", op);
            return;
        }
        if (!strcmp(op, v->edit)) {
            a = acc_find(n, user, 1);
            for (k = 0; a && k < v->count; k++) {
                int f = v->first + k;
                if (v->kind == V_MIXED && k == 1) {
                    f = 2;
                }
                if (xml_field(body, blen, FIELDS[f], val, sizeof(val))) {
                    copy_str(a->v[f], val, MAX_VALUE);
                }
            }
            acc_save(n);
            LOG_I("%s saved for '%s'", op, user);
            break;
        }
    }
    if (!strcmp(op, "changePassword") && xml_field(body, blen, "newPassword", val, sizeof(val))) {
        a = acc_find(n, user, 1);
        if (a) {
            copy_str(a->v[F_PASSWORD], val, MAX_VALUE);
            acc_save(n);
        }
    } else if (!strcmp(op, "getSecurityQuestion")) {
        sb_fmt(b, "<%sReturn xsi:type=\"xsd:string\">", op);
        sb_xml(b, a ? a->v[22] : "");
        sb_fmt(b, "</%sReturn>", op);
        return;
    } else if (!strcmp(op, "updateSecurityQA")) {
        a = acc_find(n, user, 1);
        if (a) {
            if (xml_field(body, blen, "question", val, sizeof(val))) {
                copy_str(a->v[22], val, MAX_VALUE);
            }
            if (xml_field(body, blen, "answer", val, sizeof(val))) {
                copy_str(a->v[23], val, MAX_VALUE);
            }
            acc_save(n);
        }
    }
    /* the edit, change, delete, unlink and update operations: errorCode 0 is success */
    sb_fmt(b, "<%sReturn><errorCode xsi:type=\"xsd:int\">0</errorCode></%sReturn>", op, op);
}

#define LIST_XML                                                                                          \
    "<?xml version=\"1.0\" encoding=\"utf-8\"?><list><currentStart value=\"0\"/><listTotal value=\"0\"/>" \
    "</list>"
#define PAGE_HTML                                                                                   \
    "<?xml version=\"1.0\" encoding=\"utf-8\"?><html xmlns=\"http://www.w3.org/1999/xhtml\"><head>" \
    "<title>N-Gage Arena</title></head><body><p>PhantomLink Arena emulator: nothing here yet.</p></body></html>"

static void http_answer(gcs_native *n, conn *c, const char *body, const char *ctype, const char *cookie)
{
    sbuf r;
    memset(&r, 0, sizeof(r));
    if (!body) {
        sb_str(&r, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    } else {
        sb_fmt(&r, "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n", ctype);
        if (cookie) {
            sb_fmt(&r, "Set-Cookie: JSESSIONID=%s; Path=/n-gage\r\n", cookie);
        }
        sb_fmt(&r, "Content-Length: %u\r\nConnection: close\r\n\r\n", (unsigned)strlen(body));
        sb_str(&r, body);
    }
    if (!r.oom && r.d) {
        tcp_write(n, c, r.d, r.len);
    }
    free(r.d);
    tcp_close(n, c, GCN_CLOSE_FIN);
}

/* 1 when one complete request was handled */
static int http_input(gcs_native *n, conn *c)
{
    const char *hend = find_n(c->buf, c->len, "\r\n\r\n");
    const char *p, *body;
    size_t hlen, clen = 0, blen;
    char first[512], path[512], cookie[160], user[MAX_NAME], op[64], sid[100];
    size_t k;

    if (!hend) {
        if (c->len >= GCSN_MAX_REQUEST) {
            tcp_close(n, c, GCN_CLOSE_RESET);
        }
        return 0;
    }
    hlen = (size_t)(hend - c->buf);
    cookie[0] = 0;
    for (p = c->buf; p < hend;) {
        const char *eol = find_n(p, (size_t)(hend - p) + 2, "\r\n");
        size_t ll = eol ? (size_t)(eol - p) : (size_t)(hend - p);
        if (ieq_prefix(p, "content-length:")) {
            clen = (size_t)strtoul(p + 15, NULL, 10);
        } else if (ieq_prefix(p, "cookie:")) {
            const char *j = find_n(p, ll, "JSESSIONID=");
            if (j) {
                j += 11;
                for (k = 0; j + k < p + ll && j[k] != ';' && j[k] != ' ' && k + 1 < sizeof(cookie); k++) {
                    cookie[k] = j[k];
                }
                cookie[k] = 0;
            }
        }
        p += ll + 2;
    }
    if (clen > GCSN_MAX_REQUEST) {
        tcp_close(n, c, GCN_CLOSE_RESET);
        return 0;
    }
    body = hend + 4;
    if ((size_t)(c->buf + c->len - body) < clen) {
        return 0; /* body incomplete */
    }
    blen = clen;
    n->st.http_requests++;

    k = 0;
    while (k < hlen && k + 1 < sizeof(first) && c->buf[k] != '\r') {
        first[k] = c->buf[k];
        k++;
    }
    first[k] = 0;
    path[0] = 0;
    p = strchr(first, ' ');
    if (p) {
        copy_str(path, p + 1, sizeof(path));
        if (strchr(path, ' ')) {
            *strchr(path, ' ') = 0;
        }
    }
    LOG_I("http: %s", first);

    if (strstr(path, "/services/Community")) {
        sbuf b;
        const char *o = find_n(body, blen, "<ns1:");
        k = 0;
        if (o) {
            o += 5;
            while (k + 1 < sizeof(op) && o < body + blen &&
                   ((*o >= 'a' && *o <= 'z') || (*o >= 'A' && *o <= 'Z') || (*o >= '0' && *o <= '9') || *o == '_')) {
                op[k++] = *o++;
            }
        }
        op[k] = 0;
        if (!op[0]) {
            copy_str(op, "unknown", sizeof(op));
        }
        if ((!strcmp(op, "authenticateUser2") || !strcmp(op, "createUser")) &&
            xml_field(body, blen, "username", user, sizeof(user)) && user[0]) {
            peer_user_set(n, c->key, user);
        } else if (!cookie[0] || !session_user(cookie, user, sizeof(user))) {
            copy_str(user, peer_user_get(n, c->key), sizeof(user));
        }
        session_id(user, sid, sizeof(sid));
        LOG_I("soap: %s ('%s')", op, user);
        memset(&b, 0, sizeof(b));
        sb_fmt(&b, SOAP_HEAD, op);
        soap_body(n, &b, op, user, body, blen);
        sb_fmt(&b, "</ns1:%sResponse></soapenv:Body></soapenv:Envelope>", op);
        http_answer(n, c, b.oom ? NULL : b.d, "text/xml; charset=utf-8", sid);
        free(b.d);
        return 1;
    }
    {
        char base[512];
        size_t bl;
        copy_str(base, path, sizeof(base));
        base[strcspn(base, ";?")] = 0;
        bl = strlen(base);
        if (bl >= 6 && !strcmp(base + bl - 6, "_x.jsp")) {
            http_answer(n, c, LIST_XML, "text/xml; charset=utf-8", NULL);
        } else if ((bl >= 4 && !strcmp(base + bl - 4, ".jsp")) || strstr(base, "/servlets/")) {
            http_answer(n, c, PAGE_HTML, "application/xhtml+xml; charset=utf-8", NULL);
        } else {
            http_answer(n, c, NULL, NULL, NULL);
        }
    }
    return 1;
}

/* ---- SegaChat (Jabber) ---- */

static void xmpp_send(gcs_native *n, conn *c, const char *s)
{
    LOG_D("xmpp -> %s", s);
    tcp_write(n, c, s, strlen(s));
}

static void attr(const char *tag, size_t len, const char *name, char *out, size_t cap)
{
    size_t nl = strlen(name), i;
    out[0] = 0;
    for (i = 0; i + nl + 2 < len; i++) {
        if ((i == 0 || tag[i - 1] == ' ') && !strncmp(tag + i, name, nl) && tag[i + nl] == '=' &&
            (tag[i + nl + 1] == '\'' || tag[i + nl + 1] == '"')) {
            char q = tag[i + nl + 1];
            size_t k = 0, j = i + nl + 2;
            while (j < len && tag[j] != q && k + 1 < cap) {
                out[k++] = tag[j++];
            }
            out[k] = 0;
            return;
        }
    }
}

static void xmpp_stanza(gcs_native *n, conn *c, const char *s, size_t len, size_t taglen)
{
    char id[64], type[16], idattr[80], user[MAX_NAME], ns[96], out[400];
    if (len < 3 || strncmp(s, "<iq", 3)) {
        return; /* presence and messages: nobody else is online yet */
    }
    attr(s + 1, taglen, "id", id, sizeof(id));
    attr(s + 1, taglen, "type", type, sizeof(type));
    idattr[0] = 0;
    if (id[0]) {
        snprintf(idattr, sizeof(idattr), " id='%s'", id);
    }
    if (find_n(s, len, "jabber:iq:auth") && !strcmp(type, "get")) {
        snprintf(out, sizeof(out),
                 "<iq type='result'%s><query xmlns='jabber:iq:auth'><username/><password/><resource/></query></iq>",
                 idattr);
        xmpp_send(n, c, out);
    } else if (find_n(s, len, "jabber:iq:auth") && !strcmp(type, "set")) {
        if (xml_field(s, len, "username", user, sizeof(user))) {
            copy_str(c->xmpp_user, user, sizeof(c->xmpp_user));
        }
        n->st.xmpp_logins++;
        LOG_I("xmpp: login '%s'", c->xmpp_user);
        snprintf(out, sizeof(out), "<iq type='result'%s/>", idattr);
        xmpp_send(n, c, out);
    } else if (find_n(s, len, "jabber:iq:roster") && !strcmp(type, "get")) {
        snprintf(out, sizeof(out), "<iq type='result'%s><query xmlns='jabber:iq:roster'/></iq>", idattr);
        xmpp_send(n, c, out);
    } else if (!strcmp(type, "get") || !strcmp(type, "set")) {
        const char *q = find_n(s, len, "<query xmlns=");
        ns[0] = 0;
        if (q) {
            attr(q + 7, (size_t)(s + len - q - 7), "xmlns", ns, sizeof(ns));
        }
        if (ns[0]) {
            snprintf(out, sizeof(out), "<iq type='result'%s><query xmlns='%s'/></iq>", idattr, ns);
        } else {
            snprintf(out, sizeof(out), "<iq type='result'%s/>", idattr);
        }
        xmpp_send(n, c, out);
    }
}

static void xmpp_input(gcs_native *n, conn *c)
{
    size_t pos = 0;
    if (!c->xmpp_started) {
        const char *st = find_n(c->buf, c->len, "<stream:stream");
        const char *gt;
        if (!st) {
            if (c->len >= GCSN_MAX_REQUEST) {
                tcp_close(n, c, GCN_CLOSE_RESET);
            }
            return;
        }
        gt = (const char *)memchr(st, '>', (size_t)(c->buf + c->len - st));
        if (!gt) {
            return;
        }
        c->xmpp_started = 1;
        xmpp_send(n, c,
                  "<?xml version='1.0'?><stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
                  "xmlns='jabber:client' from='ngage-auth' id='pl0001'>");
        pos = (size_t)(gt + 1 - c->buf);
    }
    for (;;) {
        const char *s, *gt, *end = c->buf + c->len;
        size_t taglen, slen;
        char tag[16], closing[24];
        size_t k = 0;
        while (pos < c->len && (c->buf[pos] == ' ' || c->buf[pos] == '\r' || c->buf[pos] == '\n' || c->buf[pos] == '\t')) {
            pos++;
        }
        s = c->buf + pos;
        if (pos >= c->len || *s != '<') {
            break;
        }
        if (find_n(s, (size_t)(end - s), "</stream:stream>") == s) {
            xmpp_send(n, c, "</stream:stream>");
            pos = c->len;
            tcp_close(n, c, GCN_CLOSE_FIN);
            return;
        }
        gt = (const char *)memchr(s, '>', (size_t)(end - s));
        if (!gt) {
            break;
        }
        while (k + 1 < sizeof(tag) && s + 1 + k < gt && s[1 + k] != ' ' && s[1 + k] != '/' && s[1 + k] != '>') {
            tag[k] = s[1 + k];
            k++;
        }
        tag[k] = 0;
        taglen = (size_t)(gt - s - 1);
        if (gt[-1] == '/') {
            slen = (size_t)(gt + 1 - s);
        } else {
            const char *e;
            snprintf(closing, sizeof(closing), "</%s>", tag);
            e = find_n(gt, (size_t)(end - gt), closing);
            if (!e) {
                break; /* incomplete */
            }
            slen = (size_t)(e + strlen(closing) - s);
        }
        xmpp_stanza(n, c, s, slen, taglen);
        if (!c->used) {
            return;
        }
        pos += slen;
    }
    if (pos) {
        memmove(c->buf, c->buf + pos, c->len - pos);
        c->len -= pos;
    }
    if (c->len >= GCSN_MAX_REQUEST) {
        tcp_close(n, c, GCN_CLOSE_RESET);
    }
}

/* ---- dispatch ---- */

static conn *conn_find(gcs_native *n, void *key, gc_u16 id)
{
    int i;
    for (i = 0; i < GCSN_MAX_CONNS; i++) {
        if (n->conns[i].used && n->conns[i].key == key && n->conns[i].id == id) {
            return &n->conns[i];
        }
    }
    return NULL;
}

static void tcp_open(gcs_native *n, void *key, const gcn_msg *m)
{
    int i;
    conn *c = conn_find(n, key, m->id);
    gcn_msg r;
    if (c) {
        conn_free(c); /* the bridge reused the id: the old one is gone */
    }
    for (i = 0; i < GCSN_MAX_CONNS && n->conns[i].used; i++) {
    }
    if (i == GCSN_MAX_CONNS || (m->port != 80 && m->port != 5222)) {
        LOG_W("tcp %u.%u.%u.%u:%u refused", m->addr[0], m->addr[1], m->addr[2], m->addr[3], m->port);
        memset(&r, 0, sizeof(r));
        r.type = GCN_MT_TCP_CLOSE;
        r.id = m->id;
        r.how = GCN_CLOSE_RESET;
        send_net(n, key, &r);
        return;
    }
    c = &n->conns[i];
    memset(c, 0, sizeof(*c));
    c->used = 1;
    c->key = key;
    c->id = m->id;
    c->port = m->port;
    c->kind = m->port == 80 ? K_HTTP : K_XMPP;
    LOG_D("tcp %04x open to port %u", m->id, m->port);
}

static void tcp_data(gcs_native *n, conn *c, const gcn_msg *m)
{
    if (c->kind == K_OTHER) {
        return;
    }
    if (!c->buf) {
        c->buf = (char *)malloc(GCSN_MAX_REQUEST + 1);
        if (!c->buf) {
            tcp_close(n, c, GCN_CLOSE_RESET);
            return;
        }
    }
    if (c->len + m->len > GCSN_MAX_REQUEST) {
        LOG_W("tcp %04x: request too large", c->id);
        tcp_close(n, c, GCN_CLOSE_RESET);
        return;
    }
    memcpy(c->buf + c->len, m->data, m->len);
    c->len += m->len;
    c->buf[c->len] = 0;
    if (c->kind == K_HTTP) {
        http_input(n, c);
    } else {
        xmpp_input(n, c);
    }
}

static void udp_in(gcs_native *n, const gcn_msg *m)
{
    char hex[3 * 48 + 1];
    size_t i, k = 0;
    n->st.udp_datagrams++;
    for (i = 0; i < m->len && i < 48; i++) {
        k += (size_t)snprintf(hex + k, sizeof(hex) - k, "%02x ", m->data[i]);
    }
    hex[k] = 0;
    LOG_I("udp to %u.%u.%u.%u:%u from port %u, %u bytes: %s%s", m->addr[0], m->addr[1], m->addr[2], m->addr[3],
          m->port, m->peer_port, (unsigned)m->len, hex, m->len > 48 ? "..." : "");
}

void gcs_native_input(gcs_native *n, void *key, const gc_u8 *data, size_t len)
{
    gcn_msg m;
    conn *c;
    if (!gcn_decode(data, len, &m)) {
        LOG_W("malformed tunnel message (type %02x)", len ? data[0] : 0);
        return;
    }
    switch (m.type) {
    case GCN_MT_TCP_OPEN:
        tcp_open(n, key, &m);
        break;
    case GCN_MT_TCP_DATA:
        c = conn_find(n, key, m.id);
        if (c) {
            tcp_data(n, c, &m);
        }
        break;
    case GCN_MT_TCP_CLOSE:
        c = conn_find(n, key, m.id);
        if (!c) {
            break;
        }
        if (m.how == GCN_CLOSE_RESET || c->kind == K_OTHER) {
            conn_free(c); /* reset, or we closed already */
        } else {
            c->peer_eof = 1;
            if (c->kind == K_XMPP) {
                tcp_close(n, c, GCN_CLOSE_FIN);
            }
        }
        break;
    case GCN_MT_UDP:
        udp_in(n, &m);
        break;
    default:
        break;
    }
}

void gcs_native_peer_gone(gcs_native *n, void *key)
{
    int i;
    for (i = 0; i < GCSN_MAX_CONNS; i++) {
        if (n->conns[i].used && n->conns[i].key == key) {
            conn_free(&n->conns[i]);
        }
    }
    for (i = 0; i < MAX_PEERS; i++) {
        if (n->peers[i].key == key) {
            n->peers[i].key = NULL;
        }
    }
}

gcs_native *gcs_native_create(const gcs_native_io *io, void *ctx, const char *accounts_file)
{
    gcs_native *n = (gcs_native *)calloc(1, sizeof(*n));
    if (!n) {
        return NULL;
    }
    n->io = *io;
    n->ctx = ctx;
    if (accounts_file) {
        copy_str(n->file, accounts_file, sizeof(n->file));
    }
    if (n->file[0]) {
        acc_load(n);
    }
    return n;
}

void gcs_native_destroy(gcs_native *n)
{
    int i;
    if (!n) {
        return;
    }
    for (i = 0; i < GCSN_MAX_CONNS; i++) {
        free(n->conns[i].buf);
    }
    free(n->acc);
    free(n);
}

void gcs_native_get_stats(const gcs_native *n, gcs_native_stats *st)
{
    int i;
    *st = n->st;
    st->conns = 0;
    for (i = 0; i < GCSN_MAX_CONNS; i++) {
        st->conns += n->conns[i].used;
    }
    st->users = n->n_acc;
}
