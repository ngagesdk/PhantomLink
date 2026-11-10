/* Interop test of gc_ip against the Linux kernel's TCP/IP stack.
 *
 * A TUN interface plays the PPP link: the kernel is the "N-Gage" at
 * 10.77.0.2, gc_ip answers as 10.77.0.1.  A client thread uses ordinary
 * sockets: TCP echo of 300 kB in both directions, many short connections,
 * a UDP echo and a DNS query.  Needs root (CAP_NET_ADMIN); exits with 77
 * (skipped) without /dev/net/tun.
 *
 * SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "gc_ip.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static int g_tun;
static gc_ip g_ip;
static volatile int g_done, g_fail;
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;

/* echo backlog per connection (bytes the send buffer did not take yet) */
static unsigned char g_back[GC_TCP_CONNS][1 << 16];
static size_t g_back_len[GC_TCP_CONNS];
static int g_eof[GC_TCP_CONNS];

static gc_u32 now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (gc_u32)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void o_send(void *ctx, const gc_u8 *d, size_t n)
{
    (void)ctx;
    if (write(g_tun, d, n) < 0) {
        perror("tun write");
    }
}

static int o_lookup(void *ctx, const char *name, gc_u8 a[4])
{
    (void)ctx;
    if (strcmp(name, "arena.n-gage.com")) {
        return 0;
    }
    a[0] = 10;
    a[1] = 77;
    a[2] = 0;
    a[3] = 1;
    return 1;
}

static void o_udp(void *ctx, const gc_u8 dst[4], gc_u16 dport, gc_u16 sport, const gc_u8 *d, size_t n)
{
    (void)ctx;
    gc_ip_udp_send(&g_ip, dst, dport, sport, d, n); /* echo */
}

static int o_accept(void *ctx, int c, const gc_u8 dst[4], gc_u16 dport, gc_u16 sport)
{
    (void)ctx;
    (void)dst;
    (void)sport;
    g_back_len[c] = 0;
    g_eof[c] = 0;
    return dport == 7;
}

static void flush(int c)
{
    int n;
    if (!g_back_len[c]) {
        if (g_eof[c] == 1) {
            g_eof[c] = 2;
            gc_ip_tcp_close(&g_ip, c, now_ms());
        }
        return;
    }
    n = gc_ip_tcp_send(&g_ip, c, g_back[c], g_back_len[c], now_ms());
    if (n > 0) {
        memmove(g_back[c], g_back[c] + n, g_back_len[c] - (size_t)n);
        g_back_len[c] -= (size_t)n;
    }
    if (!g_back_len[c] && g_eof[c] == 1) {
        g_eof[c] = 2;
        gc_ip_tcp_close(&g_ip, c, now_ms());
    }
}

static int o_data(void *ctx, int c, const gc_u8 *d, size_t n)
{
    (void)ctx;
    if (g_back_len[c] + n > sizeof(g_back[c])) {
        return 0; /* back-pressure */
    }
    memcpy(g_back[c] + g_back_len[c], d, n);
    g_back_len[c] += n;
    flush(c);
    return 1;
}

static void o_eof(void *ctx, int c)
{
    (void)ctx;
    g_eof[c] = 1;
    flush(c);
}

static void o_gone(void *ctx, int c, int reset)
{
    (void)ctx;
    (void)c;
    (void)reset;
}

static void o_writable(void *ctx, int c)
{
    (void)ctx;
    flush(c);
}

