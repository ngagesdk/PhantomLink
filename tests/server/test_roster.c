/* SegaChat friends (server/src/gcs_roster.c and the routing in
 * gcs_native.c): the store on its own, then two users through
 * gcs_direct and gcs_native with the stanzas as sc_lib.dll writes them
 * (friend request, Authorise, presence with ext_pres, chat, offline
 * messages, removing a friend).
 *
 * SPDX-License-Identifier: MIT */
#include "gc_test.h"
#include "gcs_core.h"
#include "gcs_direct.h"
#include "gcs_native.h"
#include "gcs_roster.h"

#include <stdio.h>
#include <stdlib.h>

#define ACC   "test_roster_accounts.txt"
#define RDATA "test_roster_data.txt"

static void quiet_log(void *ctx, int level, const char *line)
{
    (void)ctx;
    if (level <= 1) {
        printf("  log %d: %s\n", level, line);
    }
}

static int nf;
static char fnames[8][40];

static void collect(void *ctx, const char *name)
{
    (void)ctx;
    if (nf < 8) {
        snprintf(fnames[nf], sizeof(fnames[0]), "%s", name);
    }
    nf++;
}

static int nm;
static char mtext[4][200];

static void take(void *ctx, const char *from, long t, const char *stanza)
{
    (void)ctx, (void)from, (void)t;
    if (nm < 4) {
        snprintf(mtext[nm], sizeof(mtext[0]), "%s", stanza);
    }
    nm++;
}

static void test_store(void)
{
    gcs_roster *r;
    gcs_roster_stats st;
    char status[GCSR_MAX_STATUS + 1];
    remove(RDATA);
    r = gcs_roster_create(RDATA, quiet_log, NULL);
    CHECK(r != NULL);
    CHECK_EQ(gcs_roster_request(r, "alice", "bob", "hi|bob\n"), 1);
    CHECK_EQ(gcs_roster_request(r, "alice", "bob", "hello"), 0);
    CHECK(gcs_roster_has_request(r, "Alice", "BOB", status));
    CHECK(!strcmp(status, "hello"));
    CHECK(!gcs_roster_has_request(r, "bob", "alice", NULL));
    CHECK_EQ(gcs_roster_request(r, "alice", "alice", ""), -1);
    CHECK_EQ(gcs_roster_request(r, "a|b", "bob", ""), -1);
    CHECK_EQ(gcs_roster_add_friends(r, "bob", "alice"), 1); /* the request is gone */
    CHECK(!gcs_roster_has_request(r, "alice", "bob", NULL));
    CHECK_EQ(gcs_roster_add_friends(r, "alice", "Bob"), 0);
    CHECK(gcs_roster_are_friends(r, "ALICE", "bob"));
    CHECK_EQ(gcs_roster_add_friends(r, "carol", "alice"), 1);
    nf = 0;
    CHECK_EQ(gcs_roster_friends(r, "alice", collect, NULL), 2);
    CHECK(!strcmp(fnames[0], "bob") && !strcmp(fnames[1], "carol"));
    CHECK_EQ(gcs_roster_store_message(r, "bob", "alice", 100, "<message>\n<body>one</body></message>"), 0);
    CHECK_EQ(gcs_roster_store_message(r, "bob", "carol", 101, "<message><body>two</body></message>"), 0);
    CHECK_EQ(gcs_roster_request(r, "dave", "alice", "let me in"), 1);
    gcs_roster_destroy(r);

    /* everything comes back from the file */
    r = gcs_roster_create(RDATA, quiet_log, NULL);
    gcs_roster_get_stats(r, &st);
    CHECK_EQ(st.friendships, 2);
    CHECK_EQ(st.requests, 1);
    CHECK_EQ(st.messages, 2);
    CHECK(gcs_roster_has_request(r, "dave", "alice", status));
    CHECK(!strcmp(status, "let me in"));
    nm = 0;
    CHECK_EQ(gcs_roster_take_messages(r, "BOB", take, NULL), 2);
    CHECK(!strcmp(mtext[0], "<message>\n<body>one</body></message>"));
    CHECK(!strcmp(mtext[1], "<message><body>two</body></message>"));
    CHECK_EQ(gcs_roster_take_messages(r, "bob", take, NULL), 0);
    CHECK_EQ(gcs_roster_remove(r, "bob", "alice"), 1);
    CHECK_EQ(gcs_roster_remove(r, "bob", "alice"), 0);
    nf = 0;
    CHECK_EQ(gcs_roster_friends(r, "alice", collect, NULL), 1);
    gcs_roster_forget(r, "alice");
    gcs_roster_get_stats(r, &st);
    CHECK_EQ(st.friendships, 0);
    CHECK_EQ(st.requests, 0);
    /* per user limit: the oldest message goes */
    {
        int i;
        char m[64];
        for (i = 0; i < GCSR_MAX_USERMSG + 5; i++) {
            snprintf(m, sizeof(m), "<message><body>%d</body></message>", i);
            CHECK_EQ(gcs_roster_store_message(r, "erin", "frank", i, m), 0);
        }
        nm = 0;
        CHECK_EQ(gcs_roster_take_messages(r, "erin", take, NULL), GCSR_MAX_USERMSG);
        CHECK(!strcmp(mtext[0], "<message><body>5</body></message>"));
    }
    gcs_roster_destroy(r);
    remove(RDATA);
}

