/** @file bridge_profile.c  - see bridge_profile.h.
 *
 *  Built twice: without BP_ESP32 (pcbridge) and with BP_ESP32 (ESP32
 *  firmware). SPDX-License-Identifier: MIT
 */
#ifdef BP_ESP32
#include "esp32_symbols.h"
#endif
#include "bridge_core.h"
#include "bridge_profile.h"
#include <stdlib.h>
#include <string.h>

#ifdef BP_ESP32
#include "bt_output.h"
#include "esp32_glue.h"
#include "tcp_output.h"
#define BP_NAME "esp32"
#define BP_SYM  bp_profile_esp32
#else
#define BP_NAME "pc"
#define BP_SYM  bp_profile_pc
#endif

typedef struct
{
    bridge br;
    bp_io io;
    void *ctx;
    int tcp_wanted;
    int tcp_pending;
    int tcp_up;
    bp_stats st;
#ifdef BP_ESP32
    bt_output bt_tx; /* main.cpp: btTx */
    int bt_tx_overflow;
    tcp_output tcp_out; /* main.cpp: tcpOutput */
    int tcp_out_failed;
#endif
} bp_bridge;

static void p_dev_write(void *ctx, const gc_u8 *d, size_t n)
{
    bp_bridge *p = (bp_bridge *)ctx;
#ifdef BP_ESP32
    /* main.cpp io_dev_write / btTxAppend */
    if (p->bt_tx_overflow || !bt_output_append(&p->bt_tx, d, n)) {
        p->bt_tx_overflow = 1;
    }
#else
    p->io.bt_write(p->ctx, d, n); /* pcb_main.c d_dev_write: socket send */
#endif
}

static void p_srv_write(void *ctx, const gc_u8 *d, size_t n)
{
    bp_bridge *p = (bp_bridge *)ctx;
    if (!p->tcp_up) {
        return;
    }
#ifdef BP_ESP32
    /* main.cpp io_srv_write */
    if (!p->tcp_out_failed && !tcp_output_append(&p->tcp_out, d, n)) {
        p->tcp_out_failed = 1;
    }
#else
    p->io.tcp_write(p->ctx, d, n);
#endif
}

static void p_srv_connect(void *ctx)
{
    ((bp_bridge *)ctx)->tcp_wanted = 1;
}

static void p_tcp_close(bp_bridge *p)
{
    if (p->tcp_up || p->tcp_pending) {
        p->io.tcp_close(p->ctx);
    }
    p->tcp_up = 0;
    p->tcp_pending = 0;
#ifdef BP_ESP32
    tcp_output_clear(&p->tcp_out);
    p->tcp_out_failed = 0;
#endif
}

static void p_srv_disconnect(void *ctx)
{
    bp_bridge *p = (bp_bridge *)ctx;
    p->tcp_wanted = 0;
    p_tcp_close(p);
}

static void p_log(void *ctx, const char *line)
{
    bp_bridge *p = (bp_bridge *)ctx;
    if (strstr(line, "server link up (fresh)")) {
        p->st.srv_fresh++;
    }
    if (getenv("PARITY_LOG")) {
        extern void parity_log(void *ctx, const char *who, const char *line);
        parity_log(p->ctx, BP_NAME, line);
    }
}

static void p_sizing(bp_sizing *out)
{
    out->link_queue = GC_LINK_QUEUE;
    out->fifo_len = BRIDGE_FIFO_LEN;
    out->link_qbuf = GC_LINK_QBUF;
    out->fifo_bytes = BRIDGE_FIFO_BYTES;
    out->bridge_size = sizeof(bridge);
    out->max_msg = GC_LINK_MAX_MSG;
}

static void *p_create(const bp_io *io, void *ctx, gc_u32 link_id, gc_u32 now)
{
    bp_bridge *p = (bp_bridge *)calloc(1, sizeof(*p));
    bridge_cfg bc;
    bridge_io bio;
    if (!p) {
        return NULL;
    }
    p->io = *io;
    p->ctx = ctx;
    bridge_default_cfg(&bc);
    bc.server_link_id = link_id;
    bio.dev_write = p_dev_write;
    bio.srv_write = p_srv_write;
    bio.srv_connect = p_srv_connect;
    bio.srv_disconnect = p_srv_disconnect;
    bio.log = p_log;
    bridge_init(&p->br, &bc, &bio, p);
    bridge_set_wifi(&p->br, 1, -50, now);
    return p;
}

static void p_destroy(void *b)
{
    free(b);
}

static void p_dev_connected(void *b, const gc_u8 addr[6], gc_u32 now)
{
    bridge_dev_connected(&((bp_bridge *)b)->br, addr, now);
}

