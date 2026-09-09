/****************************************************************************
 * board/contest_board/src/bk7258_boardinit.c
 *
 * Board bring-up for the Agora ConvoAI Kit R1 (Beken BK7258).
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

#include <nuttx/arch.h>
#include <nuttx/board.h>

#include "bk7258_rtc.h"
#include "bk7258_wdt.h"
#include "bk7258_gpio.h"
#include "bk7258_reset_reason.h"

#include <nuttx/kmalloc.h>
#include <syslog.h>
#include <string.h>

#include "arm_internal.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: board_early_initialize
 *
 * Description:
 *   Called from __start() once .data and .bss are in place and the console
 *   is alive, but before nx_start().
 *
 ****************************************************************************/

#ifdef CONFIG_BOARD_EARLY_INITIALIZE
void board_early_initialize(void)
{
  /* The console pins and clocks are set up by the chip layer.  Nothing else
   * on this board has to be touched this early.
   */
}
#endif

/****************************************************************************
 * Name: board_late_initialize
 *
 * Description:
 *   Called by NuttX after the scheduler is running, so it may use the full
 *   driver API.
 *
 ****************************************************************************/

#ifdef CONFIG_BOARD_LATE_INITIALIZE
void bk7258_serial_monitor_start(void);

void board_late_initialize(void)
{
  /* Switch on the shared external 3.3 V rail.  GPIO52 is net LDO33_EN, the
   * supply gate for the motor, the LCD panels, the SD NAND and the NFC
   * front end (vendor: CONFIG_LDO3V3_CTRL_GPIO=52, driven high first thing
   * at boot).  The vendor arbitrates it with a per-module voting scheme;
   * until more than one of those consumers exists here, on-at-boot is the
   * whole policy.
   */

  bk7258_gpio_config(52, true, false, false);
  bk7258_gpio_write(52, true);

#if CONFIG_MM_REGIONS > 1
  /* Fold the upper SRAM banks into the heap.  The linked region stops well
   * short of them because early boot cannot keep .data/.bss any higher
   * (measured; see the link script), but at runtime SRAM4/5 have carried
   * stacks and test patterns through every probe.  This region starts at
   * _ebbnote rather than at the end of the linked region: the 128 bytes
   * between them are the serial black box, which has to stay out of every
   * heap.
   */

  /* Top 32 KB (0x28098000+) stays out of the heap: the camera's YUV
   * line ping-pong buffer lives at its base and SRAM-resident hot code
   * (TJpgDec, copied below) at 0x2809d000.
   */

    {
      extern uint32_t _ebbnote[];

      kumm_addregion((void *)_ebbnote,
                     0x28098000 - (uintptr_t)_ebbnote);
    }

    {
      extern uint8_t _ssramfunc[];
      extern uint8_t _esramfunc[];
      extern uint8_t _fsramfunc[];

      memcpy(_ssramfunc, _fsramfunc, _esramfunc - _ssramfunc);

      /* The copy went through the D-side; drop stale I-side lines. */

      putreg32(0, 0xe000ef50);                      /* ICIALLU */
      __asm__ __volatile__ ("dsb\n isb" : : : "memory");
    }
#endif

#if CONFIG_MM_REGIONS > 2
  /* 16 MB APS128XXO_OB9 PSRAM at 0x60000000.  Init verifies the die ID
   * and a spread-out pattern test before the memory is trusted with heap
   * duty; on any failure the region is simply not added.
   */

    {
      extern size_t bk7258_psram_init(void);
      size_t psize = bk7258_psram_init();

      if (psize > 0)
        {
          kumm_addregion((void *)0x60000000, psize);
          syslog(LOG_INFO, "psram: %u MB online\n",
                 (unsigned)(psize >> 20));
        }
      else
        {
          syslog(LOG_WARNING, "psram: init failed, running without\n");
        }
    }
#endif

  bk7258_serial_monitor_start();

  /* Latch why this boot happened before anything else can disturb the
   * always-on field.  Same domain-readiness argument as the RTC below: not
   * from __start(), but safe by the time the console and watchdog are up.
   */

  bk7258_reset_cause_latch();

  /* The NMI watchdog stage.  Same domain-readiness argument as the latch
   * above and the RTC below: the peripheral block at 0x44800000 must not be
   * addressed before its clock is running, and by here it is.
   */

  bk7258_wdt_nmi_initialize();

#ifdef CONFIG_BK7258_RTC
  /* The AON RTC is brought up here, not in up_rtc_initialize().  It needs a
   * running system tick to measure its own clock rate against, and the AON
   * domain must not be poked before it is ready -- the watchdog bricked the
   * board twice from the top of __start() proving that.  By this point the
   * console is up and the watchdog has been arming from the same domain for
   * a while, so the domain is known good.  CONFIG_RTC_EXTERNAL is what lets
   * the OS wait this long for its clock.
   */

    {
      int ret = bk7258_rtc_initialize();

      if (ret < 0)
        {
          syslog(LOG_WARNING, "rtc: not available: %d\n", ret);
        }
      else
        {
          ret = bk7258_rtc_register();
          if (ret < 0)
            {
              syslog(LOG_WARNING, "rtc: not registered: %d\n", ret);
            }
        }
    }
#endif

    {
      /* Force the BLE staging object (and with it the closed-library
       * dependency tree) out of libarch.a and into the image.  Needs
       * the bk_idk checkout on the build host (chip/CMakeLists.txt).
       */

      extern uintptr_t bk7258_ble_link_probe(void);
      syslog(LOG_INFO, "ble: staging linked, probe %08x\n",
             (unsigned)(bk7258_ble_link_probe() & 0xffffffffu));
    }

#ifdef CONFIG_FS_PROCFS
  /* Mounting procfs is handled by the init script, not here. */
#endif
}
#endif

/****************************************************************************
 * Name: board_reset
 *
 * Description:
 *   Reset the board.  Wired to boardctl(BOARDIOC_RESET) and therefore to
 *   the NSH `reboot` command.
 *
 *   The reset goes through the watchdog because that is the only mechanism
 *   that actually resets this SoC -- the vendor SDK never writes AIRCR and
 *   SYSRESETREQ was tried on hardware and does nothing.  A reboot lands in
 *   the Beken bootloader, whose UART download window is what the flashing
 *   tools wait for, so `reboot` doubles as the no-hands way to make the
 *   board flashable.
 *
 ****************************************************************************/

#ifdef CONFIG_BOARDCTL_RESET
int board_reset(int status)
{
  UNUSED(status);

  /* Say it was us, so the next boot does not report this as a spontaneous
   * watchdog bite.  Must precede the reset -- the always-on field is what
   * carries the answer across.
   */

  bk7258_reset_reason_set(BK7258_RESET_REBOOT);

  bk7258_wdt_reboot();
  return 0;
}
#endif
