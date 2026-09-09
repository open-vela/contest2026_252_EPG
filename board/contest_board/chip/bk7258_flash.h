/****************************************************************************
 * board/contest_board/chip/bk7258_flash.h
 *
 * On-chip flash MTD for the BK7258.
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

#ifndef __BOARD_CONTEST_BOARD_CHIP_BK7258_FLASH_H
#define __BOARD_CONTEST_BOARD_CHIP_BK7258_FLASH_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <nuttx/mtd/mtd.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The part is a GigaDevice gd_25Q32E, id 0xc86517, 8MB.  The boot ROM
 * reports the same id byte-reversed as 0x1765c8; both spellings appear in
 * this tree's notes and they are the same chip.
 */

#define BK7258_FLASH_CHIP_SIZE      0x00800000ul

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
 * Name: bk7258_flash_initialize
 *
 * Description:
 *   Return an MTD covering the region of on-chip flash that no partition
 *   claims -- CONFIG_BK7258_FLASH_OFFSET for CONFIG_BK7258_FLASH_SIZE
 *   bytes, in physical addresses.
 *
 *   The driver refuses every access outside that window rather than
 *   trusting its caller, because what lies outside is the boot loader, the
 *   three cores' application images, and the factory calibration block at
 *   the top of the chip.
 *
 * Returned Value:
 *   An MTD on success, NULL if the configured region is unusable.  There is
 *   one instance; repeated calls return the same one.
 *
 ****************************************************************************/

FAR struct mtd_dev_s *bk7258_flash_initialize(void);

#undef EXTERN
#if defined(__cplusplus)
}
#endif

#endif /* __ASSEMBLY__ */
#endif /* __BOARD_CONTEST_BOARD_CHIP_BK7258_FLASH_H */
