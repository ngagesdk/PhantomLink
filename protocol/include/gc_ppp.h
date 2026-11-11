/** @file gc_ppp.h
 *
 *  Minimal PPP peer (RFC 1661/1662, IPCP RFC 1332/1877) for the PPP mode
 *  of the bridges (retail N-Gage Arena games, docs/ARENA.md).
 *
 *  The N-Gage dials the access point "PhantomLink", which the PhantomLink
 *  app has turned into direct PPP over the Bluetooth serial port.  This
 *  module is the other end of that PPP link: HDLC-like framing with FCS-16
 *  and byte stuffing, LCP (MRU, ACCM, magic number, PFC/ACFC, echo) and
 *  IPCP (addresses and DNS servers for the N-Gage).  It never asks for
 *  authentication and accepts none.  IPv4 packets are handed to the owner
 *  (gc_ip.h); every other network protocol (IPV6CP, CCP, ...) is answered
 *  with a Protocol-Reject.
 *
 *  Like gc_link it is passive: no allocation, no blocking, no clock other
 *  than the millisecond counter passed into every call.  Plain C89.
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#ifndef GC_PPP_H
#define GC_PPP_H

#include "gc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef GC_PPP_MRU
#define GC_PPP_MRU 1500 /* largest information field we receive */
#endif
/* Below 1500 the MRU is asked for in LCP, so the peer sends no larger
 * packets. */

#ifndef GC_PPP_TXBUF
#define GC_PPP_TXBUF (2 * (GC_PPP_MRU + 8) + 2) /* a whole escaped frame */
#endif
/* A smaller send buffer is passed to write() in pieces per frame. */
GC_STATIC_ASSERT(ppp_txbuf_min, GC_PPP_TXBUF >= 16);
GC_STATIC_ASSERT(ppp_mru_range, GC_PPP_MRU >= 128 && GC_PPP_MRU <= 1500);

/* protocol numbers */
#define GC_PPP_IP   0x0021
#define GC_PPP_IPCP 0x8021
#define GC_PPP_LCP  0xC021

/* control protocol states (subset of the RFC 1661 automaton) */
#define GC_PPP_ST_INITIAL 0 /* lower layer up, nothing sent yet  */
#define GC_PPP_ST_REQSENT 1
#define GC_PPP_ST_ACKRCVD 2
#define GC_PPP_ST_ACKSENT 3
#define GC_PPP_ST_OPENED  4
#define GC_PPP_ST_CLOSING 5 /* we sent Terminate-Request          */
#define GC_PPP_ST_STOPPED 6 /* gave up or terminated by the peer */

/* events reported through gc_ppp_ops.on_state */
#define GC_PPP_EV_UP   1 /* IPCP opened: IP packets can flow              */
#define GC_PPP_EV_DOWN 2 /* IPCP closed: no more IP (may come up again)   */
#define GC_PPP_EV_DEAD 3 /* LCP finished (terminated, negotiation timed \
                          * out): the owner should drop the connection  */

typedef struct
{
    /** raw bytes for the serial line */
    void (*write)(void *ctx, const gc_u8 *data, size_t len);
    /** an IPv4 packet arrived (IPCP opened) */
    void (*on_ip)(void *ctx, const gc_u8 *pkt, size_t len);
    void (*on_state)(void *ctx, int event);
    /** optional diagnostics, may be NULL */
    void (*log)(void *ctx, const char *line);
} gc_ppp_ops;

typedef struct
{
    gc_u8 local[4];      /* our end of the link (also offered as DNS) */
    gc_u8 remote[4];     /* address given to the peer                 */
    gc_u8 dns[4];        /* DNS server offered to the peer            */
    gc_u16 restart_ms;   /* Configure/Terminate-Request interval  */
    gc_u8 max_configure; /* Configure-Requests before giving up   */
    gc_u8 max_terminate;
    gc_u32 magic; /* our LCP magic number, nonzero, random */
} gc_ppp_cfg;

typedef struct
{
    int state;
    gc_u8 id;      /* identifier of our last request         */
    gc_u8 retries; /* requests left before giving up          */
    gc_u32 t_restart;
    int timer;         /* restart timer running                  */
    gc_u8 rej_opts[8]; /* our options the peer rejected (bitset by type) */
} gc_ppp_fsm;

typedef struct
{
    gc_ppp_cfg cfg;
    gc_ppp_ops ops;
    void *ctx;

    /* receiver: HDLC deframer */
    gc_u8 rx[GC_PPP_MRU + 8];
    size_t rx_len;
    int rx_esc;
    int rx_drop; /* frame too long: skip to the next flag */

    /* sender */
    gc_u32 tx_accm; /* control characters we must escape          */
    gc_u32 peer_magic;
    gc_u16 peer_mru;

    gc_ppp_fsm lcp;
    gc_ppp_fsm ipcp;
    int up; /* GC_PPP_EV_UP reported */
    gc_u8 tx[GC_PPP_TXBUF];

    gc_u32 now;
    gc_u32 st_rx_frames;
    gc_u32 st_rx_bad_fcs;
    gc_u32 st_tx_frames;
} gc_ppp;

void gc_ppp_default_cfg(gc_ppp_cfg *cfg);
void gc_ppp_init(gc_ppp *p, const gc_ppp_cfg *cfg, const gc_ppp_ops *ops, void *ctx);

/** Serial line bytes from the peer. */
void gc_ppp_input(gc_ppp *p, const gc_u8 *data, size_t len, gc_u32 now);
/** Restart timers; call at least every few hundred ms. */
void gc_ppp_poll(gc_ppp *p, gc_u32 now);
/** Send one IPv4 packet; 0 on success, -1 if IPCP is not open or too big. */
int gc_ppp_send_ip(gc_ppp *p, const gc_u8 *pkt, size_t len);
/** Start LCP from our side (normally the first frame of the peer does). */
void gc_ppp_open(gc_ppp *p, gc_u32 now);
/** Send Terminate-Request; GC_PPP_EV_DEAD follows (Terminate-Ack or timeout). */
void gc_ppp_close(gc_ppp *p, gc_u32 now);
int gc_ppp_is_up(const gc_ppp *p);

/** True if data (the first bytes from a device) looks like PPP. */
int gc_ppp_detect(const gc_u8 *data, size_t len);

/** RFC 1662 FCS-16 helpers (exported for the tests). */
gc_u16 gc_ppp_fcs(gc_u16 fcs, const gc_u8 *data, size_t len);
#define GC_PPP_FCS_INIT 0xFFFF
#define GC_PPP_FCS_GOOD 0xF0B8

/** Frame an information field (protocol + payload) into out with the
 *  given ACCM. Returns the length or 0 if out is too small. */
size_t gc_ppp_frame(gc_u16 proto, const gc_u8 *info, size_t len, gc_u32 accm, gc_u8 *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* GC_PPP_H */
