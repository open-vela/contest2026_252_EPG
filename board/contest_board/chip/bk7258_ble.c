/****************************************************************************
 * boards/arm/bk7258/contest_board/chip/bk7258_ble.c
 *
 * BLE bring-up staging.  Present stage: a link probe that references the
 * closed-library entry points so the linker extracts the full dependency
 * tree and enumerates every symbol the NuttX side still owes -- the
 * concrete work inventory for the OSI/PHY/PM adaptation.
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>
#include <stdio.h>

/* Closed-library entries (bk_idk prebuilt archives, ABI-matched:
 * armv8-m.main / fpv5-sp-d16 / hard float).
 */

extern int  bt_os_adapter_init(void *funcs);
extern int  bluetooth_controller_init(void);
extern int  bk_ble_reg_hci_recv_callback(int (*evt_cb)(uint8_t *, uint16_t),
                                         int (*acl_cb)(uint8_t *, uint16_t));
extern int  bk_ble_hci_cmd_to_controller(uint8_t *buf, uint16_t len);
extern int  bt_feature_adapter_init(void *arg);
extern int  phy_adapter_init(void *funcs, void *vars);
extern void rf_adapter_init(const void *funcs, const void *vars);
extern void calibration_init(void);
extern void rf_module_vote_ctrl(uint32_t cmd, uint32_t bit);

/* The OSI table (bk7258_bt_osi.c).  Its address is taken, never called:
 * without a reference the whole object -- and every closed PHY symbol its
 * table entries point at -- is dropped by --gc-sections, and a build that
 * discards the code proves nothing about whether it links.
 */

extern int bk7258_bt_osi_init(void);

/* The PHY/RF adapter tables (bk7258_phy_osi.c), for the same reason:
 * unreferenced, --gc-sections drops the object along with every closed
 * PHY symbol its tables relocate against.
 */

extern int bk7258_phy_adapter_init(void);
extern int bk7258_rf_adapter_init(void);

/* Feature flags the controller reads once at init.  Layout must match
 * bt_feature_config.h byte for byte; all zero is the stock BLE profile,
 * and _support_lpo_rosc stays clear to agree with the OSI table, which
 * reports the divided-26M sleep clock rather than the ROSC.
 */

struct bt_feature_s
{
  uint8_t is_gatt_discovery_auto;
  uint8_t ignore_smp_key_distr_all_zero;
  uint8_t strict_smp_key_distr_check_except_all_zero;
  uint8_t ignore_smp_already_pair;
  uint8_t send_peripheral_feature_req_auto;
  uint8_t stop_smp_when_pair_err;
  uint8_t enable_smp_sec_req_evt;
  uint8_t support_lpo_rosc;
};

static struct bt_feature_s g_bt_feature;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_bt_feature_init
 *
 * Description:
 *   Hand the controller its feature-flag block.  Stock BLE profile.
 *
 ****************************************************************************/

int bk7258_bt_feature_init(void)
{
  memset(&g_bt_feature, 0, sizeof(g_bt_feature));
  return bt_feature_adapter_init(&g_bt_feature);
}

/****************************************************************************
 * Name: bk7258_bt_controller_init
 *
 * Description:
 *   Start the closed link-layer controller.  It powers the BTSP domain,
 *   gates the BTDM and XVR clocks, hooks its interrupts and votes for
 *   the radio -- all through the OSI table -- so everything it needs
 *   from us is already in place, except that the RF vote lands in the
 *   closed PHY archive, whose own adapter table is not filled yet.
 *
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_bt_cal_init
 *
 * Description:
 *   The radio calibration step, which this port had been skipping.
 *   The vendor runs it between the adapter tables and the controller
 *   (bk_init.c: vnd_cal_overlay then bk_cal_if_init on the BT-only
 *   path), and the controller's own log said so all along --
 *   cali_ready_status 0x0.  An untrimmed analogue front end explains a
 *   link layer that schedules events perfectly and neither transmits
 *   nor hears anything.
 *
 *   No external PA on this board: EPA_ENABLE_FLAG is undefined in the
 *   vendor tree, so the overlay passes 0 and the two GPIO numbers go
 *   unused.
 *
 *   Two separate things had to be fixed before this could run, and only
 *   one of them was the ADC.
 *
 *   The SARADC callbacks calibration needs -- TSSI power on channel 8,
 *   die temperature on 7, supply on 0 -- are now real, driven by
 *   bk7258_saradc.c, so the trim is solved against measurements rather
 *   than against stubs that reported failure.  Necessary, but it was
 *   never the cause of the UsageFault: nothing in this tree sets
 *   CCR.DIV_0_TRP, so a divide by zero here returns zero quietly rather
 *   than faulting.  CFSR read 0x00020000 -- UFSR.INVSTATE, a branch to an
 *   address with the Thumb bit clear -- with R3 zero.
 *
 *   That was nv_init, three statements into calibration_main, tail-calling
 *   a null _nv_phy_reg_set_hook; see the entry of that name in
 *   bk7258_phy_osi.c for why the table had a hole exactly there.  It is
 *   filled in now, and it was the only reachable hole: with --gc-sections
 *   applied, that entry was the one null slot any surviving code could
 *   still call.
 *
 *   Still unproven on hardware, so this stays off the default path.  If it
 *   faults again, read CFSR rather than assuming: calibration writes the
 *   TRX block at 0x4980c000 directly, which needs the RF domain and modem
 *   clock this function's first two table votes are supposed to raise, and
 *   that would show as a bus fault, not INVSTATE.
 *
 ****************************************************************************/

