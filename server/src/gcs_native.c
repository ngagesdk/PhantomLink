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
#include "gcs_accounts.h"
#include "gcs_rifts.h"
#include "gcs_snap.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#if defined(_WIN32) && !defined(S_ISDIR)
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif

#define MAX_NAME  64
#define MAX_PEERS 256 /* (peer, user) logins: bridges and direct clients */

#define K_OTHER 0
#define K_HTTP  1
#define K_XMPP  2

/* profile fields, the order of the <item>s in the view answers; kept as
 * account fields under these names (gcs_accounts) */
static const char *const FIELDS[] = {
    "firstName", "lastName", "emailAddress",                                                  /* 0..2   */
    "displayFirstName", "displayLastName", "displayCity", "displayState", "displayCountry",   /* 3..7   */
    "gameReleaseNews", "tournamentNews", "ladderNews", "downloadNews", "activityNews",        /* 8..12  */
    "ngageNews", "sendEmail", "favGame", "favMusic", "favFood", "hobbies", "zodiac", "quote", /* 13..20 */
    "question", "answer"                                                                      /* 21..22 */
};
#define F_QUESTION 21
#define F_ANSWER   22

#define MAX_SESSIONS 256

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

/* A Community login seen on a peer.  A bridge carries one N-Gage, but a
 * direct client address (gcs_direct.h) can be shared by several N-Gages
 * behind the same carrier NAT, so a peer can have several users; the
 * latest one is the fallback for requests without a session cookie. */
typedef struct
{
    void *key;
    gc_u32 stamp; /* order of the logins, 0 = free */
    char user[MAX_NAME];
} peer_user;

/* Community service sessions: JSESSIONID -> user */
typedef struct
{
    char id[40];
    char user[MAX_NAME];
} session;

