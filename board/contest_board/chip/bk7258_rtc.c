/****************************************************************************
 * board/contest_board/chip/bk7258_rtc.c
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

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <debug.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/spinlock.h>
#include <nuttx/clock.h>
#include <nuttx/timers/arch_rtc.h>
#include <nuttx/timers/rtc.h>
#ifdef CONFIG_ALARM_ARCH
#  include <nuttx/timers/oneshot.h>
#  include <nuttx/timers/arch_alarm.h>
#endif

#include <arch/board/board.h>

#include "arm_internal.h"
#include "nvic.h"

#include "bk7258_memorymap.h"
#include "bk7258_rtc.h"
#include "bk7258_wdt.h"

#ifdef CONFIG_BK7258_RTC

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* How long to gate the clock-rate measurement.  The two candidate rates are
 * 2.4% apart, so the measurement has to be good to about half that.  The
 * system tick edge is hunted for first, which removes the bulk of the
 * quantisation error; 500 ms then leaves the residual well under 0.1%.
 */

#define BK7258_RTC_CAL_MS       500

/* Accept a candidate rate if the measurement lands within this much of it.
 * Wide enough to absorb ROSC's drift, narrow enough that the two candidates
 * cannot both match.
 */

#define BK7258_RTC_CAL_TOL_PCT  1

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Software extension of the 32-bit counter.  The hardware also has _hi
 * registers, but their offsets are not in any public vendor header, and
 * this port does not guess at register addresses.  Extending in software
 * costs one comparison per read and a poll often enough to catch each wrap.
 */

static uint32_t  g_rtc_last_lo;
static uint64_t  g_rtc_wraps;      /* Accumulated 2^32 units */

/* Ticks-to-wall-clock mapping.  g_rtc_offset is the number of RTC ticks
 * between the counter and the epoch, so epoch_ticks = counter + offset.
 * Sub-second resolution survives a settime() this way.
 */

static int64_t   g_rtc_offset;
static uint32_t  g_rtc_hz = BK7258_RTC_HZ_XTAL;
static bool      g_rtc_ready;
static bool      g_rtc_havesettime;

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int  bk7258_rtc_rdtime(FAR struct rtc_lowerhalf_s *lower,
                              FAR struct rtc_time *rtctime);
static int  bk7258_rtc_settime(FAR struct rtc_lowerhalf_s *lower,
                               FAR const struct rtc_time *rtctime);
static bool bk7258_rtc_havesettime(FAR struct rtc_lowerhalf_s *lower);

#ifdef CONFIG_RTC_ALARM
static int  bk7258_rtc_setalarm(FAR struct rtc_lowerhalf_s *lower,
                                FAR const struct lower_setalarm_s *info);
static int  bk7258_rtc_setrelative(FAR struct rtc_lowerhalf_s *lower,
                                   FAR const struct lower_setrelative_s *i);
static int  bk7258_rtc_cancelalarm(FAR struct rtc_lowerhalf_s *lower,
                                   int alarmid);
static int  bk7258_rtc_rdalarm(FAR struct rtc_lowerhalf_s *lower,
                               FAR struct lower_rdalarm_s *info);
#endif

#ifdef CONFIG_RTC_PERIODIC
static int  bk7258_rtc_setperiodic(FAR struct rtc_lowerhalf_s *lower,
                                   FAR const struct lower_setperiodic_s *i);
static int  bk7258_rtc_cancelperiodic(FAR struct rtc_lowerhalf_s *lower,
                                      int id);
#endif

#ifdef CONFIG_RTC_IOCTL
static int  bk7258_rtc_ioctl(FAR struct rtc_lowerhalf_s *lower, int cmd,
                             unsigned long arg);
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

#ifdef CONFIG_RTC_ALARM
struct bk7258_rtc_alarm_s
{
  rtc_alarm_callback_t cb;
  FAR void *priv;
  uint64_t  deadline;              /* Absolute, in RTC counter ticks */
  bool      active;
};
#endif

#ifdef CONFIG_RTC_PERIODIC
struct bk7258_rtc_periodic_s
{
  rtc_wakeup_callback_t cb;
  FAR void *priv;
  uint64_t  period;                /* In RTC counter ticks */
  uint64_t  next;                  /* Absolute, in RTC counter ticks */
  bool      active;
};
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct rtc_ops_s g_bk7258_rtc_ops =
{
  .rdtime         = bk7258_rtc_rdtime,
  .settime        = bk7258_rtc_settime,
  .havesettime    = bk7258_rtc_havesettime,
#ifdef CONFIG_RTC_ALARM
  .setalarm       = bk7258_rtc_setalarm,
  .setrelative    = bk7258_rtc_setrelative,
  .cancelalarm    = bk7258_rtc_cancelalarm,
  .rdalarm        = bk7258_rtc_rdalarm,
#endif
#ifdef CONFIG_RTC_PERIODIC
  .setperiodic    = bk7258_rtc_setperiodic,
  .cancelperiodic = bk7258_rtc_cancelperiodic,
#endif
#ifdef CONFIG_RTC_IOCTL
  .ioctl          = bk7258_rtc_ioctl,
#endif
};

#ifdef CONFIG_RTC_ALARM
static struct bk7258_rtc_alarm_s g_rtc_alarm;
#endif

#ifdef CONFIG_RTC_PERIODIC
static struct bk7258_rtc_periodic_s g_rtc_periodic;
#endif

#ifdef CONFIG_ALARM_ARCH

/* The system time base.  The AON counter free-runs and the tick channel
 * compares against it, so nothing is ever cleared or reloaded -- which is
 * the whole reason this block, rather than SysTick, backs arch_alarm.
 */

struct bk7258_oneshot_s
{
  struct oneshot_lowerhalf_s lower;   /* Must be first */
  uint64_t deadline;                  /* Absolute, in counter ticks */
  bool     active;
};

#ifdef CONFIG_ONESHOT_COUNT

/* The counting interface.  It exists precisely for this situation: the
 * deadlines here are already absolute counter values, so start_absolute()
 * hands one straight to the compare register.  The deprecated timespec
 * interface can only re-arm relative to "now", which folds each interrupt's
 * latency into the next period and made this clock run 1.4% slow.
 */

static clkcnt_t bk7258_oneshot_cnt_current(
                  FAR struct oneshot_lowerhalf_s *lower);