static size_t p_dev_rx(void *b, const gc_u8 *d, size_t n, gc_u32 now)
{
    bp_bridge *p = (bp_bridge *)b;
    size_t pos = 0;
#ifdef BP_ESP32
    /* main.cpp btPump: reads from the Bluetooth RX buffer, handed over as read */
    while (pos < n) {
        size_t k = n - pos > ESP32_BT_RX_READ ? ESP32_BT_RX_READ : n - pos;
        bridge_dev_input(&p->br, d + pos, k, now);
        pos += k;
    }
#else
    /* pcb_main.c: recv() into a 4096 byte buffer */
    while (pos < n) {
        size_t k = n - pos > 4096 ? 4096 : n - pos;
        bridge_dev_input(&p->br, d + pos, k, now);
        pos += k;
    }
#endif
    return pos;
}

static void p_tcp_connected(void *b, gc_u32 now)
{
    bp_bridge *p = (bp_bridge *)b;
    p->tcp_pending = 0;
    p->tcp_up = 1;
    bridge_srv_connected(&p->br, now);
}

static size_t p_tcp_rx(void *b, const gc_u8 *d, size_t n, gc_u32 now)
{
    bp_bridge *p = (bp_bridge *)b;
    size_t pos = 0;
#ifdef BP_ESP32
    /* main.cpp loop(): recv() of up to ESP32_TCP_RX_READ bytes, handed over
       as read, ESP32_TCP_RX_BUDGET bytes per loop iteration */
    size_t budget = ESP32_TCP_RX_BUDGET;
    while (budget && pos < n && p->tcp_up) {
        size_t c = budget < ESP32_TCP_RX_READ ? budget : ESP32_TCP_RX_READ;
        if (c > n - pos) {
            c = n - pos;
        }
        budget -= c;
        bridge_srv_input(&p->br, d + pos, c, now);
        pos += c;
    }
#else
    /* pcb_main.c: one recv() into an 8192 byte buffer per select() */
    while (pos < n && p->tcp_up) {
        size_t k = n - pos > 8192 ? 8192 : n - pos;
        bridge_srv_input(&p->br, d + pos, k, now);
        pos += k;
    }
#endif
    return pos;
}

static void p_tcp_lost(void *b, gc_u32 now)
{
    bp_bridge *p = (bp_bridge *)b;
    p->tcp_up = 0;
    p->tcp_pending = 0;
#ifdef BP_ESP32
    tcp_output_clear(&p->tcp_out);
    p->tcp_out_failed = 0;
#endif
    bridge_srv_disconnected(&p->br, now);
}

static void p_loop(void *b, gc_u32 now)
{
    bp_bridge *p = (bp_bridge *)b;
#ifdef BP_ESP32
    /* main.cpp loop(): TCP failure check, poll, flush, btTxPump */
    if (p->tcp_up && p->tcp_out_failed) {
        p->st.tcp_teardowns++;
        p_tcp_close(p);
        bridge_srv_disconnected(&p->br, now);
    }
#endif
    if (p->tcp_wanted && !p->tcp_up && !p->tcp_pending) {
        p->tcp_wanted = 0;
        p->tcp_pending = 1;
        p->io.tcp_connect(p->ctx);
    }
    bridge_poll(&p->br, now);
    if (p->tcp_wanted && !p->tcp_up && !p->tcp_pending) {
        p->tcp_wanted = 0;
        p->tcp_pending = 1;
        p->io.tcp_connect(p->ctx);
    }
#ifdef BP_ESP32
    if (p->tcp_up && p->tcp_out.len) {
        while (p->tcp_out.len) {
            size_t c = tcp_output_chunk(&p->tcp_out);
            p->io.tcp_write(p->ctx, p->tcp_out.data + p->tcp_out.head, c);
            tcp_output_consume(&p->tcp_out, c);
        }
    }
    if (p->bt_tx_overflow) {
        /* main.cpp closes the SPP connection here; counted as a failure */
        p->st.bt_teardowns++;
        p->bt_tx_overflow = 0;
    }
    /* btTxPump: one write in flight, completed when it left the radio */
    if (p->bt_tx.inflight && !p->io.bt_busy(p->ctx)) {
        bt_output_complete(&p->bt_tx, p->bt_tx.inflight);
    }
    if (!p->bt_tx.inflight) {
        size_t c = bt_output_begin(&p->bt_tx, ESP32_BT_TX_CHUNK);
        if (c) {
            p->io.bt_write(p->ctx, p->bt_tx.queue.data + p->bt_tx.queue.head, c);
        }
    }
#endif
}

static void p_stats(void *b, bp_stats *out)
{
    bp_bridge *p = (bp_bridge *)b;
    *out = p->st;
    out->dev_backpressure = p->br.dev.st_backpressure;
    out->srv_backpressure = p->br.srv.st_backpressure;
    out->dev_retx = p->br.dev.st_retx;
    out->srv_retx = p->br.srv.st_retx;
}

const bp_profile BP_SYM = { BP_NAME, p_sizing, p_create, p_destroy, p_dev_connected, p_dev_rx,
                            p_tcp_connected, p_tcp_rx, p_tcp_lost, p_loop, p_stats };
