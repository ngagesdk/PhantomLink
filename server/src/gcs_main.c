/** @file gcs_main.c
 *
 *  GameComms endpoint server - network front end.
 *
 *  Single threaded, poll() based TCP server (WSAPoll on Windows) plus a
 *  tiny UDP responder that lets ESP32 bridges find the server on a LAN
 *  without configuration.
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#ifdef _WIN32
#define _WIN32_WINNT 0x0600
#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h> /* must precede bcrypt.h, which is not self-contained */

#include <bcrypt.h>
typedef SOCKET sock_t;
#define SOCK_INVALID   INVALID_SOCKET
#define sock_close     closesocket
#define poll           WSAPoll
#define SOCK_EAGAIN(e) ((e) == WSAEWOULDBLOCK)
#define SHUT_WR        SD_SEND
static int sock_err(void) { return WSAGetLastError(); }
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#endif
typedef int sock_t;
#define SOCK_INVALID   (-1)
#define sock_close     close
#define SOCK_EAGAIN(e) ((e) == EAGAIN || (e) == EWOULDBLOCK)
static int sock_err(void) { return errno; }
#endif

#ifndef SEND_FLAGS
#define SEND_FLAGS 0
#endif

#include "gcs_core.h"
#include "gcs_direct.h"
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define GCS_VERSION       "1.0.0"
#define GCS_DEFAULT_PORT  9888
#define GCS_DISCOVERY_REQ "GCDISCOVER1"
#define GCS_DISCOVERY_RSP "GCSERVER1"
#define MAX_CONNS         1024
/* A link never has more than GC_LINK_QUEUE frames in flight; even a full
 * window retransmitted several times fits easily. */
#define OUTBUF_MAX      (256 * 1024)
#define ACCEPT_PER_PASS 64 /* bounded work per poll pass (no starvation) */
#define UDP_PER_PASS    64
#define RATE_SLOTS      512

typedef struct
{
    gcs_conn conn; /* must be first */
    sock_t fd;
    int used;
    int closing;
    gc_u8 *out;
    size_t out_len;
    size_t out_cap;
    char addr[64];
    int limited; /* counts against the per-address limits */
} net_conn;

/* per source address new-connection token bucket */
typedef struct
{
    char addr[64];
    gc_u32 t_last;
    gc_u32 tokens_milli; /* tokens * 1000 */
} rate_slot;

static net_conn g_conns[MAX_CONNS];
static rate_slot g_rate[RATE_SLOTS];
static int g_max_conns_per_ip = 16;
static int g_conn_burst = 10;
static int g_conn_rate = 2;      /* new connections per second per address */
static int g_limit_loopback = 0; /* tunnels terminate on loopback */
static gc_u32 g_conn_counter;
static gcs_core *g_core;

/* direct Arena clients (GPRS, gcs_direct.h) */
typedef struct
{
    sock_t fd;
    int used;
    int closing;   /* close after this pass (output dropped)      */
    int fin_req;   /* gcs_direct asked for FIN after the output    */
    int fin_sent;  /* our sending side is shut down               */
    int peer_eof;  /* the client's sending side is shut down      */
    gc_u32 t_last; /* last traffic                                */
    gc_u8 *out;
    size_t out_len;
    size_t out_cap;
    char addr[64];
} direct_conn;

#define DIRECT_OUT_MAX (64 * 1024) /* answers are small pages and XML */
static direct_conn g_dconns[GCSD_MAX_CONNS];
static gcs_direct *g_direct;
static sock_t g_dudp = SOCK_INVALID; /* SNAP */
static gc_u32 g_direct_idle_ms = 120000;
static volatile sig_atomic_t g_stop;
static volatile sig_atomic_t g_dump;
static int g_log_level = 2;
static FILE *g_logfile;

static gc_u32 now_ms(void)
{
#ifdef _WIN32
    return (gc_u32)GetTickCount();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (gc_u32)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
#endif
}

static void log_line(void *ctx, int level, const char *line)
{
    static const char *lv[] = { "ERR", "WRN", "INF", "DBG" };
    char ts[32];
    time_t t = time(NULL);
    struct tm tmb;
    struct tm *tm;
    (void)ctx;

#ifdef _WIN32
    tm = localtime_s(&tmb, &t) == 0 ? &tmb : NULL;
#else
    tm = localtime_r(&t, &tmb);
#endif
    if (!tm || strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm) == 0) {
        snprintf(ts, sizeof(ts), "%lld", (long long)t);
    }
    fprintf(g_logfile ? g_logfile : stdout, "%s %s %s\n", ts, lv[level < 0 ? 0 : level > 3 ? 3
                                                                                           : level],
            line);
    fflush(g_logfile ? g_logfile : stdout);
}

static void logmsg(int level, const char *fmt, ...)
{
    char line[512];
    va_list ap;
    if (level > g_log_level) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    log_line(NULL, level, line);
}

/* ---- randomness ---- */

/* Fill buf from the operating system CSPRNG; 0 on success. */
static int os_random(void *buf, size_t n)
{
#ifdef _WIN32
    return BCRYPT_SUCCESS(BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG)) ? 0 : -1;
#else
    static FILE *f;
    if (!f) {
        f = fopen("/dev/urandom", "rb");
        if (!f) {
            return -1;
        }
        setvbuf(f, NULL, _IONBF, 0);
    }
    return fread(buf, 1, n, f) == n ? 0 : -1;
#endif
}