struct auto_pwr_cali_s
{
  uint32_t cali_mode;
  int32_t  gtx_tssi_thred_chan1_b;
  int32_t  gtx_tssi_thred_chan7_b;
  int32_t  gtx_tssi_thred_chan13_b;
  int32_t  gtx_tssi_thred_chan1_g;
  int32_t  gtx_tssi_thred_chan7_g;
  int32_t  gtx_tssi_thred_chan13_g;
};

extern void vnd_cal_set_auto_pwr_thred(struct auto_pwr_cali_s ctx);
extern void vnd_cal_set_epa_config(uint8_t epa_flag, uint16_t rx_gpio,
                                   uint16_t tx_gpio, uint32_t gainbase_b,
                                   uint32_t gainbase_g);
extern void vnd_cal_set_cca_level(uint8_t offset);
extern int  bk_cal_if_init(void);

extern const uint32_t pwr_gain_base_gain_b;
extern const uint32_t pwr_gain_base_gain_g;

/****************************************************************************
 * Name: bk7258_ble_use_bt_pll
 *
 * Description:
 *   Point the transceiver at bluetooth's own synthesiser instead of the
 *   Wi-Fi one.  rwnx_rfconfig comes up 0x101 here -- PLL and role both
 *   Wi-Fi -- because the archive this port links is the Wi-Fi PHY, and
 *   the controller library in this SDK has no polar mode to fall back
 *   on (ble_enter_polar_mode exists only in the newer AVDK tree).  So
 *   the transmitter reaches for a Wi-Fi PLL that a BLE-only build never
 *   starts, which is the shape of the symptom: the receiver hears the
 *   room, nothing hears the transmitter, and enabling advertising takes
 *   the receiver down with it.
 *
 *   The Wi-Fi stack hands the synthesiser over with this call during
 *   coexistence; with no Wi-Fi here we make the same request directly.
 *   Must run before the controller initialises the transceiver.
 *
 ****************************************************************************/

extern int rwnx_cal_set_rfconfig_BTPLL(void);
extern volatile uint16_t rwnx_rfconfig;

int bk7258_ble_use_bt_pll(void)
{
  int ret;

  syslog(LOG_INFO, "ble: rfconfig before %04x\n", rwnx_rfconfig);
  ret = rwnx_cal_set_rfconfig_BTPLL();
  syslog(LOG_INFO, "ble: set BTPLL -> %d, rfconfig now %04x\n",
         ret, rwnx_rfconfig);
  return ret;
}

int bk7258_bt_cal_init(void)
{
  static const struct auto_pwr_cali_s auto_pwr =
  {
    0x1,                    /* manual calibration mode */
    0x253, 0x253, 0x257,    /* TSSI thresholds, 802.11b channels */
    0x23f, 0x22b, 0x22b     /* 802.11g channels */
  };

  vnd_cal_set_auto_pwr_thred(auto_pwr);
  vnd_cal_set_epa_config(0, 28, 26, pwr_gain_base_gain_b,
                         pwr_gain_base_gain_g);
  vnd_cal_set_cca_level(0);

  return bk_cal_if_init();
}

