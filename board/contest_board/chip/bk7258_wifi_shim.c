/****************************************************************************
 * board/contest_board/chip/bk7258_wifi_shim.c
 *
 * The platform services Beken's WiFi stack expects from its own SDK,
 * implemented on NuttX.  bk7258_wifi_osi.c covers the RTOS primitives;
 * this file covers everything else the link asked for.
 *
 * The functions here fall into three groups, and the difference between
 * them matters when something misbehaves on hardware:
 *
 *   REAL      - does the same thing the vendor function does, verified
 *               against the vendor source and register headers.  The
 *               interrupt routing and the device clock gate are both in
 *               this group, and both had to be: they touch hardware that
 *               nothing else in this port turns on.
 *
 *   ADEQUATE  - honest implementation of a service NuttX provides
 *               differently (logging, reset, ticks, memcpy-by-DMA).
 *
 *   PENDING   - deliberately inert, because the subsystem behind it is not
 *               ported yet.  Every one of these logs once, the first time
 *               it is called, so a boot on real hardware says which of them
 *               the stack actually reaches instead of leaving us to guess.
 *               That is the whole point: a silent stub is indistinguishable
 *               from a working one right up until it isn't.
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

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#include <nuttx/arch.h>
#include <nuttx/clock.h>
#include <nuttx/irq.h>
#include <nuttx/mm/mm.h>
#include <nuttx/kmalloc.h>

#include <arch/board/board.h>

#include "arm_internal.h"
#include "bk7258_memorymap.h"
#include "chip.h"

/* Cross-domain contract with bk7258_wifi_glue.c / bk7258_wifi.c. */

#include "bk7258_wifi_scan.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Device clock gate.  SOC_SYS_REG_BASE + (0xc << 2) in Beken's sys_ll.h,
 * which is 0x44010030 here.  One bit per peripheral, and the bit number is
 * the CLK_PWR_ID_* enumerator itself -- MAC is 26, PHY is 27.
 *
 * This is the same register bk7258_wdt.c already pokes at bit 31 to start
 * the NMI watchdog clock (CLK_PWR_ID_WDG_CPU is 31), which is a useful
 * cross-check that the enumerator-equals-bit rule is right rather than a
 * coincidence of two adjacent numbers.
 */

#define BK7258_SYS_DEV_CLK_EN     (BK7258_SYS_BASE + (0xc << 2))

/* Anything at or above CLK_PWR_ID_H264 (32) lives in a second register.
 * WiFi never asks for one, so the split is recorded and refused rather
 * than half-implemented.
 */

#define BK7258_CLK_PWR_ID_H264    32

/* PM_CLK_CTRL_PWR_DOWN is 0 and PM_CLK_CTRL_PWR_UP is 1 -- from
 * modules/pm.h.  Worth naming rather than inlining: getting this polarity
 * backwards would leave the MAC and PHY clocks off, and the stack would
 * come up looking alive while every register read returned zero.
 */

#define BK7258_PM_CLK_PWR_UP      1
#define BK7258_PM_CLK_ID_PHY      27

/* Power islands.  cpu_power_sleep_wakeup, SOC_SYS_REG_BASE + (0x10 << 2).
 * One bit per domain, and the bit means "powered DOWN" -- see
 * bk_pm_module_vote_power_ctrl() for why that matters.
 *
 * Domain numbers are power_module_name_t: MEM1 is 0 and they run up to
 * ROM_PGEN at 15, which is the range sys_hal_module_power_ctrl() treats as
 * plain bits.  WIFIP_MAC is 9 and WIFI_PHY is 10.
 *
 * Sub-modules are not in that range and are not bits.  Beken derives them
 * arithmetically -- POWER_SUB_MODULE_NAME_PHY_BT is
 * POWER_MODULE_NAME_WIFI_PHY * PM_MODULE_SUB_POWER_DOMAIN_MAX, with the
 * siblings following on -- so a sub-module id divided by that multiplier
 * gives back its parent domain.  PHY_WIFI is 10 * 20 + 1 = 201, which is
 * the number the board printed when this shim first refused it.
 *
 * Deriving it beats a table of ranges: the table would have to be
 * rewritten every time Beken inserts a sub-module, and it silently would
 * not be.
 */

#define BK7258_SYS_POWER_SLEEP    (BK7258_SYS_BASE + (0x10 << 2))

#define BK7258_PM_POWER_ON        0
#define BK7258_PM_SUB_DOMAIN_MAX  20    /* PM_MODULE_SUB_POWER_DOMAIN_MAX */
#define BK7258_PM_DOMAIN_LAST     15    /* POWER_MODULE_NAME_ROM_PGEN     */

/* mac_type_t, from components/system.h */

#define BK7258_MAC_TYPE_BASE      0
#define BK7258_MAC_TYPE_STA       1
#define BK7258_MAC_TYPE_AP        2
#define BK7258_MAC_TYPE_BLUETOOTH 3

/* Log a PENDING call exactly once.  Repeating it would drown the console
 * if the stack polls, and the first call is the only one that carries
 * information anyway.
 */

#define BK7258_WIFI_PENDING(name)                                  \
  do                                                               \
    {                                                              \
      static bool once = false;                                    \
      if (!once)                                                   \
        {                                                          \
          once = true;                                             \
          syslog(LOG_WARNING, "wifi: %s not ported\n", (name));     \
        }                                                          \
    }                                                              \
  while (0)

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Locally-administered MAC, standing in until the factory address can be
 * read out of eFuse.  Bit 1 of the first octet marks it locally
 * administered and bit 0 is clear (unicast), so it is a valid address that
 * cannot collide with a real Beken-assigned one.
 */

static uint8_t g_bk7258_base_mac[6] =
{
  0x02, 0x0b, 0x77, 0x00, 0x00, 0x01
};

/****************************************************************************
 * Fault probe
 *
 * NuttX leaves BusFault and UsageFault disabled, so they escalate to Hard
 * Fault and the interrupted context is lost by the time _assert() prints --
 * every dump so far has shown _assert's own PC.  Enabling them and
 * attaching here keeps the exception unescalated, and the handler's
 * "context" argument IS the saved register frame, so REG_PC is the
 * instruction that actually faulted.
 *
 * Two details cost a flash cycle each and are worth keeping.  The enables
 * in SHCSR are not enough on their own: the exception also has to outrank
 * the BASEPRI the faulting code runs under, or it is masked and escalates
 * anyway -- the first attempt had both handlers attached and still printed
 * "Hard Fault escalation" with BASEPRI 0x80.  Hence SHPR1 = 0.  And the
 * probe is installed from WiFi bring-up only because that is where it was
 * needed; nothing about it is WiFi-specific.
 ****************************************************************************/