/* ---- two users through gcs_direct and gcs_native ---- */

typedef struct
{
    char rx[32768];
    size_t len;
    int fin, reset;
} fake_sock;

static void io_tcp_send(void *ctx, void *sock, const gc_u8 *d, size_t len)
{
    fake_sock *s = (fake_sock *)sock;
    (void)ctx;
    if (s->len + len < sizeof(s->rx)) {
        memcpy(s->rx + s->len, d, len);
        s->len += len;
        s->rx[s->len] = 0;
    }
}

static void io_tcp_close(void *ctx, void *sock, int reset)
{
    fake_sock *s = (fake_sock *)sock;
    (void)ctx;
    if (reset) {
        s->reset = 1;
    } else {
        s->fin = 1;
    }
}

static void io_udp_send(void *ctx, gc_u16 service, const gc_u8 ip[4], gc_u16 port, const gc_u8 *d, size_t len)
{
    (void)ctx, (void)service, (void)ip, (void)port, (void)d, (void)len;
}

static void s_write(void *c, gcs_conn *k, const gc_u8 *d, size_t n)
{
    (void)c, (void)k, (void)d, (void)n;
}

static void s_close(void *c, gcs_conn *k)
{
    (void)c, (void)k;
}

static gcs_direct *D;

static void clear(fake_sock *s)
{
    s->len = 0;
    s->rx[0] = 0;
}

static const char *say(fake_sock *s, const char *text)
{
    clear(s);
    gcs_direct_tcp_input(D, s, (const gc_u8 *)text, strlen(text), 1000);
    return s->rx;
}

static void login(fake_sock *s, const gc_u8 ip[4], gc_u16 port, const char *user)
{
    char q[300];
    const char *r;
    memset(s, 0, sizeof(*s));
    CHECK_EQ(gcs_direct_tcp_open(D, s, ip, port, GCSD_XMPP, 1000), 0);
    say(s, "<?xml version='1.0'?><stream:stream to='ngage-auth' xmlns='jabber:client' "
           "xmlns:stream='http://etherx.jabber.org/streams'>");
    snprintf(q, sizeof(q),
             "<iq type='set' id='segachat_auth'><query xmlns='jabber:iq:auth'><username>%s</username>"
             "<password>pw-%s</password><resource>segachat</resource></query></iq>",
             user, user);
    r = say(s, q);
    CHECK(strstr(r, "<iq type='result' id='segachat_auth'/>") != NULL);
}

