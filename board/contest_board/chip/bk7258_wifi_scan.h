/****************************************************************************
 * board/contest_board/chip/bk7258_wifi_scan.h
 *
 * The flat scan-result shape shared between the two halves of the WiFi
 * driver.  bk7258_wifi_glue.c fills it with the vendor's headers on the
 * include path; bk7258_wifi.c reads it with NuttX's.  Neither side can
 * include the other's headers, so this file is the whole contract.
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

#ifndef __BOARD_CONTEST_BOARD_CHIP_BK7258_WIFI_SCAN_H
#define __BOARD_CONTEST_BOARD_CHIP_BK7258_WIFI_SCAN_H

#include <stdint.h>

/* This header is included from both flag domains.  The vendor side has no
 * nuttx/compiler.h, so FAR (an empty macro on this architecture anyway)
 * needs a fallback.
 */

#ifndef FAR
#  define FAR
#endif

struct bk7258_scan_ap_s
{
  uint8_t bssid[6];
  char    ssid[33];      /* NUL-terminated; the vendor's is 32 unterminated */
  uint8_t channel;
  int32_t rssi;
  uint16_t caps;                    /* 802.11 capability field; bit 4 Privacy */
  int      security;                /* enum bk_wlan_sec_type_e */
};

/* Kick off a scan.  Returns 0 once the request has been accepted by the
 * MAC, which is not the same as finished -- results arrive asynchronously.
 */

int bk7258_wifi_scan_start(void);

/* Associate with an AP by SSID, using the last scan's results to find its
 * channel.  Open system only: no RSN element is sent and no EAPOL handshake
 * follows, because the supplicant is not ported.  Returns 0 when the MAC
 * accepted the association.
 */

int bk7258_wifi_connect_open(const char *ssid, int ssid_len, int *status);

/* Join through the supplicant (CONFIG_BK7258_WIFI_WPA builds): WPA2/WPA3 or
 * open by the AP's IEs; NULL key for open.  Returns 0 once handed to the
 * supplicant; completion is asynchronous via the link state, which reaches
 * CONNECTED only after the 4-way handshake.
 */

int bk7258_wifi_connect_sta(const char *ssid, int ssid_len, const char *key);

/* Leave the AP and cancel the supplicant's retry loop (WPA builds). */

int bk7258_wifi_sta_disconnect(void);

/* The station MAC as the vendor stack uses it on air.  Valid only after
 * bk7258_wifi_vendor_init().  Returns 0 on success.
 */

int bk7258_wifi_get_mac(uint8_t *mac);

/* Station link state, read from the state the closed MAC maintains rather
 * than from the unported supplicant.  Returns a wifi_link_state_t; 3 is
 * WIFI_LINKSTATE_STA_CONNECTED.  Negative on error.
 */

#define BK7258_WIFI_LINK_IDLE           0
#define BK7258_WIFI_LINK_CONNECTING     1
#define BK7258_WIFI_LINK_DISCONNECTED   2
#define BK7258_WIFI_LINK_CONNECTED      3
#define BK7258_WIFI_LINK_CONNECT_FAILED 4

int bk7258_wifi_link_state(void);

/* The station VIF index, once rw_msg_send_add_if() has created it; 0xff
 * before that.  The TX path needs it -- bmsg_tx_sender() silently discards
 * frames sent on a wrong VIF.
 */

uint8_t bk7258_wifi_vif(void);

/* TX buffer handoff, implemented in bk7258_wifi_pbuf.c because the buffer
 * is a vendor pbuf: allocated with the PBUF_RAW_TX headroom
 * (CONFIG_MSDU_RESV_HEAD_LENGTH = 96 bytes) that rwnx_start_xmit() requires
 * -- it wraps the pbuf in an sk_buff in place rather than copying.
 *
 *   alloc: returns an opaque handle and points *payload at len writable
 *          bytes.  NULL when the heap is exhausted.
 *   send:  hands the frame to the MAC via bmsg_tx_sender() and drops our
 *          reference; consumes the handle whether the vendor queue took
 *          the frame or not.  Returns 0 if it was queued.
 *   abort: frees an allocated-but-unsent frame.
 */

FAR void *bk7258_wifi_tx_alloc(unsigned int len, FAR uint8_t **payload);
int bk7258_wifi_tx_send(FAR void *frame);
void bk7258_wifi_tx_abort(FAR void *frame);

/* RX handoff in the other direction, implemented in bk7258_wifi.c and
 * called by ethernetif_input() in bk7258_wifi_pbuf.c on the vendor core
 * thread.  'data' is a flattened 802.3 frame.
 */

void bk7258_wifi_rx_frame(int iface, FAR const void *data, unsigned int len);

/* Link-state event bridge.  The shim's wpa_ctrl_event_copy() stub forwards
 * every vendor event to bk7258_wifi_wpa_event() (in the glue, which can see
 * the event enum); CONNECT_IND / DISCONNECT_IND advance the vendor's link
 * state there, and a disconnect additionally calls bk7258_wifi_link_lost()
 * (in the driver) to lower the carrier.
 */

int bk7258_wifi_wpa_event(int event, FAR const void *data, int len);
void bk7258_wifi_link_lost(void);

/* How many APs the last completed scan found.  Touches no reference count,
 * so it is safe to call without holding the set.
 */

int bk7258_wifi_scan_count(void);

/* Hold the vendor's result set for one enumeration, and let it go again.
 *
 * These must bracket every use of bk7258_wifi_scan_get(): the vendor's
 * sr_get/sr_release pair is a reference count, not a lock, and dropping the
 * last reference frees every result and nulls the global.  Acquiring per
 * item therefore destroys the list on the first item -- see the note in
 * bk7258_wifi_glue.c.  Acquire returns the entry count, or 0 if there is
 * nothing to hold; release is safe either way.
 */

int bk7258_wifi_scan_acquire(void);
void bk7258_wifi_scan_release(void);

/* Copy entry 'index' out.  Returns 0 on success, -1 if it does not exist.
 * Only valid between acquire and release.
 */

int bk7258_wifi_scan_get(int index, struct bk7258_scan_ap_s *ap);

#endif /* __BOARD_CONTEST_BOARD_CHIP_BK7258_WIFI_SCAN_H */
