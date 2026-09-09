/****************************************************************************
 * board/contest_board/chip/bk7258_timerisr.c
 *
 * System timer built on the Armv8-M SysTick counter.
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

#include <stdint.h>
#include <time.h>
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/clock.h>
#include <nuttx/irq.h>
#ifdef CONFIG_ALARM_ARCH
#  include <assert.h>
#  include <nuttx/timers/oneshot.h>
#  include <nuttx/timers/arch_alarm.h>
#endif

#include <arch/board/board.h>

#include "arm_internal.h"

#include "bk7258_rtc.h"
#include "bk7258_wdt.h"
#include "nvic.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* SysTick counts down from the reload value to zero, so the reload needed
 * for a period of N input clocks is N - 1.
 *
 * SysTick is clocked from the core clock, which the Beken bootloader has
 * already configured by the time NuttX runs.  BOARD_CPU_FREQUENCY records
 * what that frequency is; if the tick rate is visibly wrong, that constant
 * is the thing to correct.
 */

#define SYSTICK_RELOAD ((BOARD_CPU_FREQUENCY / CLK_TCK) - 1)

/* The reload field is 24 bits wide. */

#if SYSTICK_RELOAD > 0x00ffffff
#  error BOARD_CPU_FREQUENCY/CLK_TCK does not fit in the SysTick reload field
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_timerisr
 ****************************************************************************/

#ifndef CONFIG_ALARM_ARCH
static int bk7258_timerisr(int irq, uint32_t *regs, void *arg)
{
  /* Reading the control register clears the count flag. */

  /* Heartbeat: re-arm the watchdog on every 10th tick.  If ticks stop --
   * a spin with interrupts masked, a wedged handler, a runaway loop at
   * interrupt level -- the dog fires and the chip resets itself into the
   * bootloader's download window.  Decimated because the arm sequence
   * crosses into the slow AON bus: at the 1 kHz tick rate an every-tick
   * feed taxed the CPU hard enough to cost the eye animation ~1/3 of its
   * frame budget.
   */

  static unsigned int decimate = 0;

  if (++decimate >= BK7258_WDT_HEARTBEAT_TICKS)
    {
      decimate = 0;
      bk7258_wdt_service();
    }

#ifdef CONFIG_BK7258_RTC
  /* Close the AON RTC's 32-bit wrap window.  The counter wraps every 36.4
   * hours; sampling it once a second is four orders of magnitude of margin
   * for one AON read, and it costs a hundredth of what the watchdog feed
   * above already costs.
   */

  static unsigned int rtc_decimate = 0;

  if (++rtc_decimate >= MSEC2TICK(1000))
    {
      rtc_decimate = 0;
      bk7258_rtc_poll();
    }
#endif

  nxsched_process_timer();
  return OK;
}
#endif /* !CONFIG_ALARM_ARCH */

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: up_timer_initialize
 *
 * Description:
 *   Start the system timer interrupt at the rate given by CLK_TCK.
 *
 ****************************************************************************/

#ifdef CONFIG_ALARM_ARCH
void up_timer_initialize(void)
{
  FAR struct oneshot_lowerhalf_s *lower = bk7258_oneshot_initialize();

  /* No time base means no scheduler.  Say so rather than limping on with a
   * clock that never advances -- that failure mode looks like a hang and
   * costs hours to trace back here.
   */

  DEBUGASSERT(lower != NULL);

  up_alarm_set_lowerhalf(lower);
}
#else
void up_timer_initialize(void)
{
  uint32_t regval;

  /* Program the tick period and restart the counter. */

  putreg32(SYSTICK_RELOAD, NVIC_SYSTICK_RELOAD);
  putreg32(0, NVIC_SYSTICK_CURRENT);

  irq_attach(NVIC_IRQ_SYSTICK, (xcpt_t)bk7258_timerisr, NULL);

  /* Run SysTick from the core clock and enable its interrupt. */

  regval = NVIC_SYSTICK_CTRL_CLKSOURCE | NVIC_SYSTICK_CTRL_TICKINT |
           NVIC_SYSTICK_CTRL_ENABLE;
  putreg32(regval, NVIC_SYSTICK_CTRL);

  up_enable_irq(NVIC_IRQ_SYSTICK);
}
#endif /* CONFIG_ALARM_ARCH */