static int bk7258_fault_probe(int irq, FAR void *context, FAR void *arg)
{
  FAR uint32_t *regs = (FAR uint32_t *)context;

  UNUSED(arg);

  syslog(LOG_ERR, "FAULT irq=%d PC=%08" PRIx32 " LR=%08" PRIx32
                  " CFSR=%08" PRIx32 "\n",
         irq, regs[REG_PC], regs[REG_LR], getreg32(0xe000ed28));
  syslog(LOG_ERR, "  MMFAR=%08" PRIx32 " BFAR=%08" PRIx32 "\n",
         getreg32(0xe000ed34), getreg32(0xe000ed38));
  syslog(LOG_ERR, "  R0=%08" PRIx32 " R1=%08" PRIx32 " R2=%08" PRIx32
                  " R3=%08" PRIx32 "\n",
         regs[REG_R0], regs[REG_R1], regs[REG_R2], regs[REG_R3]);
  syslog(LOG_ERR, "  R4=%08" PRIx32 " R5=%08" PRIx32 " R6=%08" PRIx32
                  " R7=%08" PRIx32 "\n",
         regs[REG_R4], regs[REG_R5], regs[REG_R6], regs[REG_R7]);

  PANIC();
  return OK;
}

void bk7258_wifi_fault_probe_install(void)
{
  int r4 = irq_attach(4, bk7258_fault_probe, NULL);   /* MemManage  */
  int r5 = irq_attach(5, bk7258_fault_probe, NULL);   /* BusFault   */
  int r6 = irq_attach(6, bk7258_fault_probe, NULL);   /* UsageFault */

  modifyreg32(0xe000ed24, 0, (1u << 16) | (1u << 17) | (1u << 18));

  /* SHPR1: MemManage, BusFault and UsageFault priorities, one byte each.
   * They have to outrank whatever BASEPRI the faulting code is running
   * under or the exception is masked and escalates to Hard Fault anyway --
   * which is exactly what happened on the first attempt: the enables were
   * set, both handlers were attached, and the dump still said "Hard Fault
   * escalation" with BASEPRI 0x80.  Priority 0 cannot be masked by BASEPRI.
   */

  putreg32(0, 0xe000ed18);

  syslog(LOG_INFO, "probe: attach %d/%d/%d SHCSR=%08" PRIx32 " SHPR1=%08"
         PRIx32 "\n", r4, r5, r6,
         getreg32(0xe000ed24), getreg32(0xe000ed18));
}

/****************************************************************************
 * Zero-filled allocation for the vendor MAC
 *
 * me_strategy_mem_init() allocates the station table with plain malloc()
 * and never clears it.  sta_mgmt_init() then hands each entry to
 * sta_mgmt_entry_init(), which drains the entry's TX list *before* the
 * memset that would have initialised it -- correct when re-initialising a
 * live station, fatal on a fresh allocation, because the list head is
 * whatever was in that memory before.
 *
 * Beken get away with it because their heap is a static array in .bss and
 * their WiFi init runs early in boot, so malloc() there returns memory the
 * C startup already zeroed.  Here the stack comes up from ifup(), long
 * after tasks have been created and destroyed, and the block handed back
 * had been a task stack: NuttX colours those with 0xdeadbeef, so the list
 * head read 0xdeadbeef, and the load faulted -- an unaligned access to
 * 0xC0000000-0xDFFFFFFF, which is Device memory and faults regardless of
 * CCR.UNALIGN_TRP.  That is why the fault was UNALIGNED rather than a
 * plain bad-pointer BusFault, and why zeroing the heap once at
 * up_allocate_heap() did nothing: by then the memory had been recycled.
 *
 * So malloc() is interposed here.  Two things keep that honest:
 *
 *   - It only exists when this file does, which is CONFIG_BK7258_WIFI.
 *     An image without the WiFi driver links libc's malloc as usual.
 *   - It only zeroes once the vendor stack has been brought up, so an
 *     image that never runs ifup on wlan0 gets the ordinary allocator and
 *     pays nothing.
 *
 * The window was originally just the init call.  That was too narrow: the
 * board reached RF calibration and then faulted on a semaphore handle
 * holding garbage instead of NULL, from an allocation made on the work
 * queue after the window closed.  The assumption belongs to the vendor
 * stack as a whole, so the flag stays set once it is up.
 ****************************************************************************/

static bool g_bk7258_wifi_zeroing;

void bk7258_wifi_zeroing(bool on)
{
  g_bk7258_wifi_zeroing = on;
}

FAR void *malloc(size_t size)
{
  FAR void *ret = mm_malloc(USR_HEAP, size);

  if (ret == NULL)
    {
      set_errno(ENOMEM);
    }
  else if (g_bk7258_wifi_zeroing)
    {
      memset(ret, 0, size);
    }

  return ret;
}

/****************************************************************************
 * Public Functions -- REAL: SoC interrupt routing
 ****************************************************************************/

/****************************************************************************
 * Name: sys_drv_enable_mac_*_int / sys_drv_enable_modem_int
 *
 * Description:
 *   Beken routes these through sys_drv -> sys_hal -> sys_ll, and the sys_ll
 *   leaf sets one bit in cpu0_int_0_31_en / cpu0_int_32_63_en.  That is
 *   exactly the SoC routing matrix bk7258_irq.c already drives ahead of the
 *   NVIC, so up_enable_irq() is not an approximation of these functions --
 *   it is the same two register writes by a different name.
 *
 *   The mapping was taken from middleware/soc/bk7258/soc/sys_reg.h, where
 *   the *_EN_POS values are 31 in the 0..31 register and 0..4 in the 32..63
 *   one, i.e. interrupt lines 31 and 32..36.  BK7258_IRQ_* already carried
 *   those numbers.  modem is the odd one out: it is
 *   cpu0_wifi_int_phy_mpb_en, line 29, not a MAC line at all.
 *
 *   The vendor wraps each in a critical section; up_enable_irq() does its
 *   own read-modify-write under the same protection, so wrapping again
 *   would only nest.
 *
 ****************************************************************************/

uint32_t sys_drv_enable_mac_txrx_timer_int(void)
{
  up_enable_irq(BK7258_IRQ_MAC_TXRX_TMR);
  return 0;
}

uint32_t sys_drv_enable_mac_txrx_misc_int(void)
{
  up_enable_irq(BK7258_IRQ_MAC_TXRX_MISC);
  return 0;
}

uint32_t sys_drv_enable_mac_rx_trigger_int(void)
{
  up_enable_irq(BK7258_IRQ_MAC_RX_TRIG);
  return 0;
}

