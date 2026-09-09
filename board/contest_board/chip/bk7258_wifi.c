/****************************************************************************
 * board/contest_board/chip/bk7258_wifi.c
 *
 * WiFi network device for the BK7258 -- the openvela-facing half.
 *
 * This file is the netdev lower half: it registers wlan0 and answers the
 * wireless ioctls.  The vendor MAC stack lives beside it, compiled from
 * bk_idk with the vendor's own flags (bk7258_wifi_vendor.cmake) and driven
 * from ifup() below.
 *
 * An earlier version of this comment claimed the vendor stack could not be
 * compiled at all -- that two internal headers, sm_task.h and ps.h, were
 * missing from every published SDK.  That was wrong, and the way it was
 * wrong is worth keeping: both #includes sit inside
 * "#if NX_VERSION > NX_VERSION_PACK(6,22,0,0)", and this tree is 6.8.2.0,
 * so the preprocessor never reaches them.  The conclusion came from
 * grepping for the include lines without checking what guarded them.  All
 * 33 sources in components/bk_wifi/src compile as published.
 *
 * The openvela side follows
 * docs/zh-cn/device_dev_guide/connection/network/driver/net_driver_guide.md,
 * with a complete worked example in nuttx/drivers/net/wifi_sim.c.
 *
 * The shape openvela expects is worth stating, because it is not the one a
 * Linux background suggests: wireless_ops_s carries essid, passwd and auth
 * alongside connect, so association and the security handshake belong to the
 * driver.  There is no separate supplicant process to feed -- the vendor's
 * own supplicant is linked into the image and driven from underneath.
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

#include <nuttx/config.h>

#include <debug.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>

#include <net/if.h>
#include <net/if_arp.h>
#include <nuttx/kmalloc.h>
#include <nuttx/net/net.h>
#include <nuttx/net/netdev_lowerhalf.h>
#include <nuttx/wireless/wireless.h>

#include <nuttx/signal.h>
#include <nuttx/wqueue.h>

#include "bk7258_wifi.h"
#include "bk7258_wifi_scan.h"

/* How long to wait for SM_CONNECT_IND before giving up on an association,
 * and how often to look.  Three seconds is well past what an AP on the same
 * channel needs -- the measured auth+assoc exchange completed in tens of
 * milliseconds -- and short enough that a shell blocked on "wapi essid"
 * comes back rather than appearing hung.
 */

#ifdef CONFIG_BK7258_WIFI_WPA
/* The supplicant path adds a PBKDF2 precompute (4096 iterations, on a
 * priority-100 thread), its own scan, and the 4-way handshake.
 */

#  define BK7258_WIFI_CONNECT_TIMEOUT_MS  20000
#else
#  define BK7258_WIFI_CONNECT_TIMEOUT_MS  3000
#endif
#define BK7258_WIFI_CONNECT_POLL_MS     50

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* How many packets the driver may hold.  Kept small until the vendor side
 * exists and the real depth of its queues is known -- a quota that lies is
 * worse than one that is conservative, because the upper half throttles on
 * it.
 */

#define BK7258_WIFI_TX_QUOTA   8
#define BK7258_WIFI_RX_QUOTA   8

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct bk7258_wifi_dev_s
{
  struct netdev_lowerhalf_s dev;    /* Must be first */

  /* Association parameters, held here until connect() hands them down.  The
   * wireless ops set them one at a time in any order, so they have to live
   * somewhere until the caller says go.
   */

  uint8_t  ssid[32 + 1];
  uint8_t  ssid_len;
  uint8_t  bssid[IFHWADDRLEN];
  bool     bssid_set;
  char     passwd[64 + 1];
  uint32_t auth;                    /* IW_AUTH_WPA_VERSION_* */
  uint32_t mode;                    /* IW_MODE_INFRA or IW_MODE_MASTER */
  bool     connected;

  /* RX ring.  Filled by bk7258_wifi_rx_frame() on the vendor core thread,
   * drained by receive() on the network worker.  Sized to twice the RX
   * quota although quota alone bounds the occupancy at 8: netpkt_alloc()
   * refuses to allocate past the quota, and every allocated packet is
   * either in this ring or already handed to the upper half.  The slack
   * costs 32 bytes and turns "can never overflow" from an argument into
   * an array bound.
   */

  struct work_s lostwork;           /* Deferred carrier-off, see link_lost */
  FAR netpkt_t *rxq[BK7258_WIFI_RX_QUOTA * 2];
  uint8_t  rxhead;                  /* Next slot to fill */
  uint8_t  rxtail;                  /* Next slot to drain */
  uint32_t rxdrops;                 /* Frames dropped for want of quota */
};

#define BK7258_WIFI_RXQ_MASK  (BK7258_WIFI_RX_QUOTA * 2 - 1)

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int bk7258_wifi_ifup(FAR struct netdev_lowerhalf_s *dev);
static int bk7258_wifi_ifdown(FAR struct netdev_lowerhalf_s *dev);
static int bk7258_wifi_transmit(FAR struct netdev_lowerhalf_s *dev,
                                FAR netpkt_t *pkt);