static gc_u32 core_random(void *ctx)
{
    gc_u32 v;
    (void)ctx;
    if (os_random(&v, sizeof(v)) != 0) {
        /* checked at startup; failing later is fatal rather than weak */
        fprintf(stderr, "fatal: operating system random source failed\n");
        abort();
    }
    return v;
}

/* ---- per source address limits ---- */

static int is_loopback(const char *a)
{
    return strncmp(a, "127.", 4) == 0 || strcmp(a, "::1") == 0 || strncmp(a, "::ffff:127.", 11) == 0;
}

static int conns_from(const char *addr)
{
    int i, n = 0;
    for (i = 0; i < MAX_CONNS; i++) {
        if (g_conns[i].used && g_conns[i].limited && strcmp(g_conns[i].addr, addr) == 0) {
            n++;
        }
    }
    return n;
}

/* token bucket: 'burst' connections, refilled at 'per_sec' per second */
static int rate_allow(const char *addr, gc_u32 now, int burst, int per_sec)
{
    rate_slot *r = NULL, *oldest = &g_rate[0];
    int i;
    gc_u32 dt, tokens;

    for (i = 0; i < RATE_SLOTS; i++) {
        if (g_rate[i].addr[0] && strcmp(g_rate[i].addr, addr) == 0) {
            r = &g_rate[i];
            break;
        }
        if (!g_rate[i].addr[0] || GC_TIME_BEFORE(g_rate[i].t_last, oldest->t_last)) {
            oldest = &g_rate[i];
        }
    }
    if (!r) {
        r = oldest;
        snprintf(r->addr, sizeof(r->addr), "%s", addr);
        r->tokens_milli = (gc_u32)burst * 1000u;
        r->t_last = now;
    }
    dt = (gc_u32)(now - r->t_last);
    r->t_last = now;
    if (dt > 3600000u) {
        dt = 3600000u;
    }
    tokens = r->tokens_milli + dt * (gc_u32)per_sec;
    if (tokens > (gc_u32)burst * 1000u) {
        tokens = (gc_u32)burst * 1000u;
    }
    if (tokens < 1000u) {
        r->tokens_milli = tokens;
        return 0;
    }
    r->tokens_milli = tokens - 1000u;
    return 1;
}

static int set_nonblocking(sock_t s)
{
#ifdef _WIN32
    u_long one = 1;
    return ioctlsocket(s, FIONBIO, &one) == 0 ? 0 : -1;
#else
    int fl = fcntl(s, F_GETFL, 0);
    return fl < 0 ? -1 : fcntl(s, F_SETFL, fl | O_NONBLOCK);
#endif
}

/* ---- core I/O callbacks ---- */

static void try_flush(net_conn *c)
{
    while (c->out_len > 0) {
        size_t chunk = c->out_len > (size_t)INT_MAX ? (size_t)INT_MAX : c->out_len;
        int n = (int)send(c->fd, (const char *)c->out, (int)chunk, SEND_FLAGS);
        if (n > 0) {
            memmove(c->out, c->out + n, c->out_len - (size_t)n);
            c->out_len -= (size_t)n;
        } else {
            if (n < 0 && SOCK_EAGAIN(sock_err())) {
                return;
            }
            c->closing = 1; /* write error */
            return;
        }
    }
}

static void io_write(void *ctx, gcs_conn *conn, const gc_u8 *data, size_t len)
{
    net_conn *c = (net_conn *)conn;
    size_t needed;
    (void)ctx;
    if (c->closing || len == 0) {
        return;
    }
    if (!data || c->out_len > OUTBUF_MAX || len > OUTBUF_MAX - c->out_len) {
        logmsg(1, "%s: output buffer limit exceeded, closing", c->addr);
        c->closing = 1;
        return;
    }
    needed = c->out_len + len;
    if (needed > c->out_cap) {
        size_t cap = c->out_cap ? c->out_cap : 4096;
        gc_u8 *n;
        while (cap < needed) {
            cap = cap > OUTBUF_MAX / 2 ? OUTBUF_MAX : cap * 2;
        }
        n = (gc_u8 *)realloc(c->out, cap);
        if (!n) {
            logmsg(1, "%s: cannot allocate output buffer, closing", c->addr);
            c->closing = 1;
            return;
        }
        c->out = n;
        c->out_cap = cap;
    }
    memcpy(c->out + c->out_len, data, len);
    c->out_len = needed;
    try_flush(c);
}

static void io_close(void *ctx, gcs_conn *conn)
{
    (void)ctx;
    ((net_conn *)conn)->closing = 1; /* closed after the current pass */
}

static void conn_free(net_conn *c, gc_u32 now)
{
    gcs_core_conn_closed(g_core, &c->conn, now);
    try_flush(c);
    sock_close(c->fd);
    free(c->out);
    logmsg(3, "%s: closed", c->addr);
    memset(c, 0, sizeof(*c));
    c->fd = SOCK_INVALID;
}

/* ---- direct Arena clients (GPRS) ---- */

static void dconn_flush(direct_conn *c)
{
    while (c->out_len > 0) {
        int n = (int)send(c->fd, (const char *)c->out, (int)c->out_len, SEND_FLAGS);
        if (n > 0) {
            memmove(c->out, c->out + n, c->out_len - (size_t)n);
            c->out_len -= (size_t)n;
        } else {
            if (n < 0 && SOCK_EAGAIN(sock_err())) {
                return;
            }
            c->closing = 1;
            return;
        }
    }
    if (c->fin_req && !c->fin_sent) {
        shutdown(c->fd, SHUT_WR);
        c->fin_sent = 1;
    }
    if (c->fin_sent && c->peer_eof) {
        c->closing = 1; /* both directions done */
    }
}