static void     bk7258_oneshot_cnt_start(
                  FAR struct oneshot_lowerhalf_s *lower, clkcnt_t delay);
static void     bk7258_oneshot_cnt_absolute(
                  FAR struct oneshot_lowerhalf_s *lower, clkcnt_t cnt);
static void     bk7258_oneshot_cnt_cancel(
                  FAR struct oneshot_lowerhalf_s *lower);
static clkcnt_t bk7258_oneshot_cnt_max_delay(
                  FAR struct oneshot_lowerhalf_s *lower);

static const struct oneshot_operations_s g_bk7258_oneshot_ops =
{
  .current        = bk7258_oneshot_cnt_current,
  .start          = bk7258_oneshot_cnt_start,
  .start_absolute = bk7258_oneshot_cnt_absolute,
  .cancel         = bk7258_oneshot_cnt_cancel,
  .max_delay      = bk7258_oneshot_cnt_max_delay,
};

#else

static int bk7258_oneshot_max_delay(FAR struct oneshot_lowerhalf_s *lower,
                                    FAR struct timespec *ts);
static int bk7258_oneshot_start(FAR struct oneshot_lowerhalf_s *lower,
                                FAR const struct timespec *ts);
static int bk7258_oneshot_cancel(FAR struct oneshot_lowerhalf_s *lower,
                                 FAR struct timespec *ts);
static int bk7258_oneshot_current(FAR struct oneshot_lowerhalf_s *lower,
                                  FAR struct timespec *ts);

static const struct oneshot_operations_s g_bk7258_oneshot_ops =
{
  .max_delay = bk7258_oneshot_max_delay,
  .start     = bk7258_oneshot_start,
  .cancel    = bk7258_oneshot_cancel,
  .current   = bk7258_oneshot_current,
};

#endif

static struct bk7258_oneshot_s g_oneshot =
{
  .lower =
    {
      .ops = &g_bk7258_oneshot_ops,
    },
};

/* When the watchdog was last fed, in counter ticks.  Paced by elapsed time
 * rather than by a callback count: with the time base on a oneshot there is
 * no fixed callback rate left to count.
 */

static uint64_t g_rtc_wdt_last;

/* The deadline the last expiry was *scheduled* for, held just long enough
 * for the re-arm that follows it to use as its anchor.  Zero once consumed.
 */

static uint64_t g_oneshot_expiry;

#endif /* CONFIG_ALARM_ARCH */