static FAR netpkt_t *bk7258_wifi_receive(FAR struct netdev_lowerhalf_s *dev);

static int bk7258_wifi_connect(FAR struct netdev_lowerhalf_s *dev);
static int bk7258_wifi_disconnect(FAR struct netdev_lowerhalf_s *dev);
static int bk7258_wifi_essid(FAR struct netdev_lowerhalf_s *dev,
                             FAR struct iwreq *iwr, bool set);
static int bk7258_wifi_bssid(FAR struct netdev_lowerhalf_s *dev,
                             FAR struct iwreq *iwr, bool set);
static int bk7258_wifi_passwd(FAR struct netdev_lowerhalf_s *dev,
                              FAR struct iwreq *iwr, bool set);
static int bk7258_wifi_mode(FAR struct netdev_lowerhalf_s *dev,
                            FAR struct iwreq *iwr, bool set);
static int bk7258_wifi_auth(FAR struct netdev_lowerhalf_s *dev,
                            FAR struct iwreq *iwr, bool set);
static int bk7258_wifi_freq(FAR struct netdev_lowerhalf_s *dev,
                            FAR struct iwreq *iwr, bool set);
static int bk7258_wifi_bitrate(FAR struct netdev_lowerhalf_s *dev,
                               FAR struct iwreq *iwr, bool set);
static int bk7258_wifi_txpower(FAR struct netdev_lowerhalf_s *dev,
                               FAR struct iwreq *iwr, bool set);
static int bk7258_wifi_country(FAR struct netdev_lowerhalf_s *dev,
                               FAR struct iwreq *iwr, bool set);
static int bk7258_wifi_sensitivity(FAR struct netdev_lowerhalf_s *dev,
                                   FAR struct iwreq *iwr, bool set);
static int bk7258_wifi_scan(FAR struct netdev_lowerhalf_s *dev,
                            FAR struct iwreq *iwr, bool set);
static int bk7258_wifi_range(FAR struct netdev_lowerhalf_s *dev,
                             FAR struct iwreq *iwr);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct netdev_ops_s g_bk7258_wifi_ops =
{
  .ifup     = bk7258_wifi_ifup,
  .ifdown   = bk7258_wifi_ifdown,
  .transmit = bk7258_wifi_transmit,
  .receive  = bk7258_wifi_receive,

  /* addmac / rmmac are unimplemented on purpose: multicast filtering is the
   * MAC block's job and the vendor stack owns that register file.  reclaim
   * likewise waits for the vendor side -- there is nothing to reclaim until
   * something can queue.
   */
};

static const struct wireless_ops_s g_bk7258_wifi_iw_ops =
{
  .connect     = bk7258_wifi_connect,
  .disconnect  = bk7258_wifi_disconnect,
  .essid       = bk7258_wifi_essid,
  .bssid       = bk7258_wifi_bssid,
  .passwd      = bk7258_wifi_passwd,
  .mode        = bk7258_wifi_mode,
  .auth        = bk7258_wifi_auth,
  .freq        = bk7258_wifi_freq,
  .bitrate     = bk7258_wifi_bitrate,
  .txpower     = bk7258_wifi_txpower,
  .country     = bk7258_wifi_country,
  .sensitivity = bk7258_wifi_sensitivity,
  .scan        = bk7258_wifi_scan,
  .range       = bk7258_wifi_range,
};

static struct bk7258_wifi_dev_s g_bk7258_wifi;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* Everything below that ends in -ENOSYS is waiting on the vendor headers,
 * not on a decision.  They are separated from the ones that already do their
 * job so that "what is left" stays readable as the port fills in.
 */

/****************************************************************************
 * Name: bk7258_wifi_ifup / bk7258_wifi_ifdown
 ****************************************************************************/

