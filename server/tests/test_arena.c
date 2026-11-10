/* N-Gage Arena emulation tests: GCA codec round trips and the server
 * module (login, lobbies, rooms, data, chat, SOAP login).
 * SPDX-License-Identifier: MIT */
#include "../../protocol/tests/gc_test.h"
#include "gcs_arena.h"

#define NU   4
#define QMAX 64

typedef struct
{
    gca_msg q[QMAX];
    gc_u8 raw[QMAX][1100];
    int n;
    char http[4096];
    size_t http_len;
    int http_done;
    gc_u16 http_status;
} user;

static user U[NU];
static gcs_arena *A;

static void io_send(void *ctx, void *key, const gc_u8 *d, size_t n)
{
    user *u = (user *)key;
    gca_msg m;
    (void)ctx;
    CHECK(gca_decode(d, n, &m));
    if (m.type == GCA_MT_HTTP_RESP) {
        CHECK(u->http_len + m.len < sizeof(u->http));
        memcpy(u->http + u->http_len, m.payload, m.len);
        u->http_len += m.len;
        u->http[u->http_len] = 0;
        u->http_status = (gc_u16)m.value;
        u->http_done = !(m.flags & GCA_F_MORE);
        return;
    }
    if (u->n < QMAX) {
        memcpy(u->raw[u->n], d, n);
        gca_decode(u->raw[u->n], n, &u->q[u->n]);
        u->n++;
    }
}

static void io_log(void *ctx, int level, const char *line)
{
    (void)ctx;
    (void)level;
    if (getenv("GCS_TEST_LOG")) {
        printf("    | %s\n", line);
    }
}

static void put(int i, const gca_msg *m)
{
    gc_u8 b[1100];
    size_t n = gca_encode(m, b, sizeof(b));
    CHECK(n > 0);
    gcs_arena_input(A, &U[i], "phone", b, n);
}

static gca_msg *last(int i, gc_u8 type)
{
    int k;
    for (k = U[i].n - 1; k >= 0; k--) {
        if (U[i].q[k].type == type) {
            return &U[i].q[k];
        }
    }
    return NULL;
}

static int count(int i, gc_u8 type)
{
    int k, c = 0;
    for (k = 0; k < U[i].n; k++) {
        c += U[i].q[k].type == type;
    }
    return c;
}

static void reset(void)
{
    gcs_arena_io io;
    if (A) {
        gcs_arena_destroy(A);
    }
    memset(U, 0, sizeof(U));
    io.send = io_send;
    io.log = io_log;
    A = gcs_arena_create(&io, NULL);
    CHECK(A != NULL);
}

static gc_u32 login(int i, const char *name)
{
    gca_msg m;
    gca_msg *r;
    memset(&m, 0, sizeof(m));
    m.type = GCA_MT_LOGIN;
    m.req = 1;
    m.id = 0x101fd3dd;
    strcpy(m.name, name);
    strcpy(m.text, "secret");
    put(i, &m);
    r = last(i, GCA_MT_LOGIN_RESULT);
    CHECK(r != NULL);
    if (!r) {
        return 0;
    }
    CHECK_EQ(r->result, 0);
    return r->id;
}

static gca_msg *enter(int i, gc_u8 kind, const char *name, gc_u8 flags, gc_u32 max)
{
    gca_msg m;
    memset(&m, 0, sizeof(m));
    m.type = GCA_MT_ENTER;
    m.req = 2;
    m.kind = kind;
    m.flags = flags;
    m.value = max;
    strcpy(m.name, name);
    put(i, &m);
    return last(i, GCA_MT_ENTER_RESULT);
}

