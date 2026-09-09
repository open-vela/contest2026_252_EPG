/****************************************************************************
 * board/contest_board/chip/bk7258_reset_reason.c
 *
 * The always-on PMU field that carries the reset reason across a reset.
 *
 * Where the reason lives
 * ----------------------
 * BK7258 builds as CONFIG_SOC_BK7236XX in the vendor SDK
 * (middleware/soc/bk7258/bk7258.defconfig line 2), so the reason follows the
 * BK7236XX/BK7239XX branch of the SDK's reset_reason.c:
 *
 *   read   aon_pmu_ll_get_r7a() >> 24 & 0x7f
 *   write  read-modify-write of PMU R0 bits [30:24], then commit by writing
 *          0x424B55AA and 0xBDB4AA55 to PMU R25
 *
 * The R25 pair is the vendor's latch sequence that copies R0 into the
 * retained R7B/R7A shadow; without it a value written to R0 does not survive
 * the reset.  All three registers sit in the always-on domain
 * (SOC_AON_PMU_REG_BASE == 0x44000000, include/soc/bk7258/reg_base.h:54),
 * which is why the value outlives a reset at all.
 *
 * Register offsets, from middleware/soc/bk7258/soc/aon_pmu_ll.h:
 *   R0   base + (0x00 << 2) == 0x44000000
 *   R25  base + (0x25 << 2) == 0x44000094
 *   R7A  base + (0x7a << 2) == 0x440001e8
 *
 * Nothing in hardware writes this field.  It is software-maintained, which
 * is why an unannounced watchdog bite used to read back as "power on" --
 * see bk7258_wdt.c, where the NMI stage now records the bite before the
 * reset lands.
 *
 * When it is safe to touch
 * ------------------------
 * Never from __start().  An AON access issued before the domain is ready
 * stalls the bus on this SoC -- that is what bricked the board twice early
 * in the port (PORTING_NOTES chapter 2).  The latch runs from
 * board_late_initialize(), the same place the AON RTC comes up.
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

#include <debug.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include "arm_internal.h"

#include "bk7258_memorymap.h"
#include "bk7258_reset_reason.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define BK7258_AON_PMU_R0     (BK7258_AON_PMU_BASE + (0x00 << 2))
#define BK7258_AON_PMU_R25    (BK7258_AON_PMU_BASE + (0x25 << 2))
#define BK7258_AON_PMU_R7A    (BK7258_AON_PMU_BASE + (0x7a << 2))

/* Reset reason occupies R0/R7A bits [30:24] */

#define BK7258_RESET_SHIFT    24
#define BK7258_RESET_MASK     0x7f

/* The two magic words that latch R0 into the retained shadow */

#define BK7258_PMU_COMMIT0    0x424b55aa
#define BK7258_PMU_COMMIT1    0xbdb4aa55

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Latched once in bk7258_reset_cause_latch(); the hardware field is
 * rewritten immediately afterwards, so this is the only surviving copy.
 */

static uint8_t g_reset_reason = BK7258_RESET_POWERON;
static bool    g_reset_latched;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_reset_reason_set
 ****************************************************************************/

void bk7258_reset_reason_set(uint32_t reason)
{
  uint32_t regval = getreg32(BK7258_AON_PMU_R0);

  /* Read-modify-write: R0 carries unrelated always-on state in the bits
   * outside [30:24] and clobbering them would be a power-domain change.
   */

  regval &= ~(BK7258_RESET_MASK << BK7258_RESET_SHIFT);
  regval |= (reason & BK7258_RESET_MASK) << BK7258_RESET_SHIFT;
  putreg32(regval, BK7258_AON_PMU_R0);

  /* Commit R0 into the retained shadow.  Both words, in this order. */

  putreg32(BK7258_PMU_COMMIT0, BK7258_AON_PMU_R25);
  putreg32(BK7258_PMU_COMMIT1, BK7258_AON_PMU_R25);
}

/****************************************************************************
 * Name: bk7258_reset_cause_latch
 ****************************************************************************/

void bk7258_reset_cause_latch(void)
{
  uint32_t regval;

  if (g_reset_latched)
    {
      return;
    }

  regval = getreg32(BK7258_AON_PMU_R7A);
  g_reset_reason = (regval >> BK7258_RESET_SHIFT) & BK7258_RESET_MASK;
  g_reset_latched = true;

  binfo("BK7258 reset reason: 0x%02x (PMU R7A 0x%08" PRIx32 ")\n",
        g_reset_reason, regval);

  /* Arm the field with "power on" so that a reset nobody announced -- a pin
   * reset, a brown-out -- is not mistaken for whatever the previous boot
   * last wrote.  Anything intentional overwrites it on the way out: a
   * commanded reboot through board_reset(), or a watchdog bite through the
   * NMI stage in bk7258_wdt.c.
   */

  bk7258_reset_reason_set(BK7258_RESET_POWERON);
}

/****************************************************************************
 * Name: bk7258_reset_reason_get
 ****************************************************************************/

uint32_t bk7258_reset_reason_get(void)
{
  return g_reset_reason;
}
