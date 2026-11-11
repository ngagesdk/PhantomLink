/** @file gcs_accounts.c
 *
 *  N-Gage Arena accounts, see gcs_accounts.h.
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#include "gcs_accounts.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define F_PWSHA1 "pwsha1"
#define F_IMEI   "imei"

typedef struct
{
    char key[GCSA_MAX_FIELD + 1];
    char val[GCSA_MAX_VALUE + 1];
} field;

typedef struct
{
    char name[GCSA_MAX_NAME + 1];
    int n;
    field f[GCSA_MAX_FIELDS];
} account;

struct gcs_accounts
{
    char file[256];
    gcs_accounts_log_fn log;
    void *ctx;
    account *acc;
    int n;
};

/* ---- SHA-1 (FIPS 180-1) ---- */

typedef unsigned int u32;

typedef struct
{
    u32 h[5];
    unsigned char buf[64];
    size_t used;
    unsigned long long bits;
} sha1_ctx;

static u32 rol(u32 v, int s)
{
    return (v << s) | (v >> (32 - s));
}

static void sha1_block(sha1_ctx *c, const unsigned char *p)
{
    u32 w[80], a, b, d, e, f, k, t, cc;
    int i;
    for (i = 0; i < 16; i++) {
        w[i] = ((u32)p[4 * i] << 24) | ((u32)p[4 * i + 1] << 16) | ((u32)p[4 * i + 2] << 8) | (u32)p[4 * i + 3];
    }
    for (i = 16; i < 80; i++) {
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    a = c->h[0];
    b = c->h[1];
    cc = c->h[2];
    d = c->h[3];
    e = c->h[4];
    for (i = 0; i < 80; i++) {
        if (i < 20) {
            f = (b & cc) | (~b & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ cc ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & cc) | (b & d) | (cc & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ cc ^ d;
            k = 0xCA62C1D6u;
        }
        t = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = cc;
        cc = rol(b, 30);
        b = a;
        a = t;
    }
    c->h[0] += a;
    c->h[1] += b;
    c->h[2] += cc;
    c->h[3] += d;
    c->h[4] += e;
}

static void sha1_add(sha1_ctx *c, const unsigned char *p, size_t len)
{
    c->bits += (unsigned long long)len * 8u;
    while (len) {
        size_t k = 64 - c->used;
        if (k > len) {
            k = len;
        }
        memcpy(c->buf + c->used, p, k);
        c->used += k;
        p += k;
        len -= k;
        if (c->used == 64) {
            sha1_block(c, c->buf);
            c->used = 0;
        }
    }
}

void gcs_sha1_hex(const void *data, size_t len, char out[41])
{
    static const char hx[] = "0123456789abcdef";
    sha1_ctx c;
    unsigned char pad[72];
    unsigned long long bits;
    size_t padlen;
    int i;

    memset(&c, 0, sizeof(c));
    c.h[0] = 0x67452301u;
    c.h[1] = 0xEFCDAB89u;
    c.h[2] = 0x98BADCFEu;
    c.h[3] = 0x10325476u;
    c.h[4] = 0xC3D2E1F0u;
    sha1_add(&c, (const unsigned char *)data, len);
    bits = c.bits;
    padlen = (c.used < 56) ? 56 - c.used : 120 - c.used;
    memset(pad, 0, sizeof(pad));
    pad[0] = 0x80;
    for (i = 0; i < 8; i++) {
        pad[padlen + (size_t)i] = (unsigned char)(bits >> (56 - 8 * i));
    }
    sha1_add(&c, pad, padlen + 8);
    for (i = 0; i < 20; i++) {
        unsigned char b = (unsigned char)(c.h[i / 4] >> (24 - 8 * (i % 4)));
        out[2 * i] = hx[b >> 4];
        out[2 * i + 1] = hx[b & 15];
    }
    out[40] = 0;
}

/* ---- helpers ---- */

static void alog(gcs_accounts *a, int level, const char *fmt, ...)
{
    char line[400];
    va_list ap;
    if (!a->log) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    a->log(a->ctx, level, line);
}

static void copy_str(char *dst, const char *src, size_t cap)
{
    size_t k = strlen(src);
    if (k >= cap) {
        k = cap - 1;
    }
    memcpy(dst, src, k);
    dst[k] = 0;
}

static int lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int ieq(const char *x, const char *y)
{
    for (; *x && *y; x++, y++) {
        if (lower((unsigned char)*x) != lower((unsigned char)*y)) {
            return 0;
        }
    }
    return *x == *y;
}

static int valid_name(const char *s)
{
    size_t k = strlen(s), i;
    if (k == 0 || k > GCSA_MAX_NAME) {
        return 0;
    }
    for (i = 0; i < k; i++) {
        if ((unsigned char)s[i] < 0x20 || s[i] == 0x7f) {
            return 0;
        }
    }
    return 1;
}

static account *find(gcs_accounts *a, const char *user)
{
    int i;
    for (i = 0; i < a->n; i++) {
        if (ieq(a->acc[i].name, user)) {
            return &a->acc[i];
        }
    }
    return NULL;
}

static account *add(gcs_accounts *a, const char *user)
{
    account *na;
    if (a->n >= GCSA_MAX_USERS) {
        return NULL;
    }
    na = (account *)realloc(a->acc, sizeof(account) * (size_t)(a->n + 1));
    if (!na) {
        return NULL;
    }
    a->acc = na;
    na = &a->acc[a->n++];
    memset(na, 0, sizeof(*na));
    copy_str(na->name, user, sizeof(na->name));
    return na;
}

static const char *fget(const account *ac, const char *key)
{
    int i;
    for (i = 0; i < ac->n; i++) {
        if (!strcmp(ac->f[i].key, key)) {
            return ac->f[i].val;
        }
    }
    return "";
}

static int fset(account *ac, const char *key, const char *val)
{
    int i;
    for (i = 0; i < ac->n; i++) {
        if (!strcmp(ac->f[i].key, key)) {
            break;
        }
    }
    if (i == ac->n) {
        if (!val[0]) {
            return 0;
        }
        if (ac->n >= GCSA_MAX_FIELDS) {
            return -1;
        }
        ac->n++;
        copy_str(ac->f[i].key, key, sizeof(ac->f[i].key));
    }
    if (!val[0]) { /* unset: remove */
        ac->f[i] = ac->f[ac->n - 1];
        ac->n--;
        return 0;
    }
    copy_str(ac->f[i].val, val, sizeof(ac->f[i].val));
    return 0;
}

static account *by_imei(gcs_accounts *a, const char *imei)
{
    int i;
    for (i = 0; i < a->n; i++) {
        if (!strcmp(fget(&a->acc[i], F_IMEI), imei)) {
            return &a->acc[i];
        }
    }
    return NULL;
}

/* ---- file ---- */

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

static void save(gcs_accounts *a)
{
    char tmp[300];
    FILE *f;
    int i, k;
    if (!a->file[0]) {
        return;
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", a->file);
    f = fopen(tmp, "w");
    if (!f) {
        alog(a, 1, "accounts: cannot write %s", tmp);
        return;
    }
    fputs("# PhantomLink Arena accounts: user<TAB>field<TAB>value (pwsha1: SHA-1 of the password)\n", f);
    for (i = 0; i < a->n; i++) {
        for (k = 0; k < a->acc[i].n; k++) {
            esc_out(f, a->acc[i].name);
            fputc('\t', f);
            esc_out(f, a->acc[i].f[k].key);
            fputc('\t', f);
            esc_out(f, a->acc[i].f[k].val);
            fputc('\n', f);
        }
    }
    if (fclose(f) != 0) {
        alog(a, 1, "accounts: cannot write %s", tmp);
        return;
    }
    remove(a->file); /* rename does not replace on Windows */
    if (rename(tmp, a->file) != 0) {
        alog(a, 1, "accounts: cannot replace %s", a->file);
    }
}

static void load(gcs_accounts *a)
{
    char line[600];
    int converted = 0;
    FILE *f = fopen(a->file, "r");
    if (!f) {
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        char *t1, *t2, *e, *key, *val;
        account *ac;
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
        key = t1 + 1;
        val = t2 + 1;
        esc_in(line);
        esc_in(key);
        esc_in(val);
        if (!valid_name(line) || !key[0]) {
            continue;
        }
        ac = find(a, line);
        if (!ac) {
            ac = add(a, line);
        }
        if (!ac) {
            break;
        }
        if (!strcmp(key, "password")) { /* files written before passwords were hashed */
            char h[41];
            if (val[0]) {
                gcs_sha1_hex(val, strlen(val), h);
                fset(ac, F_PWSHA1, h);
                converted = 1;
            }
            continue;
        }
        fset(ac, key, val);
    }
    fclose(f);
    if (converted) {
        save(a);
    }
    alog(a, 2, "accounts: %d loaded from %s", a->n, a->file);
}

/* ---- API ---- */

gcs_accounts *gcs_accounts_create(const char *file, gcs_accounts_log_fn log, void *ctx)
{
    gcs_accounts *a = (gcs_accounts *)calloc(1, sizeof(*a));
    if (!a) {
        return NULL;
    }
    a->log = log;
    a->ctx = ctx;
    if (file) {
        copy_str(a->file, file, sizeof(a->file));
    }
    if (a->file[0]) {
        load(a);
    }
    return a;
}

void gcs_accounts_destroy(gcs_accounts *a)
{
    if (a) {
        free(a->acc);
        free(a);
    }
}

static int is_hex40(const char *s)
{
    int i;
    for (i = 0; i < 40; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return 0;
        }
    }
    return s[40] == 0;
}

static int login_hash(gcs_accounts *a, const char *user, const char *h, const char *imei)
{
    account *ac;
    const char *bound;
    int dirty = 0;
    if (!user || !valid_name(user)) {
        return GCSA_BAD_NAME;
    }
    if (!imei) {
        imei = "";
    }
    ac = find(a, user);
    if (!ac) {
        account *other;
        if (!h) {
            return GCSA_NO_PASSWORD;
        }
        other = imei[0] ? by_imei(a, imei) : NULL;
        if (other) {
            alog(a, 1, "accounts: '%s' refused, this N-Gage already owns '%s'", user, other->name);
            return GCSA_IMEI_TAKEN;
        }
        ac = add(a, user);
        if (!ac) {
            return GCSA_FULL;
        }
        fset(ac, F_PWSHA1, h);
        if (imei[0]) {
            fset(ac, F_IMEI, imei);
        }
        save(a);
        alog(a, 2, "accounts: '%s' created%s", ac->name, imei[0] ? " and bound to this N-Gage" : "");
        return GCSA_CREATED;
    }
    if (!h) {
        return GCSA_NO_PASSWORD;
    }
    if (!fget(ac, F_PWSHA1)[0]) {
        /* account without credentials (profile data from an older
         * version): this login sets them */
        fset(ac, F_PWSHA1, h);
        dirty = 1;
    } else if (!ieq(fget(ac, F_PWSHA1), h)) {
        alog(a, 1, "accounts: '%s': wrong password", ac->name);
        return GCSA_BAD_PASSWORD;
    }
    bound = fget(ac, F_IMEI);
    if (imei[0]) {
        if (bound[0] && strcmp(bound, imei)) {
            alog(a, 1, "accounts: '%s' refused, the account belongs to another N-Gage", ac->name);
            return GCSA_OTHER_DEVICE;
        }
        if (!bound[0]) {
            account *other = by_imei(a, imei);
            if (other && other != ac) {
                alog(a, 1, "accounts: '%s' refused, this N-Gage already owns '%s'", ac->name, other->name);
                return GCSA_IMEI_TAKEN;
            }
            fset(ac, F_IMEI, imei);
            save(a);
            alog(a, 2, "accounts: '%s' bound to this N-Gage", ac->name);
            return GCSA_OK;
        }
    }
    if (dirty) {
        save(a);
    }
    return GCSA_OK;
}

int gcs_accounts_login(gcs_accounts *a, const char *user, const char *password, const char *imei)
{
    char h[41];
    if (!password || !password[0]) {
        return (user && valid_name(user)) ? GCSA_NO_PASSWORD : GCSA_BAD_NAME;
    }
    gcs_sha1_hex(password, strlen(password), h);
    return login_hash(a, user, h, imei);
}

int gcs_accounts_login_sha1(gcs_accounts *a, const char *user, const char *sha1_hex, const char *imei)
{
    char h[41];
    size_t i;
    if (!sha1_hex || !is_hex40(sha1_hex)) {
        return (user && valid_name(user)) ? GCSA_NO_PASSWORD : GCSA_BAD_NAME;
    }
    for (i = 0; i < 41; i++) {
        h[i] = (char)lower((unsigned char)sha1_hex[i]);
    }
    return login_hash(a, user, h, imei);
}

int gcs_accounts_check(gcs_accounts *a, const char *user, const char *password)
{
    char h[41];
    account *ac = user ? find(a, user) : NULL;
    if (!ac) {
        return GCSA_UNKNOWN_USER;
    }
    if (!password || !password[0]) {
        return GCSA_BAD_PASSWORD;
    }
    gcs_sha1_hex(password, strlen(password), h);
    return ieq(fget(ac, F_PWSHA1), h) ? GCSA_OK : GCSA_BAD_PASSWORD;
}

int gcs_accounts_exists(gcs_accounts *a, const char *user, char canon[GCSA_MAX_NAME + 1])
{
    account *ac = user ? find(a, user) : NULL;
    if (ac && canon) {
        copy_str(canon, ac->name, GCSA_MAX_NAME + 1);
    }
    return ac != NULL;
}

int gcs_accounts_set_password(gcs_accounts *a, const char *user, const char *password)
{
    char h[41];
    account *ac = user ? find(a, user) : NULL;
    if (!ac) {
        return GCSA_UNKNOWN_USER;
    }
    if (!password || !password[0]) {
        return GCSA_NO_PASSWORD;
    }
    gcs_sha1_hex(password, strlen(password), h);
    fset(ac, F_PWSHA1, h);
    save(a);
    alog(a, 2, "accounts: '%s' changed the password", ac->name);
    return 0;
}

const char *gcs_accounts_password_sha1(gcs_accounts *a, const char *user)
{
    account *ac = user ? find(a, user) : NULL;
    return (ac && fget(ac, F_PWSHA1)[0]) ? fget(ac, F_PWSHA1) : NULL;
}

const char *gcs_accounts_get(gcs_accounts *a, const char *user, const char *field)
{
    account *ac = user ? find(a, user) : NULL;
    return ac ? fget(ac, field) : "";
}

int gcs_accounts_set(gcs_accounts *a, const char *user, const char *fld, const char *value)
{
    account *ac = user ? find(a, user) : NULL;
    char key[GCSA_MAX_FIELD + 1];
    if (!ac || !fld || !fld[0] || !strcmp(fld, F_PWSHA1) || !strcmp(fld, F_IMEI) || !strcmp(fld, "password")) {
        return -1;
    }
    copy_str(key, fld, sizeof(key));
    if (!strcmp(fget(ac, key), value ? value : "")) {
        return 0;
    }
    if (fset(ac, key, value ? value : "") != 0) {
        return -1;
    }
    save(a);
    return 0;
}

int gcs_accounts_count(const gcs_accounts *a)
{
    return a->n;
}

const char *gcs_accounts_result_str(int r)
{
    switch (r) {
    case GCSA_OK:
        return "ok";
    case GCSA_CREATED:
        return "account created";
    case GCSA_BAD_PASSWORD:
        return "wrong password";
    case GCSA_NO_PASSWORD:
        return "no password";
    case GCSA_OTHER_DEVICE:
        return "account belongs to another N-Gage";
    case GCSA_IMEI_TAKEN:
        return "this N-Gage already has an account";
    case GCSA_UNKNOWN_USER:
        return "unknown user";
    case GCSA_FULL:
        return "no room for more accounts";
    case GCSA_BAD_NAME:
        return "invalid user name";
    default:
        return "?";
    }
}