extern void ble_ps_enable_clear(void);

/* Power save is switched off after controller init (see the call site).
 * This flag exists so the same image can be measured both ways.
 */

static bool g_bt_ps_disable = true;

void bk7258_bt_ps_keep(void)
{
  g_bt_ps_disable = false;
}

int bk7258_bt_controller_init(void)
{
  /* Calibration goes after the controller here, not before it as the
   * vendor sequences it, and the radio is measurably dead the other
   * way round: calibrate first and a scan hears nothing, calibrate
   * second and it hears the whole room.  The controller is what
   * actually powers the transceiver on this port -- BTSP domain, BTDM
   * and XVR clocks, all through the OSI table -- while calibration
   * raises the radio through two PHY-table slots whose implementations
   * here are not doing that job.  Run first, it trims a block that is
   * not switched on.  Fixing those two slots would restore the vendor
   * order; until then this order is the one that works.
   *
   * The -1 is expected and not fatal: no factory calibration record in
   * flash, so the closed library falls back to the board default power
   * tables and still trims the crystal.
   */

  int ret = bluetooth_controller_init();

  if (ret == 0)
    {
      int cal = bk7258_bt_cal_init();

      syslog(LOG_INFO, "ble: calibration -> %d%s\n", cal,
             cal == 0 ? "" : " (no factory record; defaults in use)");

      /* Leave controller power save off.
       *
       * The controller task runs "if (ble_ps_enabled()) rwip_sleep();"
       * before every rwip_schedule(), and ble_ps_enable_set() turns that
       * on at start of day -- it ignores its argument and stores 1.  In
       * the vendor's own arrangement the UART that carries HCI wakes the
       * part again; this port carries HCI over a function call, so
       * nothing does, and a sleeping controller services commands only
       * when a timer next wakes it.  Measured: with power save on,
       * HCI_Reset took 2.35 s and each following command exactly 10.000
       * s, so the Bluetooth service's adapter took 141 s to come up and
       * its advertising start timed out.  With it off, every one of
       * those commands completes in under 10 ms.
       *
       * The cost is idle current: the radio no longer sleeps between
       * events.  bk7258_bt_ps_keep() leaves it on for measurement.
       */

      if (g_bt_ps_disable)
        {
          ble_ps_enable_clear();
        }
    }

  return ret;
}

/****************************************************************************
 * Raw HCI advertising
 *
 * The controller exposes a standard HCI boundary, so a handful of Core
 * spec commands are enough to get on the air -- no host stack in the
 * picture at all, which makes this the shortest honest proof that the
 * radio works end to end.
 ****************************************************************************/

static volatile int g_hci_evt;
static uint8_t g_hci_status;

static volatile int g_adv_reports;

static int ble_hci_evt_cb(uint8_t *buf, uint16_t len)
{
  /* LE Advertising Report: proof that the receiver hears the world.
   * Meta event 0x3e, subevent 0x02, then one report per entry:
   * event type, address type, six address bytes, data length, data,
   * and RSSI as the last byte.
   */

  if (len >= 13 && buf[0] == 0x3e && buf[2] == 0x02)
    {
      /* Report layout after the meta header and report count: event
       * type, address type, six address bytes, data length, the data
       * itself, then RSSI.  An earlier version started the address one
       * byte early, which is why every RSSI it printed was nonsense.
       */

      uint8_t atype = buf[5];
      uint8_t *addr = buf + 6;
      uint8_t dlen = buf[12];
      int8_t rssi = (len > 13 + dlen) ? (int8_t)buf[13 + dlen] : 0;
      char nm[32];
      int nlen = 0;
      int i = 13;

      nm[0] = '\0';
      while (i + 1 < 13 + dlen)
        {
          uint8_t alen = buf[i];
          uint8_t atyp = buf[i + 1];

          if (alen == 0)
            {
              break;
            }

          if ((atyp == 0x09 || atyp == 0x08) && alen > 1)
            {
              nlen = alen - 1;
              if (nlen > 30)
                {
                  nlen = 30;
                }

              memcpy(nm, buf + i + 2, nlen);
              nm[nlen] = '\0';
              break;
            }

          i += alen + 1;
        }

      g_adv_reports++;
      syslog(LOG_INFO,
             "ble: %02x:%02x:%02x:%02x:%02x:%02x t%u %ddBm %s\n",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0],
             atype, rssi, nlen ? nm : "-");
      return 0;
    }

  /* Whether the transport hands up a bare event or keeps the H4 type
   * byte in front is not documented either way, so accept both and
   * show the raw bytes: if this never prints, the events are not
   * coming through the VHCI path at all, which is a different problem
   * from parsing them wrong.
   */

  uint8_t *e = buf;
  char hex[3 * 8 + 1];
  int n = len > 8 ? 8 : len;
  int i;

  for (i = 0; i < n; i++)
    {
      snprintf(hex + i * 3, 4, "%02x ", buf[i]);
    }

  hex[n * 3] = '\0';
  syslog(LOG_INFO, "hci: evt len %u [%s]\n", (unsigned)len, hex);

  if (len >= 1 && buf[0] == 0x04)
    {
      e = buf + 1;
      len--;
    }

  if (len >= 4 && (e[0] == 0x0e || e[0] == 0x0f))
    {
      g_hci_status = (e[0] == 0x0e) ? e[5] : e[2];
      g_hci_evt = 1;
    }

  return 0;
}