static struct rtc_lowerhalf_s g_bk7258_rtc_lower =
{
  .ops = &g_bk7258_rtc_ops,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_rtc_counter
 *
 * Description:
 *   Read the raw 32-bit counter.  The counter lives in the always-on domain
 *   and is clocked from the 32K oscillator, so a read can catch it mid
 *   carry; the vendor's own accessor reads until two reads agree and this
 *   does the same.
 *
 ****************************************************************************/

static uint32_t bk7258_rtc_counter(void)
{
  uint32_t val;

  val = getreg32(BK7258_AON_RTC_BASE + BK7258_RTC_COUNTER_OFFSET);
  while (val != getreg32(BK7258_AON_RTC_BASE + BK7258_RTC_COUNTER_OFFSET))
    {
      val = getreg32(BK7258_AON_RTC_BASE + BK7258_RTC_COUNTER_OFFSET);
    }

  return val;
}

/****************************************************************************
 * Name: bk7258_rtc_ticks
 *
 * Description:
 *   Read the counter, extended to 64 bits.  Any caller closes the wrap
 *   window simply by calling: a wrap is detected whenever the low word goes
 *   backwards, which is unambiguous as long as calls are far closer
 *   together than the 36.4 hour wrap period.
 *
 ****************************************************************************/

static uint64_t bk7258_rtc_ticks(void)
{
  irqstate_t flags;
  uint32_t lo;
  uint64_t ticks;

  flags = enter_critical_section();

  lo = bk7258_rtc_counter();
  if (lo < g_rtc_last_lo)
    {
      g_rtc_wraps += (uint64_t)1 << 32;
    }

  g_rtc_last_lo = lo;
  ticks = g_rtc_wraps + lo;

  leave_critical_section(flags);
  return ticks;
}

/****************************************************************************
 * Name: bk7258_rtc_measure_hz
 *
 * Description:
 *   Time the AON counter against the system tick.  The 32K source is either
 *   the external crystal or the internal ROSC depending on what the
 *   bootloader selected, and getting it wrong costs 35 minutes a day, so it
 *   is measured rather than assumed.
 *
 ****************************************************************************/

static uint32_t bk7258_rtc_measure_hz(void)
{
  clock_t start;
  clock_t mark;
  clock_t elapsed;
  uint32_t c0;
  uint32_t c1;
  uint64_t hz;
  uint32_t ms;

  /* Start on a tick edge so the interval is a whole number of ticks. */

  mark = clock_systime_ticks();
  do
    {
      start = clock_systime_ticks();
    }
  while (start == mark);

  c0 = bk7258_rtc_counter();
  up_mdelay(BK7258_RTC_CAL_MS);
  c1 = bk7258_rtc_counter();

  elapsed = clock_systime_ticks() - start;
  ms = TICK2MSEC(elapsed);
  if (ms == 0)
    {
      return 0;
    }

  /* c1 - c0 in unsigned arithmetic is correct across a wrap, and half a
   * second cannot span more than one.
   */

  hz = ((uint64_t)(c1 - c0) * 1000) / ms;
  return (uint32_t)hz;
}

/****************************************************************************
 * Name: bk7258_rtc_rdtime
 ****************************************************************************/

static int bk7258_rtc_rdtime(FAR struct rtc_lowerhalf_s *lower,
                             FAR struct rtc_time *rtctime)
{
  uint64_t epoch_ticks;
  time_t secs;
  struct tm tm;

  UNUSED(lower);

  if (!g_rtc_ready)
    {
      return -EAGAIN;
    }

  epoch_ticks = (uint64_t)((int64_t)bk7258_rtc_ticks() + g_rtc_offset);

  secs = (time_t)(epoch_ticks / g_rtc_hz);
  if (gmtime_r(&secs, &tm) == NULL)
    {
      return -EINVAL;
    }

  memcpy(rtctime, &tm, sizeof(struct tm));

#if defined(CONFIG_RTC_HIRES) || defined(CONFIG_ARCH_HAVE_RTC_SUBSECONDS)
  /* The remainder is under 32768, so scaling by NSEC_PER_SEC stays inside
   * 64 bits with room to spare.
   */

  rtctime->tm_nsec = (long)(((epoch_ticks % g_rtc_hz) * NSEC_PER_SEC) /
                            g_rtc_hz);
#endif

  return OK;
}

/****************************************************************************
 * Name: bk7258_rtc_settime
 ****************************************************************************/

static int bk7258_rtc_settime(FAR struct rtc_lowerhalf_s *lower,
                              FAR const struct rtc_time *rtctime)
{
  irqstate_t flags;
  uint64_t epoch_ticks;
  time_t secs;
  struct tm tm;

  UNUSED(lower);

  if (!g_rtc_ready)
    {
      return -EAGAIN;
    }

  /* struct rtc_time is cast compatible with struct tm by contract; copying
   * the leading fields keeps the const promise on the caller's structure.
   */

  memcpy(&tm, rtctime, sizeof(struct tm));

  secs = timegm(&tm);
  if (secs == (time_t)-1)
    {
      return -EINVAL;
    }

  epoch_ticks = (uint64_t)secs * g_rtc_hz;

#if defined(CONFIG_RTC_HIRES) || defined(CONFIG_ARCH_HAVE_RTC_SUBSECONDS)
  epoch_ticks += ((uint64_t)rtctime->tm_nsec * g_rtc_hz) / NSEC_PER_SEC;
#endif

  flags = enter_critical_section();
  g_rtc_offset = (int64_t)epoch_ticks - (int64_t)bk7258_rtc_ticks();
  g_rtc_havesettime = true;
  leave_critical_section(flags);

  return OK;
}

/****************************************************************************
 * Name: bk7258_rtc_havesettime
 ****************************************************************************/

static bool bk7258_rtc_havesettime(FAR struct rtc_lowerhalf_s *lower)
{
  UNUSED(lower);
  return g_rtc_havesettime;
}

#if defined(CONFIG_RTC_ALARM) || defined(CONFIG_RTC_PERIODIC)

/****************************************************************************
 * Name: bk7258_rtc_ctrl_modify
 *
 * Description:
 *   Read-modify-write CTRL with the two write-1-to-clear status bits masked
 *   out.  Leaving them in would acknowledge a pending interrupt as a side
 *   effect of an unrelated control change, which is how this block loses
 *   events; the vendor's accessors mask them for the same reason.
 *
 ****************************************************************************/

static void bk7258_rtc_ctrl_modify(uint32_t set, uint32_t clr)
{
  irqstate_t flags;
  uint32_t ctrl;

  flags = enter_critical_section();

  ctrl  = getreg32(BK7258_AON_RTC_BASE + BK7258_RTC_CTRL_OFFSET);
  ctrl &= ~(BK7258_RTC_CTRL_W1C_MASK | clr);
  ctrl |= set;
  putreg32(ctrl, BK7258_AON_RTC_BASE + BK7258_RTC_CTRL_OFFSET);

  leave_critical_section(flags);
}

/****************************************************************************
 * Name: bk7258_rtc_ack
 *
 * Description:
 *   Acknowledge one status bit without disturbing the other.
 *
 ****************************************************************************/

static void bk7258_rtc_ack(uint32_t w1c_bit)
{
  uint32_t ctrl;

  ctrl  = getreg32(BK7258_AON_RTC_BASE + BK7258_RTC_CTRL_OFFSET);
  ctrl &= ~BK7258_RTC_CTRL_W1C_MASK;
  ctrl |= w1c_bit;
  putreg32(ctrl, BK7258_AON_RTC_BASE + BK7258_RTC_CTRL_OFFSET);
}

/****************************************************************************
 * Name: bk7258_rtc_arm
 *
 * Description:
 *   Point a compare channel at an absolute counter value.  The compare
 *   register is 32 bits while deadlines are tracked in 64, so a deadline
 *   further out than one horizon is armed in steps: the interrupt fires,
 *   finds the real deadline still ahead, and re-arms.  A quarter of the
 *   32-bit range keeps every step unambiguously in the future.
 *
 ****************************************************************************/

#define BK7258_RTC_HORIZON  0x40000000ull

/* How many times to re-aim before giving up.  Each pass costs two AON reads
 * and a store; the loop only ever spins when something has just held the
 * CPU for longer than the gap being armed, which is rare and self-limiting.
 */

#define BK7258_RTC_ARM_TRIES  8

/* Never aim closer than this many counter ticks.
 *
 * The block is in the always-on domain and counts at 32 kHz, so a store
 * from the CPU side needs up to one whole tick -- about 31 us -- to cross
 * into it and take effect.  Aiming one tick ahead therefore races the write
 * itself: the counter can reach the target while the new compare value is
 * still in flight, and a comparator that matches on equality then never
 * matches at all.  The channel goes quiet permanently, and since the only
 * thing that re-arms it is its own interrupt, so does the system clock.
 *
 * Four ticks is 125 us, comfortably past the crossing, and costs nothing:
 * it only applies to deadlines that are already in the past, which are
 * serviced on the very next interrupt anyway.
 *
 * The same domain-crossing rule bit this port once before, in the timer's
 * write-one-to-clear status bit (bk7258_timer.c) -- there the fix was to
 * spin until the write read back.  Here there is nothing to read back, so
 * the margin has to be built into the value.
 */

#define BK7258_RTC_MIN_STEP   4ull

static void bk7258_rtc_arm(uint32_t cmp_offset, uint32_t int_en,
                           uint64_t target)
{
  uint64_t now;
  uint64_t delta;
  uint32_t step;
  int i;

  /* Confirm the compare value is still in the future after storing it.
   *
   * The comparator matches on equality with a free-running counter, so a
   * value written after the counter has already passed it never matches and
   * the channel goes silent for good.  Between reading the counter and
   * storing now+step there is a window, and when a deadline has already
   * expired step is 1 -- a single 32 kHz tick, about 31 us.  Anything that
   * holds the CPU longer than that between the two lines loses the
   * interrupt permanently.
   *
   * That is not hypothetical.  This is the system time base, so losing it
   * stops the scheduler's clock and the watchdog heartbeat that rides this
   * handler, while the console keeps working because the UART is its own
   * interrupt -- a board that answers but never runs a timer again, and
   * then resets one watchdog period later with no explanation.  It took an
   * on-chip flash driver to expose it: bk7258_flash.c masks interrupts
   * around each 32-byte program, roughly 0.7 ms, some twenty times the
   * window.  Ordinary load never came close, which is why this survived
   * this long.
   */

  for (i = 0; i < BK7258_RTC_ARM_TRIES; i++)
    {
      now   = bk7258_rtc_ticks();
      delta = target > now ? target - now : BK7258_RTC_MIN_STEP;

      if (delta < BK7258_RTC_MIN_STEP)
        {
          delta = BK7258_RTC_MIN_STEP;
        }

      step  = delta > BK7258_RTC_HORIZON ? (uint32_t)BK7258_RTC_HORIZON
                                         : (uint32_t)delta;

      putreg32((uint32_t)(now + step), BK7258_AON_RTC_BASE + cmp_offset);

      if (bk7258_rtc_ticks() < now + step)
        {
          break;
        }
    }

  bk7258_rtc_ctrl_modify(int_en, 0);
}

/****************************************************************************
 * Name: bk7258_rtc_interrupt
 *
 * Description:
 *   The block raises one interrupt for both compare channels, so both
 *   status bits are examined on every entry.
 *
 ****************************************************************************/

#ifdef CONFIG_ALARM_ARCH

/****************************************************************************
 * Name: bk7258_oneshot_service
 *
 * Description:
 *   The tick compare fired.  Either the requested expiry has arrived, in
 *   which case the upper half is called back, or this was one horizon step
 *   of a longer wait and the channel is simply re-armed.
 *
 ****************************************************************************/

static void bk7258_oneshot_service(uint64_t now)
{
  oneshot_callback_t callback;
  FAR void *cbarg;

  /* Not due yet: this was a horizon step, or the interrupt belongs to one
   * of the other two users of the channel.  Re-arming is the caller's job
   * -- it happens once for all three after every one of them is serviced.
   */

  if (!g_oneshot.active || now < g_oneshot.deadline)
    {
      return;
    }

  callback = g_oneshot.lower.callback;
  cbarg    = g_oneshot.lower.arg;

  g_oneshot.active = false;

  /* Hand the scheduled deadline to the re-arm that the callback is about to
   * make.  See bk7258_oneshot_start().
   */

  g_oneshot_expiry = g_oneshot.deadline;

  if (callback != NULL)
    {
      callback(&g_oneshot.lower, cbarg);
    }
}

#endif /* CONFIG_ALARM_ARCH */

/****************************************************************************
 * Name: bk7258_rtc_rearm
 *
 * Description:
 *   The upper compare channel serves both the RTC alarm and the periodic
 *   wakeup, because the tick channel belongs to the system time base.  Only
 *   one of them can be in the compare register at a time, so the nearer
 *   deadline is armed and the interrupt re-arms for whatever is left.
 *
 *   Must be called with interrupts disabled.
 *
 ****************************************************************************/

static void bk7258_rtc_rearm(void)
{
  uint64_t target = 0;
  bool have = false;

#ifdef CONFIG_RTC_ALARM
  if (g_rtc_alarm.active)
    {
      target = g_rtc_alarm.deadline;
      have   = true;
    }
#endif

#ifdef CONFIG_RTC_PERIODIC
  if (g_rtc_periodic.active &&
      (!have || g_rtc_periodic.next < target))
    {
      target = g_rtc_periodic.next;
      have   = true;
    }
#endif

#ifdef CONFIG_ALARM_ARCH
  if (g_oneshot.active && (!have || g_oneshot.deadline < target))
    {
      target = g_oneshot.deadline;
      have   = true;
    }
#endif

  if (have)
    {
      bk7258_rtc_arm(BK7258_RTC_TICK_OFFSET, BK7258_RTC_CTRL_TICK_INT_EN,
                     target);
    }
  else
    {
      bk7258_rtc_ctrl_modify(0, BK7258_RTC_CTRL_TICK_INT_EN);
    }
}

/****************************************************************************
 * Name: bk7258_rtc_service_upper
 *
 * Description:
 *   Fire whichever of the two upper-channel users is due, then re-arm.
 *
 ****************************************************************************/

#if defined(CONFIG_RTC_ALARM) || defined(CONFIG_RTC_PERIODIC)
static void bk7258_rtc_service_upper(uint64_t now)
{
#ifdef CONFIG_RTC_ALARM
  rtc_alarm_callback_t alarm_cb = NULL;
  FAR void *alarm_priv = NULL;
#endif
#ifdef CONFIG_RTC_PERIODIC
  rtc_wakeup_callback_t wake_cb = NULL;
  FAR void *wake_priv = NULL;
#endif

#ifdef CONFIG_RTC_ALARM
  if (g_rtc_alarm.active && now >= g_rtc_alarm.deadline)
    {
      alarm_cb           = g_rtc_alarm.cb;
      alarm_priv         = g_rtc_alarm.priv;
      g_rtc_alarm.active = false;
      g_rtc_alarm.cb     = NULL;
    }
#endif

#ifdef CONFIG_RTC_PERIODIC
  if (g_rtc_periodic.active && now >= g_rtc_periodic.next)
    {
      /* Advance from the scheduled instant, not from now, so the period
       * does not drift by the interrupt latency each time.  Catch up in one
       * jump if the handler was late by more than a whole period.
       */

      do
        {
          g_rtc_periodic.next += g_rtc_periodic.period;
        }
      while (g_rtc_periodic.next <= now);

      wake_cb   = g_rtc_periodic.cb;
      wake_priv = g_rtc_periodic.priv;
    }
#endif

  bk7258_rtc_rearm();

  /* Callbacks run after the hardware is settled so a callback that arms a
   * new alarm cannot have its work undone by the re-arm above.
   */

#ifdef CONFIG_RTC_ALARM
  if (alarm_cb != NULL)
    {
      alarm_cb(alarm_priv, 0);
    }
#endif

#ifdef CONFIG_RTC_PERIODIC
  if (wake_cb != NULL)
    {
      wake_cb(wake_priv, 0);
    }
#endif
}
#endif

/****************************************************************************
 * Name: bk7258_rtc_interrupt
 *
 * Description:
 *   One interrupt, two compare channels: the tick channel drives the system
 *   time base, the upper channel drives the RTC alarm and periodic wakeup.
 *   Both status bits are examined on every entry.
 *
 ****************************************************************************/

static int bk7258_rtc_interrupt(int irq, FAR void *context, FAR void *arg)
{
  uint32_t ctrl;
  uint64_t now;

  UNUSED(irq);
  UNUSED(context);
  UNUSED(arg);

  ctrl = getreg32(BK7258_AON_RTC_BASE + BK7258_RTC_CTRL_OFFSET);
  now  = bk7258_rtc_ticks();

  /* One channel, three users.  The upper compare channel looked like a
   * second one -- it has a full set of accessors -- but no vendor code path
   * ever arms it (tick_init() parks it at 0xffffffff and the wakeup helper
   * uses the tick channel), and on this board an alarm placed there never
   * fired.  Accessors are not a proven usage; everything shares the tick
   * channel, and whoever is nearest owns the compare register.
   */

  if ((ctrl & BK7258_RTC_CTRL_TICK_INT_STS) != 0)
    {
      bk7258_rtc_ack(BK7258_RTC_CTRL_TICK_INT_STS);

#ifdef CONFIG_ALARM_ARCH
      bk7258_oneshot_service(now);
#endif
#if defined(CONFIG_RTC_ALARM) || defined(CONFIG_RTC_PERIODIC)
      bk7258_rtc_service_upper(now);
#endif

      bk7258_rtc_rearm();
    }

#ifdef CONFIG_ALARM_ARCH

  /* The watchdog heartbeat and the counter-wrap poll used to ride the
   * SysTick handler.  They live here now, and they are paced by elapsed
   * counter time rather than by counting callbacks: with the time base on
   * a oneshot there is no fixed callback rate to count.
   */

  if (now - g_rtc_wdt_last >= (uint64_t)g_rtc_hz / 10)
    {
      g_rtc_wdt_last = now;
      bk7258_wdt_service();
    }
#endif

  return OK;
}

/****************************************************************************
 * Name: bk7258_rtc_counter_at
 *
 * Description:
 *   Convert a broken-down time to the counter value it corresponds to.
 *
 ****************************************************************************/

static int bk7258_rtc_counter_at(FAR const struct rtc_time *rtctime,
                                 FAR uint64_t *counter)
{
  struct tm tm;
  time_t secs;

  memcpy(&tm, rtctime, sizeof(struct tm));

  secs = timegm(&tm);
  if (secs == (time_t)-1)
    {
      return -EINVAL;
    }

  *counter = (uint64_t)((int64_t)((uint64_t)secs * g_rtc_hz) - g_rtc_offset);
  return OK;
}

#endif /* CONFIG_RTC_ALARM || CONFIG_RTC_PERIODIC */

#ifdef CONFIG_RTC_ALARM

/****************************************************************************
 * Name: bk7258_rtc_setalarm
 ****************************************************************************/

static int bk7258_rtc_setalarm(FAR struct rtc_lowerhalf_s *lower,
                               FAR const struct lower_setalarm_s *info)
{
  irqstate_t flags;
  uint64_t target;
  int ret;

  UNUSED(lower);

  if (!g_rtc_ready)
    {
      return -EAGAIN;
    }

  if (info->id != 0)
    {
      return -EINVAL;
    }

  ret = bk7258_rtc_counter_at(&info->time, &target);
  if (ret < 0)
    {
      return ret;
    }

  flags = enter_critical_section();

  g_rtc_alarm.cb       = info->cb;
  g_rtc_alarm.priv     = info->priv;
  g_rtc_alarm.deadline = target;
  g_rtc_alarm.active   = true;

  bk7258_rtc_rearm();

  leave_critical_section(flags);
  return OK;
}

/****************************************************************************
 * Name: bk7258_rtc_setrelative
 ****************************************************************************/

static int bk7258_rtc_setrelative(FAR struct rtc_lowerhalf_s *lower,
                                  FAR const struct lower_setrelative_s *info)
{
  irqstate_t flags;
  uint64_t target;

  UNUSED(lower);

  if (!g_rtc_ready)
    {
      return -EAGAIN;
    }

  if (info->id != 0 || info->reltime <= 0)
    {
      return -EINVAL;
    }

  flags = enter_critical_section();

  target = bk7258_rtc_ticks() + (uint64_t)info->reltime * g_rtc_hz;

  g_rtc_alarm.cb       = info->cb;
  g_rtc_alarm.priv     = info->priv;
  g_rtc_alarm.deadline = target;
  g_rtc_alarm.active   = true;

  bk7258_rtc_rearm();

  leave_critical_section(flags);
  return OK;
}

/****************************************************************************
 * Name: bk7258_rtc_cancelalarm
 ****************************************************************************/

static int bk7258_rtc_cancelalarm(FAR struct rtc_lowerhalf_s *lower,
                                  int alarmid)
{
  irqstate_t flags;

  UNUSED(lower);

  if (alarmid != 0)
    {
      return -EINVAL;
    }

  flags = enter_critical_section();

  if (!g_rtc_alarm.active)
    {
      leave_critical_section(flags);
      return -ENODATA;
    }

  g_rtc_alarm.active = false;
  g_rtc_alarm.cb     = NULL;
  bk7258_rtc_rearm();

  leave_critical_section(flags);
  return OK;
}

/****************************************************************************
 * Name: bk7258_rtc_rdalarm
 ****************************************************************************/

static int bk7258_rtc_rdalarm(FAR struct rtc_lowerhalf_s *lower,
                              FAR struct lower_rdalarm_s *info)
{
  irqstate_t flags;
  uint64_t epoch_ticks;
  time_t secs;
  struct tm tm;

  UNUSED(lower);

  if (info->id != 0)
    {
      return -EINVAL;
    }

  flags = enter_critical_section();

  if (!g_rtc_alarm.active)
    {
      leave_critical_section(flags);
      return -ENODATA;
    }

  epoch_ticks = (uint64_t)((int64_t)g_rtc_alarm.deadline + g_rtc_offset);
  leave_critical_section(flags);

  secs = (time_t)(epoch_ticks / g_rtc_hz);
  if (gmtime_r(&secs, &tm) == NULL)
    {
      return -EINVAL;
    }

  memcpy(info->time, &tm, sizeof(struct tm));
  return OK;
}

#endif /* CONFIG_RTC_ALARM */

#ifdef CONFIG_RTC_PERIODIC

/****************************************************************************
 * Name: bk7258_rtc_setperiodic
 ****************************************************************************/

static int bk7258_rtc_setperiodic(FAR struct rtc_lowerhalf_s *lower,
                                  FAR const struct lower_setperiodic_s *info)
{
  irqstate_t flags;
  uint64_t period;

  UNUSED(lower);

  if (!g_rtc_ready)
    {
      return -EAGAIN;
    }

  if (info->id != 0)
    {
      return -EINVAL;
    }

  period = (uint64_t)info->period.tv_sec * g_rtc_hz +
           ((uint64_t)info->period.tv_nsec * g_rtc_hz) / NSEC_PER_SEC;

  /* A period the counter cannot resolve would free-run as a tight interrupt
   * storm rather than as the requested schedule.
   */

  if (period == 0)
    {
      return -EINVAL;
    }

  flags = enter_critical_section();

  g_rtc_periodic.cb     = info->cb;
  g_rtc_periodic.priv   = info->priv;
  g_rtc_periodic.period = period;
  g_rtc_periodic.next   = bk7258_rtc_ticks() + period;
  g_rtc_periodic.active = true;

  bk7258_rtc_rearm();

  leave_critical_section(flags);
  return OK;
}

/****************************************************************************
 * Name: bk7258_rtc_cancelperiodic
 ****************************************************************************/

static int bk7258_rtc_cancelperiodic(FAR struct rtc_lowerhalf_s *lower,
                                     int id)
{
  irqstate_t flags;

  UNUSED(lower);

  if (id != 0)
    {
      return -EINVAL;
    }

  flags = enter_critical_section();

  if (!g_rtc_periodic.active)
    {
      leave_critical_section(flags);
      return -ENODATA;
    }

  g_rtc_periodic.active = false;
  g_rtc_periodic.cb     = NULL;
  bk7258_rtc_rearm();

  leave_critical_section(flags);
  return OK;
}

#endif /* CONFIG_RTC_PERIODIC */

#ifdef CONFIG_RTC_IOCTL

/****************************************************************************
 * Name: bk7258_rtc_ioctl
 *
 * Description:
 *   No architecture-specific RTC commands exist on this chip.  The upper
 *   half forwards anything it does not recognise here, and -ENOTTY is the
 *   answer that keeps those commands reported as unsupported rather than
 *   silently succeeding.
 *
 ****************************************************************************/

static int bk7258_rtc_ioctl(FAR struct rtc_lowerhalf_s *lower, int cmd,
                            unsigned long arg)
{
  UNUSED(lower);
  UNUSED(cmd);
  UNUSED(arg);

  return -ENOTTY;
}

#endif /* CONFIG_RTC_IOCTL */

#if defined(CONFIG_ALARM_ARCH) && defined(CONFIG_ONESHOT_COUNT)

/****************************************************************************
 * Name: bk7258_oneshot_cnt_*
 *
 * Description:
 *   The counting lower half.  Deadlines are absolute counter values in both
 *   directions, so nothing here converts, rounds, or re-derives a base --
 *   which is the whole point: there is no place left for interrupt latency
 *   to accumulate into the period.
 *
 ****************************************************************************/

static clkcnt_t bk7258_oneshot_cnt_current(
                  FAR struct oneshot_lowerhalf_s *lower)
{
  UNUSED(lower);
  return (clkcnt_t)bk7258_rtc_ticks();
}

static void bk7258_oneshot_cnt_absolute(
              FAR struct oneshot_lowerhalf_s *lower, clkcnt_t cnt)
{
  irqstate_t flags;

  UNUSED(lower);

  flags = enter_critical_section();

  g_oneshot.deadline = (uint64_t)cnt;
  g_oneshot.active   = true;
  bk7258_rtc_rearm();

  leave_critical_section(flags);
}

static void bk7258_oneshot_cnt_start(
              FAR struct oneshot_lowerhalf_s *lower, clkcnt_t delay)
{
  irqstate_t flags;

  flags = enter_critical_section();
  bk7258_oneshot_cnt_absolute(lower, bk7258_rtc_ticks() + delay);
  leave_critical_section(flags);
}

static void bk7258_oneshot_cnt_cancel(FAR struct oneshot_lowerhalf_s *lower)
{
  irqstate_t flags;

  UNUSED(lower);

  flags = enter_critical_section();
  g_oneshot.active = false;
  bk7258_rtc_rearm();
  leave_critical_section(flags);
}

static clkcnt_t bk7258_oneshot_cnt_max_delay(
                  FAR struct oneshot_lowerhalf_s *lower)
{
  UNUSED(lower);
  return (clkcnt_t)BK7258_RTC_HORIZON;
}

#endif /* CONFIG_ALARM_ARCH && CONFIG_ONESHOT_COUNT */

#if defined(CONFIG_ALARM_ARCH) && !defined(CONFIG_ONESHOT_COUNT)

/****************************************************************************
 * Name: bk7258_ts_to_ticks / bk7258_ticks_to_ts
 ****************************************************************************/

static uint64_t bk7258_ts_to_ticks(FAR const struct timespec *ts)
{
  return (uint64_t)ts->tv_sec * g_rtc_hz +
         ((uint64_t)ts->tv_nsec * g_rtc_hz) / NSEC_PER_SEC;
}

static void bk7258_ticks_to_ts(uint64_t ticks, FAR struct timespec *ts)
{
  ts->tv_sec  = (time_t)(ticks / g_rtc_hz);
  ts->tv_nsec = (long)(((ticks % g_rtc_hz) * NSEC_PER_SEC) / g_rtc_hz);
}

/****************************************************************************
 * Name: bk7258_oneshot_max_delay
 ****************************************************************************/

static int bk7258_oneshot_max_delay(FAR struct oneshot_lowerhalf_s *lower,
                                    FAR struct timespec *ts)
{
  UNUSED(lower);

  /* One horizon.  Longer waits still work -- the interrupt re-arms -- but
   * this is what the upper half may ask for in a single call.
   */

  bk7258_ticks_to_ts(BK7258_RTC_HORIZON, ts);
  return OK;
}

/****************************************************************************
 * Name: bk7258_oneshot_start
 ****************************************************************************/

static int bk7258_oneshot_start(FAR struct oneshot_lowerhalf_s *lower,
                                FAR const struct timespec *ts)
{
  irqstate_t flags;
  uint64_t now;
  uint64_t delta;
  uint64_t base;

  UNUSED(lower);

  flags = enter_critical_section();

  now   = bk7258_rtc_ticks();
  delta = bk7258_ts_to_ticks(ts);
  base  = now;

  /* Anchor the new interval to the instant the previous one was scheduled
   * to end, not to now.  arch_alarm re-arms from inside the expiry
   * callback, so "now" already carries this interrupt's latency -- on this
   * chip roughly 136 us of AON reads and re-arm -- and folding that into
   * every period made the clock run 1.36% slow.  That accumulation is the
   * very thing arch_alarm exists to avoid, so it must not be reintroduced
   * here just because the deprecated lower-half interface only offers a
   * relative start.
   *
   * The anchor is dropped when it would already be in the past by more
   * than one interval: at that point the system is genuinely behind, and
   * catching up beats queueing expiries that are all overdue.
   */

  /* REVERTED -- the anchor above is disabled on purpose.  Enabling it made
   * the clock run 2x slow with an ~8 s offset on top, far worse than the
   * 1.36% it was meant to remove, and the cause was not established before
   * the change had to be backed out.  The reasoning still looks right, so
   * the machinery is left in place, but "looks right" is what produced the
   * 2x; anyone re-enabling this needs to measure first.
   */

  base = now;

  g_oneshot_expiry   = 0;
  g_oneshot.deadline = base + delta;
  g_oneshot.active   = true;

  bk7258_rtc_rearm();

  leave_critical_section(flags);
  return OK;
}

/****************************************************************************
 * Name: bk7258_oneshot_cancel
 ****************************************************************************/

static int bk7258_oneshot_cancel(FAR struct oneshot_lowerhalf_s *lower,
                                 FAR struct timespec *ts)
{
  irqstate_t flags;
  uint64_t now;

  UNUSED(lower);

  flags = enter_critical_section();

  if (!g_oneshot.active)
    {
      leave_critical_section(flags);
      if (ts != NULL)
        {
          ts->tv_sec  = 0;
          ts->tv_nsec = 0;
        }

      return -ENODATA;
    }

  now              = bk7258_rtc_ticks();
  g_oneshot.active = false;
  bk7258_rtc_rearm();

  leave_critical_section(flags);

  if (ts != NULL)
    {
      bk7258_ticks_to_ts(g_oneshot.deadline > now ?
                         g_oneshot.deadline - now : 0, ts);
    }

  return OK;
}

/****************************************************************************
 * Name: bk7258_oneshot_current
 ****************************************************************************/

static int bk7258_oneshot_current(FAR struct oneshot_lowerhalf_s *lower,
                                  FAR struct timespec *ts)
{
  UNUSED(lower);

  bk7258_ticks_to_ts(bk7258_rtc_ticks(), ts);
  return OK;
}

#endif /* CONFIG_ALARM_ARCH && !CONFIG_ONESHOT_COUNT */

#ifdef CONFIG_ALARM_ARCH

/****************************************************************************
 * Name: bk7258_rtc_calibrate
 *
 * Description:
 *   Measure the AON counter against SysTick.
 *
 *   The obvious way -- up_mdelay() against the OS clock -- cannot be used
 *   here: this runs from up_timer_initialize(), which is what brings the OS
 *   clock up in the first place.  SysTick breaks the circle.  It is an ARM
 *   core peripheral clocked from the CPU clock, so it needs nothing from
 *   this chip and nothing from the OS; free-running it with interrupts off
 *   and counting whole wraps gives an exact number of CPU cycles to divide
 *   the counter delta by.
 *
 *   SysTick is left disabled afterwards.  It is no longer the time base.
 *
 ****************************************************************************/

#define BK7258_RTC_CAL_WRAPS  4
#define BK7258_RTC_CAL_CYCLES ((uint64_t)BK7258_RTC_CAL_WRAPS * 0x1000000ull)

static uint32_t bk7258_rtc_calibrate(void)
{
  uint32_t c0;
  uint32_t c1;
  int wraps;

  putreg32(0, NVIC_SYSTICK_CTRL);
  putreg32(0x00ffffff, NVIC_SYSTICK_RELOAD);
  putreg32(0, NVIC_SYSTICK_CURRENT);
  putreg32(NVIC_SYSTICK_CTRL_CLKSOURCE | NVIC_SYSTICK_CTRL_ENABLE,
           NVIC_SYSTICK_CTRL);

  /* Reading CTRL clears COUNTFLAG, so start from a known state. */

  getreg32(NVIC_SYSTICK_CTRL);

  c0 = bk7258_rtc_counter();
  for (wraps = 0; wraps < BK7258_RTC_CAL_WRAPS; )
    {
      if ((getreg32(NVIC_SYSTICK_CTRL) & NVIC_SYSTICK_CTRL_COUNTFLAG) != 0)
        {
          wraps++;
        }
    }

  c1 = bk7258_rtc_counter();

  putreg32(0, NVIC_SYSTICK_CTRL);

  /* hz = counted_ticks / elapsed_seconds, and elapsed_seconds is
   * CAL_CYCLES / BOARD_CPU_FREQUENCY.
   */

  return (uint32_t)(((uint64_t)(c1 - c0) * BOARD_CPU_FREQUENCY) /
                    BK7258_RTC_CAL_CYCLES);
}

#endif /* CONFIG_ALARM_ARCH */

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_rtc_poll
 ****************************************************************************/

void bk7258_rtc_poll(void)
{
  if (g_rtc_ready)
    {
      bk7258_rtc_ticks();
    }
}

/****************************************************************************
 * Name: bk7258_rtc_lowerhalf
 ****************************************************************************/

FAR struct rtc_lowerhalf_s *bk7258_rtc_lowerhalf(void)
{
  return g_rtc_ready ? &g_bk7258_rtc_lower : NULL;
}

/****************************************************************************
 * Name: bk7258_rtc_initialize
 ****************************************************************************/

int bk7258_rtc_initialize(void)
{
  uint32_t ctrl;
  uint32_t measured;

  if (g_rtc_ready)
    {
      return OK;
    }

  /* Enable the counter and park both compare values out of the way.  This
   * mirrors the vendor's aon_rtc_ll_tick_init(): CTRL = enable only, upper
   * compare at 0xffffffff, tick compare at 0.  Neither interrupt enable is
   * set, so the parked compares cannot fire.  Writing zeros into the two
   * W1C status bits is a no-op by definition, so this does not swallow a
   * pending interrupt.
   *
   * Note that the counter is deliberately *not* reset: it has been running
   * since the always-on domain came up, and this driver tracks wall clock
   * through an offset rather than by zeroing hardware.
   */

  ctrl = BK7258_RTC_CTRL_EN;
  putreg32(0xffffffff, BK7258_AON_RTC_BASE + BK7258_RTC_UPPER_OFFSET);
  putreg32(0, BK7258_AON_RTC_BASE + BK7258_RTC_TICK_OFFSET);
  putreg32(ctrl, BK7258_AON_RTC_BASE + BK7258_RTC_CTRL_OFFSET);

  /* Seed the software extension before anything can call in. */

  g_rtc_last_lo = bk7258_rtc_counter();
  g_rtc_wraps   = 0;
  g_rtc_offset  = 0;

  /* Confirm the counter actually advances before claiming an RTC exists.
   * A stopped counter would otherwise show up as a clock frozen at the
   * epoch, which is far harder to diagnose than a refusal here.
   */

#ifdef CONFIG_ALARM_ARCH
  /* SysTick-based; this runs from up_timer_initialize() where no OS clock
   * exists yet to measure against.
   */

  measured = bk7258_rtc_calibrate();
#else
  measured = bk7258_rtc_measure_hz();
#endif
  if (measured == 0)
    {
      syslog(LOG_ERR, "rtc: AON counter is not running\n");
      return -ENODEV;
    }

  if (measured >= BK7258_RTC_HZ_XTAL -
                  BK7258_RTC_HZ_XTAL / (100 / BK7258_RTC_CAL_TOL_PCT) &&
      measured <= BK7258_RTC_HZ_XTAL +
                  BK7258_RTC_HZ_XTAL / (100 / BK7258_RTC_CAL_TOL_PCT))
    {
      g_rtc_hz = BK7258_RTC_HZ_XTAL;
    }
  else if (measured >= BK7258_RTC_HZ_ROSC -
                       BK7258_RTC_HZ_ROSC / (100 / BK7258_RTC_CAL_TOL_PCT) &&
           measured <= BK7258_RTC_HZ_ROSC +
                       BK7258_RTC_HZ_ROSC / (100 / BK7258_RTC_CAL_TOL_PCT))
    {
      g_rtc_hz = BK7258_RTC_HZ_ROSC;
    }
  else
    {
      /* Neither candidate.  Trust the measurement over a nominal value that
       * demonstrably does not describe this board, but say so loudly.
       */

      g_rtc_hz = measured;
      syslog(LOG_WARNING,
             "rtc: %" PRIu32 " Hz matches neither 32768 nor 32000; "
             "using the measured rate\n", measured);
    }

  /* syslog, not rtcinfo(): the measured rate is the one fact that says
   * whether this clock can be trusted, and rtcinfo() is compiled out unless
   * CONFIG_DEBUG_RTC_INFO happens to be on.
   */

  syslog(LOG_INFO, "rtc: AON counter measured %" PRIu32 " Hz, using %"
         PRIu32 " Hz\n", measured, g_rtc_hz);

  g_rtc_ready = true;

#if defined(CONFIG_RTC_ALARM) || defined(CONFIG_RTC_PERIODIC)
  /* Both compare channels raise this one interrupt.  Neither channel's
   * enable bit is set yet, so attaching here cannot fire anything until an
   * alarm or a periodic wakeup is actually requested.
   */

  irq_attach(BK7258_IRQ_RTC, bk7258_rtc_interrupt, NULL);
  up_enable_irq(BK7258_IRQ_RTC);
#endif

  return OK;
}

/****************************************************************************
 * Name: bk7258_rtc_register
 *
 * Description:
 *   Hand the lower half to the arch RTC layer, which makes it the system
 *   wall clock.  Separate from bk7258_rtc_initialize() because that now
 *   runs from up_timer_initialize(), far too early for the
 *   clock_synchronize() that CONFIG_RTC_EXTERNAL triggers here.  Call from
 *   board_late_initialize().
 *
 ****************************************************************************/

int bk7258_rtc_register(void)
{
  if (!g_rtc_ready)
    {
      return -ENODEV;
    }

  up_rtc_set_lowerhalf(&g_bk7258_rtc_lower, true);
  return OK;
}

#ifdef CONFIG_ALARM_ARCH

/****************************************************************************
 * Name: bk7258_oneshot_initialize
 *
 * Description:
 *   Bring up the counter and hand back the oneshot that backs arch_alarm.
 *
 ****************************************************************************/

FAR struct oneshot_lowerhalf_s *bk7258_oneshot_initialize(void)
{
  if (bk7258_rtc_initialize() < 0)
    {
      return NULL;
    }

  g_rtc_wdt_last = bk7258_rtc_ticks();

#ifdef CONFIG_ONESHOT_COUNT
  /* Publishes the measured rate and derives the count-to-nanosecond
   * mult/shift the upper half uses.  Must follow calibration -- the rate is
   * measured, not assumed, so it is not known before then.
   */

  oneshot_count_init(&g_oneshot.lower, g_rtc_hz);
#endif

  return &g_oneshot.lower;
}

#endif /* CONFIG_ALARM_ARCH */

/****************************************************************************
 * Name: up_rtc_initialize
 *
 * Description:
 *   Arch hook called from clock_initialize(), very early in nx_start().
 *
 *   Nothing touches the hardware here on purpose.  Two constraints meet at
 *   this point: an AON access issued before the domain is ready stalls the
 *   bus on this SoC -- the watchdog bricked the board twice that way from
 *   the top of __start() -- and the rate measurement needs a running system
 *   tick, which does not exist yet.  CONFIG_RTC_EXTERNAL tells the OS the
 *   clock will arrive later; bk7258_rtc_initialize() delivers it from
 *   board_late_initialize(), by which point the console and the watchdog
 *   have both proven the AON domain is up.
 *
 ****************************************************************************/

int up_rtc_initialize(void)
{
  return OK;
}

#endif /* CONFIG_BK7258_RTC */
