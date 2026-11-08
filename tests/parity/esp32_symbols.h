/** @file esp32_symbols.h
 *
 *  Renames the link layer and bridge core entry points for the ESP32
 *  copy built into the parity test (see bridge_profile.h). Only these two
 *  modules depend on the queue sizing; the codec and message modules are
 *  shared. A function added to gc_link.h or bridge_core.h without a line
 *  here shows up as a duplicate symbol when linking test_parity.
 *
 *  SPDX-License-Identifier: MIT
 */
#ifndef ESP32_SYMBOLS_H
#define ESP32_SYMBOLS_H

#define gc_link_default_cfg    esp32_gc_link_default_cfg
#define gc_link_init           esp32_gc_link_init
#define gc_link_start          esp32_gc_link_start
#define gc_link_listen         esp32_gc_link_listen
#define gc_link_accept         esp32_gc_link_accept
#define gc_link_reject         esp32_gc_link_reject
#define gc_link_input          esp32_gc_link_input
#define gc_link_poll           esp32_gc_link_poll
#define gc_link_next_timeout   esp32_gc_link_next_timeout
#define gc_link_send           esp32_gc_link_send
#define gc_link_free_slots     esp32_gc_link_free_slots
#define gc_link_transport_lost esp32_gc_link_transport_lost
#define gc_link_close          esp32_gc_link_close
#define gc_link_forget         esp32_gc_link_forget
#define gc_link_is_up          esp32_gc_link_is_up
#define gc_link_parse_hello    esp32_gc_link_parse_hello

#define bridge_default_cfg      esp32_bridge_default_cfg
#define bridge_init             esp32_bridge_init
#define bridge_dev_connected    esp32_bridge_dev_connected
#define bridge_dev_input        esp32_bridge_dev_input
#define bridge_dev_disconnected esp32_bridge_dev_disconnected
#define bridge_set_wifi         esp32_bridge_set_wifi
#define bridge_srv_connected    esp32_bridge_srv_connected
#define bridge_srv_input        esp32_bridge_srv_input
#define bridge_srv_disconnected esp32_bridge_srv_disconnected
#define bridge_poll             esp32_bridge_poll
#define bridge_next_timeout     esp32_bridge_next_timeout

#endif