struct gcs_native
{
    gcs_native_io io;
    void *ctx;
    conn conns[GCSN_MAX_CONNS];
    peer_user peers[MAX_PEERS];
    session sessions[MAX_SESSIONS];
    int next_session;
    gc_u32 token_ctr;
    gc_u32 login_ctr;
    gcs_accounts *acc;
    gcs_snap *snap;   /* SNAP game server (UDP) */
    gcs_rifts *rifts; /* Rifts: Promise of Power game server */
    char www[256];    /* device pages folder, "" none */
    gc_u32 now;
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

/* ---- accounts (gcs_accounts) ---- */

static void acc_log(void *ctx, int level, const char *line)
{
    gcs_native *n = (gcs_native *)ctx;
    nlog(n, level, "%s", line);
}

static const char *pf(gcs_native *n, const char *user, int f)
{
    return gcs_accounts_get(n->acc, user, FIELDS[f]);
}

static const char *boolstr(const char *v)
{
    return (ieq_prefix(v, "true") || ieq_prefix(v, "yes") || !strcmp(v, "1")) ? "true" : "false";
}

/* ---- users per peer (the client sends the name only at login) ---- */

static const char *peer_user_get(gcs_native *n, void *key)
{
    int i, best = -1;
    for (i = 0; i < MAX_PEERS; i++) {
        if (n->peers[i].stamp && n->peers[i].key == key &&
            (best < 0 || GC_TIME_BEFORE(n->peers[best].stamp, n->peers[i].stamp))) {
            best = i;
        }
    }
    return best < 0 ? "" : n->peers[best].user; /* "": matches no account */
}

static void peer_user_set(gcs_native *n, void *key, const char *user)
{
    int i, slot = -1;
    for (i = 0; i < MAX_PEERS; i++) {
        if (n->peers[i].stamp && n->peers[i].key == key && !strcmp(n->peers[i].user, user)) {
            slot = i; /* the same user again */
            break;
        }
    }
    for (i = 0; slot < 0 && i < MAX_PEERS; i++) {
        if (!n->peers[i].stamp) {
            slot = i;
        }
    }
    if (slot < 0) { /* full: replace the oldest login */
        slot = 0;
        for (i = 1; i < MAX_PEERS; i++) {
            if (GC_TIME_BEFORE(n->peers[i].stamp, n->peers[slot].stamp)) {
                slot = i;
            }
        }
    }
    if (++n->login_ctr == 0) {
        n->login_ctr = 1;
    }
    n->peers[slot].key = key;
    n->peers[slot].stamp = n->login_ctr;
    copy_str(n->peers[slot].user, user, MAX_NAME);
}

/* A new random session id for user ("PL" + 32 hex digits). */
static void session_new(gcs_native *n, const char *user, char *out, size_t cap)
{
    char seed[160], h[41];
    session *se = &n->sessions[n->next_session];
    n->next_session = (n->next_session + 1) % MAX_SESSIONS;
    snprintf(seed, sizeof(seed), "%s|%lu|%lu|%p|%lu", user, (unsigned long)time(NULL), (unsigned long)clock(),
             (void *)se, (unsigned long)++n->token_ctr);
    gcs_sha1_hex(seed, strlen(seed), h);
    snprintf(se->id, sizeof(se->id), "PL%.32s", h);
    copy_str(se->user, user, sizeof(se->user));
    copy_str(out, se->id, cap);
}

static int session_user(gcs_native *n, const char *sid, char *out, size_t cap)
{
    int i;
    if (!sid[0]) {
        return 0;
    }
    for (i = 0; i < MAX_SESSIONS; i++) {
        if (n->sessions[i].id[0] && !strcmp(n->sessions[i].id, sid)) {
            copy_str(out, n->sessions[i].user, cap);
            return 1;
        }
    }
    return 0;
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

static void items(gcs_native *n, sbuf *b, const char *user, const view_op *v)
{
    int i;
    for (i = 0; i < v->count; i++) {
        int f = v->first + i;
        const char *val = pf(n, user, f);
        if (v->kind == V_MIXED && i == 1) {
            val = pf(n, user, 2); /* emailAddress */
        }
        if (v->kind == V_BOOL || (v->kind == V_MIXED && i == 0)) {
            sb_fmt(b, "<item>%s</item>", boolstr(val));
        } else {
            sb_str(b, "<item>");
            sb_xml(b, val);
            sb_str(b, "</item>");
        }
    }
    if (v->kind == V_NIL) {
        sb_str(b, "<item></item>"); /* the favorites parser reads six */
    }
}

static int is_login_op(const char *op)
{
    return !strcmp(op, "authenticateUser2") || !strcmp(op, "authenticateUser") || !strcmp(op, "createUser");
}

/* Login operations: check or create the shared account (gcs_accounts).
 * Returns the gcs_accounts result; user gets the account's spelling. */
static int soap_login(gcs_native *n, sbuf *b, const char *op, char *user, size_t ucap, const char *body, size_t blen)
{
    char pw[GCSA_MAX_VALUE + 1], imei[64], canon[GCSA_MAX_NAME + 1];
    int r;
    pw[0] = imei[0] = 0;
    xml_field(body, blen, "password", pw, sizeof(pw));
    if (!xml_field(body, blen, "imei", imei, sizeof(imei))) {
        imei[0] = 0;
    }
    r = gcs_accounts_login(n->acc, user, pw, imei);
    if (r >= 0 && gcs_accounts_exists(n->acc, user, canon)) {
        copy_str(user, canon, ucap);
    }
    LOG_I("soap: %s '%s': %s", op, user, gcs_accounts_result_str(r));
    /* cafmlogin: "xsd:boolean" + "true", then the first "xsd:string"
     * after it is the Arena user name (Jabber login, URLs) */
    sb_fmt(b, "<%sReturn xsi:type=\"xsd:boolean\">%s</%sReturn>", op, r >= 0 ? "true" : "false", op);
    if (r >= 0) {
        sb_str(b, "<username xsi:type=\"xsd:string\">");
        sb_xml(b, user);
        sb_str(b, "</username>");
    }
    return r;
}

static void soap_body(gcs_native *n, sbuf *b, const char *op, const char *user, const char *body, size_t blen)
{
    char val[GCSA_MAX_VALUE + 1];
    int i, k, err = 0;
    int known = gcs_accounts_exists(n->acc, user, NULL);

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
                sb_xml(b, pf(n, user, v->first));
                sb_str(b, "</viewQuoteReturn>");
                return;
            }
            sb_fmt(b, "<%sReturn>", op);
            if (i == 0 && !pf(n, user, 0)[0] && !pf(n, user, 1)[0] && !pf(n, user, 2)[0]) {
                sb_str(b, "<item>");
                sb_xml(b, user);
                sb_str(b, "</item><item></item><item></item>");
            } else {
                items(n, b, user, v);
            }
            sb_fmt(b, "</%sReturn>", op);
            return;
        }
        if (!strcmp(op, v->edit)) {
            for (k = 0; k < v->count; k++) {
                int f = v->first + k;
                if (v->kind == V_MIXED && k == 1) {
                    f = 2;
                }
                if (xml_field(body, blen, FIELDS[f], val, sizeof(val)) && gcs_accounts_set(n->acc, user, FIELDS[f], val)) {
                    err = 1;
                }
            }
            if (!known) {
                err = 1;
            }
            LOG_I("%s for '%s': %s", op, user, err ? "not saved (not logged in)" : "saved");
            break;
        }
    }
    if (!strcmp(op, "changePassword")) {
        char old[GCSA_MAX_VALUE + 1];
        if (!known || !xml_field(body, blen, "newPassword", val, sizeof(val)) || !val[0]) {
            err = 1;
        } else if ((xml_field(body, blen, "oldPassword", old, sizeof(old)) ||
                    xml_field(body, blen, "password", old, sizeof(old))) &&
                   gcs_accounts_check(n->acc, user, old) != GCSA_OK) {
            LOG_W("changePassword for '%s': the old password is wrong", user);
            err = 1;
        } else {
            err = gcs_accounts_set_password(n->acc, user, val) != 0;
        }
    } else if (!strcmp(op, "getSecurityQuestion")) {
        sb_fmt(b, "<%sReturn xsi:type=\"xsd:string\">", op);
        sb_xml(b, pf(n, user, F_QUESTION));
        sb_fmt(b, "</%sReturn>", op);
        return;
    } else if (!strcmp(op, "updateSecurityQA")) {
        if (xml_field(body, blen, "question", val, sizeof(val)) && gcs_accounts_set(n->acc, user, FIELDS[F_QUESTION], val)) {
            err = 1;
        }
        if (xml_field(body, blen, "answer", val, sizeof(val)) && gcs_accounts_set(n->acc, user, FIELDS[F_ANSWER], val)) {
            err = 1;
        }
        if (!known) {
            err = 1;
        }
    }
    /* the edit, change, delete, unlink and update operations: errorCode 0 is success */
    sb_fmt(b, "<%sReturn><errorCode xsi:type=\"xsd:int\">%d</errorCode></%sReturn>", op, err, op);
}

