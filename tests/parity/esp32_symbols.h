/** @file esp32_symbols.h
 *
 *  Renames the link layer, IP end point and bridge core entry points for the ESP32
 *  copy built into the parity test (see bridge_profile.h). Only these
 *  modules depend on the queue and connection sizing; the codec and message modules are
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

#define gc_ip_init      esp32_gc_ip_init
#define gc_ip_set_mss   esp32_gc_ip_set_mss
#define gc_ip_input     esp32_gc_ip_input
#define gc_ip_poll      esp32_gc_ip_poll
#define gc_ip_reset     esp32_gc_ip_reset
#define gc_ip_udp_send  esp32_gc_ip_udp_send
#define gc_ip_tcp_send  esp32_gc_ip_tcp_send
#define gc_ip_tcp_space esp32_gc_ip_tcp_space
#define gc_ip_tcp_close esp32_gc_ip_tcp_close
#define gc_ip_tcp_abort esp32_gc_ip_tcp_abort
#define gc_ip_sum       esp32_gc_ip_sum
#define gc_ip_fold      esp32_gc_ip_fold

/* gc_ppp too: its buffers follow GC_PPP_MRU / GC_PPP_TXBUF */
#define gc_ppp_default_cfg  esp32_gc_ppp_default_cfg
#define gc_ppp_init         esp32_gc_ppp_init
#define gc_ppp_input        esp32_gc_ppp_input
#define gc_ppp_poll         esp32_gc_ppp_poll
#define gc_ppp_send_ip      esp32_gc_ppp_send_ip
#define gc_ppp_open         esp32_gc_ppp_open
#define gc_ppp_close        esp32_gc_ppp_close
#define gc_ppp_is_up        esp32_gc_ppp_is_up
#define gc_ppp_detect       esp32_gc_ppp_detect
#define gc_ppp_fcs          esp32_gc_ppp_fcs
#define gc_ppp_frame        esp32_gc_ppp_frame

#define bridge_default_cfg      esp32_bridge_default_cfg
#define bridge_init             esp32_bridge_init
#define bridge_release          esp32_bridge_release
#define bridge_dev_state        esp32_bridge_dev_state
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
