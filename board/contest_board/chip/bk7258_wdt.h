/****************************************************************************
 * board/contest_board/chip/bk7258_wdt.h
 *
 * Watchdog control for the BK7258.
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

#ifndef __BOARD_CONTEST_BOARD_CHIP_BK7258_WDT_H
#define __BOARD_CONTEST_BOARD_CHIP_BK7258_WDT_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/clock.h>

#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The watchdog is the only reset mechanism that works on this SoC: the
 * vendor SDK never touches AIRCR, and both its bk_reboot() and the Beken
 * bootloader restart the chip by arming a short watchdog period and
 * spinning.  SYSRESETREQ was tried on real hardware and does nothing.
 *
 * There are two independent watchdog blocks, and this port uses both for
 * different jobs (wdt_hal.c in the vendor SDK treats them as alternatives;
 * nothing in hardware says they cannot run together):
 *
 *   AON_WDT  0x44000600  always-on domain, resets the chip outright.  The
 *                        lockup net and the reboot mechanism.
 *   NMI_WDT  0x44800000  peripheral domain, raises the NMI exception.  Set
 *                        to bite first so there is a stack dump and a
 *                        recorded reset reason before the AON block resets.
 *
 * Both use the same commit protocol: unlock (0x5A in the top byte) then
 * commit (0xA5), period in the low 16 bits.
 */

#define BK7258_WDT_PERIOD_RUN    0xfffc  /* Generous heartbeat period */
#define BK7258_WDT_PERIOD_BOOT   6       /* Immediate reset, bootloader value */

/* Period given to the AON block once the NMI stage has fired, sized to let
 * the panic dump finish over a 115200 console before the reset lands.  Only
 * used when CONFIG_BK7258_WDT_NMI is on; without it the AON block keeps the
 * behaviour it always had.
 */

#define BK7258_WDT_PERIOD_DUMP   5000

/* How often the SysTick handler services the dog, in ticks.  The arm
 * sequence crosses onto the slow AON bus, so feeding on every tick cost the
 * eye animation about a third of its frame budget; decimating by ten made
 * that back.  This is also the granularity of any deadline built on top of
 * the heartbeat, which is why the /dev/watchdog0 lower half reads it here
 * instead of assuming a number.
 */

#define BK7258_WDT_HEARTBEAT_TICKS 10

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_wdt_arm
 *
 * Description:
 *   Arm (or re-arm) both watchdogs with the given period.  Feeding is the
 *   same operation as arming.
 *
 ****************************************************************************/

void bk7258_wdt_arm(uint32_t period);

/****************************************************************************
 * Name: bk7258_wdt_reboot
 *
 * Description:
 *   Reset the chip through the watchdog.  Does not return.  On the way back
 *   up the Beken bootloader reopens its UART download window, so a reboot
 *   is also how the board is made flashable without anyone pressing RST.
 *
 ****************************************************************************/

void bk7258_wdt_reboot(void) noreturn_function;

/****************************************************************************
 * Name: bk7258_wdt_service
 *
 * Description:
 *   The heartbeat, called from the SysTick handler.  Feeds the dog while
 *   the system is healthy, and stops feeding -- in fact arms the shortest
 *   period, so the reset is immediate rather than up to a hardware period
 *   away -- once a deadline handed over by the /dev/watchdog0 driver has
 *   passed.
 *
 *   With no deadline set this is exactly the original unconditional feed,
 *   which is the safety net that recovers the board from a wedged handler
 *   or a spin with interrupts masked.  That net is never removed; the
 *   userspace watchdog only adds a second, earlier reason to bite.
 *
 ****************************************************************************/

void bk7258_wdt_service(void);

/****************************************************************************
 * Name: bk7258_wdt_deadline_set
 *
 * Description:
 *   Hand the heartbeat a deadline, in system ticks, past which it must stop
 *   feeding.  Passing zero releases the claim and restores the plain
 *   always-feed behaviour.  Safe to call from any context.
 *
 ****************************************************************************/

void bk7258_wdt_deadline_set(clock_t deadline);

/****************************************************************************
 * Name: bk7258_wdt_lowerhalf_initialize
 *
 * Description:
 *   Register the AON watchdog with the NuttX watchdog upper half, normally
 *   as /dev/watchdog0.
 *
 * Returned Value:
 *   Zero on success; a negated errno value on failure.
 *
 ****************************************************************************/

#ifdef CONFIG_BK7258_WDT
int bk7258_wdt_lowerhalf_initialize(FAR const char *devpath);
#endif

/****************************************************************************
 * Name: bk7258_wdt_nmi_initialize
 *
 * Description:
 *   Bring up the NMI watchdog stage: clock the peripheral block, attach the
 *   NMI exception, and start feeding it alongside the always-on block.
 *
 *   Call from board_late_initialize() and no earlier.  The block sits in the
 *   peripheral domain, and an APB access to it before its clock is running
 *   stalls the bus -- an early write to 0x44800010 bricked the board into
 *   total silence during bring-up (see the note in bk7258_wdt_arm()).  By
 *   late init the SYS clock-enable register has been reachable for a while.
 *
 *   Without CONFIG_BK7258_WDT_NMI this is a no-op and the always-on
 *   watchdog behaves exactly as it did before the stage existed.
 *
 ****************************************************************************/

void bk7258_wdt_nmi_initialize(void);

/****************************************************************************
 * Name: bk7258_wdt_capture_set
 *
 * Description:
 *   Install a callback to run from the NMI stage instead of the panic, and
 *   return whatever was installed before.  Passing NULL restores the default
 *   behaviour, which is to record the bite and panic.
 *
 *   This is what makes WDIOC_CAPTURE implementable: the always-on watchdog
 *   resets the SoC with no warning, so before the NMI stage existed there
 *   was no moment at which a callback could run.  With a handler installed
 *   the bite becomes a notification -- the handler runs, both watchdogs are
 *   fed, and the system carries on.
 *
 *   Runs in NMI context, which is outside every lock the scheduler owns.
 *   Keep the handler short.
 *
 ****************************************************************************/

xcpt_t bk7258_wdt_capture_set(xcpt_t handler);

#endif /* __BOARD_CONTEST_BOARD_CHIP_BK7258_WDT_H */