uint32_t sys_drv_enable_mac_tx_trigger_int(void)
{
  up_enable_irq(BK7258_IRQ_MAC_TX_TRIG);
  return 0;
}

uint32_t sys_drv_enable_mac_prot_int(void)
{
  up_enable_irq(BK7258_IRQ_MAC_PORT_TRIG);
  return 0;
}

uint32_t sys_drv_enable_mac_gen_int(void)
{
  up_enable_irq(BK7258_IRQ_MAC_GEN);
  return 0;
}

uint32_t sys_drv_enable_modem_int(void)
{
  up_enable_irq(BK7258_IRQ_PHY_MBP);
  return 0;
}

/****************************************************************************
 * Public Functions -- REAL: device clock gate
 ****************************************************************************/

/****************************************************************************
 * Name: bk_pm_clock_ctrl
 *
 * Description:
 *   Turn a peripheral's clock on or off.  This one is load-bearing and is
 *   the reason this file exists at all rather than a page of empty stubs:
 *   wifi_init.c:72-73 calls it for PM_CLK_ID_MAC and PM_CLK_ID_PHY, and
 *   that is the *only* place the WiFi MAC and radio clocks get switched on.
 *   Stubbed out, bk_wifi_init() would return success against dead hardware.
 *
 *   This port has already been bitten three times by that failure mode --
 *   the TIMER soft_reset, the flash write-protect, and the AON RTC clock
 *   domain -- so anything on a clock-enable path gets implemented, not
 *   stubbed.
 *
 *   Traced through: bk_pm_clock_ctrl -> sys_drv_dev_clk_pwr_up ->
 *   sys_hal_clk_pwr_ctrl, which is a read-modify-write of one bit in
 *   cpu_device_clk_enable.
 *
 ****************************************************************************/

int bk_pm_clock_ctrl(uint32_t module, uint32_t clock_state)
{
  uint32_t bit;

  if (module >= BK7258_CLK_PWR_ID_H264)
    {
      /* Second gate register (reserver_reg0xd).  Nothing in the WiFi path
       * reaches it; refuse loudly rather than write the wrong register.
       */

      syslog(LOG_WARNING, "wifi: clock id %" PRIu32 " out of range\n",
             module);
      return -1;
    }

  bit = 1u << module;

  if (clock_state == BK7258_PM_CLK_PWR_UP)
    {
      modifyreg32(BK7258_SYS_DEV_CLK_EN, 0, bit);
    }
  else
    {
      modifyreg32(BK7258_SYS_DEV_CLK_EN, bit, 0);
    }

  return 0;
}

/****************************************************************************
 * Public Functions -- ADEQUATE: logging and reset
 ****************************************************************************/

void bk_printf(const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  vsyslog(LOG_INFO, fmt, ap);
  va_end(ap);
}

void bk_printf_ext(int level, char *tag, const char *fmt, ...)
{
  va_list ap;

  UNUSED(level);
  UNUSED(tag);

  va_start(ap, fmt);
  vsyslog(LOG_INFO, fmt, ap);
  va_end(ap);
}

void bk_null_printf(const char *fmt, ...)
{
  UNUSED(fmt);
}

/* Beken's shell buffers log output and flushes it at checkpoints.  syslog
 * writes straight through, so there is nothing to flush.
 */

void shell_log_flush(void)
{
}

/****************************************************************************
 * Name: shell_assert_out
 *
 * Description:
 *   Where the vendor stack reports a failed BK_ASSERT.
 *
 *   This must not go through syslog, and the reason is the whole point of
 *   the function.  Beken's assert macro masks interrupts before it prints:
 *   rtos_disable_int, shell_log_flush, rtos_get_time, shell_assert_out,
 *   bk_system_dump, then "while (1);".  syslog needs the character driver
 *   and therefore interrupts, so an assert printed that way is silent --
 *   the board looks like it merely stopped.
 *
 *   That is exactly what happened with rxsens: it hit an assert, printed
 *   nothing, and sat in the vendor's infinite loop looking like a task that
 *   was simply busy.  up_putc() is the OS's polled low-level path and works
 *   inside the masked window.
 *
 *   The trailing "while (1)" belongs to the vendor and runs whatever this
 *   returns, so PANIC() only changes who reports it.  It is kept for
 *   bcontinue == false because a NuttX assert dump carries the task and
 *   stack state, which the vendor's spin does not.
 *
 ****************************************************************************/

static void bk_putstr(FAR const char *s)
{
  while (*s != '\0')
    {
      up_putc(*s++);
    }
}

int shell_assert_out(bool bcontinue, char *format, ...)
{
  char buf[192];
  va_list ap;

  va_start(ap, format);
  vsnprintf(buf, sizeof(buf), format, ap);
  va_end(ap);

  bk_putstr("wifi: ASSERT ");
  bk_putstr(buf);
  bk_putstr("\r\n");

  if (!bcontinue)
    {
      PANIC();
    }

  return 0;
}

void bk_reboot_ex(uint32_t reset_reason)
{
  syslog(LOG_ERR, "wifi: stack requested reboot, reason %" PRIu32 "\n",
         reset_reason);
  up_systemreset();
}

/****************************************************************************
 * Public Functions -- ADEQUATE: time, identity, bulk copy
 ****************************************************************************/

uint64_t bk_get_tick(void)
{
  return (uint64_t)clock_systime_ticks();
}

uint32_t bk_get_ticks_per_second(void)
{
  return TICK_PER_SEC;
}

/****************************************************************************
 * Name: bk_get_mac
 *
 * Description:
 *   The vendor reads the base address out of eFuse/OTP and derives the STA
 *   and AP addresses from it.  This port has no eFuse driver, so the base
 *   is a locally-administered address and the derivation is Beken's: AP
 *   differs from STA in the low bits of the last octet, which is what keeps
 *   a simultaneous STA+AP setup from using one address twice.
 *
 *   Reading the factory address (eFuse, or the net_param flash partition
 *   the RF calibration already lives in) is still to do.  Until then a
 *   board will not present the MAC printed on its label.
 *
 ****************************************************************************/

int bk_get_mac(uint8_t *mac, int type)
{
  if (mac == NULL)
    {
      return -1;
    }

  memcpy(mac, g_bk7258_base_mac, sizeof(g_bk7258_base_mac));

  switch (type)
    {
      case BK7258_MAC_TYPE_BASE:
        break;

      case BK7258_MAC_TYPE_STA:
        break;

      case BK7258_MAC_TYPE_AP:
        mac[5] ^= 0x01;
        break;

      case BK7258_MAC_TYPE_BLUETOOTH:
        mac[5] ^= 0x02;
        break;

      default:
        return -1;
    }

  return 0;
}

