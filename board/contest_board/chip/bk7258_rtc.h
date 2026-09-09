/****************************************************************************
 * board/contest_board/chip/bk7258_rtc.h
 *
 * AON RTC for the BK7258.
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

#ifndef __BOARD_CONTEST_BOARD_CHIP_BK7258_RTC_H
#define __BOARD_CONTEST_BOARD_CHIP_BK7258_RTC_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#include <nuttx/timers/rtc.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* AON RTC register block at BK7258_AON_RTC_BASE (0x44000200).
 *
 * Offsets and bit positions come from the vendor SDK, bekencorp/bk_idk
 * master, middleware/soc/bk7258/hal/aon_rtc_ll.h.  That file addresses the
 * block two ways -- through a register struct and, in aon_rtc_ll_tick_init()
 * / aon_rtc_ll_get_current_tick() / aon_rtc_ll_open_rtc_wakeup(), through
 * explicit word offsets.  The explicit offsets are what is transcribed here
 * because they are unambiguous, and the two views cross-check:
 *
 *   tick_init() writes 0x40 to +0x0*4, and aon_rtc_ll_enable() defines the
 *   enable bit as (0x1<<6) of ctrl -- so +0x00 is CTRL.
 *   open_rtc_wakeup() writes the compare value to +0x2*4 and then sets
 *   BIT_AON_RTC_RTC_TICK_INT_EN (0x8, i.e. bit 3 = tick_int_en) in ctrl --
 *   so +0x08 is TICK_VAL, the tick compare.
 *   tick_init() parks 0xffffffff in +0x1*4, the other compare -- UPPER_VAL.
 *   get_current_tick() reads +0x3*4 -- COUNTER_VAL, the free-running count.
 *
 * The block has _hi companions for 64-bit counting (aon_rtc_hal_64bit.h),
 * but their offsets appear only in the non-public aon_rtc_hw.h.  Rather
 * than guess at register addresses, this driver extends the 32-bit counter
 * in software; see bk7258_rtc.c.
 */

#define BK7258_RTC_CTRL_OFFSET     0x00  /* Control */
#define BK7258_RTC_UPPER_OFFSET    0x04  /* Upper compare value */
#define BK7258_RTC_TICK_OFFSET     0x08  /* Tick compare value */
#define BK7258_RTC_COUNTER_OFFSET  0x0c  /* Free-running counter */

/* CTRL bits.  Bits 4 and 5 are write-1-to-clear status; every read-modify-
 * write of CTRL must mask them out or it silently acknowledges a pending
 * interrupt.  The vendor's accessors do exactly that, and so do ours.
 */

#define BK7258_RTC_CTRL_CNT_RESET   (1 << 0)
#define BK7258_RTC_CTRL_CNT_STOP    (1 << 1)
#define BK7258_RTC_CTRL_UP_INT_EN   (1 << 2)
#define BK7258_RTC_CTRL_TICK_INT_EN (1 << 3)
#define BK7258_RTC_CTRL_UP_INT_STS  (1 << 4)  /* W1C */
#define BK7258_RTC_CTRL_TICK_INT_STS (1 << 5) /* W1C */
#define BK7258_RTC_CTRL_EN          (1 << 6)

#define BK7258_RTC_CTRL_W1C_MASK \
  (BK7258_RTC_CTRL_UP_INT_STS | BK7258_RTC_CTRL_TICK_INT_STS)

/* The AON counter is clocked by the 32K low-power oscillator, which is
 * either the external crystal at 32768 Hz or the internal ROSC at 32000 Hz
 * (bk_idk include/driver/aon_rtc.h states both).  Which one a given board
 * ends up on is a function of what the bootloader configured, so the driver
 * measures the rate at init against the system tick rather than assuming.
 */

#define BK7258_RTC_HZ_XTAL     32768
#define BK7258_RTC_HZ_ROSC     32000

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_rtc_initialize
 *
 * Description:
 *   Bring up the AON RTC counter, measure its clock rate and hand the
 *   lower half to the arch RTC layer, which makes it the system time
 *   source.  Must be called after the OS clock is running -- it times the
 *   AON counter against the system tick -- and not before board bringup:
 *   an AON access made too early in __start() has been observed to stall
 *   the bus on this SoC.  board_late_initialize() is the right place.
 *
 * Returned Value:
 *   Zero on success; a negated errno value on failure.
 *
 ****************************************************************************/

int bk7258_rtc_initialize(void);

/****************************************************************************
 * Name: bk7258_rtc_lowerhalf
 *
 * Description:
 *   Return the lower half handle for rtc_initialize(), which publishes it
 *   as /dev/rtc0.  Returns NULL if bk7258_rtc_initialize() has not run or
 *   did not succeed.
 *
 ****************************************************************************/

FAR struct rtc_lowerhalf_s *bk7258_rtc_lowerhalf(void);

/****************************************************************************
 * Name: bk7258_rtc_register
 *
 * Description:
 *   Make the RTC the system wall clock.  Call from board_late_initialize();
 *   the clock_synchronize() it triggers cannot run any earlier.
 *
 ****************************************************************************/

int bk7258_rtc_register(void);

#ifdef CONFIG_ALARM_ARCH
/****************************************************************************
 * Name: bk7258_oneshot_initialize
 *
 * Description:
 *   Bring up the AON counter and return the oneshot lower half that backs
 *   arch_alarm.  Called from up_timer_initialize().
 *
 ****************************************************************************/

struct oneshot_lowerhalf_s;
FAR struct oneshot_lowerhalf_s *bk7258_oneshot_initialize(void);
#endif

/****************************************************************************
 * Name: bk7258_rtc_poll
 *
 * Description:
 *   Sample the counter so the software 64-bit extension notices a 32-bit
 *   wrap.  Cheap (one AON read) and only needs to happen far more often
 *   than the wrap period -- 36.4 hours at 32768 Hz.  Called from the
 *   SysTick handler at a heavily decimated rate.
 *
 ****************************************************************************/

void bk7258_rtc_poll(void);

#endif /* __BOARD_CONTEST_BOARD_CHIP_BK7258_RTC_H */
