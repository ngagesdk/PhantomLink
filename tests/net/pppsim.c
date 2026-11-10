/* pppsim: an "N-Gage" for the PPP mode tests with real processes.
 *
 * Connects to a bridge's TCP test device port, speaks PPP there (gc_ppp)
 * and forwards the IP packets to and from a TUN interface, so the Linux
 * kernel's TCP/IP stack plays the N-Gage's.  Local programs then reach the
 * Arena services through bridge and server as the game would.
 *
 *   pppsim <bridge host> <port> <tun name>
 *
 * Prints "UP <tun>" once IPCP is open.  Linux only, needs CAP_NET_ADMIN.
 *
 * SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "gc_ppp.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int g_sock, g_tun;
static gc_ppp g_ppp;

static gc_u32 now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (gc_u32)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void w(void *ctx, const gc_u8 *d, size_t n)
{
    (void)ctx;
    if (send(g_sock, d, n, 0) < 0) {
        perror("send");
    }
}

static void on_ip(void *ctx, const gc_u8 *d, size_t n)
{
    (void)ctx;
    if (write(g_tun, d, n) < 0) {
        perror("tun write");
    }
}

static void on_state(void *ctx, int ev)
{
    (void)ctx;
    if (ev == GC_PPP_EV_UP) {
        printf("UP\n");
        fflush(stdout);
    } else if (ev == GC_PPP_EV_DEAD) {
        printf("DEAD\n");
        fflush(stdout);
        exit(1);
    }
}

static int tun_open(const char *name)
{
    struct ifreq ifr;
    struct sockaddr_in *sa;
    int fd = open("/dev/net/tun", O_RDWR), s;
    if (fd < 0) {
        return -1;
    }
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", name);
    if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
        return -1;
    }
    s = socket(AF_INET, SOCK_DGRAM, 0);
    sa = (struct sockaddr_in *)&ifr.ifr_addr;
    sa->sin_family = AF_INET;
    inet_pton(AF_INET, "10.64.0.2", &sa->sin_addr);
    if (ioctl(s, SIOCSIFADDR, &ifr) < 0) {
        return -1;
    }
    sa = (struct sockaddr_in *)&ifr.ifr_dstaddr;
    sa->sin_family = AF_INET;
    inet_pton(AF_INET, "10.64.0.1", &sa->sin_addr);
    if (ioctl(s, SIOCSIFDSTADDR, &ifr) < 0) {
        return -1;
    }
    ifr.ifr_mtu = 1500;
    ioctl(s, SIOCSIFMTU, &ifr);
    ioctl(s, SIOCGIFFLAGS, &ifr);
    ifr.ifr_flags |= IFF_UP | IFF_RUNNING | IFF_POINTOPOINT;
    if (ioctl(s, SIOCSIFFLAGS, &ifr) < 0) {
        return -1;
    }
    close(s);
    return fd;
}

int main(int argc, char **argv)
{
    struct addrinfo hints, *ai;
    gc_ppp_cfg cfg;
    gc_ppp_ops ops = { w, on_ip, on_state, NULL };
    if (argc < 4) {
        fprintf(stderr, "usage: pppsim <host> <port> <tun>\n");
        return 2;
    }
    g_tun = tun_open(argv[3]);
    if (g_tun < 0) {
        printf("no usable /dev/net/tun\n");
        return 77;
    }
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(argv[1], argv[2], &hints, &ai) != 0) {
        return 1;
    }
    g_sock = socket(ai->ai_family, SOCK_STREAM, 0);
    if (connect(g_sock, ai->ai_addr, ai->ai_addrlen) < 0) {
        perror("connect");
        return 1;
    }
    gc_ppp_default_cfg(&cfg);
    /* the bridge's view: local = bridge, remote = N-Gage; here swapped */
    memcpy(cfg.local, cfg.remote, 4);
    cfg.remote[3] = 1;
    cfg.magic = (gc_u32)getpid() * 2654435761u;
    gc_ppp_init(&g_ppp, &cfg, &ops, NULL);
    gc_ppp_open(&g_ppp, now_ms());
    for (;;) {
        struct pollfd pf[2];
        gc_u8 b[4096];
        pf[0].fd = g_sock;
        pf[0].events = POLLIN;
        pf[1].fd = g_tun;
        pf[1].events = POLLIN;
        poll(pf, 2, 100);
        if (pf[0].revents & (POLLIN | POLLHUP)) {
            ssize_t n = recv(g_sock, b, sizeof(b), 0);
            if (n <= 0) {
                printf("bridge closed\n");
                return 1;
            }
            gc_ppp_input(&g_ppp, b, (size_t)n, now_ms());
        }
        if (pf[1].revents & POLLIN) {
            ssize_t n = read(g_tun, b, sizeof(b));
            if (n > 0 && (b[0] >> 4) == 4) {
                gc_ppp_send_ip(&g_ppp, b, (size_t)n);
            }
        }
        gc_ppp_poll(&g_ppp, now_ms());
    }
}
