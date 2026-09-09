/****************************************************************************
 * board/contest_board/chip/bk7258_wdt_lowerhalf.c
 *
 * NuttX watchdog lower half for the BK7258 AON watchdog.
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
 * How this maps onto the hardware
 *
 * The AON watchdog is not a timer this driver can hand to userspace
 * directly.  Its period register is the whole interface -- there is no
 * separate feed register, arming and feeding are the same write -- and the
 * SysTick handler already re-arms it about every 10 ms as the system's
 * lockup safety net.  A /dev/watchdog0 that merely forwarded start/stop to
 * that register would be decorative: the heartbeat would keep feeding
 * underneath it and the dog could never bite for the reason userspace
 * cared about.
 *
 * So the deadline lives one level up.  This driver hands the heartbeat a
 * system-tick deadline; the heartbeat keeps feeding while the deadline is
 * in the future and arms the 6 ms boot period the moment it is not.  The
 * hardware still guarantees recovery if ticks stop altogether -- that path
 * is untouched -- and userspace gets a watchdog whose timeout means what it
 * says, at heartbeat granularity, without being capped by the hardware's
 * ~65 s maximum period.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/clock.h>
#include <nuttx/irq.h>
#include <nuttx/spinlock.h>
#include <nuttx/kmalloc.h>
#include <nuttx/timers/watchdog.h>

#include "bk7258_wdt.h"

#ifdef CONFIG_BK7258_WDT

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* A timeout shorter than two heartbeats cannot be honoured: the deadline is
 * only ever examined when the heartbeat runs, so a one-heartbeat timeout
 * would expire and bite before a faithful pinger got its turn.  Reject
 * those rather than pretend.
 */

#define BK7258_WDT_MIN_TIMEOUT  (2 * TICK2MSEC(BK7258_WDT_HEARTBEAT_TICKS))

/* Nothing in the hardware caps the timeout -- the deadline is software --
 * but an hour is far past any plausible use and keeps the tick arithmetic
 * comfortably inside range.
 */

#define BK7258_WDT_MAX_TIMEOUT  (60 * 60 * 1000)

