/* PhantomLink Arena core against the real server core: link handshake,
 * registration as an Arena peer, chunked HTTP (SOAP login), SNAP login
 * and room data between two phones.  SPDX-License-Identifier: MIT */
#include "../../protocol/tests/gc_test.h"
#include "gcs_core.h"
#include "pla_core.h"

#define CAP 65536
typedef struct
{
    pla_core core;
    gcs_conn conn;
    gc_u8 up[CAP];   /* phone -> server */
    size_t n_up;
    gc_u8 down[CAP]; /* server -> phone */
    size_t n_down;
    int states[8];
    int n_states;
    gca_msg got[32];
    gc_u8 raw[32][1100];
    int n_got;
    char http[4096];
    size_t n_http;
    int http_done;
} phone;

static gcs_core *S;
static phone P[2];
static gc_u32 now = 1000;

static void s_write(void *ctx, gcs_conn *conn, const gc_u8 *d, size_t n)
{
    phone *p = (phone *)conn->net;
    (void)ctx;
    CHECK(p->n_down + n <= CAP);
    memcpy(p->down + p->n_down, d, n);
    p->n_down += n;
}
static void s_close(void *ctx, gcs_conn *conn)
{
    (void)ctx;
    (void)conn;
}
static void s_log(void *ctx, int l, const char *line)
{
    (void)ctx;
    (void)l;
    if (getenv("GCS_TEST_LOG")) {
        printf("    | %s\n", line);
    }
}
static void p_write(void *ctx, const gc_u8 *d, size_t n)
{
    phone *p = (phone *)ctx;
    CHECK(p->n_up + n <= CAP);
    memcpy(p->up + p->n_up, d, n);
    p->n_up += n;
}
static void p_state(void *ctx, int st)
{
    phone *p = (phone *)ctx;
    if (p->n_states < 8) {
        p->states[p->n_states++] = st;
    }
}
static void p_http(void *ctx, const gca_msg *m)
{
    phone *p = (phone *)ctx;
    memcpy(p->http + p->n_http, m->payload, m->len);
    p->n_http += m->len;
    p->http[p->n_http] = 0;
    p->http_done = !(m->flags & GCA_F_MORE);
}
static void p_snap(void *ctx, const gca_msg *m)
{
    phone *p = (phone *)ctx;
    if (p->n_got < 32) {
        p->got[p->n_got] = *m;
        if (m->len) {
            memcpy(p->raw[p->n_got], m->payload, m->len);
            p->got[p->n_got].payload = p->raw[p->n_got];
        }
        p->n_got++;
    }
}

static void pump(int steps)
{
    int s, i;
    gc_u8 t[CAP];
    for (s = 0; s < steps; s++) {
        now += 10;
        for (i = 0; i < 2; i++) {
            phone *p = &P[i];
            size_t n;
            if (p->n_up) {
                n = p->n_up;
                memcpy(t, p->up, n);
                p->n_up = 0;
                gcs_core_conn_input(S, &p->conn, t, n, now);
            }
            if (p->n_down) {
                n = p->n_down;
                memcpy(t, p->down, n);
                p->n_down = 0;
                pla_input(&p->core, t, n, now);
            }
            pla_poll(&p->core, now);
        }
        gcs_core_poll(S, now);
    }
}

static void start_phone(int i, const char *name)
{
    pla_io io;
    gc_u8 id[6] = {0, 0x0e, 0x6d, 0, 0, 0};
    phone *p = &P[i];
    memset(p, 0, sizeof(*p));
    id[5] = (gc_u8)(i + 1);
    io.write = p_write;
    io.ctx = p;
    pla_init(&p->core, &io, name, id, 0x51000 + (gc_u32)i);
    pla_set_listener(&p->core, PLA_SVC_HTTP, p_http, p_state, p);
    pla_set_listener(&p->core, PLA_SVC_SNAP, p_snap, NULL, p);
    p->conn.net = p;
    gcs_core_conn_open(S, &p->conn, now);
    pla_transport_up(&p->core, now);
}