static void test_codec_roundtrip(void)
{
    gca_msg m, d;
    gc_u8 b[1100], body[600];
    size_t n;

    memset(&m, 0, sizeof(m));
    memset(body, 'x', sizeof(body));
    m.type = GCA_MT_HTTP_REQ;
    m.req = 7;
    m.kind = GCA_HTTP_POST;
    m.flags = GCA_F_MORE;
    strcpy(m.text, "http://arena.n-gage.com/n-gage/axis/services/Community");
    strcpy(m.text2, "\"authenticateUser2\"");
    m.payload = body;
    m.len = GCA_HTTP_CHUNK;
    n = gca_encode(&m, b, sizeof(b));
    CHECK(n > 0);
    CHECK(gca_decode(b, n, &d));
    CHECK_EQ(d.req, 7);
    CHECK_EQ(d.len, GCA_HTTP_CHUNK);
    CHECK(!strcmp(d.text, m.text));
    CHECK(!strcmp(d.text2, m.text2));
    /* every truncation is rejected */
    CHECK(!gca_decode(b, n - 1, &d));
    /* oversized payload is refused by the encoder */
    m.len = GCA_MAX_DATA + 1;
    CHECK_EQ(gca_encode(&m, b, sizeof(b)), 0);
    /* lists */
    memset(&m, 0, sizeof(m));
    m.type = GCA_MT_LIST;
    m.count = 2;
    m.e[1].id = 0x1234;
    strcpy(m.e[1].name, "Scrimmage");
    n = gca_encode(&m, b, sizeof(b));
    CHECK(gca_decode(b, n, &d));
    CHECK_EQ(d.count, 2);
    CHECK_EQ(d.e[1].id, 0x1234);
    CHECK(!strcmp(d.e[1].name, "Scrimmage"));
    b[5] = GCA_MAX_LIST + 1; /* count */
    CHECK(!gca_decode(b, n, &d));
    /* GCM types are not GCA */
    b[0] = 0x40;
    CHECK(!gca_decode(b, n, &d));
}

static void test_login_required_and_unique_names(void)
{
    gca_msg m, *r;
    reset();
    memset(&m, 0, sizeof(m));
    m.type = GCA_MT_ENTER;
    m.req = 5;
    strcpy(m.name, "Scrimmage");
    put(0, &m);
    r = last(0, GCA_MT_LOGIN_RESULT);
    CHECK(r && r->result == -6 && r->req == 5);
    CHECK(login(0, "Alice") != 0);
    memset(&m, 0, sizeof(m));
    m.type = GCA_MT_LOGIN;
    m.id = 0x101fd3dd;
    strcpy(m.name, "alice");
    put(1, &m);
    r = last(1, GCA_MT_LOGIN_RESULT);
    CHECK(r && r->result == -5);
}

