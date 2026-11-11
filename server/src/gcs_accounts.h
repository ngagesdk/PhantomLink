/** @file gcs_accounts.h
 *
 *  N-Gage Arena accounts, shared by every Arena game and back end.
 *
 *  An account is created by the first login with a new user name.  That
 *  login binds the user name, the password and the IMEI of the N-Gage
 *  together:
 *
 *   - later logins need the same password,
 *   - the account only works on the N-Gage it was created on, and one
 *     N-Gage can own only one account,
 *   - the account is the same for all games: the Community web service
 *     (SOAP), SegaChat (Jabber) and the SNAP game servers all check it.
 *
 *  Passwords are kept as SHA-1 (hex), the form SegaChat logins send
 *  anyway.  To move an account to another N-Gage, remove its "imei"
 *  line from the file while the server is stopped; the next login binds
 *  the new IMEI.
 *
 *  Besides the credentials an account keeps any number of named text
 *  fields (profile data of the Community service, game modules).
 *
 *  File format, one value per line: user<TAB>field<TAB>value, with \\,
 *  \t, \n and \r escaped.  Old files with a plain "password" field are
 *  converted on load.
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#ifndef GCS_ACCOUNTS_H
#define GCS_ACCOUNTS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gcs_accounts gcs_accounts;

/** log line (level 0 errors .. 3 debug) */
typedef void (*gcs_accounts_log_fn)(void *ctx, int level, const char *line);

#define GCSA_MAX_USERS  1024 /* accounts in one file                     */
#define GCSA_MAX_NAME   32   /* user name, without the terminator        */
#define GCSA_MAX_FIELD  32   /* field name, without the terminator       */
#define GCSA_MAX_VALUE  127  /* field value, without the terminator      */
#define GCSA_MAX_FIELDS 48   /* fields per account                       */

/* login results */
#define GCSA_OK           0  /* known user, password and IMEI fit        */
#define GCSA_CREATED      1  /* first login: the account was created     */
#define GCSA_BAD_PASSWORD -1 /* wrong password                           */
#define GCSA_NO_PASSWORD  -2 /* empty password                           */
#define GCSA_OTHER_DEVICE -3 /* the account belongs to another N-Gage    */
#define GCSA_IMEI_TAKEN   -4 /* this N-Gage already owns another account */
#define GCSA_UNKNOWN_USER -5 /* no such account (checks that never create) */
#define GCSA_FULL         -6 /* no room for another account              */
#define GCSA_BAD_NAME     -7 /* empty, too long or unusable user name    */

/** file: where accounts are kept (NULL or "": memory only). */
gcs_accounts *gcs_accounts_create(const char *file, gcs_accounts_log_fn log, void *ctx);
void gcs_accounts_destroy(gcs_accounts *a);

/** Login with the plain password, as the Community service gets it.
 *  imei may be NULL or "" when the client does not send one (the binding
 *  is then neither checked nor made).  Creates the account on the first
 *  login.  Returns GCSA_OK, GCSA_CREATED or a negative GCSA_ code. */
int gcs_accounts_login(gcs_accounts *a, const char *user, const char *password, const char *imei);

/** Same, with the password as 40 hex digits of its SHA-1 (SegaChat). */
int gcs_accounts_login_sha1(gcs_accounts *a, const char *user, const char *sha1_hex, const char *imei);

/** Check without creating: GCSA_OK, GCSA_UNKNOWN_USER or GCSA_BAD_PASSWORD. */
int gcs_accounts_check(gcs_accounts *a, const char *user, const char *password);

/** 1 if the account exists.  The exact spelling of the name (logins
 *  ignore case) is copied to canon when it is not NULL. */
int gcs_accounts_exists(gcs_accounts *a, const char *user, char canon[GCSA_MAX_NAME + 1]);

/** New password for an existing account (0 ok, negative GCSA_ code). */
int gcs_accounts_set_password(gcs_accounts *a, const char *user, const char *password);

/** The SHA-1 of the account's password, or NULL. */
const char *gcs_accounts_password_sha1(gcs_accounts *a, const char *user);

/** A named field of an existing account; "" when unset or unknown. */
const char *gcs_accounts_get(gcs_accounts *a, const char *user, const char *field);
/** Set a field of an existing account and save (0 ok, -1 no such user or
 *  no room).  The credential fields (pwsha1, imei) cannot be set. */
int gcs_accounts_set(gcs_accounts *a, const char *user, const char *field, const char *value);

int gcs_accounts_count(const gcs_accounts *a);

/** Short English description of a login result. */
const char *gcs_accounts_result_str(int result);

/** SHA-1 of data as 40 lower case hex digits. */
void gcs_sha1_hex(const void *data, size_t len, char out[41]);

#ifdef __cplusplus
}
#endif

#endif /* GCS_ACCOUNTS_H */
