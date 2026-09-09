/****************************************************************************
 * board/contest_board/chip/bk7258_wifi_glue.c
 *
 * The calls into Beken's WiFi stack that need the vendor's own headers.
 *
 * Like bk7258_wifi_pbuf.c, this file is ours but is compiled inside the
 * vendor OBJECT library (bk7258_wifi_vendor.cmake) rather than with the
 * NuttX flags the rest of chip/ uses.  That is what lets it see
 * modules/wifi_types.h, and it is the whole reason the file exists.
 *
 * bk7258_wifi.c used to call the stack directly with a hand-written
 * declaration, "extern int bk_wifi_init(void)".  That is wrong, it linked
 * anyway, and the way it failed is worth recording because nothing about
 * the symptom pointed at the cause:
 *
 *   The real entry point is bk_err_t bk_wifi_init(const wifi_init_config_t
 *   *config), and its first act is to read config->os_funcs.  Called with
 *   no argument, r0 held whatever the caller happened to leave there.  The
 *   NULL check that guards this exact mistake passed, because garbage is
 *   not NULL, so the stack stored the garbage pointer in g_wifi_funcs and
 *   carried on.  rwnxl_init() then loaded a function pointer from offset
 *   0x238 of it and branched -- an instruction bus fault (CFSR 0x00000100,
 *   IBUSERR) roughly thirty function calls away from the actual error, in
 *   closed-source code, with no undefined symbol and no link warning
 *   anywhere.
 *
 * Hence this file: the config is built from the vendor's own macro, so the
 * table it points at is whatever the SDK says it should be.
 *
 * The macro also has a second, less obvious job.  g_wifi_os_funcs is an
 * ordinary global that nothing else in the tree references -- the closed
 * archives do not import it by name -- so with -ffunction-sections and
 * --gc-sections it was being collected out of the image entirely.  Naming
 * it here is what keeps it alive.
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

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <common/bk_include.h>
#include <os/mem.h>
#include <modules/wifi.h>
#include <modules/wifi_types.h>

/* Declares g_wifi_os_funcs and g_wifi_os_variable, which the config macro
 * above names.  Reached through the "generated/" prefix because the vendor
 * include list stops at components/bk_wifi/include.
 */

#include "generated/lmac_wifi_adapter.h"

/* SCAN_PARAM_T, rw_msg_send_add_if, rw_msg_send_scanu_req, sr_get_scan_*. */

#include "bk_private/bk_rw.h"

/* bk_wlan_start_sta, wlan_sta_enable, network_InitTypeDef_st. */

#include "bk_private/bk_wifi.h"

#ifdef BK7258_WIFI_WPA
#  include <components/event.h>
#endif

/* WPA_CTRL_EVENT_*, for the event bridge below. */

#include "wpa_ctrl.h"

#include "bk7258_wifi_scan.h"


/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_wifi_vendor_init
 *
 * Description:
 *   Bring the vendor MAC stack up.  Called from bk7258_wifi_ifup().
 *
 * Returned Value:
 *   BK_OK (0) on success, the vendor's negative error code otherwise.
 *
 ****************************************************************************/

int bk7258_wifi_vendor_init(void)
{
  wifi_init_config_t config = WIFI_DEFAULT_INIT_CONFIG();

  /* The PHY and RF adapters have to be registered before this runs -- see
   * bk7258_wifi_ifup(), which does it with this port's own tables from
   * bk7258_phy_osi.c rather than Beken's bk_phy_adapter.c.  Compiling
   * theirs instead is not an option: both define g_phy_os_funcs.
   */

  return bk_wifi_init(&config);
}

/****************************************************************************
 * Name: bk7258_wifi_scan_start
 *
 * Description:
 *   Start a scan without the supplicant.
 *
 *   bk_wifi_scan_start(), the documented entry point, cannot be used here:
 *   its second act is wifi_supplicant_start() -> wlan_sta_enable() ->
 *   wpa_ctrl_request(), which this port stubs out.  It logs "wifi enable
 *   fail" and returns BK_OK without ever asking the MAC to scan, so the
 *   call looks like it worked.
 *
 *   The layer underneath does not need the supplicant at all.  Scan results
 *   arrive as SCANU_RESULT_IND and are accumulated by rw_msg_rx.c into its
 *   own scan_rst_set_ptr; the supplicant only gets told afterwards, by a
 *   wpa_ctrl_event() call that is the last statement in the SCANU_START_CFM
 *   case and whose failure costs nothing.  So the results are complete and
 *   sorted whether or not anything is listening.
 *
 *   sa_station_init() is what brings the MAC up far enough to scan -- reset,
 *   me_config, chan_config, start -- and it guards itself on whether a VIF
 *   already exists, so calling it again is harmless.
 *
 ****************************************************************************/

