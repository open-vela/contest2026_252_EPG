/****************************************************************************
 * board/contest_board/src/timertest.c
 *
 * TIMER group 0 probe -- is the block actually running?
 *
 * The oneshot driver in chip/bk7258_timer.c can fail in three places that
 * all look identical from userspace (an ioctl that never returns): the
 * clock never reached the block, the channel never counted, or the counter
 * expired and the interrupt never arrived.  Telling them apart from the
 * outside is what this is for.  The build is flat, so a task can read the
 * peripheral window directly and see exactly what the driver sees.
 *
 * SPDX-License-Identifier: Apache-2.0
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
#include <stdio.h>
#include <unistd.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define TIMER_BASE      0x44810000ul
#define TIMER1_BASE     0x45800000ul
#define TIMER_GLOBAL    (TIMER_BASE + 0x08)
#define TIMER_CNT(n)    (TIMER_BASE + 0x10 + ((n) << 2))
#define TIMER_CTRL      (TIMER_BASE + 0x1c)
#define TIMER_RDCTRL    (TIMER_BASE + 0x20)
#define TIMER_RDVAL     (TIMER_BASE + 0x24)

#define SYS_BASE        0x44010000ul
#define SYS_CLK_SEL     (SYS_BASE + (0x8 << 2))
#define SYS_CLK_EN      (SYS_BASE + (0xc << 2))
#define SYS_CPU0_INT_EN (SYS_BASE + (0x20 << 2))

#define REG(a)          (*(volatile uint32_t *)(a))

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* Same latched read the driver uses: pick the channel, set the trigger,
 * wait for the hardware to drop it, then take the holding register.
 */

static uint32_t timer_count(int chan)
{
  uint32_t regval;
  int i;

  regval  = REG(TIMER_RDCTRL) & ~(0x3u << 2);
  regval |= (uint32_t)chan << 2;
  REG(TIMER_RDCTRL) = regval;
  REG(TIMER_RDCTRL) = regval | 1u;

  for (i = 0; i < 1000; i++)
    {
      if ((REG(TIMER_RDCTRL) & 1u) == 0)
        {
          break;
        }
    }

  return REG(TIMER_RDVAL);
}

/* The same read, but reporting how it went rather than just its result.  A
 * silent timeout here is indistinguishable from a stopped counter -- both
 * hand back zero -- which is the blind spot this exists to remove.
 */

static uint32_t timer_count_verbose(int chan)
{
  uint32_t regval;
  uint32_t after_idx;
  uint32_t after_trig;
  int i;

  regval  = REG(TIMER_RDCTRL) & ~(0x3u << 2);
  regval |= (uint32_t)chan << 2;
  REG(TIMER_RDCTRL) = regval;
  after_idx = REG(TIMER_RDCTRL);
  REG(TIMER_RDCTRL) = regval | 1u;
  after_trig = REG(TIMER_RDCTRL);

  for (i = 0; i < 1000; i++)
    {
      if ((REG(TIMER_RDCTRL) & 1u) == 0)
        {
          break;
        }
    }

  printf("  rd chan%d: after_idx=%08lx after_trig=%08lx  %s (%d spins)"
         "  value=%08lx\n",
         chan, (unsigned long)after_idx, (unsigned long)after_trig,
         i < 1000 ? "handshake OK" : "HANDSHAKE TIMED OUT", i,
         (unsigned long)REG(TIMER_RDVAL));

  return REG(TIMER_RDVAL);
}

/* Latched read against an arbitrary group base. */

static uint32_t timer_count_at(unsigned long base, int chan, int *timedout)
{
  uint32_t regval;
  int i;

  regval  = REG(base + 0x20) & ~(0x3u << 2);
  regval |= (uint32_t)chan << 2;
  REG(base + 0x20) = regval;
  REG(base + 0x20) = regval | 1u;

  for (i = 0; i < 1000; i++)
    {
      if ((REG(base + 0x20) & 1u) == 0)
        {
          break;
        }
    }

  *timedout = (i >= 1000);
  return REG(base + 0x24);
}

/* Stop channel 1, reload it at full scale, start it, and report how far it
 * moved in 100ms.  26MHz gives ~2600000, the 32kHz source ~3277, a dead
 * clock exactly zero.
 */