static void direct_tcp_send(void *ctx, void *sock, const gc_u8 *d, size_t len)
{
    direct_conn *c = (direct_conn *)sock;
    (void)ctx;
    if (c->closing || c->fin_req || !len) {
        return;
    }
    if (len > DIRECT_OUT_MAX - c->out_len) {
        logmsg(1, "direct %s: output buffer limit exceeded, closing", c->addr);
        c->closing = 1;
        return;
    }
    if (c->out_len + len > c->out_cap) {
        size_t cap = c->out_cap ? c->out_cap : 2048;
        gc_u8 *n;
        while (cap < c->out_len + len) {
            cap *= 2;
        }
        n = (gc_u8 *)realloc(c->out, cap);
        if (!n) {
            c->closing = 1;
            return;
        }
        c->out = n;
        c->out_cap = cap;
    }
    memcpy(c->out + c->out_len, d, len);
    c->out_len += len;
    dconn_flush(c);
}

static void direct_tcp_close(void *ctx, void *sock, int reset)
{
    direct_conn *c = (direct_conn *)sock;
    (void)ctx;
    if (reset) {
        c->closing = 1;
    } else {
        c->fin_req = 1;
        dconn_flush(c);
    }
}

static void direct_udp_send(void *ctx, gc_u16 service, const gc_u8 ip[4], gc_u16 port, const gc_u8 *d, size_t len)
{
    struct sockaddr_in sa;
    (void)ctx;
    if (service != GCSD_SNAP || g_dudp == SOCK_INVALID) {
        return;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    memcpy(&sa.sin_addr, ip, 4);
    sendto(g_dudp, (const char *)d, (int)len, 0, (struct sockaddr *)&sa, sizeof(sa));
}

static void dconn_free(direct_conn *c)
{
    gcs_direct_tcp_gone(g_direct, c);
    sock_close(c->fd);
    free(c->out);
    logmsg(3, "direct %s: closed", c->addr);
    memset(c, 0, sizeof(*c));
    c->fd = SOCK_INVALID;
}

/* IPv4 address of a peer (also v4-mapped IPv6); 0 if there is none */
static int sockaddr_ip4(const struct sockaddr_storage *sa, gc_u8 ip[4], gc_u16 *port)
{
    if (sa->ss_family == AF_INET) {
        const struct sockaddr_in *v4 = (const struct sockaddr_in *)sa;
        memcpy(ip, &v4->sin_addr, 4);
        *port = ntohs(v4->sin_port);
        return 1;
    }
    if (sa->ss_family == AF_INET6) {
        static const gc_u8 mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
        const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)sa;
        const gc_u8 *a = (const gc_u8 *)&v6->sin6_addr;
        if (!memcmp(a, mapped, 12)) {
            memcpy(ip, a + 12, 4);
            *port = ntohs(v6->sin6_port);
            return 1;
        }
    }
    return 0;
}

static int dconns_from(const char *addr)
{
    int i, n = 0;
    for (i = 0; i < GCSD_MAX_CONNS; i++) {
        if (g_dconns[i].used && strcmp(g_dconns[i].addr, addr) == 0) {
            n++;
        }
    }
    return n;
}