/* The station VIF, created on first scan.  File-scope rather than local to
 * scan_start because the TX path needs it too: bmsg_tx_sender() takes the
 * VIF index with every frame, and a wrong one makes frames vanish without
 * an error.
 */

static uint8_t g_bk7258_vif_idx = 0xff;

uint8_t bk7258_wifi_vif(void)
{
#ifdef BK7258_WIFI_WPA
  /* On the supplicant build the STA VIF is created by the supplicant deep
   * inside wlan_sta_connect(), which runs on the wpas thread AFTER
   * sta_ensure() has already tried (and failed) to recover the index.  The
   * TX path calls this for every data frame, so resolve it lazily here:
   * once the supplicant has added the VIF, rwm_mgmt_vif_mac2idx() finds it
   * by the station MAC.  Before that -- e.g. an EAPOL frame mid-handshake
   * -- 0xff is the honest answer and bmsg_tx_sender() drops the frame,
   * which is correct, there is no VIF to send it on yet.
   *
   * Symptom this fixes: DHCP DISCOVER left transmit() with vif 255 and the
   * MAC silently discarded it, so an associated WPA station never got an
   * address.
   */

  if (g_bk7258_vif_idx == 0xff)
    {
      uint8_t mac[6];

      bk_wifi_sta_get_mac(mac);
      g_bk7258_vif_idx = rwm_mgmt_vif_mac2idx(mac);
    }
#endif

  return g_bk7258_vif_idx;
}

#ifdef BK7258_WIFI_WPA

/****************************************************************************
 * Name: bk7258_wifi_sta_ensure
 *
 * Description:
 *   Bring the supplicant's STA interface up, once.  wlan_sta_enable() makes
 *   the supplicant create its own STA VIF (wpa_driver_init ->
 *   PRISM2_HOSTAPD_WPA_INIT_VIF -> rw_msg_send_add_if), so this port must
 *   NOT pre-add one the way the supplicant-less build does -- two adds of
 *   the same MAC/type is a state the vendor flow never exercises.  The VIF
 *   index is recovered afterwards from the MAC, for the TX and scan paths.
 *
 *   The wpas thread itself was started by bk_wifi_init(): the vendor's
 *   wifi_init() ends by calling wpas_thread_start() once the real
 *   main_supplicant.c is in the image.
 *
 ****************************************************************************/

static int bk7258_wifi_sta_ensure(void)
{
  static bool enabled = false;
  uint8_t mac[6];

  if (!enabled)
    {
      /* Vendor order (bk_wlan_start_sta -> bk_wlan_sta_init): the MAC gets
       * reset/me_config/chan_config BEFORE the supplicant comes up.  The
       * first WPA build skipped this and every channel lookup in the scan
       * came back null (freq 0xaaaa) -- the channel table was never
       * configured.  sa_station_init() guards itself on an existing VIF,
       * so it runs the full init exactly once.
       */

      sa_station_init();

      if (wlan_sta_enable() != 0)
        {
          return -1;
        }

      enabled = true;
    }

  if (g_bk7258_vif_idx == 0xff)
    {
      bk_wifi_sta_get_mac(mac);
      g_bk7258_vif_idx = rwm_mgmt_vif_mac2idx(mac);
    }

  return g_bk7258_vif_idx == 0xff ? -1 : 0;
}

/* The supplicant's scan results, fetched once per scan and held until the
 * next one.  wlan_sta_scan_result() drains AND flushes the supplicant's BSS
 * table (wifi_v2.c:1121), so fetching per ioctl would hand the first caller
 * everything and every later caller nothing -- the same shape as the
 * sr_get/sr_release lesson, one layer up.  The fetch happens only once the
 * supplicant has marked the scan complete (SCAN_DONE), because a mid-scan
 * fetch would flush the half-filled table and lose the early APs.
 */

static ScanResult_adv g_bk7258_aplist;

/* Set by bk_event_post() when the supplicant announces EVENT_WIFI_SCAN_DONE,
 * cleared when a new scan starts.  The linkstate cannot serve here: nothing
 * on the supplicant's normal path ever writes WIFI_LINKSTATE_STA_SCAN_DONE
 * (only the AT-command layer does, wifi_at.c:802), so a gate on it never
 * opens.  The event, by contrast, must pass through our bk_event_post.
 */

static volatile bool g_bk7258_scan_done;