static int ble_hci_acl_cb(uint8_t *buf, uint16_t len)
{
  return 0;
}

static int hci_register_once(void)
{
  static int s_registered;

  if (s_registered)
    {
      return 0;
    }

  if (bk_ble_reg_hci_recv_callback(ble_hci_evt_cb, ble_hci_acl_cb) != 0)
    {
      return -1;
    }

  s_registered = 1;
  return 0;
}

static int hci_cmd(uint16_t opcode, const uint8_t *params, uint8_t plen)
{
  uint8_t buf[64];
  int waited;

  buf[0] = opcode & 0xff;
  buf[1] = opcode >> 8;
  buf[2] = plen;
  if (plen > 0)
    {
      memcpy(buf + 3, params, plen);
    }

  g_hci_evt = 0;
  g_hci_status = 0xff;

  if (bk_ble_hci_cmd_to_controller(buf, plen + 3) != 0)
    {
      syslog(LOG_INFO, "hci: cmd %04x rejected on submit\n", opcode);
      return -1;
    }

  syslog(LOG_INFO, "hci: cmd %04x sent (%u bytes)\n", opcode,
         (unsigned)(plen + 3));

  for (waited = 0; waited < 1500 && g_hci_evt == 0; waited++)
    {
      usleep(10 * 1000);
    }

  syslog(LOG_INFO, "hci: cmd %04x waited %d, evt %d, status %02x\n",
         opcode, waited, g_hci_evt, g_hci_status);

  if (g_hci_evt == 0)
    {
      return -2;
    }

  return g_hci_status;
}

/****************************************************************************
 * Name: bk7258_ble_txpwr
 *
 * Description:
 *   Report and optionally override the transmit power index.  The
 *   receiver demonstrably works while nothing hears the transmitter,
 *   and an index left at the bottom of the table is the cheapest
 *   explanation: calibration found no factory record, so whatever the
 *   fallback put here decides how far the advertisement carries.
 *
 ****************************************************************************/

extern uint8_t manual_cal_get_ble_pwr_idx(uint8_t channel);
extern void ble_cal_set_txpwr(uint8_t idx);

int bk7258_ble_txpwr(int idx)
{
  uint8_t ch;

  for (ch = 0; ch < 40; ch += 13)
    {
      syslog(LOG_INFO, "ble: pwr idx ch%u = %u\n", ch,
             manual_cal_get_ble_pwr_idx(ch));
    }

  if (idx >= 0)
    {
      ble_cal_set_txpwr((uint8_t)idx);
      syslog(LOG_INFO, "ble: forced pwr idx %d\n", idx);
    }

  return 0;
}

