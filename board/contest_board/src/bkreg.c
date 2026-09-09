/****************************************************************************
 * board/contest_board/src/bkreg.c
 *
 * Read SoC registers from the console.
 *
 * The WiFi bring-up keeps arriving at questions of the form "is this bit
 * actually set on the board right now", and every previous answer has come
 * from reading vendor source and reasoning forward.  Three of those answers
 * turned out to be wrong.  This is the reading instead of the argument.
 *
 * Usage:
 *
 *   bkreg                    the WiFi preset, decoded (see below)
 *   bkreg rf [ms]            watch the radio controller while it is open
 *   bkreg <addr>             one 32-bit word
 *   bkreg <addr> <count>     count consecutive words
 *
 * The preset is the four registers that decide whether the MAC is alive:
 *
 *   0x44010030  cpu_device_clk_enable      bit 26 mac_cken, bit 27 phy_cken
 *   0x44010040  cpu_power_sleep_wakeup     bit 9 pwd_wifp_mac, bit 10 pwd_wifp_phy
 *                                          -- these are power-DOWN bits, 0 = on
 *   0x49000004  MAC core clock gate        bit 13, 1 = gated OFF
 *   0x49108050  MAC soft reset             bit 0, hardware clears it when the
 *                                          reset completes
 *
 * The first two are from the SDK, not from memory:
 *   bk_idk/middleware/soc/bk7258/soc/sys_reg.h:344,424,427  (clk enable)
 *   bk_idk/middleware/soc/bk7258/soc/sys_reg.h:673,702,705  (power sleep)
 *   bk_idk/include/soc/bk7258/reg_base.h:51                 (SOC_SYS_REG_BASE)
 *
 * The last two are not in the SDK at all -- the 0x49000000 block is defined
 * only inside the closed library -- so they come from the linked image:
 *   hal_machw_stop (0x0208ab30) writes 1 to 0x49108050 and then polls it
 *   through nxmac_soft_reset_getf, and hal_machw_enable_maccore_clk
 *   (0x0208b4a4) is exactly "ldr r3,[0x49000004]; bic r3,r3,#0x2000; str",
 *   which is what identifies bit 13 as the MAC core clock and 1 as off.
 *   rwnxl_sleep (0x020888e4) sets that bit; rwnxl_wakeup (0x02088a08)
 *   clears it.
 *
 * That last pair is the reason this command exists.  rwnx_intf_init calls
 * rwnxl_sleep at the tail of wifi_init(), so the MAC core clock is gated off
 * once bring-up finishes.  The core thread re-opens it for every message it
 * pops, but "rxsens" runs on the NSH task and reaches hal_machw_stop without
 * going through the core thread at all -- so the soft-reset poll may be
 * waiting on a MAC that has no clock.  Whether bit 13 is actually set at
 * that moment is a runtime question, which is what this reads.
 *
 * WARNING: reading a peripheral whose power domain is off hangs the bus --
 * this port has already done that once with the NMI watchdog block at
 * 0x44800000 (see PORTING_NOTES).  There is no way for this command to know
 * which domains are up, so it does not pretend to: an arbitrary address is
 * the caller's risk.  The preset only touches blocks that are always on
 * (the system controller) or that the caller has just been running (the
 * MAC).
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

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <inttypes.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SYS_DEV_CLK_EN      0x44010030ul
#define SYS_POWER_SLEEP     0x44010040ul
#define MAC_CORE_CLK        0x49000004ul
#define MAC_SOFT_RESET      0x49108050ul
#define RC_REG0             0x4980c000ul   /* SOC_RC_REG_BASE word 0 */

#define CLK_MAC_BIT         26
#define CLK_PHY_BIT         27
#define PWD_WIFP_MAC_BIT    9
#define PWD_WIFP_PHY_BIT    10
#define MAC_CORE_GATE_BIT   13

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t peek(uintptr_t addr)
{
  return *(volatile uint32_t *)addr;
}

/* The power bits read 1 when the domain is *down*, which is the opposite of
 * what everyone reading the output will expect.  Say "on"/"off", not the raw
 * bit, so the polarity cannot be misread a second time.
 */