#define LIST_XML                                                                                          \
    "<?xml version=\"1.0\" encoding=\"utf-8\"?><list><currentStart value=\"0\"/><listTotal value=\"0\"/>" \
    "</list>"
/* black like the Arena menus around it */
#define PAGE_HTML                                                                                                  \
    "<?xml version=\"1.0\" encoding=\"utf-8\"?><html xmlns=\"http://www.w3.org/1999/xhtml\"><head>"                \
    "<title>N-Gage Arena</title></head><body bgcolor=\"#000000\" text=\"#ffffff\" "                                \
    "style=\"background-color:#000000;color:#ffffff\"><p>PhantomLink Arena emulator: nothing here yet.</p></body>" \
    "</html>"

static void http_answer_n(gcs_native *n, conn *c, const char *body, size_t blen, const char *ctype,
                          const char *cookie)
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
        sb_fmt(&r, "Content-Length: %u\r\nConnection: close\r\n\r\n", (unsigned)blen);
        sb_add(&r, body, blen);
    }
    if (!r.oom && r.d) {
        tcp_write(n, c, r.d, r.len);
    }
    free(r.d);
    tcp_close(n, c, GCN_CLOSE_FIN);
}

static void http_answer(gcs_native *n, conn *c, const char *body, const char *ctype, const char *cookie)
{
    http_answer_n(n, c, body, body ? strlen(body) : 0, ctype, cookie);
}

