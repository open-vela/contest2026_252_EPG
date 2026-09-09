/****************************************************************************
 * board/contest_board/chip/bk7258_reset_reason.h
 *
 * Access to the always-on PMU field that carries "why did this boot happen"
 * across a reset.  Chip level rather than board level because it is nothing
 * but AON PMU register knowledge; src/bk7258_reset.c sits on top and turns
 * it into the boardctl(BOARDIOC_RESET_CAUSE) answer.
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

#ifndef __BOARD_CONTEST_BOARD_CHIP_BK7258_RESET_REASON_H
#define __BOARD_CONTEST_BOARD_CHIP_BK7258_RESET_REASON_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Vendor reason codes, from include/components/system.h of the Beken SDK.
 * Only the ones this port can produce or observe are named.
 */

#define BK7258_RESET_POWERON      0x00
#define BK7258_RESET_REBOOT       0x01
#define BK7258_RESET_WATCHDOG     0x02
#define BK7258_RESET_DEEPPS_GPIO  0x03
#define BK7258_RESET_DEEPPS_RTC   0x04
#define BK7258_RESET_DEEPPS_USB   0x05
#define BK7258_RESET_DEEPPS_TOUCH 0x06
#define BK7258_RESET_CRASH_FIRST  0x07  /* 0x07..0x0e are the crash codes */
#define BK7258_RESET_CRASH_LAST   0x0e
#define BK7258_RESET_SUPER_DEEP   0x0f
#define BK7258_RESET_NMI_WDT      0x10

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_reset_reason_set
 *
 * Description:
 *   Record why the chip is about to reset, so the next boot can report it.
 *   Safe from interrupt and NMI context: it is three register writes with
 *   no locking, and the field has a single writer at any moment.
 *
 ****************************************************************************/

void bk7258_reset_reason_set(uint32_t reason);

/****************************************************************************
 * Name: bk7258_reset_cause_latch
 *
 * Description:
 *   Cache the reason for this boot and re-arm the field with "power on".
 *   Call once, from board_late_initialize(); not safe before the AON domain
 *   is up.  Repeat calls do nothing.
 *
 ****************************************************************************/

void bk7258_reset_cause_latch(void);

/****************************************************************************
 * Name: bk7258_reset_reason_get
 *
 * Description:
 *   The value latched at boot.  Returns BK7258_RESET_POWERON if the latch
 *   has not run yet.
 *
 ****************************************************************************/

uint32_t bk7258_reset_reason_get(void);

#endif /* __BOARD_CONTEST_BOARD_CHIP_BK7258_RESET_REASON_H */