static void test_friends(void)
{
    static const gc_u8 ipa[4] = { 203, 0, 113, 1 }, ipb[4] = { 203, 0, 113, 2 };
    gcs_config cfg;
    gcs_io sio;
    gcs_direct_io dio;
    gcs_core *core;
    fake_sock a, b;
    const char *r;
    gcs_native_stats nst;

    remove(ACC);
    remove(RDATA);
    gcs_default_config(&cfg);
    strcpy(cfg.arena_accounts, ACC);
    strcpy(cfg.roster_data, RDATA);
    cfg.scores_data[0] = 0;
    cfg.pk_data[0] = 0;
    cfg.rifts_data[0] = 0;
    cfg.habbo_data[0] = 0;
    memset(&sio, 0, sizeof(sio));
    sio.write = s_write;
    sio.close = s_close;
    core = gcs_core_create(&cfg, &sio, NULL, 1);
    CHECK(core != NULL);
    memset(&dio, 0, sizeof(dio));
    dio.tcp_send = io_tcp_send;
    dio.tcp_close = io_tcp_close;
    dio.udp_send = io_udp_send;
    D = gcs_direct_create(core, &dio, NULL);
    CHECK(D != NULL);

    login(&a, ipa, 4000, "Alice");
    login(&b, ipb, 4001, "Bob");

    /* empty rosters, initial presence */
    r = say(&a, "<iq type='get' id='segachat_get_roster'><query xmlns='jabber:iq:roster'/></iq>");
    CHECK(strstr(r, "<iq type='result' id='segachat_get_roster'><query xmlns='jabber:iq:roster'/></iq>") != NULL);
    say(&a, "<presence><ext_pres GameClassId='1'/></presence>");
    clear(&b);
    r = say(&b, "<presence/>");
    CHECK(!strstr(a.rx, "<presence")); /* not friends yet */

    /* Alice asks Bob (Add friend) */
    clear(&b);
    r = say(&a, "<presence to='bob@ngage-auth' type='subscribe' id='segachat_buddy_req'><status>hi</status></presence>");
    CHECK(strstr(r, "<item jid='Bob@ngage-auth' name='Bob' subscription='none' ask='subscribe'/>") != NULL);
    CHECK(strstr(b.rx, "<presence from='Alice@ngage-auth' to='Bob@ngage-auth' type='subscribe' id='segachat_buddy_req'>"
                       "<status>hi</status></presence>") != NULL);
    r = say(&a, "<iq type='get' id='r2'><query xmlns='jabber:iq:roster'/></iq>");
    CHECK(strstr(r, "subscription='none' ask='subscribe'") != NULL);

    /* Bob authorises: both are friends, presence goes both ways */
    clear(&a);
    r = say(&b, "<presence to='alice@ngage-auth' type='subscribed' id='segachat_buddy_accrej'/>");
    CHECK(strstr(r, "<item jid='Alice@ngage-auth' name='Alice' subscription='both'/>") != NULL);
    CHECK(strstr(r, "<presence from='Alice@ngage-auth/segachat' to='Bob@ngage-auth'><ext_pres GameClassId='1'/></presence>") !=
          NULL);
    CHECK(strstr(a.rx, "<item jid='Bob@ngage-auth' name='Bob' subscription='both'/>") != NULL);
    CHECK(strstr(a.rx, "<presence from='Bob@ngage-auth' to='Alice@ngage-auth' type='subscribed'/>") != NULL);
    CHECK(strstr(a.rx, "<presence from='Bob@ngage-auth/segachat' to='Alice@ngage-auth'/>") != NULL);
    r = say(&b, "<iq type='get' id='r3'><query xmlns='jabber:iq:roster'/></iq>");
    CHECK(strstr(r, "<item jid='Alice@ngage-auth' name='Alice' subscription='both'/>") != NULL);

    /* presence update (Play Now lobby) reaches the friend */
    clear(&b);
    say(&a, "<presence><ext_pres GameClassId='1'/><ext_pres Lobby='Scrimmage'/></presence>");
    CHECK(strstr(b.rx, "<ext_pres Lobby='Scrimmage'/>") != NULL);

    /* chat; a forged from is replaced */
    clear(&b);
    say(&a, "<message to='bob@ngage-auth' from='carol@ngage-auth' type='chat'><body>hello &amp; bye</body></message>");
    CHECK(strstr(b.rx, "<message from='Alice@ngage-auth/segachat' to='Bob@ngage-auth' type='chat'><body>hello &amp; "
                       "bye</body></message>") != NULL);

    /* unknown receiver */
    r = say(&a, "<message to='nobody@ngage-auth' id='m1'><body>?</body></message>");
    CHECK(strstr(r, "<message type='error' from='nobody@ngage-auth' to='Alice@ngage-auth/segachat' id='m1'><error "
                    "code='404'>") != NULL);
    r = say(&a, "<presence to='nobody@ngage-auth' type='subscribe'/>");
    CHECK(strstr(r, "type='unsubscribed'") != NULL);

    /* Bob leaves: Alice sees him go; a message waits for him */
    clear(&a);
    gcs_direct_tcp_gone(D, &b);
    CHECK(strstr(a.rx, "<presence from='Bob@ngage-auth/segachat' to='Alice@ngage-auth' type='unavailable'/>") != NULL);
    say(&a, "<message to='bob@ngage-auth' type='chat'><body>are you there?</body></message>");
    say(&a, "<message to='bob@ngage-auth' id='segachat_data_msg'><x>game data</x></message>"); /* not kept */

    clear(&a);
    login(&b, ipb, 4002, "Bob");
    r = say(&b, "<presence/>");
    CHECK(strstr(r, "<presence from='Alice@ngage-auth/segachat' to='Bob@ngage-auth'><ext_pres GameClassId='1'/>") != NULL);
    CHECK(strstr(r, "<body>are you there?</body><x xmlns='jabber:x:delay' from='ngage-auth' stamp='") != NULL);
    CHECK(!strstr(r, "game data"));
    CHECK(strstr(a.rx, "<presence from='Bob@ngage-auth/segachat' to='Alice@ngage-auth'/>") != NULL);

    /* Bob removes Alice (Remove friend) */
    clear(&a);
    r = say(&b, "<iq type='set' id='rm'><query xmlns='jabber:iq:roster'><item jid='alice@ngage-auth' "
                "subscription='remove'/></query></iq>");
    CHECK(strstr(r, "<iq type='result' id='rm'/>") != NULL);
    CHECK(strstr(r, "subscription='remove'") != NULL);
    CHECK(strstr(a.rx, "<item jid='Bob@ngage-auth' name='Bob' subscription='remove'/>") != NULL);
    CHECK(strstr(a.rx, "type='unavailable'") != NULL);

    /* a request while the other one is offline waits for his login;
     * "Do not authorise" */
    gcs_direct_tcp_gone(D, &b);
    say(&a, "<presence to='bob@ngage-auth' type='subscribe'><status>again</status></presence>");
    login(&b, ipb, 4003, "Bob");
    r = say(&b, "<presence/>");
    CHECK(strstr(r, "<presence from='Alice@ngage-auth' to='Bob@ngage-auth' type='subscribe'><status>again</status>") != NULL);
    clear(&a);
    say(&b, "<presence to='alice@ngage-auth' type='unsubscribed' id='segachat_buddy_accrej'/>");
    CHECK(strstr(a.rx, "<presence from='Bob@ngage-auth' to='Alice@ngage-auth' type='unsubscribed'/>") != NULL);
    CHECK(strstr(a.rx, "subscription='remove'") != NULL);

    gcs_native_get_stats(gcs_core_native(core), &nst);
    CHECK(nst.chat_messages >= 2);

    gcs_direct_tcp_gone(D, &a);
    gcs_direct_tcp_gone(D, &b);
    gcs_direct_destroy(D);
    gcs_core_destroy(core);
    remove(ACC);
    remove(RDATA);
}

int main(void)
{
    RUN(test_store);
    RUN(test_friends);
    TEST_MAIN_END();
}