int bk7258_wifi_scan_start(void)
{
  if (bk7258_wifi_sta_ensure() != 0)
    {
      return -1;
    }

  if (g_bk7258_aplist.ApList != NULL)
    {
      os_free(g_bk7258_aplist.ApList);
      g_bk7258_aplist.ApList = NULL;
      g_bk7258_aplist.ApNum = 0;
    }

  g_bk7258_scan_done = false;

  return wlan_sta_scan_once() == 0 ? 0 : -1;
}

static void bk7258_wifi_scan_fetch(void)
{
  if (g_bk7258_aplist.ApList != NULL)
    {
      return;
    }

  if (!g_bk7258_scan_done)
    {
      return;
    }

  g_bk7258_aplist.ApList = NULL;
  g_bk7258_aplist.ApNum = 0;

  if (wlan_sta_scan_result(&g_bk7258_aplist) != 0)
    {
      g_bk7258_aplist.ApList = NULL;
      g_bk7258_aplist.ApNum = 0;
    }
}

int bk7258_wifi_scan_count(void)
{
  bk7258_wifi_scan_fetch();
  return g_bk7258_aplist.ApList != NULL ? g_bk7258_aplist.ApNum : 0;
}

int bk7258_wifi_scan_acquire(void)
{
  bk7258_wifi_scan_fetch();
  return g_bk7258_aplist.ApList != NULL ? g_bk7258_aplist.ApNum : 0;
}

void bk7258_wifi_scan_release(void)
{
  /* The cache lives until the next scan replaces it. */
}

int bk7258_wifi_scan_get(int index, struct bk7258_scan_ap_s *ap)
{
  const struct ApListStruct *item;

  if (ap == NULL || index < 0 || g_bk7258_aplist.ApList == NULL ||
      index >= g_bk7258_aplist.ApNum)
    {
      return -1;
    }

  item = &g_bk7258_aplist.ApList[index];

  os_memcpy(ap->bssid, item->bssid, sizeof(ap->bssid));
  os_memcpy(ap->ssid, item->ssid, 32);
  ap->ssid[32] = '\0';
  ap->channel  = (uint8_t)item->channel;

  /* ApPower, despite its declaration comment ("min:0, max:100"), is the
   * raw dBm level on this fork: the ctrl handler stores bss->level into it
   * unconverted (ctrl_iface.c:295).  The first version here inverted the
   * documented 0..100 scale and reported every AP 60-80 dB too low.
   */

  ap->rssi     = (int32_t)(int8_t)item->ApPower;
  ap->caps     = 0;
  ap->security = (int)item->security;
  return 0;
}

#else /* !BK7258_WIFI_WPA */

int bk7258_wifi_scan_start(void)
{
  SCAN_PARAM_T param;
  int ret;

  sa_station_init();

  if (g_bk7258_vif_idx == 0xff)
    {
      struct mm_add_if_cfm cfm;
      uint8_t mac[6];

      bk_wifi_sta_get_mac(mac);

      ret = rw_msg_send_add_if(mac, NL80211_IFTYPE_STATION, 0, &cfm);
      if (ret != 0 || cfm.status != 0)
        {
          return -1;
        }

      g_bk7258_vif_idx = cfm.inst_nbr;
    }

  /* All-zero means: every supported channel, no SSID filter, no extra IEs.
   * rw_msg_send_scanu_req() reads freqs[0] == 0 as "use
   * rw_ieee80211_init_scan_chan()", which is the full channel list.
   *
   * The BSSID is the exception, and zeroing it was a real bug: the wildcard
   * BSSID in 802.11 is broadcast, not all-zero.  rw_msg_send_scanu_req()
   * copies this field into the request verbatim (rw_msg_tx.c:1046) and the
   * LMAC filters received frames against it in scanu_frame_handler
   * (0x0209062e..0x2090670), so an all-zero filter matches no real AP and
   * every beacon is dropped after being counted.  That is exactly what the
   * board reported once the receiver started working: recv_cnt=39 with
   * upload_cnt=0.
   */

  os_memset(&param, 0, sizeof(param));
  os_memset(&param.bssid, 0xff, sizeof(param.bssid));
  param.vif_idx = g_bk7258_vif_idx;

  return rw_msg_send_scanu_req(&param) == 0 ? 0 : -1;
}

#endif /* BK7258_WIFI_WPA */