static void wifi_preset(void)
{
  uint32_t clk = peek(SYS_DEV_CLK_EN);
  uint32_t pwr = peek(SYS_POWER_SLEEP);
  uint32_t core;
  uint32_t rst;

  printf("clk_enable   0x%08" PRIx32 "  mac_cken=%" PRIu32
         " phy_cken=%" PRIu32 "\n",
         clk,
         (clk >> CLK_MAC_BIT) & 1,
         (clk >> CLK_PHY_BIT) & 1);

  printf("power_sleep  0x%08" PRIx32 "  wifp_mac=%s wifp_phy=%s\n",
         pwr,
         ((pwr >> PWD_WIFP_MAC_BIT) & 1) ? "OFF" : "on",
         ((pwr >> PWD_WIFP_PHY_BIT) & 1) ? "OFF" : "on");

  /* Reading the MAC block with its clock gated or its domain powered down is
   * the bus hang this file's header warns about.  Check first.
   */

  if (((clk >> CLK_MAC_BIT) & 1) == 0 ||
      ((pwr >> PWD_WIFP_MAC_BIT) & 1) != 0)
    {
      printf("mac_core     -- not read: MAC clock gate or power domain is off\n");
      printf("mac_reset    -- not read: MAC clock gate or power domain is off\n");
      return;
    }

  core = peek(MAC_CORE_CLK);
  printf("mac_core     0x%08" PRIx32 "  core_clk=%s\n",
         core,
         ((core >> MAC_CORE_GATE_BIT) & 1) ? "GATED (rwnxl_sleep)" : "running");

  rst = peek(MAC_SOFT_RESET);
  printf("mac_reset    0x%08" PRIx32 "  soft_reset=%" PRIu32 "%s\n",
         rst, rst & 1,
         (rst & 1) ? "  <-- stuck, hardware has not cleared it" : "");
}

/****************************************************************************
 * Name: rf_watch
 *
 * Description:
 *   Sample the radio controller's word 0 for as long as the radio is open.
 *
 *   Reading 0x4980c000 is only safe while the Wi-Fi PHY domain is powered
 *   and clocked, and that window is about 1.7 s wide -- it opens when
 *   rwnxl_wakeup re-votes the radio and closes again after scanu_confirm.
 *   Racing it from the console does not work and gets the bus hang wrong
 *   half the time, so this polls the gate instead and only touches the
 *   block on the samples where it is genuinely up.  Start it first, then
 *   run the scan.
 *
 *   What matters is bit 0 (rf_en, the only bit rc_drv_set_rf_en writes) and
 *   bits 24..27, which nothing in the image writes and which
 *   rwnx_cal_set_rfconfig reports as "rf on"/"rf off".  Distinct values are
 *   accumulated rather than streamed: the interesting question is whether
 *   that nibble is ever anything but zero.
 *
 ****************************************************************************/

#define RF_MAX_SEEN 8

static void rf_watch(unsigned long ms)
{
  struct timespec start;
  struct timespec now;
  uint32_t seen[RF_MAX_SEEN];
  unsigned long count[RF_MAX_SEEN];
  int nseen = 0;
  unsigned long samples = 0;
  unsigned long up = 0;
  int i;

  clock_gettime(CLOCK_MONOTONIC, &start);

  for (; ; )
    {
      uint32_t clk;
      uint32_t pwr;

      clock_gettime(CLOCK_MONOTONIC, &now);
      if ((now.tv_sec - start.tv_sec) * 1000 +
          (now.tv_nsec - start.tv_nsec) / 1000000 >= (long)ms)
        {
          break;
        }

      samples++;

      clk = peek(SYS_DEV_CLK_EN);
      pwr = peek(SYS_POWER_SLEEP);

      if (((clk >> CLK_PHY_BIT) & 1) != 0 &&
          ((pwr >> PWD_WIFP_PHY_BIT) & 1) == 0)
        {
          uint32_t v = peek(RC_REG0);

          up++;

          for (i = 0; i < nseen; i++)
            {
              if (seen[i] == v)
                {
                  count[i]++;
                  break;
                }
            }

          if (i == nseen && nseen < RF_MAX_SEEN)
            {
              seen[nseen] = v;
              count[nseen] = 1;
              nseen++;
            }
        }

      usleep(1000);
    }

  printf("samples=%lu  phy-up=%lu  distinct=%d\n", samples, up, nseen);

  for (i = 0; i < nseen; i++)
    {
      printf("  rc0=0x%08" PRIx32 "  rf_en=%" PRIu32 "  nibble[27:24]=0x%" PRIx32
             "  (%lu samples)\n",
             seen[i], seen[i] & 1, (seen[i] >> 24) & 0xf, count[i]);
    }

  if (up == 0)
    {
      printf("  radio never opened during the window -- run the scan\n");
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  uintptr_t addr;
  unsigned long count = 1;
  unsigned long i;

  if (argc < 2)
    {
      wifi_preset();
      return 0;
    }

  if (strcmp(argv[1], "rf") == 0)
    {
      rf_watch(argc > 2 ? strtoul(argv[2], NULL, 0) : 5000);
      return 0;
    }

  addr = (uintptr_t)strtoul(argv[1], NULL, 0);

  if ((addr & 3) != 0)
    {
      /* An unaligned access to Device memory faults regardless of
       * CCR.UNALIGN_TRP, so this would be a hard fault rather than a bad
       * reading.
       */

      printf("bkreg: address must be 4-byte aligned\n");
      return 1;
    }

  if (argc > 2)
    {
      count = strtoul(argv[2], NULL, 0);
      if (count == 0)
        {
          count = 1;
        }
    }

  for (i = 0; i < count; i++)
    {
      uintptr_t a = addr + i * 4;
      printf("0x%08" PRIxPTR ": 0x%08" PRIx32 "\n", a, peek(a));
    }

  return 0;
}