/****************************************************************************
 * Name: bk7258_ble_scan
 *
 * Description:
 *   Listen for other people's advertisements for a few seconds.  This
 *   is the one proof of a working radio that needs nothing but the
 *   board: any phone, headset or fitness band nearby is transmitting,
 *   and hearing them exercises the same transceiver, antenna and
 *   calibration the transmitter uses.
 *
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_ble_adv_stop
 *
 * Description:
 *   Turn the transmitter off.  Exists to settle one question the earlier
 *   "advertising kills the scan" measurement could not: a controller with
 *   a single activity slot would show the same thing while working
 *   perfectly.  If reception comes back after this, advertising was
 *   simply holding the radio; if it stays dead, the radio is wedged.
 *
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_ble_tx_test / bk7258_ble_hci_reset
 *
 * Description:
 *   Direct Test Mode transmit is the shortest path to keying the
 *   transmitter -- one command, no advertising state machine, no PDU
 *   construction -- so if it wedges the radio the same way advertising
 *   does, the fault is in the transmit chain itself rather than
 *   anywhere above it.  The reset is here to find out whether the wedge
 *   is soft state the controller can be talked out of.
 *
 ****************************************************************************/

int bk7258_ble_tx_test(int channel, int seconds)
{
  uint8_t p[3];
  int ret;

  p[0] = (uint8_t)channel;   /* 0..39, (F - 2402) / 2 */
  p[1] = 37;                 /* payload length */
  p[2] = 0;                  /* PRBS9 */

  ret = hci_cmd(0x201e, p, 3);
  syslog(LOG_INFO, "ble: tx_test start ch%d -> %d\n", channel, ret);
  if (ret != 0)
    {
      return ret;
    }

  sleep(seconds);

  ret = hci_cmd(0x201f, NULL, 0);
  syslog(LOG_INFO, "ble: tx_test stop -> %d\n", ret);
  return ret;
}

int bk7258_ble_hci_reset(void)
{
  return hci_cmd(0x0c03, NULL, 0);
}

int bk7258_ble_adv_stop(void)
{
  uint8_t off = 0;

  return hci_cmd(0x200a, &off, 1);
}

int bk7258_ble_scan(int seconds)
{
  static const uint8_t scan_params[7] =
  {
    0x00,                   /* passive: listen, never ask for more */
    0x10, 0x00,             /* interval 16 * 0.625 ms = 10 ms */
    0x10, 0x00,             /* window: listen the whole interval */
    0x00,                   /* own address: public */
    0x00                    /* accept every advertiser */
  };

  uint8_t enable[2] = { 0x01, 0x00 };
  int ret;
  int i;

  /* Register once per boot.  Handing the controller a fresh callback
   * pair on every scan left the second scan hearing nothing at all --
   * an artefact that masqueraded as the radio being wedged by whatever
   * ran in between.
   */

  ret = hci_register_once();
  if (ret != 0)
    {
      return -1;
    }

  g_adv_reports = 0;

  ret = hci_cmd(0x200b, scan_params, sizeof(scan_params));
  syslog(LOG_INFO, "ble: scan params -> %d\n", ret);
  if (ret != 0)
    {
      return ret;
    }

  ret = hci_cmd(0x200c, enable, 2);
  syslog(LOG_INFO, "ble: scan enable -> %d\n", ret);
  if (ret != 0)
    {
      return ret;
    }

  for (i = 0; i < seconds; i++)
    {
      sleep(1);
      syslog(LOG_INFO, "ble: %d s, %d reports\n", i + 1, g_adv_reports);
    }

  enable[0] = 0x00;
  hci_cmd(0x200c, enable, 2);
  return g_adv_reports;
}

/****************************************************************************
 * Name: bk7258_ble_adv_start
 *
 * Description:
 *   Reset, configure a 100 ms connectable advertisement carrying name,
 *   and switch the transmitter on.  Returns 0 when the controller
 *   accepted every step.
 *
 ****************************************************************************/