/****************************************************************************
 * Name: dma_memcpy
 *
 * Description:
 *   Beken hands large copies to a general-DMA channel.  We have no DMA
 *   driver for that block, and the CPU copy is correct if slower -- the
 *   caller cannot tell the difference because the vendor API is
 *   synchronous either way.
 *
 ****************************************************************************/

int dma_memcpy(void *out, const void *in, uint32_t len)
{
  memcpy(out, in, len);
  return 0;
}

/****************************************************************************
 * Name: ate_is_enabled / bk7258_wifi_ate_enable
 *
 * Description:
 *   ATE is Beken's factory test mode, entered by a strapping pin their
 *   bootloader samples.  This port never enters it.
 *
 *   The original note here said answering "yes" would divert the stack into
 *   calibration paths that expect test equipment on the other end of the
 *   radio.  That was a guess, and having now read all four call sites in the
 *   linked image it is wrong -- every one of them makes the stack *more*
 *   permissive, and none of them reaches for an instrument:
 *
 *     rw_ieee80211_init+0x8a   sets one byte per channel (regulatory flag)
 *     rwnx_handle_dynparams    rwnx_params.c:775, enables VHT/HE MCS 0-9
 *     rwnx_intf_init+0x54      skips rwnxl_sleep() and ps_env_set_ps_on(true)
 *     wifi_ate_is_enabled_wrapper   the adapter slot itself
 *
 *   The third one is why this is now switchable.  On the "no" answer the
 *   stack parks the MAC in doze and turns power save on at the tail of
 *   wifi_init(), and this port has no wake machinery outside the core
 *   thread, so anything reaching the MAC from another context finds it
 *   asleep -- which is exactly what the vendor's own assert reported:
 *   "MAC is in doze, open maccore and phy clock" at
 *   nxmac_current_state_getf:2001.  The vendor's radio tools never meet this
 *   because they run with ATE on.
 *
 *   Kept as a Kconfig with a runtime override rather than a hard-coded
 *   answer: this is a test mode, the product path should keep saying no, and
 *   the switch has to be settable before bk_wifi_init() runs, which is
 *   inside bk7258_wifi_ifup().
 *
 ****************************************************************************/

static bool g_bk7258_ate_enabled =
#ifdef CONFIG_BK7258_WIFI_ATE
  true;
#else
  false;
#endif

bool ate_is_enabled(void)
{
  return g_bk7258_ate_enabled;
}

void bk7258_wifi_ate_enable(bool enable)
{
  g_bk7258_ate_enabled = enable;
}

/****************************************************************************
 * Public Functions -- REAL: power islands
 ****************************************************************************/

/****************************************************************************
 * Name: bk_pm_module_vote_power_ctrl
 *
 * Description:
 *   Switch a power island on.  Like the clock gate above, this one is
 *   load-bearing: wifi_init.c:67-69 powers WIFIP_MAC, PHY and the PHY's
 *   WiFi sub-domain here, three lines *before* it touches the clock, and
 *   rwnxl_init() a few lines further down is the first code to read MAC
 *   registers.  With this stubbed out, that read faults -- which is exactly
 *   what the board did: BusFault escalated to Hard Fault inside
 *   rwnxl_init(), with "wifi: power vote not ported" as the line above it
 *   in the log.
 *
 *   Two details are not guessable from the call site:
 *
 *   - The polarity is inverted.  The register is cpu_power_sleep_wakeup at
 *     SOC_SYS_REG_BASE + (0x10 << 2) = 0x44010040, and its bits are
 *     power-*down* bits: sys_hal_module_power_ctrl() clears the bit to
 *     power a domain on.  Writing the obvious way round would switch off
 *     whatever was already running.
 *
 *   - Sub-modules are not domains.  PM_POWER_SUB_MODULE_NAME_PHY_WIFI is
 *     201, a vote on the PHY domain (10), not a bit of its own; the vendor
 *     folds it into the parent and keeps a separate refcount so the domain
 *     survives until the last voter leaves.
 *
 *     Worth knowing where those numbers come from: bk_idk carries two
 *     modules/pm.h, and the one under components/tfm/ is a different,
 *     older layout that happens to agree about MAC and PHY and disagrees
 *     about everything after them.  The header on the vendor include path
 *     -- the one these numbers must match -- is bk_idk/include/modules/pm.h.
 *
 *   That refcount is why power-off is not implemented here.  WiFi is the
 *   only voter in this image, nothing ever asks for a domain to go away,
 *   and honouring an off vote without the vendor's bookkeeping would let
 *   one subsystem cut power out from under another.  An off request is
 *   accepted and ignored rather than half-obeyed.
 *
 *   Not done: the vendor also calls phy_wakeup_reinit() when the PHY
 *   domain comes up.  That is the resume-from-sleep path; a cold boot runs
 *   calibration_init() a few lines later in wifi_init(), which initialises
 *   the same hardware.  If the radio turns out to need it anyway, this is
 *   the first place to look.
 *
 ****************************************************************************/

int bk_pm_module_vote_power_ctrl(uint32_t module, uint32_t power_state)
{
  uint32_t domain;

  /* Fold a sub-module vote onto the domain that actually has a bit. */

  domain = module >= BK7258_PM_SUB_DOMAIN_MAX ?
           module / BK7258_PM_SUB_DOMAIN_MAX : module;

  /* CPU1, CPU2 and APP sit above the bit range -- CPU1 and CPU2 have their
   * own halt/power sequences and APP is not a power domain at all.  Nothing
   * in the WiFi path asks for any of them.
   */

  if (domain > BK7258_PM_DOMAIN_LAST)
    {
      syslog(LOG_WARNING, "wifi: power domain %" PRIu32 " unsupported\n",
             module);
      return -1;
    }

  if (power_state == BK7258_PM_POWER_ON)
    {
      modifyreg32(BK7258_SYS_POWER_SLEEP, 1u << domain, 0);
    }

  return 0;
}

int bk_pm_sleep_register_cb(int sleep_mode, int dev_id,
                            void *enter_config, void *exit_config)
{
  UNUSED(sleep_mode);
  UNUSED(dev_id);
  UNUSED(enter_config);
  UNUSED(exit_config);

  BK7258_WIFI_PENDING("sleep callback");
  return 0;
}

/****************************************************************************
 * Public Functions -- PENDING: supplicant
 ****************************************************************************/

/* wpa_supplicant is not ported.  Everything below is the seam between the
 * driver and it: the control interface the driver uses to ask for a
 * connection, the socket interface that carries management and EAPOL
 * frames between them, and two IE helpers that live on the supplicant side
 * of the split.
 *
 * Scan and monitor do not go through here, so an unported supplicant costs
 * association, not the radio.  Which of these the stack reaches first is
 * the next thing to find out on hardware, and each one says so when it is
 * called.
 */