static int tun_open(void)
{
    struct ifreq ifr;
    struct sockaddr_in *sa;
    int fd = open("/dev/net/tun", O_RDWR), s;
    if (fd < 0) {
        return -1;
    }
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
    snprintf(ifr.ifr_name, IFNAMSIZ, "gcip%d", (int)getpid() % 1000);
    if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
        close(fd);
        return -1;
    }
    s = socket(AF_INET, SOCK_DGRAM, 0);
    sa = (struct sockaddr_in *)&ifr.ifr_addr;
    sa->sin_family = AF_INET;
    inet_pton(AF_INET, "10.77.0.2", &sa->sin_addr);
    if (ioctl(s, SIOCSIFADDR, &ifr) < 0) {
        perror("SIOCSIFADDR");
        return -1;
    }
    sa = (struct sockaddr_in *)&ifr.ifr_dstaddr;
    sa->sin_family = AF_INET;
    inet_pton(AF_INET, "10.77.0.1", &sa->sin_addr);
    if (ioctl(s, SIOCSIFDSTADDR, &ifr) < 0) {
        perror("SIOCSIFDSTADDR");
        return -1;
    }
    ifr.ifr_mtu = 1500;
    ioctl(s, SIOCSIFMTU, &ifr);
    ioctl(s, SIOCGIFFLAGS, &ifr);
    ifr.ifr_flags |= IFF_UP | IFF_RUNNING | IFF_POINTOPOINT;
    if (ioctl(s, SIOCSIFFLAGS, &ifr) < 0) {
        perror("SIOCSIFFLAGS");
        return -1;
    }
    close(s);
    return fd;
}

#define FAIL(...)                     \
    do {                              \
        fprintf(stderr, __VA_ARGS__); \
        g_fail = 1;                   \
        return NULL;                  \
    } while (0)

static int conn7(void)
{
    struct sockaddr_in a;
    struct timeval tv = { 10, 0 };
    int s = socket(AF_INET, SOCK_STREAM, 0);
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(7);
    inet_pton(AF_INET, "10.77.0.1", &a.sin_addr);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
        close(s);
        return -1;
    }
    return s;
}

static void *client(void *arg)
{
    static unsigned char out[300000], in[300000];
    size_t sent = 0, got = 0, i;
    int s, k;
    struct pollfd pf;
    (void)arg;
    usleep(200000);
    for (i = 0; i < sizeof(out); i++) {
        out[i] = (unsigned char)(i * 7 + (i >> 9));
    }
    /* bulk echo, both directions at once */
    s = conn7();
    if (s < 0) {
        FAIL("connect: %s\n", strerror(errno));
    }
    fcntl(s, F_SETFL, O_NONBLOCK);
    while (got < sizeof(out)) {
        pf.fd = s;
        pf.events = POLLIN | (sent < sizeof(out) ? POLLOUT : 0);
        if (poll(&pf, 1, 10000) <= 0) {
            FAIL("bulk stalled at sent %zu got %zu\n", sent, got);
        }
        if ((pf.revents & POLLOUT) && sent < sizeof(out)) {
            ssize_t n = send(s, out + sent, sizeof(out) - sent > 7000 ? 7000 : sizeof(out) - sent, 0);
            if (n > 0) {
                sent += (size_t)n;
            }
            if (sent == sizeof(out)) {
                shutdown(s, SHUT_WR);
            }
        }
        if (pf.revents & (POLLIN | POLLHUP)) {
            ssize_t n = recv(s, in + got, sizeof(in) - got, 0);
            if (n == 0) {
                break;
            }
            if (n > 0) {
                got += (size_t)n;
            }
        }
    }
    if (got != sizeof(out) || memcmp(in, out, got)) {
        FAIL("bulk echo mismatch (%zu bytes)\n", got);
    }
    {
        /* our FIN after the echo */
        ssize_t n;
        fcntl(s, F_SETFL, 0);
        n = recv(s, in, 1, 0);
        if (n != 0) {
            FAIL("no EOF after echo (%zd)\n", n);
        }
    }
    close(s);
    printf("tcp bulk echo 300000 bytes ok\n");

    /* many short connections (more than the table holds over time) */
    for (k = 0; k < 40; k++) {
        char msg[32], rb[32];
        int l = snprintf(msg, sizeof(msg), "hello %d", k);
        ssize_t n;
        s = conn7();
        if (s < 0) {
            FAIL("connect %d: %s\n", k, strerror(errno));
        }
        send(s, msg, (size_t)l, 0);
        shutdown(s, SHUT_WR);
        n = recv(s, rb, sizeof(rb), MSG_WAITALL);
        if (n != l || memcmp(rb, msg, (size_t)l)) {
            FAIL("short connection %d: %zd\n", k, n);
        }
        close(s);
    }
    printf("40 short connections ok\n");

    /* refused port */
    {
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons(9);
        inet_pton(AF_INET, "10.77.0.1", &a.sin_addr);
        s = socket(AF_INET, SOCK_STREAM, 0);
        if (connect(s, (struct sockaddr *)&a, sizeof(a)) == 0 || errno != ECONNREFUSED) {
            FAIL("port 9 not refused (%s)\n", strerror(errno));
        }
        close(s);
    }
    /* UDP echo and DNS */
    {
        struct sockaddr_in a;
        struct timeval tv = { 3, 0 };
        unsigned char q[64] = { 0x12, 0x34, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0 }, r[512];
        size_t qn = 12;
        const char *labels[3] = { "arena", "n-gage", "com" };
        ssize_t n;
        s = socket(AF_INET, SOCK_DGRAM, 0);
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons(8080);
        inet_pton(AF_INET, "10.77.0.1", &a.sin_addr);
        sendto(s, "snap", 4, 0, (struct sockaddr *)&a, sizeof(a));
        n = recv(s, r, sizeof(r), 0);
        if (n != 4 || memcmp(r, "snap", 4)) {
            FAIL("udp echo: %zd\n", n);
        }
        for (k = 0; k < 3; k++) {
            q[qn++] = (unsigned char)strlen(labels[k]);
            memcpy(q + qn, labels[k], strlen(labels[k]));
            qn += strlen(labels[k]);
        }
        q[qn++] = 0;
        q[qn++] = 0;
        q[qn++] = 1;
        q[qn++] = 0;
        q[qn++] = 1;
        a.sin_port = htons(53);
        sendto(s, q, qn, 0, (struct sockaddr *)&a, sizeof(a));
        n = recv(s, r, sizeof(r), 0);
        if (n < (ssize_t)qn + 16 || r[0] != 0x12 || (r[3] & 15) || memcmp(r + n - 4, "\x0a\x4d\x00\x01", 4)) {
            FAIL("dns answer bad (%zd)\n", n);
        }
        close(s);
        printf("udp echo and dns ok\n");
    }
    return NULL;
}

