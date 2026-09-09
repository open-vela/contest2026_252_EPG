/****************************************************************************
 * board/contest_board/chip/bk7258_timer.h
 *
 * Oneshot timer for the BK7258, built on TIMER group 0.
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

#ifndef __BOARD_CONTEST_BOARD_CHIP_BK7258_TIMER_H
#define __BOARD_CONTEST_BOARD_CHIP_BK7258_TIMER_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <nuttx/timers/oneshot.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifndef __ASSEMBLY__

#undef EXTERN
#if defined(__cplusplus)
#define EXTERN extern "C"
extern "C"
{
#else
#define EXTERN extern
#endif

/****************************************************************************
 * Name: bk7258_timer_oneshot_initialize
 *
 * Description:
 *   Bring up TIMER group 0 and return a oneshot lower half driven by it.
 *
 *   The group has three channels behind one interrupt line.  Two are used:
 *   channel 0 is armed per request and is the oneshot proper, channel 1 free
 *   runs at full scale as the timebase that ONESHOT_CURRENT reports.  The
 *   second channel is not optional -- the oneshot contract wants a clock
 *   that keeps counting monotonically across arm, expiry and cancel, and a
 *   channel that restarts at every expiry cannot be one.
 *
 *   This deliberately leaves the system tick alone.  The scheduler runs off
 *   the AON RTC through arch_alarm(), and /dev/rtc0 owns that block's alarm;
 *   TIMER group 0 is a third, independent source so that a test can drive
 *   the oneshot as hard as it likes without perturbing either.
 *
 * Returned Value:
 *   A oneshot lower half on success, NULL if the interrupt could not be
 *   attached.  There is one instance; a second call returns the same one.
 *
 ****************************************************************************/

FAR struct oneshot_lowerhalf_s *bk7258_timer_oneshot_initialize(void);

/****************************************************************************
 * Name: bk7258_timer_oneshot_register
 *
 * Description:
 *   Initialize the timer and expose it at devname, e.g. "/dev/oneshot0".
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int bk7258_timer_oneshot_register(FAR const char *devname);

#undef EXTERN
#if defined(__cplusplus)
}
#endif

#endif /* __ASSEMBLY__ */
#endif /* __BOARD_CONTEST_BOARD_CHIP_BK7258_TIMER_H */