/* Returns 1 for a username, 0 when absent, and -1 for an invalid value. */
static int profile_query_user(const char *path, char out[GCSA_MAX_NAME + 1])
{
    const char *p = strchr(path, '?');
    int found = 0;
    if (!p) {
        return 0;
    }
    p++;
    while (*p) {
        const char *end = p + strcspn(p, "&");
        if ((size_t)(end - p) >= 9 && !strncmp(p, "username=", 9)) {
            size_t len = 0;
            if (found) {
                return -1;
            }
            found = 1;
            p += 9;
            while (p < end) {
                unsigned char ch = (unsigned char)*p++;
                if (ch == '%') {
                    int a, b;
                    if (end - p < 2) {
                        return -1;
                    }
                    a = (*p >= '0' && *p <= '9') ? *p - '0' : (*p >= 'a' && *p <= 'f') ? *p - 'a' + 10
                                                          : (*p >= 'A' && *p <= 'F')   ? *p - 'A' + 10
                                                                                       : -1;
                    p++;
                    b = (*p >= '0' && *p <= '9') ? *p - '0' : (*p >= 'a' && *p <= 'f') ? *p - 'a' + 10
                                                          : (*p >= 'A' && *p <= 'F')   ? *p - 'A' + 10
                                                                                       : -1;
                    p++;
                    if (a < 0 || b < 0) {
                        return -1;
                    }
                    ch = (unsigned char)((a << 4) | b);
                } else if (ch == '+') {
                    ch = ' ';
                }
                if (ch < 0x20 || ch == 0x7f || len >= GCSA_MAX_NAME) {
                    return -1;
                }
                out[len++] = (char)ch;
            }
            out[len] = 0;
            if (!len) {
                return -1;
            }
        }
        p = *end ? end + 1 : end;
    }
    return found;
}

static void profile_line(sbuf *b, const char *label, const char *value)
{
    if (!value[0]) {
        return;
    }
    sb_str(b, "<p><b>");
    sb_str(b, label);
    sb_str(b, ":</b> ");
    sb_xml(b, value);
    sb_str(b, "</p>");
}

static void profile_answer(gcs_native *n, conn *c, const char *path, const char *viewer)
{
    static const char *const labels[] = { "Favorite game", "Favorite music", "Favorite food", "Hobbies", "Zodiac", "Quote" };
    char requested[GCSA_MAX_NAME + 1], canon[GCSA_MAX_NAME + 1];
    int query = profile_query_user(path, requested), i;
    sbuf b;
    if (query == 0) {
        copy_str(requested, viewer, sizeof(requested));
    }
    if (query < 0 || !gcs_accounts_exists(n->acc, requested, canon)) {
        LOG_W("http: profile requested with invalid or unknown username");
        http_answer(n, c, NULL, NULL, NULL);
        return;
    }
    memset(&b, 0, sizeof(b));
    sb_str(&b, "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
               "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>Profile</title></head>"
               "<body bgcolor=\"#000000\" text=\"#ffffff\" style=\"background-color:#000000;color:#ffffff\">"
               "<p><b>Profile</b></p>");
    profile_line(&b, "Username", canon);
    if (!strcmp(boolstr(pf(n, canon, 3)), "true")) {
        profile_line(&b, "First name", pf(n, canon, 0));
    }
    if (!strcmp(boolstr(pf(n, canon, 4)), "true")) {
        profile_line(&b, "Last name", pf(n, canon, 1));
    }
    for (i = 0; i < 6; i++) {
        profile_line(&b, labels[i], pf(n, canon, 15 + i));
    }
    sb_str(&b, "</body></html>");
    if (b.oom) {
        LOG_W("http: out of memory rendering profile");
    }
    http_answer(n, c, b.oom ? NULL : b.d, "application/xhtml+xml; charset=utf-8", NULL);
    free(b.d);
}

/* ---- device pages from www_root ---- */

#define WWW_MAX_FILE 65536

/* a path that stays inside the folder: /, letters, digits, '-', '_', '.'
 * and no ".." */