int main(void)
{
    gc_ip_ops ops = { o_send, o_lookup, o_udp, o_accept, o_data, o_eof, o_gone, o_writable, NULL };
    pthread_t th;
    gc_u32 start;
    g_tun = tun_open();
    if (g_tun < 0) {
        printf("no usable /dev/net/tun: skipped\n");
        return 77;
    }
    gc_ip_init(&g_ip, &ops, NULL, (gc_u32)time(NULL));
    pthread_create(&th, NULL, client, NULL);
    start = now_ms();
    for (;;) {
        struct pollfd pf;
        unsigned char b[2000];
        int i;
        pf.fd = g_tun;
        pf.events = POLLIN;
        poll(&pf, 1, 50);
        pthread_mutex_lock(&g_mx);
        if (pf.revents & POLLIN) {
            ssize_t n = read(g_tun, b, sizeof(b));
            if (n > 0) {
                gc_ip_input(&g_ip, b, (size_t)n, now_ms());
            }
        }
        gc_ip_poll(&g_ip, now_ms());
        for (i = 0; i < GC_TCP_CONNS; i++) {
            if (g_ip.tcp[i].state != GC_TCP_FREE) {
                flush(i);
            }
        }
        pthread_mutex_unlock(&g_mx);
        if (pthread_tryjoin_np(th, NULL) == 0) {
            break;
        }
        if (now_ms() - start > 60000) {
            fprintf(stderr, "timeout\n");
            return 1;
        }
    }
    printf("%s (rx %u bad %u tx %u)\n", g_fail ? "FAILED" : "ok", g_ip.st_rx, g_ip.st_rx_bad, g_ip.st_tx);
    return g_fail ? 1 : 0;
}