/* With the supplicant compiled in (CONFIG_BK7258_WIFI_WPA) every one of
 * these has a real, strong definition -- ctrl_iface.c, main_supplicant.c,
 * main_none.c, bk_patch/sk_intf.c, ieee802_11_common.c, notify.c -- and a
 * stub would be a duplicate-definition link error.  The stubs exist only
 * for the supplicant-less build.
 */

#ifndef CONFIG_BK7258_WIFI_WPA

int wpa_ctrl_request(int cmd, void *data)
{
  UNUSED(cmd);
  UNUSED(data);

  BK7258_WIFI_PENDING("wpa_ctrl_request");
  return -1;
}

int wpa_ctrl_request_async(int cmd, void *data)
{
  UNUSED(cmd);
  UNUSED(data);

  BK7258_WIFI_PENDING("wpa_ctrl_request_async");
  return -1;
}

int wpa_ctrl_event(int event, void *data)
{
  UNUSED(event);
  UNUSED(data);

  BK7258_WIFI_PENDING("wpa_ctrl_event");
  return -1;
}

int wpa_ctrl_event_copy(int event, void *data, int len)
{
  /* Two of these events carry the link state, and with no supplicant to
   * receive them nobody would ever advance it -- association succeeded on
   * the air while the state read IDLE forever.  The bridge in
   * bk7258_wifi_glue.c consumes CONNECT_IND and DISCONNECT_IND; the rest
   * remain honestly unported.
   */

  if (bk7258_wifi_wpa_event(event, data, len) == 0)
    {
      return 0;
    }

  BK7258_WIFI_PENDING("wpa_ctrl_event_copy");
  return -1;
}

void wpas_thread_start(void)
{
  BK7258_WIFI_PENDING("supplicant thread");
}

bool is_wpah_queue_full(void)
{
  return false;
}

/* The driver/supplicant socket interface (bk_patch/sk_intf.c).  A peek that
 * reports zero means "nothing waiting", which is the correct answer while
 * there is no supplicant to have queued anything.
 */

int ke_mgmt_packet_tx(unsigned char *buf, int len, int flag)
{
  UNUSED(buf);
  UNUSED(len);
  UNUSED(flag);

  BK7258_WIFI_PENDING("mgmt frame TX to supplicant");
  return 0;
}

int ke_mgmt_packet_rx(unsigned char *buf, int len, int flag)
{
  UNUSED(buf);
  UNUSED(len);
  UNUSED(flag);

  return 0;
}

int ke_l2_packet_rx(unsigned char *buf, int len, int flag)
{
  UNUSED(buf);
  UNUSED(len);
  UNUSED(flag);

  return 0;
}

int ke_mgmt_peek_rxed_next_payload_size(int flag)
{
  UNUSED(flag);
  return 0;
}

int ke_data_peek_rxed_next_payload_size(int flag)
{
  UNUSED(flag);
  return 0;
}

/* IE helpers that live in the supplicant's common code. */

const uint8_t *get_ie(const uint8_t *ies, size_t len, uint8_t eid)
{
  size_t i = 0;

  if (ies == NULL)
    {
      return NULL;
    }

  /* Walk the element chain.  This one is short enough to implement
   * correctly rather than stub: two bytes of header, then the body, and
   * a truncated element ends the walk.
   */

  while (i + 2 <= len)
    {
      uint8_t id  = ies[i];
      uint8_t ien = ies[i + 1];

      if (i + 2 + ien > len)
        {
          break;
        }

      if (id == eid)
        {
          return &ies[i];
        }

      i += 2 + ien;
    }

  return NULL;
}

bool sta_check_user_is_11b_1mbps_supported(void)
{
  return false;
}

#endif /* !CONFIG_BK7258_WIFI_WPA */

#ifdef CONFIG_BK7258_WIFI_WPA

/****************************************************************************
 * Supplicant platform gaps
 *
 * Symbols the supplicant sources consume that neither they, the closed
 * archives, nor NuttX define.  Each has a verified caller; none is
 * speculative.
 ****************************************************************************/

/* wpa_psk_cache.c precomputes PSKs on its own thread and nudges that
 * thread's priority around the computation.  Our OSI runs every vendor
 * thread at a fixed NuttX priority, so this is accepted and ignored --
 * the only cost is that the precompute does not yield more aggressively.
 */

int rtos_thread_set_priority(void *thread, int priority)
{
  UNUSED(thread);
  UNUSED(priority);
  return 0;
}

/* os_none.c derives wall-clock-ish time from these. */

uint32_t bk_get_second(void)
{
  return (uint32_t)(clock_systime_ticks() / TICK_PER_SEC);
}

uint32_t rtos_get_ms_per_tick(void)
{
  return MSEC_PER_TICK;
}

/* Constant-time compare.  The vendor gets this from a FreeRTOS support
 * file we do not compile; semantics per the reference implementation
 * there: 0 iff equal, nonzero otherwise, no early exit.
 */

int os_memcmp_const(const void *a, const void *b, size_t len)
{
  const unsigned char *pa = a;
  const unsigned char *pb = b;
  unsigned char diff = 0;
  size_t i;

  for (i = 0; i < len; i++)
    {
      diff |= pa[i] ^ pb[i];
    }

  return diff;
}

/* Fast-connect credential storage (easy_flash).  Not ported: a miss makes
 * the stack fall back to a normal scan-and-join, which is correct if a
 * few hundred milliseconds slower.  Returning failure/zeroes is the miss.
 */

int bk_get_env_enhance(const char *key, void *value, int value_len)
{
  UNUSED(key);

  if (value != NULL && value_len > 0)
    {
      memset(value, 0, value_len);
    }

  return 0;
}

int bk_set_env_enhance(const char *key, const void *value, int value_len)
{
  UNUSED(key);
  UNUSED(value);
  UNUSED(value_len);
  return 0;
}

/****************************************************************************
 * Randomness
 *
 * The vendor tree backs both of these with libc rand(): os_none.c's
 * os_get_random and the mbedtls-port's mbedtls_hardware_poll.  Every WPA2
 * nonce and SAE scalar flows through one of them.  Here both are backed by
 * arc4random_buf(), the CSPRNG already in this image; the rand()-backed
 * originals are renamed away at compile time (see bk7258_wifi_vendor.cmake)
 * and tls_hardware.c is not compiled at all.
 ****************************************************************************/

int os_get_random(unsigned char *buf, size_t len)
{
  arc4random_buf(buf, len);
  return 0;
}

unsigned long os_random(void)
{
  unsigned long v;

  arc4random_buf(&v, sizeof(v));
  return v;
}