/****************************************************************************
 * Name: bk7258_wifi_connect_open
 *
 * Description:
 *   Associate with an AP, without the supplicant.
 *
 *   Same seam as bk7258_wifi_scan_start(): bk_wifi_sta_connect() goes
 *   through wpa_ctrl_request(), which this port stubs out, so this drops to
 *   the layer underneath.  sa_station_send_associate_cmd() (sa_station.c:52,
 *   the !CONFIG_SME branch -- CONFIG_SME is not set in the vendor
 *   sdkconfig.h this build uses) looks the SSID up in the scan results with
 *   scanu_search_by_ssid(), takes the channel from there, and issues
 *   SM_CONNECT_REQ.
 *
 *   Two consequences of that lookup worth knowing before reading a failure:
 *   a scan has to have run, and the AP has to still be in its results -- the
 *   set is discarded on the next scan or on connect (rw_msg_rx.c:1322,
 *   :1350), so "connect" means "connect to something the last scan saw".
 *
 *   WHAT THIS DOES NOT DO IS THE HANDSHAKE.  ie_len is zero, so the request
 *   carries no RSN element and nothing performs the EAPOL four-way exchange
 *   afterwards -- that is the supplicant's job and the supplicant is not
 *   ported.  Against an open AP that is the whole story and the link comes
 *   up.  Against WPA2 the association itself can still succeed, and then the
 *   AP deauthenticates when the handshake never arrives, which looks like a
 *   connection that works for a second or two.  Do not read that as success.
 *
 ****************************************************************************/

int bk7258_wifi_connect_open(const char *ssid, int ssid_len, int *status)
{
  CONNECT_PARAM_T param;
  int ret;

  if (ssid == NULL || ssid_len <= 0 || ssid_len > MAC_SSID_LEN)
    {
      return -1;
    }

  os_memset(&param, 0, sizeof(param));

  param.ssid.length = (uint8_t)ssid_len;
  os_memcpy(param.ssid.array, ssid, ssid_len);

  /* Broadcast BSSID: any radio advertising this SSID will do.  Same wildcard
   * rule the scan request needed -- all-zero is a literal address here, not
   * "don't care".
   */

  os_memset(&param.bssid, 0xff, sizeof(param.bssid));

  /* freq 0 selects the "normal case" branch, i.e. look the channel up from
   * the scan results rather than fast-connect to a remembered one.
   */

  param.chan.freq = 0;
  param.auth_type = 0;            /* open system */
  param.ie_len    = 0;
  param.bcn_len   = 0;

  ret = sa_station_send_associate_cmd(&param);

  if (status != NULL)
    {
      *status = ret;
    }

  return ret;
}

#ifdef BK7258_WIFI_WPA

/****************************************************************************
 * Name: bk7258_wifi_connect_sta
 *
 * Description:
 *   Join an AP through the supplicant -- WPA2/WPA3 or open, decided by the
 *   AP's IEs and whether a key is given.
 *
 *   This is deliberately just bk_wlan_start_sta(): the open-source template
 *   in wifi_v2.c:633 that does the whole vendor-blessed sequence --
 *   disconnect, set CONNECTING, fill g_sta_param_ptr, wpa_psk_request (the
 *   PBKDF2 precompute on the PSK-cache thread), wlan_sta_enable,
 *   wlan_sta_set (SSID -> PSK -> KEY_MGMT -> PROTO -> ciphers), and
 *   wlan_sta_connect.  Replicating those steps here would only create a
 *   second copy to keep honest.
 *
 * Returned Value:
 *   0 once the join has been handed to the supplicant.  Completion is
 *   asynchronous; the driver polls the link state, which the supplicant
 *   advances to CONNECTED only at WPA_COMPLETED -- after the 4-way
 *   handshake, which is the right moment for the carrier.
 *
 ****************************************************************************/

int bk7258_wifi_connect_sta(const char *ssid, int ssid_len, const char *key)
{
  network_InitTypeDef_st param;

  if (ssid == NULL || ssid_len <= 0 || ssid_len > 32)
    {
      return -1;
    }

  os_memset(&param, 0, sizeof(param));
  param.wifi_mode = BK_STATION;
  os_memcpy(param.wifi_ssid, ssid, ssid_len);

  if (key != NULL)
    {
      os_strlcpy(param.wifi_key, key, sizeof(param.wifi_key));
    }

  return bk_wlan_start_sta(&param) == kNoErr ? 0 : -1;
}

/****************************************************************************
 * Name: bk_event_post / bk_event_register_cb
 *
 * Description:
 *   The vendor event bus (components/bk_event), reduced to the one duty
 *   this port needs from it.  The supplicant's notify.c posts
 *   EVENT_WIFI_STA_CONNECTED / _DISCONNECTED here; wifi_netif.c registers
 *   callbacks it never gets.  Everything that matters for the driver --
 *   the link state -- travels through mhdr_set_station_status(), which the
 *   supplicant also advances; the one thing that does not is the carrier
 *   drop on a disconnect that the driver is not watching for, and that is
 *   what this forwards.
 *
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_wifi_sta_disconnect
 *
 * Description:
 *   Leave the AP through the supplicant.  wlan_sta_disconnect() both
 *   deauthenticates and cancels the supplicant's connect-retry loop; the
 *   same call heads bk_wlan_start_sta(), so a new connect after this is
 *   exactly the vendor's own resequence.
 *
 ****************************************************************************/

