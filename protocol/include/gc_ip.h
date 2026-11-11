/** @file gc_ip.h
 *
 *  Minimal IPv4 end point for the PPP mode of the bridges (docs/ARENA.md).
 *
 *  Sits on top of gc_ppp and terminates everything the N-Gage sends:
 *
 *   - ICMP echo requests are answered.
 *   - DNS queries (UDP port 53, to any address) are answered here; the
 *     owner's lookup callback maps names to addresses (the Arena host
 *     names to the bridge, say).  Unknown names get NXDOMAIN.
 *   - Every other UDP datagram goes to the owner, together with the
 *     address and port the N-Gage sent it to.
 *   - TCP connections to any address and port are accepted on behalf of
 *     the owner (or refused with a reset).  Each connection is a reliable
 *     byte stream in both directions: the owner gets the received bytes
 *     and queues bytes to send; segmentation, acknowledgements,
 *     retransmission and the close handshake happen here.
 *
 *  The stack answers with the address the N-Gage used as destination,
 *  so the owner can relay each flow wherever it wants (bridges tunnel them
 *  to the server, see gc_net.h).  Passive like gc_ppp/gc_link: no
 *  allocation, no blocking, time is passed in.  Plain C89.
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#ifndef GC_IP_H
#define GC_IP_H

#include "gc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef GC_IP_MTU
#define GC_IP_MTU 1500
#endif
#ifndef GC_TCP_CONNS
#define GC_TCP_CONNS 8 /* simultaneous TCP connections */
#endif
#ifndef GC_TCP_TXBUF
#define GC_TCP_TXBUF 8192 /* unacknowledged + unsent bytes per connection */
#endif
#ifndef GC_TCP_WND
#define GC_TCP_WND 8192 /* receive window we advertise */
#endif

#define GC_IP_PROTO_ICMP 1
#define GC_IP_PROTO_TCP  6
#define GC_IP_PROTO_UDP  17

/* TCP connection states */
#define GC_TCP_FREE       0
#define GC_TCP_SYN_RCVD   1
#define GC_TCP_ESTAB      2
#define GC_TCP_CLOSE_WAIT 3 /* peer sent FIN, we may still send     */
#define GC_TCP_LAST_ACK   4 /* both closed, waiting for the last ACK */
#define GC_TCP_FIN_WAIT_1 5 /* we sent FIN                          */
#define GC_TCP_FIN_WAIT_2 6 /* our FIN acked, peer may still send   */
#define GC_TCP_CLOSING    7

typedef struct
{
    /** an IPv4 packet for the N-Gage (normally gc_ppp_send_ip) */
    void (*send)(void *ctx, const gc_u8 *pkt, size_t len);
    /** DNS: resolve name (lower case, dotted). Return 1 and fill addr,
     *  or 0 for NXDOMAIN.  May be NULL: every query fails. */
    int (*lookup)(void *ctx, const char *name, gc_u8 addr[4]);
    /** a UDP datagram that is not DNS. dst/dport: where the N-Gage sent it */
    void (*udp)(void *ctx, const gc_u8 dst[4], gc_u16 dport, gc_u16 sport, const gc_u8 *data, size_t len);
    /** new TCP connection (SYN) to dst:dport; return 1 to accept, 0 to
     *  answer with a reset. conn is the handle for the calls below. */
    int (*tcp_accept)(void *ctx, int conn, const gc_u8 dst[4], gc_u16 dport, gc_u16 sport);
    /** bytes received on conn, in order. Return 0 if they cannot be taken
     *  now: they are not acknowledged and the N-Gage sends them again. */
    int (*tcp_data)(void *ctx, int conn, const gc_u8 *data, size_t len);
    /** peer finished sending (FIN): no more tcp_data for conn. The owner
     *  calls gc_ip_tcp_close() when it is done sending too. */
    void (*tcp_eof)(void *ctx, int conn);
    /** conn is gone (closed normally, reset, or timed out); the handle
     *  may be reused afterwards. */
    void (*tcp_gone)(void *ctx, int conn, int reset);
    /** optional: send buffer space became free on conn */
    void (*tcp_writable)(void *ctx, int conn);
    /** optional diagnostics */
    void (*log)(void *ctx, const char *line);
} gc_ip_ops;