static void direct_accept(sock_t lfd, gc_u16 service, gc_u32 now)
{
    int budget;
    for (budget = 0; budget < ACCEPT_PER_PASS; budget++) {
        struct sockaddr_storage sa;
        socklen_t sl = sizeof(sa);
        sock_t fd = accept(lfd, (struct sockaddr *)&sa, &sl);
        direct_conn *c = NULL;
        gc_u8 ip[4];
        gc_u16 port;
        char addr[64];
        int i, one = 1;

        if (fd == SOCK_INVALID) {
            return;
        }
        if (sl > (socklen_t)sizeof(sa) || !sockaddr_ip4(&sa, ip, &port)) {
            sock_close(fd); /* the N-Gage only speaks IPv4 */
            continue;
        }
        snprintf(addr, sizeof(addr), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
        if (dconns_from(addr) >= g_max_conns_per_ip) {
            logmsg(1, "direct %s: too many connections from this address, refusing", addr);
            sock_close(fd);
            continue;
        }
        if (!rate_allow(addr, now, g_conn_burst, g_conn_rate)) {
            logmsg(3, "direct %s: connection rate limit, refusing", addr);
            sock_close(fd);
            continue;
        }
        for (i = 0; i < GCSD_MAX_CONNS; i++) {
            if (!g_dconns[i].used) {
                c = &g_dconns[i];
                break;
            }
        }
        if (!c || set_nonblocking(fd) != 0) {
            logmsg(1, "direct %s: connection table full", addr);
            sock_close(fd);
            continue;
        }
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
        memset(c, 0, sizeof(*c));
        c->used = 1;
        c->fd = fd;
        c->t_last = now;
        /* by address without the port: the per address limit counts these */
        snprintf(c->addr, sizeof(c->addr), "%s", addr);
        logmsg(3, "direct %s:%u: connected to port %u", addr, port, service);
        if (gcs_direct_tcp_open(g_direct, c, ip, port, service, now) != 0) {
            sock_close(fd);
            memset(c, 0, sizeof(*c));
            c->fd = SOCK_INVALID;
        }
    }
}

static void direct_udp(sock_t ufd, gc_u32 now)
{
    int k;
    for (k = 0; k < UDP_PER_PASS; k++) {
        gc_u8 buf[2048];
        struct sockaddr_storage sa;
        socklen_t sl = sizeof(sa);
        gc_u8 ip[4];
        gc_u16 port;
        int n = (int)recvfrom(ufd, (char *)buf, sizeof(buf), 0, (struct sockaddr *)&sa, &sl);
        if (n < 0) {
            return;
        }
        if (sl > (socklen_t)sizeof(sa) || !sockaddr_ip4(&sa, ip, &port)) {
            continue;
        }
        gcs_direct_udp_input(g_direct, GCSD_SNAP, ip, port, buf, (size_t)n, now);
    }
}

static void direct_input(direct_conn *c, short revents, gc_u32 now)
{
    if (revents & POLLNVAL) {
        c->closing = 1;
        return;
    }
    if (revents & POLLOUT) {
        dconn_flush(c);
    }
    if (!c->closing && !c->peer_eof && (revents & (POLLIN | POLLHUP | POLLERR))) {
        gc_u8 buf[4096];
        int k = (int)recv(c->fd, (char *)buf, sizeof(buf), 0);
        if (k > 0) {
            c->t_last = now;
            gcs_direct_tcp_input(g_direct, c, buf, (size_t)k, now);
        } else if (k == 0) {
            c->peer_eof = 1;
            gcs_direct_tcp_eof(g_direct, c, now);
            if (c->fin_sent || (!c->out_len && !c->fin_req)) {
                c->closing = 1; /* nothing more will be sent */
            }
        } else if (!SOCK_EAGAIN(sock_err())) {
            c->closing = 1;
        }
    } else if (c->peer_eof && (revents & (POLLHUP | POLLERR))) {
        c->closing = 1;
    }
}

/* ---- sockets ---- */

static sock_t open_listener(const char *bind_addr, int port, int type)
{
    struct addrinfo hints, *res = NULL, *ai;
    char ports[16];
    sock_t s = SOCK_INVALID;
    int one = 1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = type;
    hints.ai_flags = AI_PASSIVE;
    sprintf(ports, "%d", port);
    if (getaddrinfo(bind_addr[0] ? bind_addr : NULL, ports, &hints, &res) != 0) {
        return SOCK_INVALID;
    }
    /* prefer IPv4 for maximum compatibility with the ESP32 lwIP default */
    for (ai = res; ai; ai = ai->ai_next) {
        if (ai->ai_family == AF_INET) {
            break;
        }
    }
    if (!ai) {
        ai = res;
    }
    if (!ai) {
        return SOCK_INVALID;
    }
    s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (s != SOCK_INVALID) {
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
        if (bind(s, ai->ai_addr, (int)ai->ai_addrlen) != 0 ||
            (type == SOCK_STREAM && listen(s, 64) != 0) || set_nonblocking(s) != 0) {
            sock_close(s);
            s = SOCK_INVALID;
        }
    }
    freeaddrinfo(res);
    return s;
}

static void accept_new(sock_t lfd, gc_u32 now)
{
    int budget;

    for (budget = 0; budget < ACCEPT_PER_PASS; budget++) {
        struct sockaddr_storage sa;
        socklen_t sl = sizeof(sa);
        sock_t fd = accept(lfd, (struct sockaddr *)&sa, &sl);
        int i, one = 1;
        net_conn *c = NULL;
        char addr[64];

        if (fd == SOCK_INVALID) {
            return;
        }
        if (sl > (socklen_t)sizeof(sa) ||
            getnameinfo((struct sockaddr *)&sa, sl, addr, sizeof(addr), NULL, 0, NI_NUMERICHOST) != 0) {
            /* unknown source: unique placeholder that never matches another */
            snprintf(addr, sizeof(addr), "?unknown-%u", (unsigned)++g_conn_counter);
        }
        addr[sizeof(addr) - 1] = '\0';
        if (g_limit_loopback || !is_loopback(addr)) {
            if (conns_from(addr) >= g_max_conns_per_ip) {
                logmsg(1, "%s: too many connections from this address, refusing", addr);
                sock_close(fd);
                continue;
            }
            if (!rate_allow(addr, now, g_conn_burst, g_conn_rate)) {
                logmsg(3, "%s: connection rate limit, refusing", addr);
                sock_close(fd);
                continue;
            }
        }
        if (set_nonblocking(fd) != 0) {
            logmsg(1, "cannot set accepted socket nonblocking");
            sock_close(fd);
            continue;
        }
        for (i = 0; i < MAX_CONNS; i++) {
            if (!g_conns[i].used) {
                c = &g_conns[i];
                break;
            }
        }
        if (!c) {
            logmsg(1, "connection table full");
            sock_close(fd);
            continue;
        }
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (const char *)&one, sizeof(one));
        memset(c, 0, sizeof(*c));
        c->used = 1;
        c->fd = fd;
        c->conn.net = c;
        c->limited = g_limit_loopback || !is_loopback(addr);
        snprintf(c->addr, sizeof(c->addr), "%s", addr);
        snprintf(c->conn.addr, sizeof(c->conn.addr), "%s", addr);
        logmsg(3, "%s: connected", c->addr);
        gcs_core_conn_open(g_core, &c->conn, now);
    }
}

