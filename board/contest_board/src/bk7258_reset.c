/****************************************************************************
 * board/contest_board/src/bk7258_reset.c
 *
 * boardctl(BOARDIOC_RESET_CAUSE) back end, which the xTS general self-test
 * 1.3.15 (Watchdog) reads back after each bite.
 *
 * The AON PMU register work lives one layer down in
 * chip/bk7258_reset_reason.c; all this file does is map the vendor reason
 * code onto the NuttX enumeration.
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

#include <errno.h>
#include <stdint.h>

#include <nuttx/board.h>
#include <sys/boardctl.h>

#include "bk7258_reset_reason.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: board_reset_cause
 *
 * Description:
 *   boardctl(BOARDIOC_RESET_CAUSE) back end.
 *
 *   The watchdog on this SoC is the always-on one at 0x44000600 -- there is
 *   no separate main watchdog -- so a bite maps to SYS_RWDT, which is the
 *   value the xTS watchdog case expects.  Note that the field only carries
 *   BK7258_RESET_WATCHDOG because the NMI stage in chip/bk7258_wdt.c writes
 *   it there on the way down; nothing in hardware records a bite.
 *
 ****************************************************************************/

#ifdef CONFIG_BOARDCTL_RESET_CAUSE
int board_reset_cause(FAR struct boardioc_reset_cause_s *cause)
{
  uint32_t reason;

  if (cause == NULL)
    {
      return -EINVAL;
    }

  reason = bk7258_reset_reason_get();
  cause->flag = 0;

  switch (reason)
    {
      case BK7258_RESET_POWERON:
        cause->cause = BOARDIOC_RESETCAUSE_SYS_CHIPPOR;
        break;

      case BK7258_RESET_REBOOT:
        cause->cause = BOARDIOC_RESETCAUSE_CORE_SOFT;
        cause->flag = BOARDIOC_SOFTRESETCAUSE_USER_REBOOT;
        break;

      case BK7258_RESET_WATCHDOG:
      case BK7258_RESET_NMI_WDT:
        cause->cause = BOARDIOC_RESETCAUSE_SYS_RWDT;
        break;

      case BK7258_RESET_DEEPPS_GPIO:
      case BK7258_RESET_DEEPPS_RTC:
      case BK7258_RESET_DEEPPS_USB:
      case BK7258_RESET_DEEPPS_TOUCH:
      case BK7258_RESET_SUPER_DEEP:
        cause->cause = BOARDIOC_RESETCAUSE_CORE_DPSP;
        break;

      default:
        if (reason >= BK7258_RESET_CRASH_FIRST &&
            reason <= BK7258_RESET_CRASH_LAST)
          {
            /* The vendor crash codes all mean "the previous image took a
             * fault and reset itself", which is a software core reset.
             */

            cause->cause = BOARDIOC_RESETCAUSE_CORE_SOFT;
            cause->flag = BOARDIOC_SOFTRESETCAUSE_PANIC;
          }
        else
          {
            cause->cause = BOARDIOC_RESETCAUSE_UNKOWN;
            cause->flag = reason;
          }
        break;
    }

  return OK;
}
#endif /* CONFIG_BOARDCTL_RESET_CAUSE */