static gca_msg *got(int i, gc_u8 type)
{
    int k;
    for (k = P[i].n_got - 1; k >= 0; k--) {
        if (P[i].got[k].type == type) {
            return &P[i].got[k];
        }
    }
    return NULL;
}

static void test_end_to_end(void)
{
    gcs_config cfg;
    gcs_io io;
    gca_msg m, *r;
    char body[1300];
    size_t off = 0;
    int i;

    gcs_default_config(&cfg);
    io.write = s_write;
    io.close = s_close;
    io.log = s_log;
    S = gcs_core_create(&cfg, &io, NULL, 7);
    start_phone(0, "Alice");
    start_phone(1, "Bob");
    /* not ready before registration */
    memset(&m, 0, sizeof(m));
    m.type = GCA_MT_LOGIN;
    CHECK(pla_send(&P[0].core, &m, now) < 0);
    pump(20);
    CHECK_EQ(P[0].core.state, PLA_ST_READY);
    CHECK(P[0].n_states >= 3 && P[0].states[P[0].n_states - 1] == PLA_ST_READY);

    /* SOAP login in three chunks */
    memset(body, ' ', sizeof(body));
    memcpy(body, "<ns1:authenticateUser2>", 23);
    while (off < sizeof(body)) {
        size_t n = sizeof(body) - off > GCA_HTTP_CHUNK ? GCA_HTTP_CHUNK : sizeof(body) - off;
        memset(&m, 0, sizeof(m));
        m.type = GCA_MT_HTTP_REQ;
        m.req = 77;
        m.kind = GCA_HTTP_POST;
        strcpy(m.text, "http://arena.n-gage.com/n-gage/axis/services/Community");
        m.flags = (gc_u8)(off + n < sizeof(body) ? GCA_F_MORE : 0);
        m.payload = (const gc_u8 *)body + off;
        m.len = (gc_u16)n;
        CHECK_EQ(pla_send(&P[0].core, &m, now), GC_LINK_OK);
        off += n;
    }
    pump(20);
    CHECK(P[0].http_done);
    CHECK(strstr(P[0].http, "authenticateUser2Return") != NULL);

    /* SNAP: both log in, meet in a room, exchange data */
    for (i = 0; i < 2; i++) {
        memset(&m, 0, sizeof(m));
        m.type = GCA_MT_LOGIN;
        m.id = 0x101fd3dd;
        strcpy(m.name, i ? "Bob" : "Alice");
        CHECK_EQ(pla_send(&P[i].core, &m, now), GC_LINK_OK);
        CHECK(m.req != 0);
        pump(10);
        r = got(i, GCA_MT_LOGIN_RESULT);
        CHECK(r && r->result == 0 && r->req == m.req);
        memset(&m, 0, sizeof(m));
        m.type = GCA_MT_ENTER;
        m.kind = GCA_K_LOBBY;
        strcpy(m.name, "Scrimmage");
        pla_send(&P[i].core, &m, now);
        pump(10);
        memset(&m, 0, sizeof(m));
        m.type = GCA_MT_ENTER;
        m.kind = GCA_K_ROOM;
        m.flags = GCA_F_CREATE;
        m.value = 4;
        strcpy(m.name, "Auction");
        pla_send(&P[i].core, &m, now);
        pump(10);
        r = got(i, GCA_MT_ENTER_RESULT);
        CHECK(r && r->result == 0 && r->kind == GCA_K_ROOM);
    }
    memset(&m, 0, sizeof(m));
    m.type = GCA_MT_DATA;
    m.kind = GCA_K_ROOM;
    m.payload = (const gc_u8 *)"lot 7";
    m.len = 5;
    CHECK_EQ(pla_send(&P[1].core, &m, now), GC_LINK_OK);
    pump(10);
    r = got(0, GCA_MT_DATA);
    CHECK(r && r->len == 5 && !memcmp(r->payload, "lot 7", 5));
    gcs_core_destroy(S);
}

int main(void)
{
    RUN(test_end_to_end);
    TEST_MAIN_END();
}