static void udp_discovery(sock_t ufd, int tcp_port, const char *name, gc_u32 now)
{
    /* global response budget: the reply is larger than the request, so an
     * unlimited responder is a (small) reflection amplifier */
    static gc_u32 t_last;
    static int budget = 20;
    int k;

    if ((gc_u32)(now - t_last) >= 1000u) {
        t_last = now;
        budget = 20;
    }
    for (k = 0; k < UDP_PER_PASS; k++) {
        char buf[64];
        char rsp[96];
        struct sockaddr_storage sa;
        socklen_t sl = sizeof(sa);
        int n = (int)recvfrom(ufd, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&sa, &sl);

        if (n <= 0) {
            return;
        }
        buf[n] = '\0';
        if ((size_t)n < strlen(GCS_DISCOVERY_REQ) || strncmp(buf, GCS_DISCOVERY_REQ, strlen(GCS_DISCOVERY_REQ)) != 0) {
            continue;
        }
        if (sl > (socklen_t)sizeof(sa) || budget <= 0) {
            continue;
        }
        budget--;
        n = snprintf(rsp, sizeof(rsp), "%s %d %s", GCS_DISCOVERY_RSP, tcp_port, name);
        if (n < 0) {
            continue;
        }
        if (n >= (int)sizeof(rsp)) {
            n = (int)sizeof(rsp) - 1;
        }
        sendto(ufd, rsp, n, 0, (struct sockaddr *)&sa, sl);
    }
}

#ifndef _WIN32
static void on_signal(int sig)
{
    if (sig == SIGUSR1) {
        g_dump = 1;
    } else {
        g_stop = 1;
    }
}
#else
static BOOL WINAPI on_console(DWORD ev)
{
    (void)ev;
    g_stop = 1;
    return TRUE;
}
#endif

/* ---- configuration ---- */

typedef struct
{
    char bind_addr[64];
    int allow_plaintext_network;
    int port;
    int discovery;
    gcs_config core;
    char logfile[256];
    int status_interval;
    int direct; /* direct Arena clients (GPRS) */
    char direct_bind[64];
    int direct_http_port; /* 0: off */
    int direct_xmpp_port;
    int direct_snap_port;
} app_cfg;

static void trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t') {
        memmove(s, s + 1, strlen(s));
    }
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) {
        *--e = '\0';
    }
}

/* strict decimal parse with range check; 1 on success */
static int parse_long(const char *val, long lo, long hi, long *out)
{
    char *end;
    long v;

    if (!val || !*val) {
        return 0;
    }
    errno = 0;
    v = strtol(val, &end, 10);
    if (errno != 0 || *end != '\0' || v < lo || v > hi) {
        return 0;
    }
    *out = v;
    return 1;
}

/* bounded string option: values that do not fit are an error, never
 * silently truncated (a truncated key would still "work" but be weaker) */
static int set_str(char *dst, size_t cap, const char *val)
{
    size_t n = strlen(val);
    if (n >= cap) {
        return 0;
    }
    memcpy(dst, val, n + 1);
    return 1;
}

/* returns 1 ok, 0 unknown key, -1 invalid value */
static int set_option(app_cfg *a, const char *key, const char *val)
{
    long v;
#define NUM(name, lo, hi, dst)                  \
    if (!strcmp(key, name)) {                   \
        if (!parse_long(val, (lo), (hi), &v)) { \
            return -1;                          \
        }                                       \
        dst = v;                                \
        return 1;                               \
    }
    if (!strcmp(key, "bind")) {
        return set_str(a->bind_addr, sizeof(a->bind_addr), val) ? 1 : -1;
    }
    if (!strcmp(key, "name")) {
        return set_str(a->core.server_name, sizeof(a->core.server_name), val) ? 1 : -1;
    }
    if (!strcmp(key, "access_key")) {
        return set_str(a->core.access_key, sizeof(a->core.access_key), val) ? 1 : -1;
    }
    if (!strcmp(key, "board_file")) {
        return *val && set_str(a->core.board_file, sizeof(a->core.board_file), val) ? 1 : -1;
    }
    if (!strcmp(key, "arena_accounts")) {
        return set_str(a->core.arena_accounts, sizeof(a->core.arena_accounts), val) ? 1 : -1;
    }
    if (!strcmp(key, "www_root")) {
        return set_str(a->core.www_root, sizeof(a->core.www_root), val) ? 1 : -1;
    }
    if (!strcmp(key, "rifts_data")) {
        return set_str(a->core.rifts_data, sizeof(a->core.rifts_data), val) ? 1 : -1;
    }
    if (!strcmp(key, "direct_bind")) {
        return set_str(a->direct_bind, sizeof(a->direct_bind), val) ? 1 : -1;
    }
    if (!strcmp(key, "log_file")) {
        return set_str(a->logfile, sizeof(a->logfile), val) ? 1 : -1;
    }
    NUM("allow_plaintext_network", 0, 1, a->allow_plaintext_network)
    NUM("port", 1, 65535, a->port)
    NUM("discovery", 0, 1, a->discovery)
    NUM("max_peers", 1, MAX_CONNS, a->core.max_peers)
    NUM("max_sessions", 1, 65536, a->core.max_sessions)
    NUM("link_grace_ms", 0, 86400000L, a->core.link_grace_ms)
    NUM("reserve_ms", 0, 86400000L, a->core.reserve_ms)
    NUM("pending_ms", 0, 86400000L, a->core.pending_ms)
    NUM("max_outq", 1, 4096, a->core.max_outq)
    NUM("log_level", 0, 3, a->core.log_level)
    NUM("status_interval", 0, 86400, a->status_interval)
    NUM("hello_timeout_ms", 1000, 600000L, a->core.hello_timeout_ms)
    NUM("max_auth_failures", 1, 100, a->core.max_auth_failures)
    NUM("max_conns_per_ip", 1, MAX_CONNS, g_max_conns_per_ip)
    NUM("conn_burst", 1, 10000, g_conn_burst)
    NUM("conn_rate", 1, 10000, g_conn_rate)
    NUM("limit_loopback", 0, 1, g_limit_loopback)
    NUM("direct", 0, 1, a->direct)
    NUM("direct_http_port", 0, 65535, a->direct_http_port)
    NUM("direct_xmpp_port", 0, 65535, a->direct_xmpp_port)
    NUM("direct_snap_port", 0, 65535, a->direct_snap_port)
    NUM("direct_idle_ms", 10000, 3600000L, g_direct_idle_ms)
#undef NUM
    return 0;
}