int bk7258_wifi_sta_disconnect(void)
{
  return wlan_sta_disconnect();
}

bk_err_t bk_event_post(event_module_t module, int event_id,
                       void *event_data, size_t event_data_size,
                       uint32_t timeout)
{
  (void)event_data;
  (void)event_data_size;
  (void)timeout;

  if (module == EVENT_MOD_WIFI)
    {
      if (event_id == EVENT_WIFI_STA_DISCONNECTED)
        {
          bk7258_wifi_link_lost();
        }
      else if (event_id == EVENT_WIFI_SCAN_DONE)
        {
          g_bk7258_scan_done = true;
        }
    }

  return BK_OK;
}

bk_err_t bk_event_register_cb(event_module_t module, int event_id,
                              event_cb_t event_cb, void *event_cb_arg)
{
  (void)module;
  (void)event_id;
  (void)event_cb;
  (void)event_cb_arg;
  return BK_OK;
}

#endif /* BK7258_WIFI_WPA */

/****************************************************************************
 * Name: bk7258_wifi_get_mac
 *
 * Description:
 *   The station's MAC address, as the vendor stack actually uses it on air.
 *
 *   This has to come from the vendor rather than from the shim's
 *   g_bk7258_base_mac, even though the two currently agree: the base address
 *   is what we hand the stack, and what it derives per interface from that
 *   is its business.  Reading it back is the only way to be sure the netdev
 *   and the radio claim the same address.
 *
 *   Valid only after bk_wifi_init(), i.e. after bk7258_wifi_vendor_init().
 *
 ****************************************************************************/

int bk7258_wifi_get_mac(uint8_t *mac)
{
  if (mac == NULL)
    {
      return -1;
    }

  return bk_wifi_sta_get_mac(mac) == BK_OK ? 0 : -1;
}

/****************************************************************************
 * Name: bk7258_wifi_link_state
 *
 * Description:
 *   Report the station link state, without the supplicant.
 *
 *   Two vendor APIs look like they answer this and only one of them can be
 *   used here:
 *
 *     wlan_sta_state()               -> wpa_ctrl_request(), which this port
 *                                       stubs out.  Always fails.
 *     bk_wifi_sta_get_link_status()  -> gates on wifi_sta_is_connected(),
 *                                       which reads s_wifi_state_bits, set
 *                                       only along the supplicant-driven
 *                                       bk_wifi_sta_connect() path we do not
 *                                       take.  Would always say
 *                                       DISCONNECTED.
 *
 *   bk_wifi_sta_get_linkstate_with_reason() has neither problem: it returns
 *   mhdr_get_station_status() directly (wifi_v2.c:2718-2725), and that state
 *   is written by the closed MAC itself through the adapter table entry
 *   bk_set_sta_status_wrapper (bk_wifi_adapter.c:215-218).  So it reflects
 *   what the LMAC saw -- SM_CONNECT_IND, a deauth, a disassoc -- rather than
 *   what a supplicant we do not run believes.
 *
 * Returned Value:
 *   A wifi_link_state_t value; WIFI_LINKSTATE_STA_CONNECTED (3) is up.
 *   Negative on error.
 *
 ****************************************************************************/

int bk7258_wifi_link_state(void)
{
  wifi_linkstate_reason_t info;

  if (bk_wifi_sta_get_linkstate_with_reason(&info) != BK_OK)
    {
      return -1;
    }

  return (int)info.state;
}

#ifndef BK7258_WIFI_WPA

/****************************************************************************
 * Name: bk7258_wifi_wpa_event
 *
 * Description:
 *   The supplicant's side of the connect/disconnect notifications, without
 *   the supplicant.
 *
 *   Why this exists: association against an open AP succeeds on the air --
 *   mm_set_vif_state, AID assigned, beacons tracked -- and the link state
 *   still reads IDLE forever.  The state variable
 *   (mhdr_get_station_status()) is only ever advanced by the supplicant
 *   when it receives WPA_CTRL_EVENT_CONNECT_IND, and this port stubs the
 *   whole wpa_ctrl surface, so nobody advanced it.  The stubbed events are
 *   exactly where the information leaves the vendor stack
 *   (mhdr_connect_ind, rw_msg_rx.c:347; SM_DISCONNECT_IND, :1384), so this
 *   is the honest place to stand in.
 *
 *   Called from the shim's wpa_ctrl_event_copy() stub with whatever event
 *   the vendor posted.  Handles the two that carry link state; everything
 *   else stays a logged stub.  Runs on the vendor core thread.
 *
 * Returned Value:
 *   0 if the event was consumed here, -1 for events not handled.
 *
 ****************************************************************************/