static int www_path_ok(const char *path)
{
    const char *p;
    if (path[0] != '/' || strstr(path, "..") || strstr(path, "//")) {
        return 0;
    }
    for (p = path; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '/' ||
              *p == '-' || *p == '_' || *p == '.')) {
            return 0;
        }
    }
    return p[-1] != '/';
}

static const char *www_type(const char *path)
{
    size_t l = strlen(path);
#define ENDS(x) (l >= sizeof(x) - 1 && !strcmp(path + l - (sizeof(x) - 1), x))
    if (ENDS("_x.jsp") || ENDS(".xml")) {
        return "text/xml; charset=utf-8";
    }
    if (ENDS(".jsp") || ENDS(".html") || ENDS(".htm") || ENDS(".xhtml") || !strchr(strrchr(path, '/'), '.')) {
        return "application/xhtml+xml; charset=utf-8";
    }
    if (ENDS(".gif")) {
        return "image/gif";
    }
    if (ENDS(".png")) {
        return "image/png";
    }
    if (ENDS(".jpg") || ENDS(".jpeg")) {
        return "image/jpeg";
    }
    if (ENDS(".css")) {
        return "text/css";
    }
    if (ENDS(".txt")) {
        return "text/plain; charset=utf-8";
    }
#undef ENDS
    return "application/octet-stream";
}

/* reads dir[/sub]path; malloc'ed, NULL when missing or too large */
static char *www_read(gcs_native *n, const char *sub, const char *path, size_t *len)
{
    char fn[1024];
    FILE *f;
    char *d;
    size_t got;
    if (snprintf(fn, sizeof(fn), "%s%s%s%s", n->www, sub[0] ? "/" : "", sub, path) >= (int)sizeof(fn)) {
        return NULL;
    }
    f = fopen(fn, "rb");
    if (!f) {
        return NULL;
    }
    d = (char *)malloc(WWW_MAX_FILE + 1);
    got = d ? fread(d, 1, WWW_MAX_FILE + 1, f) : 0;
    if (d && ferror(f)) { /* a folder, or unreadable */
        fclose(f);
        free(d);
        return NULL;
    }
    fclose(f);
    if (!d || got > WWW_MAX_FILE) {
        LOG_W("http: %s not served (larger than %u bytes, or out of memory)", fn, (unsigned)WWW_MAX_FILE);
        free(d);
        return NULL;
    }
    *len = got;
    return d;
}

/* 1 when answered from www_root */
static int www_answer(gcs_native *n, conn *c, const char *path, const char *user)
{
    char sub[16];
    gc_u32 gc;
    char *d = NULL;
    size_t len = 0;
    if (!n->www[0] || !www_path_ok(path)) {
        return 0;
    }
    gc = gcs_snap_game_class_of(n->snap, user);
    if (gc) {
        snprintf(sub, sizeof(sub), "%u", (unsigned)gc);
        d = www_read(n, sub, path, &len);
    }
    if (!d) {
        d = www_read(n, "", path, &len);
    }
    if (!d) {
        LOG_D("http: no file %s%s in %s, placeholder sent", gc ? "(game class folder or) " : "", path, n->www);
        return 0;
    }
    LOG_D("http: %s from %s (game class %u)", path, n->www, (unsigned)gc);
    http_answer_n(n, c, d, len, www_type(path), NULL);
    free(d);
    return 1;
}