/****************************************************************************
 * Name: _ctype_
 *
 * Description:
 *   Newlib's character-classification table.  The supplicant sources are
 *   compiled against the arm-none-eabi toolchain's own <ctype.h>, whose
 *   isspace()/isdigit() macros index this table (offset by one, EOF at
 *   slot 0); NuttX's libc classifies differently and never defines it.
 *   Bits are newlib's: 01 upper, 02 lower, 04 digit, 010 space, 020 punct,
 *   040 control, 0100 hex, 0200 blank.
 *
 ****************************************************************************/

const char _ctype_[257] =
{
  0, 040, 040, 040, 040, 040, 040, 040, 040, 040, 050, 050,
  050, 050, 050, 040, 040, 040, 040, 040, 040, 040, 040, 040,
  040, 040, 040, 040, 040, 040, 040, 040, 040, 0210, 020, 020,
  020, 020, 020, 020, 020, 020, 020, 020, 020, 020, 020, 020,
  020, 0104, 0104, 0104, 0104, 0104, 0104, 0104, 0104, 0104, 0104, 020,
  020, 020, 020, 020, 020, 020, 0101, 0101, 0101, 0101, 0101, 0101,
  01, 01, 01, 01, 01, 01, 01, 01, 01, 01, 01, 01,
  01, 01, 01, 01, 01, 01, 01, 01, 020, 020, 020, 020,
  020, 020, 0102, 0102, 0102, 0102, 0102, 0102, 02, 02, 02, 02,
  02, 02, 02, 02, 02, 02, 02, 02, 02, 02, 02, 02,
  02, 02, 02, 02, 020, 020, 020, 020, 040, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0
};

/* Heap diagnostics the vendor logging paths call.  Honest numbers where
 * NuttX has them, and the dumps go to syslog like everything else.
 */

uint32_t rtos_get_total_heap_size(void)
{
  struct mallinfo info = kmm_mallinfo();

  return (uint32_t)info.arena;
}

uint32_t rtos_get_minimum_free_heap_size(void)
{
  /* NuttX does not track the low-water mark; the current free total is the
   * closest honest answer and errs on the optimistic side.
   */

  struct mallinfo info = kmm_mallinfo();

  return (uint32_t)info.fordblks;
}

void os_dump_memory_stats(uint32_t start_tick, uint32_t ticks_since_malloc,
                          const char *task)
{
  UNUSED(start_tick);
  UNUSED(ticks_since_malloc);
  UNUSED(task);
}

void os_show_memory_config_info(void)
{
}

/* mbedtls entropy source (MBEDTLS_ENTROPY_HARDWARE_ALT in tls_config.h). */

int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len,
                          size_t *olen)
{
  UNUSED(data);

  arc4random_buf(output, len);
  if (olen != NULL)
    {
      *olen = len;
    }

  return 0;
}

#endif /* CONFIG_BK7258_WIFI_WPA */

/****************************************************************************
 * Public Functions -- PENDING: IP-stack glue
 ****************************************************************************/

/* These two are lwIP's, and NuttX owns the IP layer here instead.
 * sta_ip_down() tells lwIP to drop the interface's address; the netdev
 * lower half will do that through its own ifdown path once connect() is
 * real.  net_begin_send_arp_reply() is a gratuitous-ARP helper the vendor
 * uses after roaming, which NuttX's ARP layer handles on its own.
 */

void sta_ip_down(void)
{
  BK7258_WIFI_PENDING("sta_ip_down");
}

void net_begin_send_arp_reply(bool is_send_arp, bool is_allow_send_req)
{
  UNUSED(is_send_arp);
  UNUSED(is_allow_send_req);
}

/****************************************************************************
 * Public Functions -- REAL: interrupt registration
 ****************************************************************************/

/* Beken's ISRs take no arguments and return nothing; NuttX's take the irq
 * number, the interrupted context and the registered argument.  One
 * trampoline per line bridges the two, and the line number indexes it.
 */

#define BK7258_WIFI_ISR_MAX  64

static void (*g_bk7258_wifi_isr[BK7258_WIFI_ISR_MAX])(void);
static uint32_t g_bk7258_wifi_isr_hits[BK7258_WIFI_ISR_MAX];

static int bk7258_wifi_isr_trampoline(int irq, FAR void *context,
                                      FAR void *arg)
{
  unsigned int line = (unsigned int)(irq - NVIC_IRQ_FIRST);

  UNUSED(context);
  UNUSED(arg);

  if (line < BK7258_WIFI_ISR_MAX && g_bk7258_wifi_isr[line] != NULL)
    {
      g_bk7258_wifi_isr_hits[line]++;
      g_bk7258_wifi_isr[line]();
    }

  return OK;
}

/****************************************************************************
 * Name: bk_int_isr_register
 *
 * Description:
 *   Attach a vendor ISR to an interrupt source.
 *
 *   Beken looks the source up in icu_int_map_table[src] to get the hardware
 *   line.  On BK7258 that table is the identity -- every one of its sixty
 *   entries maps source N to line N (middleware/soc/bk7258/soc/icu_map.h),
 *   MODEM at 29 and the six MAC lines at 31..36 included -- so the lookup
 *   collapses to an offset from NVIC_IRQ_FIRST.  Checked rather than
 *   assumed, because it is the kind of thing that is true until a chip
 *   revision reorders one entry.
 *
 *   This is what keeps the MAC interrupts from landing in
 *   irq_unexpected_isr(): sys_drv_enable_mac_*_int() above turns the lines
 *   on, and nothing else in this port attaches a handler to them.
 *
 ****************************************************************************/

/* Diagnostic: has any of the MAC's interrupts ever actually been taken?
 * The registrations and the enables are both visible in the log, but
 * neither says a line has fired -- and a MAC that never interrupts looks
 * exactly like a MAC that never answers, which is where scan is stuck.
 */

void bk7258_wifi_irq_report(void)
{
  int i;

  syslog(LOG_INFO, "wifi: irq en 0-31 %08" PRIx32 " 32-63 %08" PRIx32 "\n",
         getreg32(BK7258_SYS_CPU0_INT_EN(0)),
         getreg32(BK7258_SYS_CPU0_INT_EN(32)));

  for (i = 0; i < BK7258_WIFI_ISR_MAX; i++)
    {
      if (g_bk7258_wifi_isr[i] != NULL)
        {
          syslog(LOG_INFO, "wifi: irq src %d hits %" PRIu32 "\n",
                 i, g_bk7258_wifi_isr_hits[i]);
          up_enable_irq(NVIC_IRQ_FIRST + i);
        }
    }

  syslog(LOG_INFO, "wifi: irq re-en 0-31 %08" PRIx32 " 32-63 %08" PRIx32 "\n",
         getreg32(BK7258_SYS_CPU0_INT_EN(0)),
         getreg32(BK7258_SYS_CPU0_INT_EN(32)));
}