static int load_config(app_cfg *a, const char *path)
{
    FILE *f = fopen(path, "r");
    char line[512];
    int ln = 0;
    if (!f) {
        fprintf(stderr, "cannot open config file %s\n", path);
        return 0;
    }
    while (fgets(line, sizeof(line), f)) {
        char *eq;
        ln++;
        if (!strchr(line, '\n') && !feof(f)) {
            fprintf(stderr, "%s:%d: line too long\n", path, ln);
            fclose(f);
            return 0;
        }
        trim(line);
        if (!line[0] || line[0] == '#' || line[0] == ';' || line[0] == '[') {
            continue;
        }
        eq = strchr(line, '=');
        if (!eq) {
            fprintf(stderr, "%s:%d: expected key = value\n", path, ln);
            continue;
        }
        *eq = '\0';
        trim(line);
        trim(eq + 1);
        switch (set_option(a, line, eq + 1)) {
        case 0:
            fprintf(stderr, "%s:%d: unknown option '%s'\n", path, ln, line);
            break;
        case -1:
            fprintf(stderr, "%s:%d: invalid value for '%s'\n", path, ln, line);
            fclose(f);
            return 0;
        default:
            break;
        }
    }
    fclose(f);
    return 1;
}

static void usage(const char *argv0)
{
    printf("GameComms endpoint server %s\n\n"
           "usage: %s [-c file] [--key=value ...]\n\n"
           "  -c FILE              read options from FILE (key = value)\n"
           "  --port=N             TCP port (default %d)\n"
           "  --bind=ADDR          bind address (default 127.0.0.1)\n"
           "  --allow_plaintext_network=1\n"
           "                       allow non-loopback bind; requires access_key and a secured network\n"
           "  --discovery=0|1      answer LAN discovery on UDP port (default 1)\n"
           "  --name=TEXT          server name shown to devices\n"
           "  --access_key=TEXT    require this key from bridges (default none)\n"
           "  --board_file=PATH    persistent message board file\n"
           "  --arena_accounts=PATH Arena accounts, shared by all Arena games (empty: not kept)\n"
           "  --rifts_data=PATH    Rifts: Promise of Power battles and turns (empty: not kept)\n"
           "  --www_root=DIR       Arena device pages: news, support, lists (empty: placeholders)\n"
           "  --max_peers=N --max_sessions=N\n"
           "  --link_grace_ms=N    keep silent links for resumption (default 20000)\n"
           "  --reserve_ms=N       keep slots of lost players (default 180000)\n"
           "  --pending_ms=N       confirmation timeout (default 60000)\n"
           "  --log_level=0..3 --log_file=PATH --status_interval=SECONDS\n"
           "  --hello_timeout_ms=N drop connections without handshake (default 10000)\n"
           "  --max_auth_failures=N bad access keys per connection (default 3)\n"
           "  --max_conns_per_ip=N --conn_burst=N --conn_rate=N\n"
           "                       per source address limits (default 16, 10, 2/s)\n"
           "  --limit_loopback=0|1 apply them to loopback too (default 0: tunnels)\n"
           "  --direct=0|1         serve Arena games that connect on their own, e.g. over\n"
           "                       GPRS (default 0; docs/ARENA.md, Arena over GPRS)\n"
           "  --direct_bind=ADDR   their address (default 0.0.0.0: all IPv4 interfaces)\n"
           "  --direct_http_port=N --direct_xmpp_port=N --direct_snap_port=N\n"
           "                       listening ports for the game's ports 80, 5222 and UDP 9090\n"
           "                       (defaults 80, 5222, 9090; 0 = off)\n"
           "  --direct_idle_ms=N   close silent direct TCP connections (default 120000)\n",
           GCS_VERSION, argv0, GCS_DEFAULT_PORT);
}

