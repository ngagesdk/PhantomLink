/** @file bridge_profile.h
 *
 *  One bridge implementation as seen by the parity harness: the shared
 *  bridge core built with one platform's compile-time sizing, plus that
 *  platform's transport glue (how received bytes are fed into the core
 *  and how output leaves it).
 *
 *  bridge_profile.c is compiled once per platform:
 *    pc     - pcbridge sizing (defaults), pcbridge glue (pcb_main.c)
 *    esp32  - sizing from firmware/platformio.ini, ESP32 glue (main.cpp,
 *             esp32_transport.h, bt_output.h, tcp_output.h)
 *  The ESP32 copy of the bridge core and link layer is renamed
 *  (esp32_symbols.h), so both coexist in one test program while the
 *  N-Gage engine and the server keep their normal sizing.
 *
 *  SPDX-License-Identifier: MIT
 */
#ifndef BRIDGE_PROFILE_H
#define BRIDGE_PROFILE_H

#include "gc_types.h"
#include <stddef.h>

typedef struct
{
    /** bytes onto the Bluetooth link towards the N-Gage */
    void (*bt_write)(void *ctx, const gc_u8 *data, size_t len);
    /** 1 while earlier Bluetooth output is still being transmitted */
    int (*bt_busy)(void *ctx);
    /** bytes onto the TCP connection towards the server */
    void (*tcp_write)(void *ctx, const gc_u8 *data, size_t len);
    void (*tcp_connect)(void *ctx);
    void (*tcp_close)(void *ctx);
} bp_io;

typedef struct
{
    int link_queue;   /* GC_LINK_QUEUE: messages per link window */
    int fifo_len;     /* BRIDGE_FIFO_LEN */
    size_t link_qbuf; /* GC_LINK_QBUF */
    size_t fifo_bytes;
    size_t bridge_size; /* sizeof(bridge) */
    size_t max_msg;     /* largest message the queues can hold */
} bp_sizing;

typedef struct
{
    unsigned long dev_backpressure; /* device messages left unacknowledged */
    unsigned long srv_backpressure;
    unsigned long dev_retx;
    unsigned long srv_retx;
    unsigned long bt_teardowns;  /* glue closed Bluetooth (buffer overflow) */
    unsigned long tcp_teardowns; /* glue closed TCP (buffer overflow) */
    unsigned long srv_fresh;     /* fresh (non-resumed) server links */
} bp_stats;

typedef struct
{
    const char *name;
    void (*sizing)(bp_sizing *out);
    void *(*create)(const bp_io *io, void *ctx, gc_u32 server_link_id, gc_u32 now);
    void (*destroy)(void *b);
    void (*dev_connected)(void *b, const gc_u8 addr[6], gc_u32 now);
    /** received Bluetooth bytes; returns how many the glue consumed */
    size_t (*dev_rx)(void *b, const gc_u8 *data, size_t len, gc_u32 now);
    void (*tcp_connected)(void *b, gc_u32 now);
    /** received TCP bytes; returns how many the glue consumed */
    size_t (*tcp_rx)(void *b, const gc_u8 *data, size_t len, gc_u32 now);
    void (*tcp_lost)(void *b, gc_u32 now);
    /** one main loop iteration (timers, output) */
    void (*loop)(void *b, gc_u32 now);
    void (*stats)(void *b, bp_stats *out);
} bp_profile;

extern const bp_profile bp_profile_pc;
extern const bp_profile bp_profile_esp32;

#endif