int bk_int_isr_register(uint32_t src, void (*isr)(void), void *arg)
{
  UNUSED(arg);

  if (src >= BK7258_WIFI_ISR_MAX)
    {
      syslog(LOG_WARNING, "wifi: isr source %" PRIu32 " out of range\n", src);
      return -1;
    }

  syslog(LOG_INFO, "wifi: isr register src %" PRIu32 "\n", src);

  g_bk7258_wifi_isr[src] = isr;

  if (isr == NULL)
    {
      irq_detach(NVIC_IRQ_FIRST + src);
      return 0;
    }

  return irq_attach(NVIC_IRQ_FIRST + src,
                    bk7258_wifi_isr_trampoline, NULL) == OK ? 0 : -1;
}

/****************************************************************************
 * Public Functions -- REAL: interrupt group masks and modem clock
 ****************************************************************************/

/****************************************************************************
 * Name: sys_drv_int_enable / disable and their group-2 twins
 *
 * Description:
 *   These take a bit mask, not a line number, and write the same
 *   cpu0_int_*_en pair bk7258_irq.c drives one bit at a time.  Group 2 is
 *   the 32..63 half.
 *
 *   The disable half MUST return what was enabled before, because that
 *   return value is the only thing the restore has to work from.  From
 *   rw_task.c, which is the contract:
 *
 *     #define WIFI_INT_DISABLE()  int_en_low32  = sys_drv_int_disable(...); \
 *                                 int_en_high32 = sys_drv_int_group2_disable(...)
 *     #define WIFI_INT_RESTORE()  sys_drv_int_enable(int_en_low32); \
 *                                 sys_drv_int_group2_enable(int_en_high32)
 *
 *   This port returned 0 from both disablers.  Every critical section the
 *   MAC took therefore switched its interrupts off and "restored" them to
 *   an empty mask -- so the first WIFI_INT_DISABLE() in the life of the
 *   image disabled the MAC's seven lines permanently.
 *
 *   Everything that followed is explained by it: the enable bits read back
 *   clear at scan time although bk_wifi_interrupt_init() had set them, the
 *   interrupt hit counters stayed at zero on all seven lines, and messages
 *   to the MAC got no confirmation and asserted five seconds later in
 *   rw_msg_send().  A zero return is a plausible-looking value for a
 *   function whose name says nothing about returning anything, which is
 *   why it survived so long.
 *
 *   The masked AND matters: returning the whole register would re-enable
 *   lines the caller never asked about, including ones another subsystem
 *   had deliberately masked.
 *
 ****************************************************************************/

uint32_t sys_drv_int_enable(uint32_t param)
{
  modifyreg32(BK7258_SYS_CPU0_INT_EN(0), 0, param);
  return 0;
}

uint32_t sys_drv_int_disable(uint32_t param)
{
  uint32_t prev = getreg32(BK7258_SYS_CPU0_INT_EN(0)) & param;

  modifyreg32(BK7258_SYS_CPU0_INT_EN(0), param, 0);
  return prev;
}

uint32_t sys_drv_int_group2_enable(uint32_t param)
{
  modifyreg32(BK7258_SYS_CPU0_INT_EN(32), 0, param);
  return 0;
}

uint32_t sys_drv_int_group2_disable(uint32_t param)
{
  uint32_t prev = getreg32(BK7258_SYS_CPU0_INT_EN(32)) & param;

  modifyreg32(BK7258_SYS_CPU0_INT_EN(32), param, 0);
  return prev;
}

/****************************************************************************
 * Name: sys_drv_modem_clk_ctrl / sys_drv_modem_bus_clk_ctrl
 *
 * Description:
 *   The modem clock is the PHY's, bit 27 of the same device clock gate
 *   bk_pm_clock_ctrl() drives -- sys_hal_modem_clk_ctrl() is one call to
 *   sys_ll_set_cpu_device_clk_enable_phy_cken().
 *
 *   The bus variant is empty here because it is empty in the vendor HAL
 *   too: sys_hal_modem_bus_clk_ctrl() on BK7258 carries the comment "7256
 *   no bus clock enable" and no body.  Worth stating, so nobody later reads
 *   this as an unfinished stub and goes looking for the register.
 *
 ****************************************************************************/

uint32_t sys_drv_modem_clk_ctrl(bool clk_en)
{
  bk_pm_clock_ctrl(BK7258_PM_CLK_ID_PHY,
                   clk_en ? BK7258_PM_CLK_PWR_UP : 0);
  return 0;
}

uint32_t sys_drv_modem_bus_clk_ctrl(bool clk_en)
{
  UNUSED(clk_en);
  return 0;
}

/****************************************************************************
 * Name: bk_pm_module_power_state_get
 *
 * Description:
 *   Note the sense: the register bit means "powered down", and the vendor's
 *   callers test the result against zero to mean "on".  Returning the raw
 *   bit is therefore correct and inverting it would be wrong, however
 *   backwards it reads.
 *
 ****************************************************************************/

int32_t bk_pm_module_power_state_get(uint32_t module)
{
  uint32_t domain = module >= BK7258_PM_SUB_DOMAIN_MAX ?
                    module / BK7258_PM_SUB_DOMAIN_MAX : module;

  if (domain > BK7258_PM_DOMAIN_LAST)
    {
      return 1;
    }

  return (getreg32(BK7258_SYS_POWER_SLEEP) >> domain) & 1;
}

/****************************************************************************
 * Public Functions -- ADEQUATE: time, logging
 ****************************************************************************/

uint64_t bk_aon_rtc_get_current_tick(uint32_t id)
{
  UNUSED(id);
  return (uint64_t)clock_systime_ticks();
}

float bk_rtc_get_ms_tick_count(void)
{
  return (float)TICK2MSEC(clock_systime_ticks());
}

void bk_printf_raw(int level, char *tag, const char *fmt, ...)
{
  va_list ap;

  UNUSED(level);
  UNUSED(tag);

  va_start(ap, fmt);
  vsyslog(LOG_INFO, fmt, ap);
  va_end(ap);
}

void bk_system_dump(void)
{
  syslog(LOG_ERR, "wifi: stack requested a system dump\n");
}

/* Build stamps for the closed PHY archives.  The real ones come from a
 * generated file the vendor build produces; nothing reads these except a
 * version print.
 */

char *bk_get_phy_libs_build_date(void)
{
  return "unknown";
}

char *bk_get_phy_libs_build_time(void)
{
  return "unknown";
}

char *bk_get_phy_libs_commit_id(void)
{
  return "unknown";
}