typedef struct
{
    int state;
    gc_u8 laddr[4]; /* address the N-Gage connected to (our side) */
    gc_u8 raddr[4];
    gc_u16 lport;
    gc_u16 rport;

    gc_u32 snd_una; /* oldest unacknowledged sequence number */
    gc_u32 snd_nxt; /* next sequence number to send          */
    gc_u32 rcv_nxt;
    gc_u32 snd_wnd; /* peer's window                          */
    gc_u16 mss;     /* peer's MSS                             */

    /* send buffer: bytes from snd_una on (SYN/FIN not included) */
    gc_u8 tx[GC_TCP_TXBUF];
    size_t tx_head; /* ring index of the byte at snd_una      */
    size_t tx_len;  /* buffered bytes (sent but unacked + unsent) */
    int fin_queued; /* owner closed: FIN after the buffered data */
    int fin_sent;
    int eof_reported;

    gc_u32 t_rto;
    gc_u32 rto;
    gc_u8 retries;
    int timer;
    int writable_pending;
} gc_tcp_conn;

typedef struct
{
    gc_ip_ops ops;
    void *ctx;
    gc_u32 now;
    gc_u32 iss_seed;
    gc_u16 ip_id;
    gc_u16 mss;    /* largest segment either side sends */
    gc_u8 peer[4]; /* the N-Gage, from its packets */
    int have_peer;
    gc_tcp_conn tcp[GC_TCP_CONNS];
    gc_u8 out[GC_IP_MTU]; /* outgoing packet; TCP data is assembled in place */

    gc_u32 st_rx;
    gc_u32 st_rx_bad;
    gc_u32 st_tx;
} gc_ip;

void gc_ip_init(gc_ip *ip, const gc_ip_ops *ops, void *ctx, gc_u32 seed);
/** Largest TCP segment (default MTU - 40). The bridges use a value that
 *  fits one link message, so that every segment is one tunnel message. */
void gc_ip_set_mss(gc_ip *ip, gc_u16 mss);
/** One IPv4 packet from the N-Gage (gc_ppp on_ip). */
void gc_ip_input(gc_ip *ip, const gc_u8 *pkt, size_t len, gc_u32 now);
/** Retransmission timers; call every few hundred ms. */
void gc_ip_poll(gc_ip *ip, gc_u32 now);
/** Link went away: every connection is gone (tcp_gone with reset). */
void gc_ip_reset(gc_ip *ip);

/** Send a UDP datagram to the N-Gage, from src:sport (normally the dst and
 *  dport the N-Gage used) to its port dport. 0 or -1 (too big). */
int gc_ip_udp_send(gc_ip *ip, const gc_u8 src[4], gc_u16 sport, gc_u16 dport, const gc_u8 *data, size_t len);

/** Queue bytes on conn; returns how many were taken (0..len), -1 if conn
 *  is not open for sending. */
int gc_ip_tcp_send(gc_ip *ip, int conn, const gc_u8 *data, size_t len, gc_u32 now);
/** Free send buffer space on conn. */
size_t gc_ip_tcp_space(const gc_ip *ip, int conn);
/** Close after the queued bytes (FIN). */
void gc_ip_tcp_close(gc_ip *ip, int conn, gc_u32 now);
/** Reset conn right away; tcp_gone follows. */
void gc_ip_tcp_abort(gc_ip *ip, int conn);

/** Internet checksum (one's complement sum) of data, starting from sum. */
gc_u32 gc_ip_sum(gc_u32 sum, const gc_u8 *data, size_t len);
gc_u16 gc_ip_fold(gc_u32 sum);

#ifdef __cplusplus
}
#endif

#endif /* GC_IP_H */