int main(int argc, char *argv[])
{
    app_cfg a;
    gcs_io io;
    sock_t lfd, ufd = SOCK_INVALID;
    sock_t dhttp = SOCK_INVALID, dxmpp = SOCK_INVALID;
    int i;
    gc_u32 last_status;
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    memset(&a, 0, sizeof(a));
    snprintf(a.bind_addr, sizeof(a.bind_addr), "%s", "127.0.0.1");
    a.port = GCS_DEFAULT_PORT;
    a.discovery = 1;
    a.status_interval = 300;
    snprintf(a.direct_bind, sizeof(a.direct_bind), "%s", "0.0.0.0");
    a.direct_http_port = GCSD_HTTP;
    a.direct_xmpp_port = GCSD_XMPP;
    a.direct_snap_port = GCSD_SNAP;
    gcs_default_config(&a.core);

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        } else if (!strcmp(argv[i], "-c") && i + 1 < argc) {
            if (!load_config(&a, argv[++i])) {
                return 2;
            }
        } else if (!strncmp(argv[i], "--", 2) && strchr(argv[i], '=')) {
            char key[64];
            const char *eq = strchr(argv[i], '=');
            size_t kl = (size_t)(eq - argv[i] - 2);
            int rc;
            if (kl >= sizeof(key)) {
                fprintf(stderr, "unknown option %s\n", argv[i]);
                return 2;
            }
            memcpy(key, argv[i] + 2, kl);
            key[kl] = '\0';
            rc = set_option(&a, key, eq + 1);
            if (rc <= 0) {
                fprintf(stderr, "%s option %s\n", rc ? "invalid value for" : "unknown", argv[i]);
                return 2;
            }
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (strcmp(a.bind_addr, "127.0.0.1") != 0 && strcmp(a.bind_addr, "::1") != 0) {
        if (!a.allow_plaintext_network) {
            fprintf(stderr,
                    "refusing non-loopback bind '%s': TCP is unencrypted; use a secured tunnel or explicitly set "
                    "allow_plaintext_network=1 on a secured network\n",
                    a.bind_addr);
            return 2;
        }
        if (!a.core.access_key[0]) {
            fprintf(stderr, "refusing non-loopback bind '%s' without an access_key\n", a.bind_addr);
            return 2;
        }
        fprintf(stderr, "WARNING: non-loopback TCP is unencrypted; restrict it to a secured tunnel/VPN interface\n");
    }
    g_log_level = a.core.log_level;
    if (a.logfile[0]) {
        g_logfile = fopen(a.logfile, "a");
        if (!g_logfile) {
            fprintf(stderr, "cannot open log file %s, logging to stdout\n", a.logfile);
        }
    }
    {
        gc_u32 probe;
        if (os_random(&probe, sizeof(probe)) != 0) {
            fprintf(stderr, "no operating system random source available\n");
            return 1;
        }
    }

    for (i = 0; i < MAX_CONNS; i++) {
        g_conns[i].fd = SOCK_INVALID;
    }
    lfd = open_listener(a.bind_addr, a.port, SOCK_STREAM);
    if (lfd == SOCK_INVALID) {
        logmsg(0, "cannot listen on %s:%d", a.bind_addr[0] ? a.bind_addr : "*", a.port);
        return 1;
    }
    if (a.discovery) {
        ufd = open_listener(a.bind_addr, a.port, SOCK_DGRAM);
        if (ufd == SOCK_INVALID) {
            logmsg(1, "LAN discovery disabled: cannot bind UDP port %d", a.port);
        }
    }

    io.write = io_write;
    io.close = io_close;
    io.log = log_line;
    g_core = gcs_core_create(&a.core, &io, NULL, core_random(NULL));
    if (!g_core) {
        return 1;
    }
    gcs_core_set_random(g_core, core_random, NULL);
    for (i = 0; i < GCSD_MAX_CONNS; i++) {
        g_dconns[i].fd = SOCK_INVALID;
    }
    if (a.direct) {
        gcs_direct_io dio;
        memset(&dio, 0, sizeof(dio));
        dio.tcp_send = direct_tcp_send;
        dio.tcp_close = direct_tcp_close;
        dio.udp_send = direct_udp_send;
        dio.log = log_line;
        g_direct = gcs_direct_create(g_core, &dio, NULL);
        if (!g_direct) {
            logmsg(0, "cannot set up the direct Arena service");
            return 1;
        }
        /* plain HTTP, Jabber and UDP as on the original Arena: there is
         * nothing else to protect here than the Arena accounts, which
         * gcs_accounts binds to password and IMEI */
        if (a.direct_http_port) {
            dhttp = open_listener(a.direct_bind, a.direct_http_port, SOCK_STREAM);
        }
        if (a.direct_xmpp_port) {
            dxmpp = open_listener(a.direct_bind, a.direct_xmpp_port, SOCK_STREAM);
        }
        if (a.direct_snap_port) {
            g_dudp = open_listener(a.direct_bind, a.direct_snap_port, SOCK_DGRAM);
        }
        if ((a.direct_http_port && dhttp == SOCK_INVALID) || (a.direct_xmpp_port && dxmpp == SOCK_INVALID) ||
            (a.direct_snap_port && g_dudp == SOCK_INVALID)) {
            logmsg(0, "cannot listen for direct Arena clients on %s (ports %d, %d, UDP %d)", a.direct_bind,
                   a.direct_http_port, a.direct_xmpp_port, a.direct_snap_port);
            return 1;
        }
        logmsg(2, "direct Arena clients on %s: HTTP %d, Jabber %d, SNAP UDP %d", a.direct_bind, a.direct_http_port,
               a.direct_xmpp_port, a.direct_snap_port);
    }
#ifndef _WIN32
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sigemptyset(&sa.sa_mask);
        sa.sa_handler = SIG_IGN;
        sigaction(SIGPIPE, &sa, NULL);
        sa.sa_handler = on_signal;
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
        sigaction(SIGUSR1, &sa, NULL);
    }
#else
    SetConsoleCtrlHandler(on_console, TRUE);