int bk7258_wifi_wpa_event(int event, const void *data, int len)
{
  wifi_linkstate_reason_t info;

  switch (event)
    {
      case WPA_CTRL_EVENT_CONNECT_IND:
        {
          const struct sm_connect_ind *ind = data;

          if (ind == NULL || len < (int)sizeof(*ind))
            {
              return -1;
            }

          info.state = ind->status_code == 0 ?
                       WIFI_LINKSTATE_STA_CONNECTED :
                       WIFI_LINKSTATE_STA_CONNECT_FAILED;
          info.reason_code = WIFI_REASON_MAX;
          mhdr_set_station_status(info);
          return 0;
        }

      case WPA_CTRL_EVENT_DISCONNECT_IND:
        {
          info.state = WIFI_LINKSTATE_STA_DISCONNECTED;
          info.reason_code = WIFI_REASON_MAX;
          mhdr_set_station_status(info);

          /* The AP is gone; the netdev must stop offering the route.  The
           * driver lowers the carrier -- from this (task) context that is
           * legal, it only schedules work.
           */

          bk7258_wifi_link_lost();
          return 0;
        }

      default:
        return -1;
    }
}

#endif /* !BK7258_WIFI_WPA */

#ifndef BK7258_WIFI_WPA

int bk7258_wifi_scan_count(void)
{
  /* Reads scan_rst_set_ptr->scanu_num under a critical section and touches
   * no reference count, so it is safe to call at any time.
   */

  return (int)sr_get_scan_number();
}

/****************************************************************************
 * Name: bk7258_wifi_scan_acquire / _release / _get
 *
 * Description:
 *   Read the vendor's scan result set without taking a reference on it.
 *
 *   sr_get_scan_results() / sr_release_scan_results() look like a lock and
 *   are not one -- they are a reference count, and rw_msg_rx.c:199 destroys
 *   everything when it reaches zero:
 *
 *       ptr->ref -= 1;
 *       if (ptr->ref) goto release_exit;
 *       sr_free_all(ptr);
 *       scan_rst_set_ptr = 0;
 *
 *   The set is created by SCANU_RESULT_IND with ref 0 (rw_msg_rx.c:1330) and
 *   is meant to live until the *next* scan flushes it or a connect does
 *   (:1322, :1350).  So any balanced get/release by a reader takes it 0 -> 1
 *   -> 0 and frees the results it was trying to read.  That is not a bug in
 *   the pairing, it is what the pairing is for: hostapd_intf.c:549 takes its
 *   reference and holds it for the whole upload precisely so that its
 *   eventual release is the consuming one.
 *
 *   This port only wants to look.  Two versions got this wrong before
 *   settling here -- first a get/release around every item, then one around
 *   the whole enumeration -- and both freed the list, because with ref
 *   starting at 0 it makes no difference where you put the pair.  On the
 *   board it showed as wapi's two ioctls reporting 23 results and then 0:
 *   the first is wapi_scan_stat()'s one-byte probe, whose only job is to ask
 *   whether results exist.
 *
 *   So read the global directly and touch no counter, which is exactly what
 *   the vendor's own sr_get_scan_number() does (rw_msg_rx.c:161).  It is not
 *   declared in any header, hence the extern here.
 *
 *   Acquire/release remain as a pair so the caller still brackets its walk
 *   and so this decision has somewhere to live; they cache and drop the
 *   pointer, nothing more.
 *
 ****************************************************************************/

extern SCAN_RST_UPLOAD_T *scan_rst_set_ptr;

static SCAN_RST_UPLOAD_T *g_bk7258_scan_set;

int bk7258_wifi_scan_acquire(void)
{
  g_bk7258_scan_set = scan_rst_set_ptr;
  if (g_bk7258_scan_set == NULL)
    {
      return 0;
    }

  return (int)g_bk7258_scan_set->scanu_num;
}

void bk7258_wifi_scan_release(void)
{
  g_bk7258_scan_set = NULL;
}

