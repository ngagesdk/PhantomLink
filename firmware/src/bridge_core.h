/** @file bridge_core.h
 *
 *  ESP32 bridge core: translates between the N-Gage side (Bluetooth SPP,
 *  GCL link, bridge is acceptor) and the server side (TCP, GCL link,
 *  bridge is initiator).
 *
 *  Hardware independent: the Arduino front end (main.cpp) and the host
 *  test harness drive it through the functions below and receive output
 *  through bridge_io.
 *
 *  Copyright (c) 2023-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#ifndef BRIDGE_CORE_H
#define BRIDGE_CORE_H

#include "gc_link.h"
#include "gc_msg.h"

/* PPP mode (retail N-Gage Arena games, docs/ARENA.md): an N-Gage that
 * starts PPP instead of the GameComms link is served by an embedded PPP
 * peer and IPv4 end point, and its TCP/UDP flows are tunnelled to the
 * server (gc_net.h). Builds can leave it out with -DBRIDGE_PPP=0. */
#ifndef BRIDGE_PPP
#define BRIDGE_PPP 1
#endif
#if BRIDGE_PPP
#include "gc_arena.h"
#include "gc_ip.h"
#include "gc_net.h"
#include "gc_ppp.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifndef BRIDGE_FIFO_LEN
#define BRIDGE_FIFO_LEN 8 /* messages per direction */
#endif

/* Bytes for the messages in one FIFO. As with GC_LINK_QBUF, the default
 * makes the message count the only limit; the ESP32 uses less to save
 * RAM, which keeps the message window and limits only large bursts. */
#ifndef BRIDGE_FIFO_BYTES
#define BRIDGE_FIFO_BYTES ((BRIDGE_FIFO_LEN + 1) * GC_LINK_MAX_MSG)
#endif

#define BRIDGE_VERSION "1.0.0"

#if BRIDGE_PPP
struct bridge_ppp
{
    gc_ppp ppp;
    gc_ip ip;
    struct
    {
        gc_u8 gen;  /* generation, high byte of the GCN id       */
        gc_u8 open; /* the server knows this connection          */
        gc_u8 pend; /* TCP_CLOSE still to send: 1 FIN, 2 reset   */
        gc_u8 pend_gen;
    } conn[GC_TCP_CONNS];
};
#endif

typedef struct
{
    gc_u16 len;
    gc_u16 off; /* message bytes start at buf + off */
} bridge_slot;

GC_STATIC_ASSERT(bridge_fifo_fits_largest_msg, BRIDGE_FIFO_BYTES >= GC_LINK_MAX_MSG);
GC_STATIC_ASSERT(bridge_fifo_offsets_fit_u16, BRIDGE_FIFO_BYTES <= 65535);

typedef struct
{
    bridge_slot s[BRIDGE_FIFO_LEN];
    int head;
    int count;
    gc_u16 wr; /* buf offset after the newest message */
    gc_u8 buf[BRIDGE_FIFO_BYTES];
} bridge_fifo;

typedef struct
{
    void (*dev_write)(void *ctx, const gc_u8 *data, size_t len);
    void (*srv_write)(void *ctx, const gc_u8 *data, size_t len);
    /** start an asynchronous TCP connect; report with
     *  bridge_srv_connected() / bridge_srv_disconnected() */
    void (*srv_connect)(void *ctx);
    void (*srv_disconnect)(void *ctx);
    void (*log)(void *ctx, const char *line);
} bridge_io;

typedef struct
{
    gc_u32 server_link_id; /* random per boot */
    char access_key[GC_MAX_NAME + 1];
    gc_u32 device_grace_ms; /* keep the device's server state after BT loss */
    gc_u32 reconnect_min_ms;
    gc_u32 reconnect_max_ms;
    int ppp;             /* accept PPP connections (default on)        */
    gc_u8 ppp_local[4];  /* bridge end, DNS server, Arena host names   */
    gc_u8 ppp_remote[4]; /* address given to the N-Gage                */
} bridge_cfg;

#define BRIDGE_MODE_NONE 0 /* device connected, first bytes not seen yet */
#define BRIDGE_MODE_LINK 1 /* GameComms link (gc_link)                   */
#define BRIDGE_MODE_PPP  2 /* PPP (retail Arena games)                   */

typedef struct
{
    bridge_cfg cfg;
    bridge_io io;
    void *ctx;

    gc_link srv;        /* server side */
    bridge_fifo to_srv; /* waiting for the server link */

    /* The N-Gage side. A GameComms device uses the device link and its
     * queue; an Arena game (PPP) uses neither, so PPP mode keeps its state
     * in the same memory (the ESP32 has no heap to spare for it). Which
     * one is valid follows mode: pp in BRIDGE_MODE_PPP, gc otherwise. */
    union
    {
        struct
        {
            gc_link dev;        /* GameComms link to the N-Gage */
            bridge_fifo to_dev; /* waiting for the device link */
        } gc;
#if BRIDGE_PPP
        struct bridge_ppp pp;
#endif
    } u;

    int dev_connected; /* SPP transport present */
    gc_u8 dev_addr[GC_DEVICE_ID_LEN];
    gc_u32 t_dev_lost;
    int dev_lost_pending;

    int wifi_up;
    gc_i8 rssi;
    int srv_state; /* GC_BS_* */
    gc_u32 t_reconnect;
    gc_u32 backoff;
    int srv_ever_up;

    /* last REGISTER from the device, replayed after a fresh server link */
    gc_u8 reg_msg[GC_LINK_MAX_MSG];
    gc_u16 reg_len;
    int registered; /* server confirmed the registration */

    gc_u32 now;
    gc_u32 st_dev_msgs;
    gc_u32 st_srv_msgs;

    int mode; /* BRIDGE_MODE_* of the current device connection */
#if BRIDGE_PPP
    gc_u32 st_ppp_tcp;
    gc_u32 st_ppp_udp;
#endif
} bridge;

void bridge_default_cfg(bridge_cfg *cfg);
void bridge_init(bridge *b, const bridge_cfg *cfg, const bridge_io *io, void *ctx);
/** Call before discarding a bridge (nothing is allocated at present). */
void bridge_release(bridge *b);
/** State of the GameComms device link (GC_LS_*). PPP mode has no device
 *  link: GC_LS_UP while the PPP device is connected, GC_LS_IDLE after. */
int bridge_dev_state(const bridge *b);

void bridge_dev_connected(bridge *b, const gc_u8 *bt_addr, gc_u32 now);
void bridge_dev_input(bridge *b, const gc_u8 *data, size_t len, gc_u32 now);
void bridge_dev_disconnected(bridge *b, gc_u32 now);

void bridge_set_wifi(bridge *b, int up, gc_i8 rssi, gc_u32 now);
void bridge_srv_connected(bridge *b, gc_u32 now);
void bridge_srv_input(bridge *b, const gc_u8 *data, size_t len, gc_u32 now);
void bridge_srv_disconnected(bridge *b, gc_u32 now);

void bridge_poll(bridge *b, gc_u32 now);
gc_u32 bridge_next_timeout(bridge *b, gc_u32 now);

#ifdef __cplusplus
}
#endif

#endif /* BRIDGE_CORE_H */