static int bk7258_wifi_ifup(FAR struct netdev_lowerhalf_s *dev)
{
  UNUSED(dev);

#ifdef CONFIG_BK7258_WIFI_VENDOR
  /* First real call into the vendor stack.  Nothing above this point in the
   * port has ever executed vendor MAC code, so a link failure here is the
   * honest signal that the integration is not finished -- which is why the
   * call is made from ifup() rather than hidden behind another stub.
   *
   * It goes through bk7258_wifi_glue.c rather than straight to
   * bk_wifi_init() because the stack's entry point takes a config struct
   * whose type only exists behind the vendor include path.  Declaring it
   * here by hand once seemed harmless and cost an afternoon: see that
   * file's header.
   */

    {
      extern int bk7258_phy_adapter_init(void);
      extern int bk7258_rf_adapter_init(void);
      extern int bk7258_wifi_vendor_init(void);
      extern void bk7258_wifi_zeroing(bool on);
      static bool phy_ready = false;
      uint8_t mac[IFHWADDRLEN];
      int ret;

      /* The radio's OS abstraction, before anything reaches the radio.
       * bk7258_phy_osi.c has carried these tables since the BLE port and
       * says so in its own header -- but only bk7258_ble.c ever called the
       * initialisers, and the xts configuration builds without BLE.
       *
       * The cost of missing this is not a link error.  wifi_init() calls
       * rf_module_vote_ctrl() inside libbk_phy.a, which loads g_rf_funcs_t
       * out of .bss and branches through offset 16 of the NULL it finds:
       * an instruction bus fault in closed code, six frames below anything
       * we wrote, with the WiFi stack looking like the culprit.
       *
       * Guarded because a BLE-enabled image runs them at BLE bring-up and
       * this would be the second time.
       */

      if (!phy_ready)
        {
          extern void bk7258_wifi_fault_probe_install(void);
          bk7258_wifi_fault_probe_install();

          bk7258_phy_adapter_init();
          bk7258_rf_adapter_init();
          phy_ready = true;
        }

      /* Left on for the life of the stack, not just across init.  The
       * first attempt scoped it to bk7258_wifi_vendor_init() and the board
       * got all the way through RF calibration before faulting on a
       * semaphore handle that was garbage rather than NULL -- the same
       * assumption, in an allocation made from the work queue after the
       * window had closed.  The assumption belongs to the vendor stack as
       * a whole.
       */

      /* bk_wifi_init() is one-shot: the vendor stack has no deinit path,
       * and running it twice re-creates its threads and queues over the
       * live ones.  "ifconfig wlan0 down; ifconfig wlan0 up" therefore
       * re-enters here with the stack already running, and must not
       * initialise it again.
       */

      static bool vendor_ready = false;

      if (vendor_ready)
        {
          return OK;
        }

      bk7258_wifi_zeroing(true);
      ret = bk7258_wifi_vendor_init();

      if (ret != 0)
        {
          nerr("ERROR: bk_wifi_init: %d\n", ret);
          return -EIO;
        }

      vendor_ready = true;

      /* Adopt the address the radio actually uses.  Until this ran, wlan0
       * came up with d_mac all zeroes while the MAC associated as its real
       * address -- so every ARP reply and DHCP request the stack built
       * carried a source address no AP would ever send a frame back to.
       * Nothing else in this port writes d_mac: netinit's assignment path
       * is compiled out in both configurations.
       *
       * It has to happen here rather than in initialize(), because the
       * address is only meaningful once bk_wifi_init() has run.
       */

      if (bk7258_wifi_get_mac(mac) == 0)
        {
          memcpy(dev->netdev.d_mac.ether.ether_addr_octet, mac, IFHWADDRLEN);
          ninfo("wlan0 mac %02x:%02x:%02x:%02x:%02x:%02x\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        }
      else
        {
          nerr("ERROR: could not read the station MAC\n");
        }

      ninfo("vendor WiFi stack initialised\n");
      return OK;
    }
#else
  nerr("ERROR: vendor WiFi sources not built (CONFIG_BK7258_WIFI_VENDOR)\n");
  return -ENOSYS;
#endif
}

static int bk7258_wifi_ifdown(FAR struct netdev_lowerhalf_s *dev)
{
  /* Administrative down: leave the AP cleanly and stop offering the route.
   * The vendor stack itself stays up -- it has no deinit path, which is
   * also why ifup() refuses to run its one-shot init twice.  Downing the
   * interface is about the link, not the radio.
   */

  return bk7258_wifi_disconnect(dev);
}

/****************************************************************************
 * Name: bk7258_wifi_transmit
 *
 * Description:
 *   Hand one ethernet frame to the MAC.
 *
 *   Ownership: on OK the packet is ours and we must netpkt_free() it; on a
 *   negative return the upper half recycles it and does NOT retry -- it
 *   bumps TXERRORS, puts the iob back and aborts the current poll
 *   (netdev_upperhalf.c:315-325).  An earlier comment here claimed it
 *   retried; it does not, which is why a vendor-side drop is reported as
 *   OK-and-dropped below rather than as an error the stack cannot act on.
 *
 *   The frame is always copied out rather than handed over by pointer, for
 *   two reasons that would each suffice.  NETPKT_BUFLEN is
 *   CONFIG_IOB_BUFSIZE = 196 in this build, so any full-size frame is
 *   fragmented across iobs and netpkt_getdata() would see only the first
 *   piece.  And the vendor consumes a pbuf whose payload must be preceded
 *   by CONFIG_MSDU_RESV_HEAD_LENGTH (96) bytes of headroom --
 *   rwnx_start_xmit() wraps the pbuf in an sk_buff in place
 *   (alloc_skb_with_pbuf) rather than copying -- so the bytes have to land
 *   in a vendor-shaped buffer anyway.  bk7258_wifi_tx_alloc() allocates it
 *   with exactly the headroom the vendor's own lwIP port would have given
 *   it (PBUF_RAW_TX).
 *
 *   Serialization: the upper half sends one packet at a time (upper->txing
 *   plus the net lock), so there is no queue here and no reclaim() needed
 *   -- the netpkt is freed before this function returns.
 *
 ****************************************************************************/

static int bk7258_wifi_transmit(FAR struct netdev_lowerhalf_s *dev,
                                FAR netpkt_t *pkt)
{
  unsigned int len = netpkt_getdatalen(dev, pkt);
  FAR uint8_t *payload;
  FAR void *frame;
  int ret;

  frame = bk7258_wifi_tx_alloc(len, &payload);
  if (frame == NULL)
    {
      /* Out of heap.  The upper half recycles the netpkt. */

      return -ENOMEM;
    }


  ret = netpkt_copyout(dev, payload, pkt, len, 0);
  if (ret < 0)
    {
      bk7258_wifi_tx_abort(frame);
      return ret;
    }

  /* tx_send consumes the pbuf whether the vendor queue took it or not.  A
   * full vendor queue is congestion; the frame is gone either way and the
   * stack's own timers are the recovery, so the answer is OK.
   */

  bk7258_wifi_tx_send(frame);
  netpkt_free(dev, pkt, NETPKT_TX);
  return OK;
}

/****************************************************************************
 * Name: bk7258_wifi_receive
 *
 * Description:
 *   Called after netdev_lower_rxready().  Returns one packet or NULL when
 *   the driver has none left; the upper half keeps calling until NULL.
 *
 *   Must tolerate being called with nothing queued: netdev_upper_work()
 *   runs the RX poll before the TX poll, so this is also called after
 *   every transmit completion, not only after rxready.
 *
 ****************************************************************************/

static FAR netpkt_t *bk7258_wifi_receive(FAR struct netdev_lowerhalf_s *dev)
{
  FAR struct bk7258_wifi_dev_s *priv = (FAR struct bk7258_wifi_dev_s *)dev;
  FAR netpkt_t *pkt = NULL;
  irqstate_t flags;

  flags = enter_critical_section();
  if (priv->rxtail != priv->rxhead)
    {
      pkt = priv->rxq[priv->rxtail & BK7258_WIFI_RXQ_MASK];
      priv->rxtail++;
    }

  leave_critical_section(flags);
  return pkt;
}

/****************************************************************************
 * Name: bk7258_wifi_rx_frame
 *
 * Description:
 *   Where received frames arrive from the vendor stack -- handed across
 *   the flag boundary by ethernetif_input() in bk7258_wifi_pbuf.c, running
 *   on the vendor core thread.  Task context, so netpkt_alloc() is legal
 *   here.
 *
 *   The frame is already an 802.3 ethernet frame: the closed MAC converts
 *   from 802.11 before upload (rwm_upload_data), and ethernetif_input
 *   flattens any chain before calling.
 *
 *   netpkt_alloc() returning NULL is the flow control: it refuses past the
 *   RX quota, which means the network worker has 8 packets it has not
 *   drained yet.  Dropping at the edge is what every driver in the tree
 *   does when the stack is behind; the counter makes it visible.
 *
 ****************************************************************************/

void bk7258_wifi_rx_frame(int iface, FAR const void *data, unsigned int len)
{
  FAR struct bk7258_wifi_dev_s *priv = &g_bk7258_wifi;
  FAR struct netdev_lowerhalf_s *dev = &priv->dev;
  FAR netpkt_t *pkt;
  irqstate_t flags;

  UNUSED(iface);

  pkt = netpkt_alloc(dev, NETPKT_RX);
  if (pkt == NULL)
    {
      priv->rxdrops++;
      return;
    }

  if (netpkt_copyin(dev, pkt, data, len, 0) < 0)
    {
      netpkt_free(dev, pkt, NETPKT_RX);
      priv->rxdrops++;
      return;
    }

  flags = enter_critical_section();
  priv->rxq[priv->rxhead & BK7258_WIFI_RXQ_MASK] = pkt;
  priv->rxhead++;
  leave_critical_section(flags);

  netdev_lower_rxready(dev);
}

/****************************************************************************
 * Name: bk7258_wifi_link_lost
 *
 * Description:
 *   The AP dropped us -- SM_DISCONNECT_IND arrived, relayed by the event
 *   bridge in bk7258_wifi_glue.c.  Lower the carrier so the stack stops
 *   routing to an interface that can no longer deliver.  Runs on the
 *   vendor core thread; carrier_off only schedules work, so that is legal.
 *
 ****************************************************************************/

static void bk7258_wifi_link_lost_worker(FAR void *arg)
{
  FAR struct bk7258_wifi_dev_s *priv = arg;

  netdev_lower_carrier_off(&priv->dev);
  syslog(LOG_WARNING, "wifi: link lost, carrier down\n");
}

void bk7258_wifi_link_lost(void)
{
  FAR struct bk7258_wifi_dev_s *priv = &g_bk7258_wifi;

  if (priv->connected)
    {
      priv->connected = false;

      /* Deferred, never inline: this runs on the wpas thread, and
       * netdev_lower_carrier_off() takes the netdev lock.  During ifdown
       * that lock is held by the caller's ioctl, which is itself blocked
       * inside wlan_sta_disconnect() waiting for THIS thread to post the
       * ctrl semaphore -- calling carrier_off here closes the cycle and
       * both threads wait forever.  Measured, not theorized: the first
       * ifdown against a live association hung the shell exactly this way,
       * after a perfectly clean deauth.  (The driver's own disconnect()
       * calls carrier_off inline and safely: same thread as the ioctl, and
       * the netdev lock is recursive.)
       */

      work_queue(LPWORK, &priv->lostwork, bk7258_wifi_link_lost_worker,
                 priv, 0);
    }
}

/****************************************************************************
 * Name: bk7258_wifi_connect / bk7258_wifi_disconnect
 ****************************************************************************/

static int bk7258_wifi_connect(FAR struct netdev_lowerhalf_s *dev)
{
  FAR struct bk7258_wifi_dev_s *priv = (FAR struct bk7258_wifi_dev_s *)dev;

  int status = 0;
  int state = -1;
  int ret;
  int i;

  ninfo("connect: ssid '%.*s' auth %" PRIu32 " mode %" PRIu32 "\n",
        priv->ssid_len, priv->ssid, priv->auth, priv->mode);

  if (priv->ssid_len == 0)
    {
      return -EINVAL;
    }

#ifdef CONFIG_BK7258_WIFI_WPA
  /* Through the supplicant: WPA2/WPA3 by the AP's IEs, open if no key was
   * set.  The supplicant sets CONNECTING itself and advances to CONNECTED
   * only at WPA_COMPLETED -- after the 4-way handshake -- so the poll below
   * raises the carrier at the right moment for both cases.
   */

  ret = bk7258_wifi_connect_sta((FAR const char *)priv->ssid,
                                priv->ssid_len,
                                priv->passwd[0] != '\0' ? priv->passwd
                                                         : NULL);
#else
  ret = bk7258_wifi_connect_open((FAR const char *)priv->ssid,
                                 priv->ssid_len, &status);
#endif

  syslog(LOG_INFO, "wifi: connect '%.*s' -> %d (status %d)\n",
         priv->ssid_len, priv->ssid, ret, status);

  if (ret != 0)
    {
      return -EIO;
    }

  /* The request was accepted; that is not the same as being associated.
   * sa_station_send_associate_cmd() returns once SM_CONNECT_CFM says the
   * MAC took the request, and the outcome arrives later as SM_CONNECT_IND.
   *
   * Wait for it rather than guessing.  bk7258_wifi_link_state() reads the
   * state the closed MAC maintains through its own adapter callback, so it
   * reports what the LMAC saw -- association, or the deauth that follows a
   * WPA2 AP when no handshake arrives -- and not what the unported
   * supplicant thinks.  See the note in bk7258_wifi_glue.c for why the two
   * more obvious vendor APIs cannot be used here.
   *
   * Bounded, because this runs in the caller's ioctl context: wapi blocks
   * on it, and an unbounded wait against an AP that simply ignores us would
   * hang the shell.
   */

  for (i = 0; i < BK7258_WIFI_CONNECT_TIMEOUT_MS /
                  BK7258_WIFI_CONNECT_POLL_MS; i++)
    {
      state = bk7258_wifi_link_state();

      if (state == BK7258_WIFI_LINK_CONNECTED)
        {
          priv->connected = true;

          /* Only now is the link real, so only now does the carrier go up.
           * Until this call netdev_findbyaddr() and netdev_default() skip
           * wlan0 entirely -- they require IFF_RUNNING -- so nothing the
           * stack sends could ever reach transmit().
           */

          netdev_lower_carrier_on(dev);

          syslog(LOG_INFO, "wifi: connected to '%.*s'\n",
                 priv->ssid_len, priv->ssid);
          return OK;
        }

      if (state == BK7258_WIFI_LINK_CONNECT_FAILED ||
          state == BK7258_WIFI_LINK_DISCONNECTED)
        {
          syslog(LOG_ERR, "wifi: association to '%.*s' rejected (state %d)\n",
                 priv->ssid_len, priv->ssid, state);
          return -ECONNREFUSED;
        }

      nxsig_usleep(BK7258_WIFI_CONNECT_POLL_MS * 1000);
    }

  syslog(LOG_ERR, "wifi: '%.*s' never reached CONNECTED (last state %d)\n",
         priv->ssid_len, priv->ssid, state);
  return -ETIMEDOUT;
}

static int bk7258_wifi_disconnect(FAR struct netdev_lowerhalf_s *dev)
{
  FAR struct bk7258_wifi_dev_s *priv = (FAR struct bk7258_wifi_dev_s *)dev;

#ifdef CONFIG_BK7258_WIFI_WPA
  /* Tell the supplicant, not just the netdev.  Besides deauthenticating
   * cleanly, this is what stops the supplicant's connect-retry loop -- a
   * failed join otherwise keeps issuing directed scans forever.
   */

  bk7258_wifi_sta_disconnect();
#endif

  priv->connected = false;
  netdev_lower_carrier_off(dev);
  return OK;
}

/****************************************************************************
 * Name: bk7258_wifi_essid
 *
 * Description:
 *   Store or report the SSID.  This one is real: the parameter setters can
 *   be finished without the vendor stack, and having them work means the
 *   wapi command line can be exercised against the driver now.
 *
 ****************************************************************************/

static int bk7258_wifi_essid(FAR struct netdev_lowerhalf_s *dev,
                             FAR struct iwreq *iwr, bool set)
{
  FAR struct bk7258_wifi_dev_s *priv = (FAR struct bk7258_wifi_dev_s *)dev;
  FAR struct iw_point *essid = &iwr->u.essid;

  if (set)
    {
      if (essid->length > sizeof(priv->ssid) - 1)
        {
          return -EINVAL;
        }

      memcpy(priv->ssid, essid->pointer, essid->length);
      priv->ssid[essid->length] = '\0';
      priv->ssid_len = essid->length;
    }
  else
    {
      essid->length = priv->ssid_len;
      memcpy(essid->pointer, priv->ssid, priv->ssid_len);
      essid->flags = priv->connected ? IW_ESSID_ON : IW_ESSID_OFF;
    }

  return OK;
}

/****************************************************************************
 * Name: bk7258_wifi_bssid
 ****************************************************************************/

static int bk7258_wifi_bssid(FAR struct netdev_lowerhalf_s *dev,
                             FAR struct iwreq *iwr, bool set)
{
  FAR struct bk7258_wifi_dev_s *priv = (FAR struct bk7258_wifi_dev_s *)dev;

  if (set)
    {
      memcpy(priv->bssid, iwr->u.ap_addr.sa_data, IFHWADDRLEN);
      priv->bssid_set = true;
    }
  else
    {
      memcpy(iwr->u.ap_addr.sa_data, priv->bssid, IFHWADDRLEN);
    }

  return OK;
}

/****************************************************************************
 * Name: bk7258_wifi_passwd
 ****************************************************************************/

static int bk7258_wifi_passwd(FAR struct netdev_lowerhalf_s *dev,
                              FAR struct iwreq *iwr, bool set)
{
  FAR struct bk7258_wifi_dev_s *priv = (FAR struct bk7258_wifi_dev_s *)dev;
  FAR struct iw_point *data = &iwr->u.data;

  /* Only ever accepted, never reported: handing a key back out to any
   * caller that asks is not something this driver should do.
   */

  if (!set)
    {
      return -ENOTSUP;
    }

  /* The buffer is not the passphrase.  SIOCSIWENCODEEXT carries a struct
   * iw_encode_ext with the key appended after it (wapi builds it at
   * driver_wext.c:167-183: pointer -> ext, length = sizeof + key_len, key
   * at ext + 1).  The first version here copied the raw buffer -- header
   * and all -- and stored 32 bytes of struct as the credential, then
   * rejected any real passphrase over 24 characters because the header ate
   * the rest of the length budget.
   */

    {
      FAR struct iw_encode_ext *ext = (FAR struct iw_encode_ext *)
                                      data->pointer;

      if (ext == NULL || data->length < sizeof(*ext) ||
          ext->key_len != data->length - sizeof(*ext) ||
          ext->key_len > sizeof(priv->passwd) - 1)
        {
          return -EINVAL;
        }

      memcpy(priv->passwd, ext->key, ext->key_len);
      priv->passwd[ext->key_len] = '\0';
    }

  return OK;
}

/****************************************************************************
 * Name: bk7258_wifi_mode / bk7258_wifi_auth
 ****************************************************************************/

static int bk7258_wifi_mode(FAR struct netdev_lowerhalf_s *dev,
                            FAR struct iwreq *iwr, bool set)
{
  FAR struct bk7258_wifi_dev_s *priv = (FAR struct bk7258_wifi_dev_s *)dev;

  if (set)
    {
      if (iwr->u.mode != IW_MODE_INFRA && iwr->u.mode != IW_MODE_MASTER)
        {
          return -ENOSYS;
        }

      priv->mode = iwr->u.mode;
    }
  else
    {
      iwr->u.mode = priv->mode;
    }

  return OK;
}

static int bk7258_wifi_auth(FAR struct netdev_lowerhalf_s *dev,
                            FAR struct iwreq *iwr, bool set)
{
  FAR struct bk7258_wifi_dev_s *priv = (FAR struct bk7258_wifi_dev_s *)dev;

  /* One SIOCSIWAUTH carries many different parameters, disambiguated by
   * the index in flags -- wapi psk sends IW_AUTH_WPA_VERSION and
   * IW_AUTH_CIPHER_PAIRWISE back to back.  The first version ignored the
   * index and let the second call overwrite the first's value.
   */

  if (set)
    {
      if ((iwr->u.param.flags & IW_AUTH_INDEX) == IW_AUTH_WPA_VERSION)
        {
          priv->auth = iwr->u.param.value;
        }
    }
  else
    {
      iwr->u.param.value = priv->auth;
    }

  return OK;
}

/****************************************************************************
 * Name: the radio parameters
 *
 * Description:
 *   Frequency, bit rate, transmit power, country and sensitivity all read
 *   or write MAC/PHY state that only the vendor stack can reach.
 *
 ****************************************************************************/

static int bk7258_wifi_freq(FAR struct netdev_lowerhalf_s *dev,
                            FAR struct iwreq *iwr, bool set)
{
  UNUSED(dev);
  UNUSED(iwr);
  UNUSED(set);
  return -ENOSYS;
}

static int bk7258_wifi_bitrate(FAR struct netdev_lowerhalf_s *dev,
                               FAR struct iwreq *iwr, bool set)
{
  UNUSED(dev);
  UNUSED(iwr);
  UNUSED(set);
  return -ENOSYS;
}

static int bk7258_wifi_txpower(FAR struct netdev_lowerhalf_s *dev,
                               FAR struct iwreq *iwr, bool set)
{
  UNUSED(dev);
  UNUSED(iwr);
  UNUSED(set);
  return -ENOSYS;
}

static int bk7258_wifi_country(FAR struct netdev_lowerhalf_s *dev,
                               FAR struct iwreq *iwr, bool set)
{
  UNUSED(dev);
  UNUSED(iwr);
  UNUSED(set);
  return -ENOSYS;
}

static int bk7258_wifi_sensitivity(FAR struct netdev_lowerhalf_s *dev,
                                   FAR struct iwreq *iwr, bool set)
{
  UNUSED(dev);
  UNUSED(iwr);
  UNUSED(set);
  return -ENOSYS;
}

/****************************************************************************
 * Name: bk7258_wifi_scan
 *
 * Description:
 *   SIOCSIWSCAN (set == true) starts a scan; SIOCGIWSCAN (set == false)
 *   returns what the last one found, as the stream of variable-length
 *   struct iw_event records the wireless extensions define.  One AP is four
 *   records: BSSID, ESSID, frequency (as a channel), and quality.
 *
 *   -EAGAIN is the documented answer to a GET issued before the results are
 *   in; wapi and iwlist both retry on it.  There is no completion callback
 *   to hang that on -- the vendor's is a wpa_ctrl_event() this port stubs
 *   out -- so "in" means the MAC has reported at least one AP, and the
 *   caller decides how long to keep asking.
 *
 ****************************************************************************/

static int bk7258_wifi_scan(FAR struct netdev_lowerhalf_s *dev,
                            FAR struct iwreq *iwr, bool set)
{
  FAR struct iw_event *iwe;
  struct bk7258_scan_ap_s ap;
  FAR char *buf;
  size_t used = 0;
  size_t len;
  int count;
  int i;

  UNUSED(dev);

  if (set)
    {
      int ret = bk7258_wifi_scan_start();

      syslog(LOG_INFO, "wifi: scan start -> %d\n", ret);
      return ret == 0 ? OK : -EIO;
    }

  if (iwr == NULL || iwr->u.data.pointer == NULL)
    {
      return -EINVAL;
    }

  count = bk7258_wifi_scan_count();

  /* Report the interrupt counters here, not at scan start.  At start they
   * are trivially zero; what matters is whether the MAC took a single
   * RX-trigger interrupt while the radio was walking the channels.  wapi
   * polls this path, so once is enough -- rate-limit it.
   */

    {
      extern void bk7258_wifi_irq_report(void);
      static int reported = 0;

      if (reported++ == 0)
        {
          bk7258_wifi_irq_report();
        }
    }

  syslog(LOG_INFO, "wifi: scan results -> %d\n", count);
  if (count <= 0)
    {
      return -EAGAIN;
    }

  /* One reference for the whole enumeration.  Taking and dropping it per
   * item frees the vendor's result set on the first drop -- see
   * bk7258_wifi_scan_acquire() in bk7258_wifi_glue.c.
   */

  count = bk7258_wifi_scan_acquire();
  if (count <= 0)
    {
      bk7258_wifi_scan_release();
      return -EAGAIN;
    }

  /* Size the answer first.  wapi asks twice: wapi_scan_stat() probes with a
   * one-byte buffer purely to learn whether results exist, and treats -E2BIG
   * as "ready" and -EAGAIN as "not yet"; wapi_scan_coll() then doubles its
   * buffer and retries for as long as it keeps getting -E2BIG
   * (apps/wireless/wapi/src/wireless.c:1289 and :1371).  Returning OK with a
   * truncated stream instead, as this used to, tells the probe that a
   * one-byte buffer was enough and loses every result.
   */

  len = 0;
  for (i = 0; i < count; i++)
    {
      if (bk7258_wifi_scan_get(i, &ap) != 0)
        {
          continue;
        }

      len += IW_EV_LEN(ap_addr) + IW_EV_LEN(freq) + IW_EV_LEN(qual) +
             IW_EV_LEN(data) +
             IW_EV_LEN(essid) + ((strnlen(ap.ssid, 32) + 3) & ~3);
    }

  if (iwr->u.data.length < len)
    {
      iwr->u.data.length = len;
      bk7258_wifi_scan_release();
      return -E2BIG;
    }

  buf = (FAR char *)iwr->u.data.pointer;

  for (i = 0; i < count; i++)
    {
      if (bk7258_wifi_scan_get(i, &ap) != 0)
        {
          continue;
        }

      iwe = (FAR struct iw_event *)&buf[used];
      iwe->len = IW_EV_LEN(ap_addr);
      iwe->cmd = SIOCGIWAP;
      iwe->u.ap_addr.sa_family = ARPHRD_ETHER;
      memcpy(iwe->u.ap_addr.sa_data, ap.bssid, IFHWADDRLEN);
      used += iwe->len;

      /* The SSID travels inline, immediately after the iw_point, and
       * u.essid.pointer carries the offset to it rather than an address:
       * wapi_event_stream_extract() computes the real pointer as
       * "current + offsetof(struct iw_event, u) + (unsigned long)pointer"
       * (wireless.c:296).  This used to store &ap.ssid, the address of a
       * stack local that is reused every iteration and gone by the time the
       * caller looks -- so wapi added a stack address to its own buffer base
       * and read from somewhere arbitrary.  Same encoding as the in-tree
       * bcm43xxx driver (bcmf_driver.c:1065).
       */

      iwe = (FAR struct iw_event *)&buf[used];
      iwe->cmd = SIOCGIWESSID;
      iwe->u.essid.flags   = 1;
      iwe->u.essid.length  = strnlen(ap.ssid, 32);
      iwe->u.essid.pointer = (FAR void *)sizeof(iwe->u.essid);
      memcpy(&iwe->u.essid + 1, ap.ssid, iwe->u.essid.length);
      iwe->len = IW_EV_LEN(essid) + ((iwe->u.essid.length + 3) & ~3);
      used += iwe->len;

      iwe = (FAR struct iw_event *)&buf[used];
      iwe->len = IW_EV_LEN(freq);
      iwe->cmd = SIOCGIWFREQ;
      iwe->u.freq.m = ap.channel;
      iwe->u.freq.e = 0;
      iwe->u.freq.i = 0;
      used += iwe->len;

      iwe = (FAR struct iw_event *)&buf[used];
      iwe->len = IW_EV_LEN(qual);
      iwe->cmd = IWEVQUAL;
      iwe->u.qual.qual    = 0;
      iwe->u.qual.level   = ap.rssi;
      iwe->u.qual.noise   = 0;
      iwe->u.qual.updated = IW_QUAL_DBM;
      used += iwe->len;

      /* Encryption.  wapi only reads u.data.flags here (wireless.c:470), and
       * prints it as the "encode" column -- 0xffff until this event existed,
       * because that is what it initialises the field to.  There is no key
       * to report, so length is zero and the payload offset is unused.
       */

      iwe = (FAR struct iw_event *)&buf[used];
      iwe->len = IW_EV_LEN(data);
      iwe->cmd = SIOCGIWENCODE;
      iwe->u.data.length  = 0;
      iwe->u.data.pointer = NULL;
      iwe->u.data.flags   = ap.security == 0 ? IW_ENCODE_DISABLED
                                             : IW_ENCODE_ENABLED;
      used += iwe->len;
    }

  bk7258_wifi_scan_release();

  iwr->u.data.length = used;
  return OK;
}

static int bk7258_wifi_range(FAR struct netdev_lowerhalf_s *dev,
                             FAR struct iwreq *iwr)
{
  UNUSED(dev);
  UNUSED(iwr);
  return -ENOSYS;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_wifi_initialize
 *
 * Description:
 *   See bk7258_wifi.h.
 *
 ****************************************************************************/

int bk7258_wifi_initialize(void)
{
  FAR struct bk7258_wifi_dev_s *priv = &g_bk7258_wifi;
  int ret;

  memset(priv, 0, sizeof(*priv));

  priv->dev.ops    = &g_bk7258_wifi_ops;
  priv->dev.iw_ops = &g_bk7258_wifi_iw_ops;
  priv->mode       = IW_MODE_INFRA;

  /* The MAC address stays zero until the vendor stack can read it out of the
   * chip.  Leaving it zero is deliberate: an invented address would be a
   * plausible-looking lie, and this interface cannot pass traffic yet
   * anyway.
   */

  priv->dev.quota[NETPKT_TX] = BK7258_WIFI_TX_QUOTA;
  priv->dev.quota[NETPKT_RX] = BK7258_WIFI_RX_QUOTA;

  ret = netdev_lower_register(&priv->dev, NET_LL_IEEE80211);
  if (ret < 0)
    {
      nerr("ERROR: netdev_lower_register: %d\n", ret);
      return ret;
    }

  ninfo("wlan registered; MAC not built, see PORTING_NOTES ch.21\n");
  return OK;
}