int bk7258_ble_adv_start(const char *name)
{
  static const uint8_t adv_params[15] =
  {
    0xa0, 0x00,             /* min interval, 160 * 0.625 ms = 100 ms */
    0xa0, 0x00,             /* max interval */
    0x00,                   /* ADV_IND, connectable undirected */
    0x01,                   /* own address: random -- see below */
    0x00,                   /* peer address type */
    0, 0, 0, 0, 0, 0,       /* peer address, unused for undirected */
    0x07,                   /* all three advertising channels */
    0x00                    /* no scan/connect filtering */
  };

  uint8_t adv_data[32];
  uint8_t enable = 0x01;
  size_t nlen = strlen(name);
  int ret;

  if (nlen > 26)
    {
      nlen = 26;
    }

  memset(adv_data, 0, sizeof(adv_data));
  adv_data[1] = 0x02;                 /* flags AD: length */
  adv_data[2] = 0x01;                 /* flags AD: type */
  adv_data[3] = 0x06;                 /* general discoverable, LE only */
  adv_data[4] = (uint8_t)(nlen + 1);  /* name AD: length */
  adv_data[5] = 0x09;                 /* name AD: complete local name */
  memcpy(adv_data + 6, name, nlen);
  adv_data[0] = (uint8_t)(5 + nlen);  /* significant part length */

  ret = hci_register_once();
  syslog(LOG_INFO, "hci: reg callback -> %d\n", ret);
  if (ret != 0)
    {
      return -1;
    }

  /* Probe first, and do not stop at the first silence: HCI_Reset is
   * answered but LE_Set_Advertising_Parameters was not, so the useful
   * question is which command groups this path answers at all.  A
   * vendor-info read, an LE buffer read and an LE feature read bracket
   * the three cases (base band, LE informational, LE control).
   */

    {
      extern void bk7258_bt_osi_diag(void);

      bk7258_bt_osi_diag();
      usleep(500 * 1000);
      bk7258_bt_osi_diag();
    }

  syslog(LOG_INFO, "hci: probe read_local_version -> %d\n",
         hci_cmd(0x1001, NULL, 0));
  syslog(LOG_INFO, "hci: probe le_read_buffer_size -> %d\n",
         hci_cmd(0x2002, NULL, 0));
  syslog(LOG_INFO, "hci: probe le_read_local_features -> %d\n",
         hci_cmd(0x2003, NULL, 0));

  /* Advertise from a static random address, which is what the product
   * firmware for this board does: its source sets own_addr_type to
   * random with the public option commented out beside it.  Nothing
   * here confirms the controller ever adopted a usable public address,
   * and enabling advertising with a declared address type that has no
   * address behind it is rejected outright -- error 0x12 -- so this
   * command has to come first.  The top two bits of the last byte mark
   * the address static random, as the vendor also does.
   */

    {
      uint8_t rnd[6] = { 0x26, 0x20, 0x25, 0x8c, 0x47, 0xc8 };

      rnd[5] |= 0xc0;
      syslog(LOG_INFO, "hci: set_random_addr -> %d\n",
             hci_cmd(0x2005, rnd, 6));
    }

  syslog(LOG_INFO, "hci: adv_params -> %d\n",
         hci_cmd(0x2006, adv_params, sizeof(adv_params)));
  syslog(LOG_INFO, "hci: adv_data -> %d\n",
         hci_cmd(0x2008, adv_data, 32));

  ret = hci_cmd(0x200a, &enable, 1);
  syslog(LOG_INFO, "hci: adv_enable -> %d\n", ret);

    {
      /* Sample the interrupt counter only now: before the transmitter
       * is enabled the link layer has nothing to schedule, so a zero
       * reading earlier said nothing.  A count that climbs here is the
       * radio actually running advertising events.
       */

      extern void bk7258_bt_osi_diag(void);

      bk7258_bt_osi_diag();
      sleep(2);
      bk7258_bt_osi_diag();
      sleep(2);
      bk7258_bt_osi_diag();
    }

  return ret;
}

uintptr_t bk7258_ble_link_probe(void)
{
  return (uintptr_t)bk7258_bt_osi_init +
         (uintptr_t)bk7258_phy_adapter_init +
         (uintptr_t)bk7258_rf_adapter_init +
         (uintptr_t)bk7258_bt_feature_init +
         (uintptr_t)bk7258_bt_controller_init +
         (uintptr_t)bk7258_ble_adv_start +
         (uintptr_t)bk7258_ble_scan +
         (uintptr_t)bk7258_ble_adv_stop +
         (uintptr_t)bk7258_ble_tx_test +
         (uintptr_t)bk7258_ble_hci_reset +
         (uintptr_t)bk7258_bt_cal_init +
         (uintptr_t)bk7258_ble_use_bt_pll +
         (uintptr_t)bk7258_ble_txpwr +
         (uintptr_t)bt_os_adapter_init +
         (uintptr_t)bluetooth_controller_init +
         (uintptr_t)bk_ble_reg_hci_recv_callback +
         (uintptr_t)bk_ble_hci_cmd_to_controller +
         (uintptr_t)phy_adapter_init +
         (uintptr_t)rf_adapter_init +
         (uintptr_t)calibration_init +
         (uintptr_t)rf_module_vote_ctrl;
}