void gcs_native_set_www_root(gcs_native *n, const char *dir)
{
    size_t l;
    copy_str(n->www, dir ? dir : "", sizeof(n->www));
    l = strlen(n->www);
    while (l > 1 && (n->www[l - 1] == '/' || n->www[l - 1] == '\\')) {
        n->www[--l] = 0;
    }
    if (n->www[0]) {
        struct stat st;
        if (stat(n->www, &st) != 0 || !S_ISDIR(st.st_mode)) {
            LOG_W("http: www_root %s is not a readable folder: built-in placeholders only", n->www);
        } else {
            LOG_I("http: device pages from %s", n->www);
        }
    }
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
        memset(&b, 0, sizeof(b));
        sb_fmt(&b, SOAP_HEAD, op);
        sid[0] = 0;
        if (is_login_op(op)) {
            if (!xml_field(body, blen, "username", user, sizeof(user))) {
                user[0] = 0;
            }
            if (soap_login(n, &b, op, user, sizeof(user), body, blen) >= 0) {
                peer_user_set(n, c->key, user);
                session_new(n, user, sid, sizeof(sid));
            }
        } else {
            if (!session_user(n, cookie, user, sizeof(user))) {
                copy_str(user, peer_user_get(n, c->key), sizeof(user));
            }
            LOG_I("soap: %s ('%s')", op, user);
            soap_body(n, &b, op, user, body, blen);
        }
        sb_fmt(&b, "</ns1:%sResponse></soapenv:Body></soapenv:Envelope>", op);
        http_answer(n, c, b.oom ? NULL : b.d, "text/xml; charset=utf-8", sid[0] ? sid : NULL);
        free(b.d);
        return 1;
    }
    {
        char base[512];
        size_t bl;
        copy_str(base, path, sizeof(base));
        base[strcspn(base, ";?")] = 0;
        bl = strlen(base);
        if (!session_user(n, cookie, user, sizeof(user))) {
            copy_str(user, peer_user_get(n, c->key), sizeof(user));
        }
        if (!strcmp(base, "/n-gage/device/global/customize/profile/profile_view.jsp")) {
            profile_answer(n, c, path, user);
            return 1;
        }
        if (www_answer(n, c, base, user)) {
            return 1;
        }
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
        char pw[GCSA_MAX_VALUE + 1];
        int r;
        user[0] = pw[0] = 0;
        xml_field(s, len, "username", user, sizeof(user));
        if (!xml_field(s, len, "password", pw, sizeof(pw))) {
            pw[0] = 0;
        }
        /* SegaChat sends the SHA-1 of the Arena password (hex); accept the
         * plain password too.  Never creates an account: the Community
         * login comes first. */
        if (!gcs_accounts_exists(n->acc, user, NULL)) {
            r = GCSA_UNKNOWN_USER;
        } else if (strlen(pw) == 40 && gcs_accounts_login_sha1(n->acc, user, pw, NULL) >= 0) {
            r = GCSA_OK;
        } else {
            r = gcs_accounts_check(n->acc, user, pw);
        }
        if (r == GCSA_OK) {
            gcs_accounts_exists(n->acc, user, user);
            copy_str(c->xmpp_user, user, sizeof(c->xmpp_user));
            n->st.xmpp_logins++;
            LOG_I("xmpp: login '%s'", c->xmpp_user);
            snprintf(out, sizeof(out), "<iq type='result'%s/>", idattr);
        } else {
            LOG_W("xmpp: login '%s' refused: %s", user, gcs_accounts_result_str(r));
            snprintf(out, sizeof(out), "<iq type='error'%s><error code='401'>Unauthorized</error></iq>", idattr);
        }
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

static void udp_in(gcs_native *n, void *key, const gcn_msg *m)
{
    char hex[3 * 48 + 1];
    size_t i, k = 0;
    n->st.udp_datagrams++;
    if (m->len >= 20 && m->data[m->len - 4] == 0xBA && m->data[m->len - 3] == 0x47 && m->data[m->len - 2] == 0x66) {
        gcs_snap_input(n->snap, key, m->addr, m->port, m->peer_port, m->data, m->len, n->now);
        return;
    }
    for (i = 0; i < m->len && i < 48; i++) {
        k += (size_t)snprintf(hex + k, sizeof(hex) - k, "%02x ", m->data[i]);
    }
    hex[k] = 0;
    LOG_I("udp to %u.%u.%u.%u:%u from port %u, %u bytes: %s%s", m->addr[0], m->addr[1], m->addr[2], m->addr[3],
          m->port, m->peer_port, (unsigned)m->len, hex, m->len > 48 ? "..." : "");
}

/* ---- SNAP glue ---- */

static void snap_send(void *ctx, void *key, const gc_u8 addr[4], gc_u16 port, gc_u16 peer_port, const gc_u8 *d,
                      size_t len)
{
    gcs_native *n = (gcs_native *)ctx;
    gcn_msg m;
    memset(&m, 0, sizeof(m));
    m.type = GCN_MT_UDP;
    memcpy(m.addr, addr, 4);
    m.port = port;
    m.peer_port = peer_port;
    m.data = d;
    m.len = len;
    send_net(n, key, &m);
}

static void snap_log(void *ctx, int level, const char *line)
{
    gcs_native *n = (gcs_native *)ctx;
    nlog(n, level, "%s", line);
}

/* SNAP login: the account must exist and must have logged in to the
 * Community service through the same peer: the same bridge, or for direct
 * clients the same public address (the SNAP login carries no password the
 * server could check) */
static int snap_auth(void *ctx, void *key, const char *user, char canon[GCSS_MAX_NAME + 1])
{
    gcs_native *n = (gcs_native *)ctx;
    char acc[GCSA_MAX_NAME + 1];
    int i;
    if (!gcs_accounts_exists(n->acc, user, acc)) {
        return 0;
    }
    for (i = 0; i < MAX_PEERS; i++) {
        char seen_canon[GCSA_MAX_NAME + 1];
        if (n->peers[i].stamp && n->peers[i].key == key && n->peers[i].user[0] &&
            gcs_accounts_exists(n->acc, n->peers[i].user, seen_canon) && !strcmp(seen_canon, acc)) {
            copy_str(canon, acc, GCSS_MAX_NAME + 1);
            return 1;
        }
    }
    return 0;
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
        udp_in(n, key, &m);
        break;
    default:
        break;
    }
}

