/****************************************************************************
 * board/contest_board/chip/bk7258_timer.c
 *
 * Oneshot timer for the BK7258, built on TIMER group 0.
 *
 * The SoC has two timer groups, 0x44810000 and 0x45800000, three channels
 * each behind a single interrupt line per group (3 and 13).  Every register
 * fact below was taken from the Beken SDK rather than inferred:
 * middleware/soc/bk7258/soc/timer_struct.h for the layout,
 * middleware/soc/bk7258/hal/timer_ll.h for the access rules, and
 * middleware/soc/common/hal/timer_hal.c for the arithmetic.  Three of those
 * rules are not guessable and all fail silently if missed:
 *
 *   - The counter clock does not run until global_ctrl's soft reset is
 *     written, and clocking the block from the system controller is not
 *     enough on its own.  See bk7258_timer_oneshot_initialize().
 *
 *   - There is no interrupt enable.  The bit the SDK calls timerN_int_en is
 *     the pending status, it reads back as status, and it is cleared by
 *     writing one to it.  A channel that runs, interrupts.
 *
 *   - Clearing that status takes several 26MHz cycles to cross out of the
 *     timer clock domain, so a read straight after the write still returns
 *     one.  The SDK spins until it reads back clear and so do we; without
 *     the spin the handler re-enters immediately.
 *
 * This implements the clkcnt half of the oneshot interface (ONESHOT_COUNT),
 * which the Kconfig entry selects.  That half deals only in raw counts and
 * leaves every conversion to the upper half, so there is no timespec
 * arithmetic here to get wrong.
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

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/spinlock.h>
#include <nuttx/timers/oneshot.h>

#include "arm_internal.h"

#include "bk7258_memorymap.h"
#include "bk7258_timer.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* TIMER group 0.  Group 1 is the same block 0xff0000 higher up; only group
 * 0 is used here, so the group index never appears in an address.
 */

#define BK7258_TIMER_BASE           BK7258_TIMER0_BASE

#define BK7258_TIMER_GLOBAL_CTRL    (BK7258_TIMER_BASE + 0x08)
#define BK7258_TIMER_SOFT_RESET     (1u << 0)

#define BK7258_TIMER_CNT(n)         (BK7258_TIMER_BASE + 0x10 + ((n) << 2))
#define BK7258_TIMER_CTRL           (BK7258_TIMER_BASE + 0x1c)
#define BK7258_TIMER_READ_CTRL      (BK7258_TIMER_BASE + 0x20)
#define BK7258_TIMER_READ_VALUE     (BK7258_TIMER_BASE + 0x24)

#define TIMER_CTRL_EN(n)            (1u << (n))
#define TIMER_CTRL_DIV_SHIFT        (3)
#define TIMER_CTRL_DIV_MASK         (0xfu << TIMER_CTRL_DIV_SHIFT)
#define TIMER_CTRL_INT(n)           (1u << (7 + (n)))

/* Bits 0..6 -- the three channel enables and the divider -- are ordinary
 * state that a read-modify-write has to preserve.  Bits 7..9 are the
 * write-one-to-clear status, so they must be masked out of anything written
 * back or we would silently acknowledge a channel we were not asked about.
 * Every write to CTRL in this file goes through this mask.
 */

#define TIMER_CTRL_KEEP             (0x7fu)

#define TIMER_READ_CTRL_TRIGGER     (1u << 0)
#define TIMER_READ_CTRL_IDX_SHIFT   (2)
#define TIMER_READ_CTRL_IDX_MASK    (0x3u << TIMER_READ_CTRL_IDX_SHIFT)

/* System control: one gate bit and one clock-source bit.  The gate bit
 * index is the CLK_PWR_ID_* enumerator value from the SDK, which maps
 * straight onto the bit position -- CLK_PWR_ID_WDG_CPU is 31 and the NMI
 * watchdog in bk7258_wdt.c already drives bit 31 of this same register.
 */

#define BK7258_SYS_CLK_ENABLE       (BK7258_SYS_BASE + (0xc << 2))
#define BK7258_SYS_CLK_EN_TIMER0    (1u << 4)
#define BK7258_SYS_CLK_SEL          (BK7258_SYS_BASE + (0x8 << 2))
#define BK7258_SYS_CLK_SEL_TIM0     (1u << 20)

/* Channel roles.  See bk7258_timer_oneshot_initialize() for why two. */

#define TIMER_CHAN_ONESHOT          (0)
#define TIMER_CHAN_FREERUN          (1)