#endif
    logmsg(2, "GameComms server %s '%s' listening on %s:%d (protocol %d)%s", GCS_VERSION, a.core.server_name,
           a.bind_addr[0] ? a.bind_addr : "*", a.port, GC_PROTO_VERSION_MAX,
           a.core.access_key[0] ? ", access key required" : "");
    last_status = now_ms();

    while (!g_stop) {
        static struct pollfd pfd[MAX_CONNS + GCSD_MAX_CONNS + 5];
        static net_conn *map[MAX_CONNS + GCSD_MAX_CONNS + 5];
        static direct_conn *dmap[MAX_CONNS + GCSD_MAX_CONNS + 5];
        int n = 0, r;
        gc_u32 now;

        pfd[n].fd = lfd;
        pfd[n].events = POLLIN;
        dmap[n] = NULL;
        map[n++] = NULL;
        if (ufd != SOCK_INVALID) {
            pfd[n].fd = ufd;
            pfd[n].events = POLLIN;
            dmap[n] = NULL;
            map[n++] = NULL;
        }
        if (dhttp != SOCK_INVALID) {
            pfd[n].fd = dhttp;
            pfd[n].events = POLLIN;
            dmap[n] = NULL;
            map[n++] = NULL;
        }
        if (dxmpp != SOCK_INVALID) {
            pfd[n].fd = dxmpp;
            pfd[n].events = POLLIN;
            dmap[n] = NULL;
            map[n++] = NULL;
        }
        if (g_dudp != SOCK_INVALID) {
            pfd[n].fd = g_dudp;
            pfd[n].events = POLLIN;
            dmap[n] = NULL;
            map[n++] = NULL;
        }
        for (i = 0; i < MAX_CONNS; i++) {
            if (g_conns[i].used) {
                pfd[n].fd = g_conns[i].fd;
                pfd[n].events = POLLIN | (g_conns[i].out_len ? POLLOUT : 0);
                dmap[n] = NULL;
                map[n++] = &g_conns[i];
            }
        }
        for (i = 0; i < GCSD_MAX_CONNS; i++) {
            if (g_dconns[i].used) {
                pfd[n].fd = g_dconns[i].fd;
                pfd[n].events = (short)((g_dconns[i].peer_eof ? 0 : POLLIN) | (g_dconns[i].out_len ? POLLOUT : 0));
                dmap[n] = &g_dconns[i];
                map[n++] = NULL;
            }
        }
        r = poll(pfd, (unsigned)n, 50);
        now = now_ms();
        if (r > 0) {
            for (i = 0; i < n; i++) {
                net_conn *c = map[i];
                if (!pfd[i].revents) {
                    continue;
                }
                if (pfd[i].fd == lfd) {
                    accept_new(lfd, now);
                    continue;
                }
                if (pfd[i].fd == ufd) {
                    udp_discovery(ufd, a.port, a.core.server_name, now);
                    continue;
                }
                if (dmap[i]) {
                    if (dmap[i]->used && !dmap[i]->closing) {
                        direct_input(dmap[i], pfd[i].revents, now);
                    }
                    continue;
                }
                if (pfd[i].fd == dhttp) {
                    direct_accept(dhttp, GCSD_HTTP, now);
                    continue;
                }
                if (pfd[i].fd == dxmpp) {
                    direct_accept(dxmpp, GCSD_XMPP, now);
                    continue;
                }
                if (pfd[i].fd == g_dudp) {
                    direct_udp(g_dudp, now);
                    continue;
                }
                if (!c || !c->used || c->closing) {
                    continue; /* closing: no more input, flushed on free */
                }
                if (pfd[i].revents & POLLNVAL) {
                    c->closing = 1;
                    continue;
                }
                if (pfd[i].revents & POLLOUT) {
                    try_flush(c);
                }
                if (pfd[i].revents & (POLLIN | POLLHUP | POLLERR)) {
                    gc_u8 buf[8192];
                    int k = (int)recv(c->fd, (char *)buf, sizeof(buf), 0);
                    if (k > 0) {
                        gcs_core_conn_input(g_core, &c->conn, buf, (size_t)k, now);
                    } else if (k == 0 || !SOCK_EAGAIN(sock_err())) {
                        c->closing = 1;
                    }
                }
            }
        }
        gcs_core_poll(g_core, now);
        for (i = 0; i < MAX_CONNS; i++) {
            if (g_conns[i].used && g_conns[i].closing) {
                conn_free(&g_conns[i], now);
            }
        }
        if (g_direct) {
            gcs_direct_poll(g_direct, now);
            for (i = 0; i < GCSD_MAX_CONNS; i++) {
                direct_conn *c = &g_dconns[i];
                if (c->used && !c->closing && (gc_u32)(now - c->t_last) >= g_direct_idle_ms) {
                    logmsg(3, "direct %s: idle, closing", c->addr);
                    c->closing = 1;
                }
                if (c->used && c->closing) {
                    dconn_free(c);
                }
            }
        }
        if (g_dump || (a.status_interval > 0 && (gc_u32)(now - last_status) >= (gc_u32)a.status_interval * 1000u)) {
            g_dump = 0;
            last_status = now;
            gcs_core_dump(g_core);
            if (g_direct) {
                gcs_direct_stats ds;
                gcs_direct_get_stats(g_direct, &ds);
                logmsg(2, "direct: %d clients, %d connections, %lu accepted, %lu datagrams (%lu dropped)", ds.clients,
                       ds.conns, (unsigned long)ds.tcp_accepted, (unsigned long)ds.udp_in,
                       (unsigned long)ds.udp_dropped);
            }
        }
    }

    logmsg(2, "shutting down");
    for (i = 0; i < MAX_CONNS; i++) {
        if (g_conns[i].used) {
            conn_free(&g_conns[i], now_ms());
        }
    }
    for (i = 0; i < GCSD_MAX_CONNS; i++) {
        if (g_dconns[i].used) {
            dconn_free(&g_dconns[i]);
        }
    }
    gcs_direct_destroy(g_direct);
    if (dhttp != SOCK_INVALID) {
        sock_close(dhttp);
    }
    if (dxmpp != SOCK_INVALID) {
        sock_close(dxmpp);
    }
    if (g_dudp != SOCK_INVALID) {
        sock_close(g_dudp);
    }
    gcs_core_destroy(g_core);
    sock_close(lfd);
    if (ufd != SOCK_INVALID) {
        sock_close(ufd);
    }
    if (g_logfile) {
        fclose(g_logfile);
    }
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