void gcs_native_poll(gcs_native *n, gc_u32 now)
{
    n->now = now;
    gcs_snap_poll(n->snap, now);
}

void gcs_native_peer_gone(gcs_native *n, void *key)
{
    int i;
    gcs_snap_peer_gone(n->snap, key);
    for (i = 0; i < GCSN_MAX_CONNS; i++) {
        if (n->conns[i].used && n->conns[i].key == key) {
            conn_free(&n->conns[i]);
        }
    }
    for (i = 0; i < MAX_PEERS; i++) {
        if (n->peers[i].key == key) {
            memset(&n->peers[i], 0, sizeof(n->peers[i]));
        }
    }
}

gcs_native *gcs_native_create(const gcs_native_io *io, void *ctx, const char *accounts_file, const char *rifts_file)
{
    gcs_native *n = (gcs_native *)calloc(1, sizeof(*n));
    gcs_snap_io sio;
    if (!n) {
        return NULL;
    }
    n->io = *io;
    n->ctx = ctx;
    n->acc = gcs_accounts_create(accounts_file, acc_log, n);
    memset(&sio, 0, sizeof(sio));
    sio.send = snap_send;
    sio.log = snap_log;
    sio.auth = snap_auth;
    n->snap = gcs_snap_create(&sio, n);
    n->rifts = gcs_rifts_create(rifts_file, snap_log, n);
    if (!n->acc || !n->snap || !n->rifts) {
        gcs_accounts_destroy(n->acc);
        gcs_snap_destroy(n->snap);
        gcs_rifts_destroy(n->rifts);
        free(n);
        return NULL;
    }
    gcs_snap_add_game(n->snap, gcs_rifts_game(n->rifts));
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
    gcs_snap_destroy(n->snap);
    gcs_rifts_destroy(n->rifts);
    gcs_accounts_destroy(n->acc);
    free(n);
}

struct gcs_snap *gcs_native_snap(gcs_native *n)
{
    return n ? n->snap : NULL;
}

struct gcs_rifts *gcs_native_rifts(gcs_native *n)
{
    return n ? n->rifts : NULL;
}

gcs_accounts *gcs_native_accounts(gcs_native *n)
{
    return n ? n->acc : NULL;
}

void gcs_native_get_stats(const gcs_native *n, gcs_native_stats *st)
{
    int i;
    *st = n->st;
    st->conns = 0;
    for (i = 0; i < GCSN_MAX_CONNS; i++) {
        st->conns += n->conns[i].used;
    }
    st->users = gcs_accounts_count(n->acc);
}
