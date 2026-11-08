/** @file esp32_glue.h
 *
 *  Transport sizes of the ESP32 front end (main.cpp), shared with the
 *  bridge parity test (tests/parity), which runs the same glue against
 *  pcbridge's.
 *
 *  Received bytes are handed to the bridge core exactly as they were read,
 *  like pcbridge does: the link layer copes with any split and applies its
 *  own backpressure. No frame splitting or read pacing here.
 *
 *  SPDX-License-Identifier: MIT
 **/
#ifndef ESP32_GLUE_H
#define ESP32_GLUE_H

#define ESP32_BT_RX_READ    256  /* bytes per Bluetooth RX buffer read */
#define ESP32_BT_TX_CHUNK   512  /* bytes per SPP write */
#define ESP32_TCP_RX_READ   512  /* bytes per TCP recv() */
#define ESP32_TCP_RX_BUDGET 2048 /* TCP bytes read per loop iteration */

#endif