static void test_lobby_room_flow(void)
{
    gca_msg *r, m;
    gc_u32 a, b, room;

    reset();
    a = login(0, "Alice");
    b = login(1, "Bob");
    CHECK(a != b);
    r = enter(0, GCA_K_LOBBY, "Scrimmage", 0, 0);
    CHECK(r && r->result == 0);
    r = enter(1, GCA_K_LOBBY, "scrimmage", 0, 0);
    CHECK(r && r->result == 0);
    CHECK(last(0, GCA_MT_MEMBER) && last(0, GCA_MT_MEMBER)->id == b);
    /* entering a missing room without CREATE fails */
    r = enter(0, GCA_K_ROOM, "Auction", 0, 0);
    CHECK(r && r->result == -1);
    r = enter(0, GCA_K_ROOM, "Auction", GCA_F_CREATE, 2);
    CHECK(r && r->result == 0 && (r->flags & GCA_F_HOST));
    room = r->id;
    r = enter(1, GCA_K_ROOM, "Auction", 0, 0);
    CHECK(r && r->result == 0 && r->id == room && !(r->flags & GCA_F_HOST));
    r = last(1, GCA_MT_MEMBER);
    CHECK(r && r->id == a && (r->flags & GCA_F_HOST) && r->kind == GCA_K_ROOM);
    /* room is full at two */
    login(2, "Carol");
    enter(2, GCA_K_LOBBY, "Scrimmage", 0, 0);
    r = enter(2, GCA_K_ROOM, "Auction", 0, 0);
    CHECK(r && r->result == -2);
    /* room list */
    memset(&m, 0, sizeof(m));
    m.type = GCA_MT_LIST_REQ;
    m.kind = GCA_K_ROOM;
    put(2, &m);
    r = last(2, GCA_MT_LIST);
    CHECK(r && r->count == 1 && r->e[0].users == 2 && r->e[0].max_users == 2);
    /* data to the room reaches Bob only, with Alice's id */
    memset(&m, 0, sizeof(m));
    m.type = GCA_MT_DATA;
    m.kind = GCA_K_ROOM;
    m.payload = (const gc_u8 *)"bid 100";
    m.len = 7;
    put(0, &m);
    r = last(1, GCA_MT_DATA);
    CHECK(r && r->id == a && r->len == 7 && !memcmp(r->payload, "bid 100", 7));
    CHECK_EQ(count(2, GCA_MT_DATA), 0);
    CHECK_EQ(count(0, GCA_MT_DATA), 0);
    /* private data to Carol in the same game */
    m.kind = GCA_K_USER;
    m.id = login(2, "Carol");
    put(0, &m);
    CHECK_EQ(count(2, GCA_MT_DATA), 1);
    /* room status: only the owner may set it */
    memset(&m, 0, sizeof(m));
    m.type = GCA_MT_STATUS;
    m.kind = GCA_K_ROOM;
    m.value = 0x5;
    m.mask = 0xF;
    put(1, &m);
    CHECK_EQ(count(0, GCA_MT_STATUS), 0);
    put(0, &m);
    r = last(1, GCA_MT_STATUS);
    CHECK(r && r->value == 5 && r->id == room);
    /* owner leaves: Bob becomes owner, Alice's departure is announced */
    U[1].n = 0;
    gcs_arena_peer_gone(A, &U[0]);
    CHECK_EQ(count(1, GCA_MT_MEMBER), 3); /* left room, new owner, left lobby */
    r = &U[1].q[1];
    CHECK(r->type == GCA_MT_MEMBER && r->kind == GCA_K_ROOM && r->id == b && (r->flags & GCA_F_HOST) &&
          (r->flags & GCA_F_JOINED));
    r = &U[1].q[2];
    CHECK(r->kind == GCA_K_LOBBY && r->id == a && !(r->flags & GCA_F_JOINED));
    {
        gcs_arena_stats st;
        gcs_arena_get_stats(A, &st);
        CHECK_EQ(st.users, 2);
        CHECK_EQ(st.rooms, 1);
        gcs_arena_peer_gone(A, &U[1]);
        gcs_arena_get_stats(A, &st);
        CHECK_EQ(st.rooms, 0);
    }
}

static void test_chat_and_presence(void)
{
    gca_msg m, *r;
    reset();
    login(0, "Alice");
    login(1, "Bob");
    memset(&m, 0, sizeof(m));
    m.type = GCA_MT_PRESENCE;
    m.kind = GCA_PRES_AVAILABLE;
    put(0, &m);
    r = last(1, GCA_MT_PRESENCE);
    CHECK(r && !strcmp(r->name, "Alice") && r->kind == GCA_PRES_AVAILABLE);
    put(1, &m);
    r = last(1, GCA_MT_PRESENCE);
    CHECK(r && !strcmp(r->name, "Alice")); /* newcomer learns who is online */
    memset(&m, 0, sizeof(m));
    m.type = GCA_MT_CHAT;
    strcpy(m.name, "bob");
    m.payload = (const gc_u8 *)"hi";
    m.len = 2;
    put(0, &m);
    r = last(1, GCA_MT_CHAT);
    CHECK(r && !strcmp(r->name, "Alice") && r->len == 2);
    memset(&m, 0, sizeof(m));
    m.type = GCA_MT_SERVICE;
    m.req = 9;
    strcpy(m.name, "retrieval1");
    m.payload = (const gc_u8 *)"<itemlist/>";
    m.len = 11;
    put(0, &m);
    r = last(0, GCA_MT_SERVICE_RESP);
    CHECK(r && r->req == 9 && r->result == 0);
    gcs_arena_peer_gone(A, &U[0]);
    r = last(1, GCA_MT_PRESENCE);
    CHECK(r && r->kind == GCA_PRES_UNAVAILABLE);
}