int bk7258_wifi_scan_get(int index, struct bk7258_scan_ap_s *ap)
{
  SCAN_RST_ITEM_T *item;

  if (ap == NULL || index < 0 || g_bk7258_scan_set == NULL)
    {
      return -1;
    }

  if (index >= g_bk7258_scan_set->scanu_num ||
      g_bk7258_scan_set->res[index] == NULL)
    {
      return -1;
    }

  item = g_bk7258_scan_set->res[index];

  os_memcpy(ap->bssid, item->bssid, sizeof(ap->bssid));
  os_memcpy(ap->ssid, item->ssid, sizeof(item->ssid));
  ap->ssid[sizeof(item->ssid)] = '\0';
  ap->channel = item->channel;
  ap->rssi    = item->level;
  ap->caps     = item->caps;
  ap->security = item->security;
  return 0;
}

#endif /* !BK7258_WIFI_WPA */

/* With the supplicant compiled in, get_security_type_from_ie comes from
 * wpa_supplicant/events.c (the original this block reimplemented) and
 * get_ie/get_vendor_ie from src/common/ieee802_11_common.c -- the vendor
 * versions are a strict superset (they add OWE detection).  Ours exist only
 * for the supplicant-less build.
 */

#ifndef BK7258_WIFI_WPA

/****************************************************************************
 * Name: bk7258_wifi_akm_cipher
 *
 * Description:
 *   Pull the AKM suite and pairwise cipher selectors out of an RSN or WPA
 *   information element.  Both have the same shape once the header is past:
 *
 *     RSN (EID 48):   version(2) groupcipher(4)
 *                     pairwise_cnt(2) pairwise[](4) akm_cnt(2) akm[](4) ...
 *     WPA (EID 221):  OUI(3) type(1) version(2) then the same
 *
 *   Only the last octet of each selector is returned, which is what
 *   distinguishes the suites within a given OUI: for 00-0F-AC (RSN) 2 is
 *   PSK and 8 is SAE on the AKM side, and 2 is TKIP and 4 is CCMP on the
 *   cipher side; 00-50-F2 (WPA) uses the same numbering.  Selectors with a
 *   foreign OUI are skipped rather than misread.
 *
 *   Every length is checked against the element's own end, because these
 *   bytes come off the air from an unknown transmitter.
 *
 ****************************************************************************/

#define BK7258_WIFI_OUI_RSN  0x00ac0f00u   /* 00-0F-AC, little-endian read */
#define BK7258_WIFI_OUI_WPA  0x00f25000u   /* 00-50-F2 */

static void bk7258_wifi_akm_cipher(const uint8_t *ie, size_t len,
                                   size_t offset, uint32_t oui,
                                   uint8_t *akm, uint8_t *cipher)
{
  uint16_t count;
  size_t i;

  *akm = 0;
  *cipher = 0;

  /* version(2) + group cipher(4) */

  if (offset + 6 > len)
    {
      return;
    }

  offset += 6;

  /* Pairwise cipher list */

  if (offset + 2 > len)
    {
      return;
    }

  count = (uint16_t)(ie[offset] | (ie[offset + 1] << 8));
  offset += 2;

  for (i = 0; i < count; i++)
    {
      if (offset + 4 > len)
        {
          return;
        }

      if ((uint32_t)(ie[offset] | (ie[offset + 1] << 8) |
                     (ie[offset + 2] << 16)) == (oui & 0x00ffffffu))
        {
          *cipher |= (uint8_t)(1u << (ie[offset + 3] & 7));
        }

      offset += 4;
    }

  /* AKM suite list */

  if (offset + 2 > len)
    {
      return;
    }

  count = (uint16_t)(ie[offset] | (ie[offset + 1] << 8));
  offset += 2;

  for (i = 0; i < count; i++)
    {
      if (offset + 4 > len)
        {
          return;
        }

      if ((uint32_t)(ie[offset] | (ie[offset + 1] << 8) |
                     (ie[offset + 2] << 16)) == (oui & 0x00ffffffu))
        {
          *akm |= (uint8_t)(1u << (ie[offset + 3] & 7));
        }

      offset += 4;
    }
}

/****************************************************************************
 * Name: get_security_type_from_ie
 *
 * Description:
 *   Classify an AP's security from its beacon IEs.  rw_msg_rx.c:1030 calls
 *   this for every scan result and stores the answer in the result item, so
 *   with it stubbed every AP came back with a meaningless security type --
 *   which is why "wapi scan_results" showed encode 0xffff for all of them.
 *
 *   The vendor's own version lives in the supplicant this port does not
 *   build (wpa_supplicant-2.10/wpa_supplicant/events.c:533) and leans on
 *   wpa_parse_wpa_ie_key_mgmt_and_pairwise_cipher() from wpa_common.c.
 *   This is the same decision tree over a local parser: privacy bit clear
 *   means open, RSN wins over the WPA vendor IE, SAE outranks PSK, CCMP
 *   outranks TKIP, and privacy set with neither IE present is WEP.
 *
 ****************************************************************************/