/****************************************************************************
 * Public Functions -- PENDING: flash-backed calibration
 ****************************************************************************/

/****************************************************************************
 * Name: bk_flash_partition_read / bk_spec_flash_write_bytes
 *
 * Description:
 *   The stack reads factory RF calibration and the MAC address out of named
 *   flash partitions (rf_firmware, net_param).  bk7258_flash.c can reach
 *   that flash, but this port has no partition table to resolve a name
 *   against -- the bootloader's table is read by name and offset only, and
 *   nothing in the tree parses it yet.
 *
 *   Failing is the right answer rather than returning zeroed data: the
 *   vendor treats a read failure as "no stored calibration" and falls back
 *   to the compiled defaults in bk7258_vnd_cal.c, whereas a successful read
 *   of zeros would be taken as real calibration and drive the radio with
 *   it.
 *
 *   This is the first thing to implement when transmit power looks wrong.
 *
 ****************************************************************************/

int bk_flash_partition_read(int partition, uint8_t *dst, uint32_t offset,
                           uint32_t size)
{
  UNUSED(partition);
  UNUSED(dst);
  UNUSED(offset);
  UNUSED(size);

  BK7258_WIFI_PENDING("flash partition read");
  return -1;
}

int bk_spec_flash_write_bytes(int partition, const uint8_t *src,
                              uint32_t size, uint32_t offset)
{
  UNUSED(partition);
  UNUSED(src);
  UNUSED(size);
  UNUSED(offset);

  BK7258_WIFI_PENDING("flash partition write");
  return -1;
}

/****************************************************************************
 * Public Functions -- PENDING: sensors, clocks, GPIO mux, power policy
 ****************************************************************************/

/* RC32K trim measurement, used to correct sleep timing.  This port does not
 * sleep the WiFi domain, so zero ppm error is the honest answer.
 */

uint32_t bk_ckmn_driver_get_rc32k_ppm(void)
{
  return 0;
}

/* Die temperature feeds the PHY's temperature compensation.  Without a
 * SARADC reading wired up the stack keeps its default compensation, which
 * is what it does on a board with no sensor.
 */

int bk_sensor_set_current_temperature(int temperature)
{
  UNUSED(temperature);
  return 0;
}

/* GPIO multiplexing for external RF control (antenna switch, PA enable).
 * The R1 board's radio has no such pins, so there is nothing to map.
 */

int gpio_dev_map(uint32_t gpio_id, uint32_t dev)
{
  UNUSED(gpio_id);
  UNUSED(dev);
  return 0;
}

int gpio_dev_unmap(uint32_t gpio_id)
{
  UNUSED(gpio_id);
  return 0;
}

int gpio_dev_unprotect_map(uint32_t gpio_id, uint32_t dev)
{
  UNUSED(gpio_id);
  UNUSED(dev);
  return 0;
}

int gpio_dev_unprotect_unmap(uint32_t gpio_id)
{
  UNUSED(gpio_id);
  return 0;
}

/* Low-power analog trimming, entered around deep sleep. */

void sys_hal_enter_low_analog(void)
{
}

void sys_hal_exit_low_analog(void)
{
}

/* Remaining power-policy hooks.  All of them concern sleep, DVFS or the
 * external 32 kHz source, none of which this port uses.
 */

int bk_pm_lpo_src_get(void)
{
  return 0;
}

int bk_pm_module_vote_cpu_freq(uint32_t module, uint32_t cpu_freq)
{
  UNUSED(module);
  UNUSED(cpu_freq);
  return 0;
}

int bk_pm_module_vote_sleep_ctrl(uint32_t module, uint32_t sleep_state,
                                 uint32_t sleep_time)
{
  UNUSED(module);
  UNUSED(sleep_state);
  UNUSED(sleep_time);
  return 0;
}

/* The PHY re-initialises itself after the domain has been powered down.
 * Since this port never powers it down, the flag is always clear.
 */

uint32_t bk_pm_phy_reinit_flag_get(void)
{
  return 0;
}

void bk_pm_phy_reinit_flag_clear(void)
{
}

int pm_extern32k_register_cb(void *cfg)
{
  UNUSED(cfg);
  return 0;
}

/****************************************************************************
 * Public Functions -- PENDING: lwIP interface
 ****************************************************************************/

/* NuttX owns the IP stack, so none of lwIP is present.  The byte-order
 * helpers are real because they are pure arithmetic and the stack uses them
 * to build frames; everything else is a socket or a netif operation that
 * belongs to a stack we do not have, and will be replaced by the netdev
 * lower half as the RX and TX paths land.
 */

uint16_t lwip_htons(uint16_t n)
{
  return (uint16_t)((n << 8) | (n >> 8));
}

uint32_t lwip_htonl(uint32_t n)
{
  return ((n & 0xff) << 24) | ((n & 0xff00) << 8) |
         ((n & 0xff0000) >> 8) | (n >> 24);
}

int lwip_socket(int domain, int type, int protocol)
{
  UNUSED(domain);
  UNUSED(type);
  UNUSED(protocol);

  BK7258_WIFI_PENDING("lwip_socket");
  return -1;
}

int lwip_close(int s)
{
  UNUSED(s);
  return -1;
}

int lwip_sendto(int s, const void *data, size_t size, int flags,
                const void *to, uint32_t tolen)
{
  UNUSED(s);
  UNUSED(data);
  UNUSED(size);
  UNUSED(flags);
  UNUSED(to);
  UNUSED(tolen);

  BK7258_WIFI_PENDING("lwip_sendto");
  return -1;
}

int ip4addr_aton(const char *cp, void *addr)
{
  UNUSED(cp);
  UNUSED(addr);
  return 0;
}

char *ip4addr_ntoa(const void *addr)
{
  UNUSED(addr);
  return "0.0.0.0";
}

/* netif registration.  wlan0 already exists -- bk7258_wifi.c registered it
 * with netdev_lower_register() long before the vendor stack came up -- so
 * these are the vendor asking for something that has already happened.
 */

void *net_wlan_add_netif(void *mac)
{
  UNUSED(mac);

  BK7258_WIFI_PENDING("net_wlan_add_netif");
  return NULL;
}

int net_wlan_remove_netif(void *mac)
{
  UNUSED(mac);
  return 0;
}

int dhcp_lookup_mac(void *netif, void *ip, void *mac)
{
  UNUSED(netif);
  UNUSED(ip);
  UNUSED(mac);
  return -1;
}

void sta_ip_mode_set(int dhcp)
{
  UNUSED(dhcp);
}

void uap_ip_start(void)
{
  BK7258_WIFI_PENDING("uap_ip_start");
}

void uap_ip_down(void)
{
}