static void test_soap_login(void)
{
    static const char body[] =
        "<?xml version='1.0' encoding='UTF-8'?><soapenv:Envelope><soapenv:Body><ns1:authenticateUser2>"
        "<username xsi:type=\"xsd:string\">alice</username>"
        "<password xsi:type=\"xsd:string\">pw</password></ns1:authenticateUser2></soapenv:Body></soapenv:Envelope>";
    char big[1500];
    gca_msg m;
    size_t off = 0, len;

    reset();
    /* chunked request body, assembled by the server */
    memset(big, ' ', sizeof(big));
    memcpy(big, body, strlen(body));
    len = sizeof(big);
    while (off < len) {
        size_t n = len - off > GCA_HTTP_CHUNK ? GCA_HTTP_CHUNK : len - off;
        memset(&m, 0, sizeof(m));
        m.type = GCA_MT_HTTP_REQ;
        m.req = 3;
        m.kind = GCA_HTTP_POST;
        strcpy(m.text, "http://arena.n-gage.com/n-gage/axis/services/Community");
        m.flags = (gc_u8)(off + n < len ? GCA_F_MORE : 0);
        m.payload = (const gc_u8 *)big + off;
        m.len = (gc_u16)n;
        put(0, &m);
        off += n;
        CHECK_EQ(U[0].http_done, off == len);
    }
    CHECK_EQ(U[0].http_status, 200);
    CHECK(strstr(U[0].http, "<authenticateUser2Return xsi:type=\"xsd:boolean\">true</authenticateUser2Return>") != NULL);
    /* unknown URL */
    memset(&U[0].http, 0, sizeof(U[0].http));
    U[0].http_len = 0;
    memset(&m, 0, sizeof(m));
    m.type = GCA_MT_HTTP_REQ;
    m.req = 4;
    strcpy(m.text, "http://arena.n-gage.com/n-gage/servlets/update");
    put(0, &m);
    CHECK_EQ(U[0].http_status, 404);
}

static int hook_calls;
static int hook_data(void *ctx, gcs_arena *a, gc_u32 user, const gca_msg *m)
{
    gca_msg r;
    (void)ctx;
    hook_calls++;
    if (m->len && m->payload[0] == '!') {
        memset(&r, 0, sizeof(r));
        r.type = GCA_MT_DATA;
        r.kind = GCA_K_USER;
        r.payload = (const gc_u8 *)"ok";
        r.len = 2;
        CHECK(gcs_arena_send_to(a, user, &r));
        return 1; /* consumed: not relayed */
    }
    return 0;
}

static void test_game_modules(void)
{
    gcs_arena_game g;
    gca_msg m;

    reset();
    memset(&g, 0, sizeof(g));
    g.game = 0x101fd3dd;
    g.name = "Pocket Kingdom";
    g.on_data = hook_data;
    CHECK(gcs_arena_add_game(A, &g));
    CHECK(!gcs_arena_add_game(A, &g)); /* duplicate */
    login(0, "Alice");
    login(1, "Bob");
    enter(0, GCA_K_LOBBY, "L", 0, 0);
    enter(1, GCA_K_LOBBY, "L", 0, 0);
    enter(0, GCA_K_ROOM, "R", GCA_F_CREATE, 4);
    enter(1, GCA_K_ROOM, "R", 0, 0);
    memset(&m, 0, sizeof(m));
    m.type = GCA_MT_DATA;
    m.kind = GCA_K_ROOM;
    m.payload = (const gc_u8 *)"!x";
    m.len = 2;
    put(0, &m);
    CHECK_EQ(count(1, GCA_MT_DATA), 0);
    CHECK(last(0, GCA_MT_DATA) && last(0, GCA_MT_DATA)->len == 2);
    m.payload = (const gc_u8 *)"hi";
    put(0, &m);
    CHECK_EQ(count(1, GCA_MT_DATA), 1);
    CHECK_EQ(hook_calls, 2);
}

int main(void)
{
    RUN(test_codec_roundtrip);
    RUN(test_login_required_and_unique_names);
    RUN(test_lobby_room_flow);
    RUN(test_chat_and_presence);
    RUN(test_soap_login);
    RUN(test_game_modules);
    gcs_arena_destroy(A);
    TEST_MAIN_END();
}