#define BK7258_WDT_DEFAULT_TIMEOUT 10000

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct bk7258_wdt_lowerhalf_s
{
  FAR const struct watchdog_ops_s *ops;  /* Must be first */
  uint32_t timeout;                      /* Current timeout, milliseconds */
  clock_t  deadline;                     /* Expiry, in system ticks */
  bool     started;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int bk7258_wdt_start(FAR struct watchdog_lowerhalf_s *lower);
static int bk7258_wdt_stop(FAR struct watchdog_lowerhalf_s *lower);
static int bk7258_wdt_keepalive(FAR struct watchdog_lowerhalf_s *lower);
static int bk7258_wdt_getstatus(FAR struct watchdog_lowerhalf_s *lower,
                                FAR struct watchdog_status_s *status);
static int bk7258_wdt_settimeout(FAR struct watchdog_lowerhalf_s *lower,
                                 uint32_t timeout);
#ifdef CONFIG_BK7258_WDT_NMI
static xcpt_t bk7258_wdt_capture(FAR struct watchdog_lowerhalf_s *lower,
                                 xcpt_t handler);
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct watchdog_ops_s g_bk7258_wdt_ops =
{
  .start      = bk7258_wdt_start,
  .stop       = bk7258_wdt_stop,
  .keepalive  = bk7258_wdt_keepalive,
  .getstatus  = bk7258_wdt_getstatus,
  .settimeout = bk7258_wdt_settimeout,

#ifdef CONFIG_BK7258_WDT_NMI
  /* Capture is only honest because of the NMI stage.  The always-on block
   * resets the SoC with no warning; the peripheral block raises NMI first,
   * and that is the moment the callback runs.  Without the stage there is no
   * such moment, so the entry below goes back to NULL and the upper half
   * reports WDIOC_CAPTURE as unsupported.
   */

  .capture    = bk7258_wdt_capture,
#else
  .capture    = NULL,
#endif
  .ioctl      = NULL,
};

static struct bk7258_wdt_lowerhalf_s g_bk7258_wdt_lower =
{
  .ops     = &g_bk7258_wdt_ops,
  .timeout = BK7258_WDT_DEFAULT_TIMEOUT,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_wdt_refresh
 *
 * Description:
 *   Push the deadline out by one full timeout from now and publish it.
 *
 ****************************************************************************/

static void bk7258_wdt_refresh(FAR struct bk7258_wdt_lowerhalf_s *priv)
{
  priv->deadline = clock_systime_ticks() + MSEC2TICK(priv->timeout);

  /* A deadline of exactly zero is the release signal, so nudge past it on
   * the once-in-a-blue-moon tick where the sum lands there.
   */

  if (priv->deadline == 0)
    {
      priv->deadline = 1;
    }

  bk7258_wdt_deadline_set(priv->deadline);
}

/****************************************************************************
 * Name: bk7258_wdt_start
 ****************************************************************************/

static int bk7258_wdt_start(FAR struct watchdog_lowerhalf_s *lower)
{
  FAR struct bk7258_wdt_lowerhalf_s *priv =
    (FAR struct bk7258_wdt_lowerhalf_s *)lower;
  irqstate_t flags;

  flags = enter_critical_section();
  bk7258_wdt_refresh(priv);
  priv->started = true;
  leave_critical_section(flags);

  wdinfo("started, timeout %" PRIu32 " ms\n", priv->timeout);
  return OK;
}

/****************************************************************************
 * Name: bk7258_wdt_stop
 ****************************************************************************/

static int bk7258_wdt_stop(FAR struct watchdog_lowerhalf_s *lower)
{
  FAR struct bk7258_wdt_lowerhalf_s *priv =
    (FAR struct bk7258_wdt_lowerhalf_s *)lower;
  irqstate_t flags;

  /* Releasing the claim returns the heartbeat to its unconditional feed.
   * The hardware watchdog stays armed either way -- it is the system's
   * lockup net, not this driver's to switch off.
   */

  flags = enter_critical_section();
  priv->started  = false;
  priv->deadline = 0;
  bk7258_wdt_deadline_set(0);
  leave_critical_section(flags);

  wdinfo("stopped\n");
  return OK;
}

/****************************************************************************
 * Name: bk7258_wdt_keepalive
 ****************************************************************************/

static int bk7258_wdt_keepalive(FAR struct watchdog_lowerhalf_s *lower)
{
  FAR struct bk7258_wdt_lowerhalf_s *priv =
    (FAR struct bk7258_wdt_lowerhalf_s *)lower;
  irqstate_t flags;

  flags = enter_critical_section();
  if (priv->started)
    {
      bk7258_wdt_refresh(priv);
    }

  leave_critical_section(flags);

  return OK;
}

/****************************************************************************
 * Name: bk7258_wdt_getstatus
 ****************************************************************************/

static int bk7258_wdt_getstatus(FAR struct watchdog_lowerhalf_s *lower,
                                FAR struct watchdog_status_s *status)
{
  FAR struct bk7258_wdt_lowerhalf_s *priv =
    (FAR struct bk7258_wdt_lowerhalf_s *)lower;
  irqstate_t flags;
  sclock_t remaining;

  flags = enter_critical_section();

  status->flags   = WDFLAGS_RESET;
  status->timeout = priv->timeout;

  if (priv->started)
    {
      status->flags |= WDFLAGS_ACTIVE;
      remaining = (sclock_t)(priv->deadline - clock_systime_ticks());
      status->timeleft = remaining > 0 ? TICK2MSEC(remaining) : 0;
    }
  else
    {
      status->timeleft = 0;
    }

  leave_critical_section(flags);

  return OK;
}

/****************************************************************************
 * Name: bk7258_wdt_settimeout
 ****************************************************************************/

static int bk7258_wdt_settimeout(FAR struct watchdog_lowerhalf_s *lower,
                                 uint32_t timeout)
{
  FAR struct bk7258_wdt_lowerhalf_s *priv =
    (FAR struct bk7258_wdt_lowerhalf_s *)lower;
  irqstate_t flags;

  if (timeout < BK7258_WDT_MIN_TIMEOUT || timeout > BK7258_WDT_MAX_TIMEOUT)
    {
      wderr("ERROR: timeout %" PRIu32 " ms outside %d..%d\n",
            timeout, (int)BK7258_WDT_MIN_TIMEOUT,
            (int)BK7258_WDT_MAX_TIMEOUT);
      return -ERANGE;
    }

  flags = enter_critical_section();
  priv->timeout = timeout;

  /* Setting a timeout also restarts the count, per the ops contract. */

  if (priv->started)
    {
      bk7258_wdt_refresh(priv);
    }

  leave_critical_section(flags);

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_wdt_lowerhalf_initialize
 *
 * Description:
 *   Register the AON watchdog at the given path, normally /dev/watchdog0.
 *
 * Returned Value:
 *   Zero on success; a negated errno value on failure.
 *
 ****************************************************************************/

int bk7258_wdt_lowerhalf_initialize(FAR const char *devpath)
{
  FAR void *handle;

  handle = watchdog_register(devpath,
                             (FAR struct watchdog_lowerhalf_s *)
                             &g_bk7258_wdt_lower);
  if (handle == NULL)
    {
      wderr("ERROR: watchdog_register(%s) failed\n", devpath);
      return -EEXIST;
    }

  return OK;
}

#endif /* CONFIG_BK7258_WDT */

/****************************************************************************
 * Name: bk7258_wdt_capture
 *
 * Description:
 *   WDIOC_CAPTURE.  Hands the callback to the NMI stage, which runs it in
 *   place of the panic and keeps both watchdogs fed so the notification does
 *   not become a reset.  Returns the previously installed handler.
 *
 ****************************************************************************/

#ifdef CONFIG_BK7258_WDT_NMI
static xcpt_t bk7258_wdt_capture(FAR struct watchdog_lowerhalf_s *lower,
                                 xcpt_t handler)
{
  UNUSED(lower);

  return bk7258_wdt_capture_set(handler);
}
#endif