/* The 26MHz crystal with the divider left at zero, which the SDK documents
 * as divide-by-one: timer_hal_init_timer() writes a divider of 0 and then
 * computes end counts with a divisor of 1.  So a count is 1/26 of a
 * microsecond and a 32-bit channel spans a little over 165 seconds.
 */

#define BK7258_TIMER_FREQ           (26000000u)
#define BK7258_TIMER_MAX_COUNT      (0xffffffffu)

/* Writing the end count and setting the enable are separate stores a few
 * bus cycles apart, so an end count of one or two can already be behind the
 * counter by the time the channel starts, and the shot would then take a
 * full 165-second lap to arrive.  Round very short requests up instead; a
 * couple of microseconds is invisible next to the call that asked for them.
 */

#define BK7258_TIMER_MIN_COUNT      (64u)

/* Both hardware handshakes cross into the 26MHz domain and settle in a
 * handful of cycles.  Bound the spins anyway: this code runs in interrupt
 * context, and a gated clock must not be able to hang the system.
 */

#define BK7258_TIMER_SPINS          (1000)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct bk7258_oneshot_s
{
  struct oneshot_lowerhalf_s lh;      /* Must be first: we cast between them */
  uint32_t                   wraps;   /* High word of the free-running count */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static clkcnt_t bk7258_oneshot_current(FAR struct oneshot_lowerhalf_s *lower);
static void bk7258_oneshot_start(FAR struct oneshot_lowerhalf_s *lower,
                                 clkcnt_t delay);
static void bk7258_oneshot_cancel(FAR struct oneshot_lowerhalf_s *lower);
static clkcnt_t bk7258_oneshot_max_delay(
                                 FAR struct oneshot_lowerhalf_s *lower);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* start_absolute is deliberately absent.  The channel counts up from zero
 * against an end count, so it has no notion of an absolute instant on the
 * free-running timeline; the generic path in oneshot_start_absolute()
 * subtracts current() with interrupts masked and arms a relative delay,
 * which is exactly what this hardware can do.
 */

static const struct oneshot_operations_s g_bk7258_oneshot_ops =
{
  .current   = bk7258_oneshot_current,
  .start     = bk7258_oneshot_start,
  .cancel    = bk7258_oneshot_cancel,
  .max_delay = bk7258_oneshot_max_delay,
};

static struct bk7258_oneshot_s g_bk7258_oneshot =
{
  .lh =
    {
      .ops = &g_bk7258_oneshot_ops,
    },
};

static bool g_bk7258_oneshot_ready;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_timer_modctrl
 *
 * Description:
 *   Read-modify-write the control register without disturbing any channel's
 *   pending status.  See TIMER_CTRL_KEEP.
 *
 ****************************************************************************/

static void bk7258_timer_modctrl(uint32_t clr, uint32_t set)
{
  uint32_t regval = getreg32(BK7258_TIMER_CTRL) & TIMER_CTRL_KEEP;

  regval &= ~clr;
  regval |= set;
  putreg32(regval, BK7258_TIMER_CTRL);
}

/****************************************************************************
 * Name: bk7258_timer_clrpend
 *
 * Description:
 *   Acknowledge one channel's interrupt.  The status bit lives in the timer
 *   clock domain, so the write takes a few 26MHz cycles to land and a read
 *   issued before then still returns one.  Spin until it reads back clear,
 *   which is what the vendor HAL does; skipping this re-enters the handler.
 *
 ****************************************************************************/

static void bk7258_timer_clrpend(int chan)
{
  uint32_t bit = TIMER_CTRL_INT(chan);
  int i;

  for (i = 0; i < BK7258_TIMER_SPINS; i++)
    {
      bk7258_timer_modctrl(0, bit);

      if ((getreg32(BK7258_TIMER_CTRL) & bit) == 0)
        {
          return;
        }
    }
}

/****************************************************************************
 * Name: bk7258_timer_count
 *
 * Description:
 *   Sample one channel's live count.  The value is latched on request:
 *   select the channel, set the trigger, and wait for the hardware to clear
 *   the trigger before reading the holding register.
 *
 *   The counter runs up from zero towards the end count, so this is elapsed
 *   time within the current period rather than time remaining.
 *
 ****************************************************************************/

static uint32_t bk7258_timer_count(int chan)
{
  uint32_t regval;
  int i;

  regval  = getreg32(BK7258_TIMER_READ_CTRL) & ~TIMER_READ_CTRL_IDX_MASK;
  regval |= (uint32_t)chan << TIMER_READ_CTRL_IDX_SHIFT;
  putreg32(regval, BK7258_TIMER_READ_CTRL);
  putreg32(regval | TIMER_READ_CTRL_TRIGGER, BK7258_TIMER_READ_CTRL);

  for (i = 0; i < BK7258_TIMER_SPINS; i++)
    {
      if ((getreg32(BK7258_TIMER_READ_CTRL) & TIMER_READ_CTRL_TRIGGER) == 0)
        {
          break;
        }
    }

  return getreg32(BK7258_TIMER_READ_VALUE);
}

/****************************************************************************
 * Name: bk7258_timer_interrupt
 *
 * Description:
 *   Both channels share this line, so both statuses have to be examined on
 *   every entry.  Sample the register once: acknowledging one channel is a
 *   read-modify-write of the same register the other channel's status lives
 *   in, and re-reading afterwards would race with it.
 *
 ****************************************************************************/

static int bk7258_timer_interrupt(int irq, FAR void *context, FAR void *arg)
{
  FAR struct bk7258_oneshot_s *priv = (FAR struct bk7258_oneshot_s *)arg;
  uint32_t status = getreg32(BK7258_TIMER_CTRL);

  UNUSED(irq);
  UNUSED(context);

  if ((status & TIMER_CTRL_INT(TIMER_CHAN_FREERUN)) != 0)
    {
      bk7258_timer_clrpend(TIMER_CHAN_FREERUN);
      priv->wraps++;
    }

  if ((status & TIMER_CTRL_INT(TIMER_CHAN_ONESHOT)) != 0)
    {
      bk7258_timer_clrpend(TIMER_CHAN_ONESHOT);
      bk7258_timer_modctrl(TIMER_CTRL_EN(TIMER_CHAN_ONESHOT), 0);

      /* Last, and only once the channel is quiet: the callback runs the
       * upper half, which is entitled to arm the next shot from in here.
       */

      if (priv->lh.callback != NULL)
        {
          priv->lh.callback(&priv->lh, priv->lh.arg);
        }
    }

  return OK;
}

/****************************************************************************
 * Name: bk7258_oneshot_current
 *
 * Description:
 *   Read the free-running channel as a 64-bit count.
 *
 *   The wrap that advances the high word is delivered by interrupt, so
 *   between the wrap itself and the handler running, the two halves
 *   disagree.  That window is not hypothetical -- callers reach here with
 *   interrupts masked (oneshot_start_absolute does exactly that) and from
 *   inside other handlers.  The pending bit is the only thing that knows,
 *   so consult it, and if a wrap is outstanding re-sample the low half so
 *   the pair is consistent.
 *
 ****************************************************************************/

static clkcnt_t bk7258_oneshot_current(FAR struct oneshot_lowerhalf_s *lower)
{
  FAR struct bk7258_oneshot_s *priv = (FAR struct bk7258_oneshot_s *)lower;
  irqstate_t flags;
  uint64_t   wraps;
  uint32_t   count;

  flags = enter_critical_section();

  count = bk7258_timer_count(TIMER_CHAN_FREERUN);
  wraps = priv->wraps;

  if ((getreg32(BK7258_TIMER_CTRL) &
       TIMER_CTRL_INT(TIMER_CHAN_FREERUN)) != 0)
    {
      count = bk7258_timer_count(TIMER_CHAN_FREERUN);
      wraps++;
    }

  leave_critical_section(flags);

  return (clkcnt_t)((wraps << 32) + count);
}

/****************************************************************************
 * Name: bk7258_oneshot_start
 ****************************************************************************/

static void bk7258_oneshot_start(FAR struct oneshot_lowerhalf_s *lower,
                                 clkcnt_t delay)
{
  irqstate_t flags;

  UNUSED(lower);

  if (delay > BK7258_TIMER_MAX_COUNT)
    {
      delay = BK7258_TIMER_MAX_COUNT;
    }
  else if (delay < BK7258_TIMER_MIN_COUNT)
    {
      delay = BK7258_TIMER_MIN_COUNT;
    }

  flags = enter_critical_section();

  /* Stop before reloading.  The end count is compared continuously, so
   * lowering it under a running counter that has already passed the new
   * value costs a full lap.
   */

  bk7258_timer_modctrl(TIMER_CTRL_EN(TIMER_CHAN_ONESHOT), 0);
  putreg32((uint32_t)delay, BK7258_TIMER_CNT(TIMER_CHAN_ONESHOT));
  bk7258_timer_clrpend(TIMER_CHAN_ONESHOT);
  bk7258_timer_modctrl(0, TIMER_CTRL_EN(TIMER_CHAN_ONESHOT));

  leave_critical_section(flags);
}

/****************************************************************************
 * Name: bk7258_oneshot_cancel
 *
 * Description:
 *   Stop the channel and drop any bite that had already landed, so a shot
 *   cancelled a cycle too late cannot still call back.  Idempotent: the
 *   upper half cancels freely whether or not anything is armed.
 *
 ****************************************************************************/

static void bk7258_oneshot_cancel(FAR struct oneshot_lowerhalf_s *lower)
{
  irqstate_t flags;

  UNUSED(lower);

  flags = enter_critical_section();

  bk7258_timer_modctrl(TIMER_CTRL_EN(TIMER_CHAN_ONESHOT), 0);
  bk7258_timer_clrpend(TIMER_CHAN_ONESHOT);

  leave_critical_section(flags);
}

/****************************************************************************
 * Name: bk7258_oneshot_max_delay
 ****************************************************************************/

static clkcnt_t bk7258_oneshot_max_delay(
                                  FAR struct oneshot_lowerhalf_s *lower)
{
  UNUSED(lower);

  return (clkcnt_t)BK7258_TIMER_MAX_COUNT;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_timer_oneshot_initialize
 *
 * Description:
 *   See bk7258_timer.h.
 *
 ****************************************************************************/

FAR struct oneshot_lowerhalf_s *bk7258_timer_oneshot_initialize(void)
{
  FAR struct bk7258_oneshot_s *priv = &g_bk7258_oneshot;
  uint32_t regval;
  int ret;

  if (g_bk7258_oneshot_ready)
    {
      return &priv->lh;
    }

  /* Feed the block, then pick the 26MHz crystal over the 32kHz slow clock.
   * The order matters: the source multiplexer sits inside the gated domain.
   */

  regval = getreg32(BK7258_SYS_CLK_ENABLE);
  putreg32(regval | BK7258_SYS_CLK_EN_TIMER0, BK7258_SYS_CLK_ENABLE);

  regval = getreg32(BK7258_SYS_CLK_SEL);
  putreg32(regval | BK7258_SYS_CLK_SEL_TIM0, BK7258_SYS_CLK_SEL);

  /* Pulse the block's soft reset.  This is not hygiene, it is what starts
   * the counter clock: until it is written, a channel with its enable set
   * and its end count loaded reads zero forever and the latched-read
   * handshake never completes, even though the bus side works fine -- the
   * device ID reads back "TIMR" and every register holds what you store in
   * it.  Measured with src/timertest.c: without this write the free-running
   * channel advances 0 counts in 100ms, with it, 2.78 million.
   *
   * The bit does not self-clear, and staying set is the working state
   * rather than a held reset; the vendor HAL writes it once at the end of
   * timer_ll_init() and never clears it either.  Do it before touching
   * anything else, because the write-one-to-clear status bits below live
   * in the clock domain this starts.
   */

  regval = getreg32(BK7258_TIMER_GLOBAL_CTRL);
  putreg32(regval | BK7258_TIMER_SOFT_RESET, BK7258_TIMER_GLOBAL_CTRL);

  /* All channels stopped and the divider at zero, no half-finished latched
   * read, then acknowledge whatever the boot loader left pending -- writing
   * zeroes above did not, and could not, clear a write-one-to-clear bit.
   */

  putreg32(0, BK7258_TIMER_CTRL);
  putreg32(0, BK7258_TIMER_READ_CTRL);
  bk7258_timer_clrpend(TIMER_CHAN_ONESHOT);
  bk7258_timer_clrpend(TIMER_CHAN_FREERUN);

  priv->wraps = 0;

  /* Hand the upper half the tick rate it converts against. */

  oneshot_count_init(&priv->lh, BK7258_TIMER_FREQ);

  ret = irq_attach(BK7258_IRQ_TIMER0, bk7258_timer_interrupt, priv);
  if (ret < 0)
    {
      return NULL;
    }

  up_enable_irq(BK7258_IRQ_TIMER0);

  /* Start the timebase at full scale.  It runs from here on, wrapping every
   * 165 seconds into the high word the handler keeps.
   */

  putreg32(BK7258_TIMER_MAX_COUNT, BK7258_TIMER_CNT(TIMER_CHAN_FREERUN));
  bk7258_timer_modctrl(0, TIMER_CTRL_EN(TIMER_CHAN_FREERUN));

  g_bk7258_oneshot_ready = true;
  return &priv->lh;
}

/****************************************************************************
 * Name: bk7258_timer_oneshot_register
 *
 * Description:
 *   See bk7258_timer.h.
 *
 ****************************************************************************/

int bk7258_timer_oneshot_register(FAR const char *devname)
{
  FAR struct oneshot_lowerhalf_s *lower = bk7258_timer_oneshot_initialize();

  if (lower == NULL)
    {
      return -ENODEV;
    }

  return oneshot_register(devname, lower);
}