#define BK7258_WIFI_CAP_PRIVACY   (1u << 4)
#define BK7258_WIFI_EID_RSN       48
#define BK7258_WIFI_EID_VENDOR    221

#define BK7258_WIFI_AKM_8021X     (1u << 1)   /* selector 1 */
#define BK7258_WIFI_AKM_PSK       (1u << 2)   /* selector 2 */
#define BK7258_WIFI_AKM_SAE       (1u << 0)   /* selector 8, & 7 == 0 */
#define BK7258_WIFI_CIPHER_TKIP   (1u << 2)   /* selector 2 */
#define BK7258_WIFI_CIPHER_CCMP   (1u << 4)   /* selector 4 */

int get_security_type_from_ie(uint8_t *ie_start, int len, uint16_t caps)
{
  const uint8_t *rsn;
  const uint8_t *wpa;
  uint8_t akm;
  uint8_t cipher;
  size_t i;

  if ((caps & BK7258_WIFI_CAP_PRIVACY) == 0)
    {
      return BK_SECURITY_TYPE_NONE;
    }

  if (ie_start == NULL || len <= 0)
    {
      return BK_SECURITY_TYPE_WEP;
    }

  rsn = get_ie(ie_start, (size_t)len, BK7258_WIFI_EID_RSN);
  if (rsn != NULL)
    {
      bk7258_wifi_akm_cipher(rsn, 2 + rsn[1], 2, BK7258_WIFI_OUI_RSN,
                             &akm, &cipher);

      if ((akm & BK7258_WIFI_AKM_SAE) != 0)
        {
          return (akm & BK7258_WIFI_AKM_PSK) != 0 ?
                 BK_SECURITY_TYPE_WPA3_WPA2_MIXED : BK_SECURITY_TYPE_WPA3_SAE;
        }

      if ((akm & BK7258_WIFI_AKM_PSK) != 0)
        {
          if ((cipher & BK7258_WIFI_CIPHER_CCMP) != 0 &&
              (cipher & BK7258_WIFI_CIPHER_TKIP) != 0)
            {
              return BK_SECURITY_TYPE_WPA2_MIXED;
            }

          return (cipher & BK7258_WIFI_CIPHER_CCMP) != 0 ?
                 BK_SECURITY_TYPE_WPA2_AES : BK_SECURITY_TYPE_WPA2_TKIP;
        }

      if ((akm & BK7258_WIFI_AKM_8021X) != 0)
        {
          return BK_SECURITY_TYPE_EAP;
        }
    }

  /* The WPA element is a vendor element: 00-50-F2 with type 1. */

  wpa = NULL;
  for (i = 0; i + 2 <= (size_t)len; i += 2 + ie_start[i + 1])
    {
      if (ie_start[i] == BK7258_WIFI_EID_VENDOR && ie_start[i + 1] >= 4 &&
          i + 2 + ie_start[i + 1] <= (size_t)len &&
          ie_start[i + 2] == 0x00 && ie_start[i + 3] == 0x50 &&
          ie_start[i + 4] == 0xf2 && ie_start[i + 5] == 0x01)
        {
          wpa = &ie_start[i];
          break;
        }
    }

  if (wpa != NULL)
    {
      bk7258_wifi_akm_cipher(wpa, 2 + wpa[1], 6, BK7258_WIFI_OUI_WPA,
                             &akm, &cipher);

      if ((akm & BK7258_WIFI_AKM_PSK) != 0)
        {
          if ((cipher & BK7258_WIFI_CIPHER_CCMP) != 0 &&
              (cipher & BK7258_WIFI_CIPHER_TKIP) != 0)
            {
              return BK_SECURITY_TYPE_WPA_MIXED;
            }

          return (cipher & BK7258_WIFI_CIPHER_CCMP) != 0 ?
                 BK_SECURITY_TYPE_WPA_AES : BK_SECURITY_TYPE_WPA_TKIP;
        }

      if ((akm & BK7258_WIFI_AKM_8021X) != 0)
        {
          return BK_SECURITY_TYPE_EAP;
        }
    }

  if (rsn == NULL && wpa == NULL)
    {
      return BK_SECURITY_TYPE_WEP;
    }

  return BK_SECURITY_TYPE_NONE;
}

#endif /* !BK7258_WIFI_WPA */