static void arm_and_measure(unsigned long base, const char *label)
{
  uint32_t a;
  uint32_t b;
  int to1;
  int to2;

  REG(base + 0x1c) = REG(base + 0x1c) & 0x7fu & ~(1u << 1);
  REG(base + 0x14) = 0xffffffffu;
  REG(base + 0x1c) = (REG(base + 0x1c) & 0x7fu) | (1u << 1);

  a = timer_count_at(base, 1, &to1);
  usleep(100000);
  b = timer_count_at(base, 1, &to2);

  printf("  %-28s ctrl=%08lx  %10lu -> %10lu  delta %10ld%s\n",
         label, (unsigned long)REG(base + 0x1c),
         (unsigned long)a, (unsigned long)b, (long)(b - a),
         (to1 || to2) ? "   [read handshake timed out]" : "");
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  uint32_t ctrl;
  uint32_t a;
  uint32_t b;
  int i;

  printf("SYS  clk_en   %08lx  (bit4 timer0 = %s)\n",
         (unsigned long)REG(SYS_CLK_EN),
         (REG(SYS_CLK_EN) & (1u << 4)) ? "on" : "OFF");
  printf("SYS  clk_sel  %08lx  (bit20 tim0  = %s)\n",
         (unsigned long)REG(SYS_CLK_SEL),
         (REG(SYS_CLK_SEL) & (1u << 20)) ? "26M xtal" : "32K");
  printf("SYS  int_en0  %08lx  (bit3 irq3   = %s)\n",
         (unsigned long)REG(SYS_CPU0_INT_EN),
         (REG(SYS_CPU0_INT_EN) & (1u << 3)) ? "routed" : "NOT ROUTED");

  ctrl = REG(TIMER_CTRL);
  printf("TIMER dev_id  %08lx  ver %08lx  status %08lx\n",
         (unsigned long)REG(TIMER_BASE), (unsigned long)REG(TIMER_BASE + 4),
         (unsigned long)REG(TIMER_BASE + 0x0c));
  printf("TIMER global  %08lx\n", (unsigned long)REG(TIMER_GLOBAL));
  printf("TIMER ctrl    %08lx  en=%lu%lu%lu div=%lu int=%lu%lu%lu\n",
         (unsigned long)ctrl,
         (unsigned long)((ctrl >> 0) & 1), (unsigned long)((ctrl >> 1) & 1),
         (unsigned long)((ctrl >> 2) & 1), (unsigned long)((ctrl >> 3) & 0xf),
         (unsigned long)((ctrl >> 7) & 1), (unsigned long)((ctrl >> 8) & 1),
         (unsigned long)((ctrl >> 9) & 1));
  printf("TIMER end0    %08lx\n", (unsigned long)REG(TIMER_CNT(0)));
  printf("TIMER end1    %08lx\n", (unsigned long)REG(TIMER_CNT(1)));

  /* Does the free-running channel move?  Everything else is downstream of
   * this one answer.
   */

  printf("-- latched read, instrumented --\n");
  timer_count_verbose(1);
  usleep(100000);
  timer_count_verbose(1);

  for (i = 0; i < 3; i++)
    {
      a = timer_count(1);
      usleep(100000);
      b = timer_count(1);
      printf("chan1 %10lu -> %10lu   delta %10ld  (expect ~2600000)\n",
             (unsigned long)a, (unsigned long)b, (long)(b - a));
    }

  a = timer_count(0);
  printf("chan0 count   %10lu\n", (unsigned long)a);

  /* Four experiments, one flash.  Each re-arms channel 1 and measures; the
   * one that produces a nonzero delta names the missing step.
   */

  printf("-- E1 soft reset then re-arm --\n");
  REG(TIMER_GLOBAL) = REG(TIMER_GLOBAL) | 1u;
  printf("global after soft_reset write: %08lx\n",
         (unsigned long)REG(TIMER_GLOBAL));
  REG(TIMER_GLOBAL) = REG(TIMER_GLOBAL) | 2u;
  arm_and_measure(TIMER_BASE, "E1");

  printf("-- E2 clk_gate_bypass off --\n");
  REG(TIMER_GLOBAL) = REG(TIMER_GLOBAL) & ~2u;
  arm_and_measure(TIMER_BASE, "E2");
  REG(TIMER_GLOBAL) = REG(TIMER_GLOBAL) | 2u;

  printf("-- E3 source 32k instead of xtal --\n");
  REG(SYS_CLK_SEL) = REG(SYS_CLK_SEL) & ~(1u << 20);
  arm_and_measure(TIMER_BASE, "E3 (expect ~3277 if alive)");
  REG(SYS_CLK_SEL) = REG(SYS_CLK_SEL) | (1u << 20);

  printf("-- E4 timer group 1 at 0x45800000 --\n");
  REG(SYS_CLK_EN)  = REG(SYS_CLK_EN) | (1u << 13);
  REG(SYS_CLK_SEL) = REG(SYS_CLK_SEL) | (1u << 21);
  REG(TIMER1_BASE + 0x08) = REG(TIMER1_BASE + 0x08) | 2u;
  printf("group1 dev_id %08lx\n", (unsigned long)REG(TIMER1_BASE));
  arm_and_measure(TIMER1_BASE, "E4");

  return 0;
}
