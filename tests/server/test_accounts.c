/** @file test_accounts.c
 *
 *  Shared N-Gage Arena accounts (server/src/gcs_accounts.c).
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#include "gc_test.h"
#include "gcs_accounts.h"

#include <stdio.h>
#include <string.h>

#define FILE_NAME "test-accounts.txt"

static void test_sha1(void)
{
    char h[41];
    static char million[1000000];
    gcs_sha1_hex("", 0, h);
    CHECK(!strcmp(h, "da39a3ee5e6b4b0d3255bfef95601890afd80709"));
    gcs_sha1_hex("abc", 3, h);
    CHECK(!strcmp(h, "a9993e364706816aba3e25717850c26c9cd0d89d"));
    gcs_sha1_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, h);
    CHECK(!strcmp(h, "84983e441c3bd26ebaae4aa1f95129e5e54670f1"));
    memset(million, 'a', sizeof(million));
    gcs_sha1_hex(million, sizeof(million), h);
    CHECK(!strcmp(h, "34aa973cd4c4daa4f61eeb2bdbad27316534016f"));
}

static void test_binding(void)
{
    gcs_accounts *a;
    remove(FILE_NAME);
    a = gcs_accounts_create(FILE_NAME, NULL, NULL);
    CHECK(a != NULL);
    CHECK_EQ(gcs_accounts_login(a, "Ann", "", "1111"), GCSA_NO_PASSWORD);
    CHECK_EQ(gcs_accounts_login(a, "", "pw", "1111"), GCSA_BAD_NAME);
    CHECK_EQ(gcs_accounts_login(a, "x\ty", "pw", "1111"), GCSA_BAD_NAME);
    CHECK_EQ(gcs_accounts_login(a, "Ann", "pw", "1111"), GCSA_CREATED);
    CHECK_EQ(gcs_accounts_login(a, "ann", "pw", "1111"), GCSA_OK);
    CHECK_EQ(gcs_accounts_login(a, "Ann", "PW", "1111"), GCSA_BAD_PASSWORD);
    CHECK_EQ(gcs_accounts_login(a, "Ann", "pw", "2222"), GCSA_OTHER_DEVICE);
    CHECK_EQ(gcs_accounts_login(a, "Bob", "pw", "1111"), GCSA_IMEI_TAKEN);
    CHECK_EQ(gcs_accounts_login(a, "Bob", "pw", "2222"), GCSA_CREATED);
    /* no IMEI (a game that does not send one): password only */
    CHECK_EQ(gcs_accounts_login(a, "Bob", "pw", NULL), GCSA_OK);
    CHECK_EQ(gcs_accounts_login_sha1(a, "bob", "1A91D62F7CA67399625A4368A6AB5D4A3BAA6073", NULL), GCSA_OK);
    CHECK_EQ(gcs_accounts_login_sha1(a, "bob", "1a91d62f", NULL), GCSA_NO_PASSWORD);
    CHECK_EQ(gcs_accounts_check(a, "Carl", "pw"), GCSA_UNKNOWN_USER);
    CHECK_EQ(gcs_accounts_check(a, "Ann", "pw"), GCSA_OK);
    CHECK_EQ(gcs_accounts_set(a, "Ann", "firstName", "Ann\tTab"), 0);
    CHECK_EQ(gcs_accounts_set(a, "Ann", "pwsha1", "0"), -1);
    CHECK_EQ(gcs_accounts_set(a, "Ann", "imei", "2222"), -1);
    CHECK_EQ(gcs_accounts_set(a, "Carl", "firstName", "x"), -1);
    CHECK_EQ(gcs_accounts_set_password(a, "Ann", "new"), 0);
    CHECK_EQ(gcs_accounts_count(a), 2);
    gcs_accounts_destroy(a);

    /* everything survives a restart */
    a = gcs_accounts_create(FILE_NAME, NULL, NULL);
    CHECK_EQ(gcs_accounts_count(a), 2);
    CHECK_EQ(gcs_accounts_login(a, "Ann", "pw", "1111"), GCSA_BAD_PASSWORD);
    CHECK_EQ(gcs_accounts_login(a, "Ann", "new", "1111"), GCSA_OK);
    CHECK_EQ(gcs_accounts_login(a, "Ann", "new", "2222"), GCSA_OTHER_DEVICE);
    CHECK(!strcmp(gcs_accounts_get(a, "ANN", "firstName"), "Ann\tTab"));
    gcs_accounts_destroy(a);
}

static void test_old_file(void)
{
    gcs_accounts *a;
    char buf[1024];
    size_t n;
    FILE *f = fopen(FILE_NAME, "w");
    CHECK(f != NULL);
    if (!f) {
        return;
    }
    /* format of the first version: plain password, no IMEI */
    fputs("# old\nmupf\tpassword\tpw\nmupf\tfirstName\tMichael\nnopw\tlastName\tX\n", f);
    fclose(f);
    a = gcs_accounts_create(FILE_NAME, NULL, NULL);
    CHECK_EQ(gcs_accounts_count(a), 2);
    CHECK(!strcmp(gcs_accounts_get(a, "mupf", "firstName"), "Michael"));
    /* the first login binds the N-Gage; an account without a password gets one */
    CHECK_EQ(gcs_accounts_login(a, "mupf", "pw", "1111"), GCSA_OK);
    CHECK_EQ(gcs_accounts_login(a, "mupf", "pw", "2222"), GCSA_OTHER_DEVICE);
    CHECK_EQ(gcs_accounts_login(a, "nopw", "abc", "3333"), GCSA_OK);
    CHECK_EQ(gcs_accounts_login(a, "nopw", "abd", "3333"), GCSA_BAD_PASSWORD);
    gcs_accounts_destroy(a);
    f = fopen(FILE_NAME, "r");
    n = f ? fread(buf, 1, sizeof(buf) - 1, f) : 0;
    if (f) {
        fclose(f);
    }
    buf[n] = 0;
    CHECK(strstr(buf, "\tpassword\t") == NULL);
    CHECK(strstr(buf, "mupf\tpwsha1\t1a91d62f7ca67399625a4368a6ab5d4a3baa6073\n") != NULL);
    CHECK(strstr(buf, "mupf\timei\t1111\n") != NULL);
    remove(FILE_NAME);
}

int main(void)
{
    RUN(test_sha1);
    RUN(test_binding);
    RUN(test_old_file);
    TEST_MAIN_END();
}
