/****************************************************************************
 * board/contest_board/chip/include/irq.h
 *
 * BK7258 interrupt vector definitions.
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

/* This file should never be included directly but, rather, only indirectly
 * through nuttx/irq.h
 */

#ifndef __BOARD_CONTEST_BOARD_CHIP_INCLUDE_IRQ_H
#define __BOARD_CONTEST_BOARD_CHIP_INCLUDE_IRQ_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Vector number of the first peripheral interrupt.  Vectors 0-15 are the
 * ARMv8-M system exceptions.
 */

#define NVIC_IRQ_FIRST          (16)

/* BK7258 peripheral interrupt sources.
 *
 * These match arch_int_src_t in the Beken SDK
 * (middleware/soc/bk7258/interrupts.h) one-for-one.  NuttX IRQ numbers are
 * vector numbers, so each peripheral source n maps to NVIC_IRQ_FIRST + n.
 */

#define BK7258_IRQ_DMA0_NSEC    (NVIC_IRQ_FIRST + 0)
#define BK7258_IRQ_ENC_SEC      (NVIC_IRQ_FIRST + 1)
#define BK7258_IRQ_ENC_NSEC     (NVIC_IRQ_FIRST + 2)
#define BK7258_IRQ_TIMER0       (NVIC_IRQ_FIRST + 3)
#define BK7258_IRQ_UART0        (NVIC_IRQ_FIRST + 4)
#define BK7258_IRQ_PWM0         (NVIC_IRQ_FIRST + 5)
#define BK7258_IRQ_I2C0         (NVIC_IRQ_FIRST + 6)
#define BK7258_IRQ_SPI0         (NVIC_IRQ_FIRST + 7)
#define BK7258_IRQ_SARADC       (NVIC_IRQ_FIRST + 8)
#define BK7258_IRQ_IRDA         (NVIC_IRQ_FIRST + 9)
#define BK7258_IRQ_SDIO         (NVIC_IRQ_FIRST + 10)
#define BK7258_IRQ_GDMA         (NVIC_IRQ_FIRST + 11)
#define BK7258_IRQ_LA           (NVIC_IRQ_FIRST + 12)
#define BK7258_IRQ_TIMER1       (NVIC_IRQ_FIRST + 13)
#define BK7258_IRQ_I2C1         (NVIC_IRQ_FIRST + 14)
#define BK7258_IRQ_UART1        (NVIC_IRQ_FIRST + 15)
#define BK7258_IRQ_UART2        (NVIC_IRQ_FIRST + 16)
#define BK7258_IRQ_SPI1         (NVIC_IRQ_FIRST + 17)
#define BK7258_IRQ_CAN          (NVIC_IRQ_FIRST + 18)
#define BK7258_IRQ_USB          (NVIC_IRQ_FIRST + 19)
#define BK7258_IRQ_QSPI0        (NVIC_IRQ_FIRST + 20)
#define BK7258_IRQ_FFT          (NVIC_IRQ_FIRST + 21)
#define BK7258_IRQ_SBC          (NVIC_IRQ_FIRST + 22)
#define BK7258_IRQ_AUD          (NVIC_IRQ_FIRST + 23)
#define BK7258_IRQ_I2S0         (NVIC_IRQ_FIRST + 24)
#define BK7258_IRQ_JPEGENC      (NVIC_IRQ_FIRST + 25)
#define BK7258_IRQ_JPEGDEC      (NVIC_IRQ_FIRST + 26)
#define BK7258_IRQ_LCD          (NVIC_IRQ_FIRST + 27)
#define BK7258_IRQ_DMA2D        (NVIC_IRQ_FIRST + 28)
#define BK7258_IRQ_PHY_MBP      (NVIC_IRQ_FIRST + 29)
#define BK7258_IRQ_PHY_RIU      (NVIC_IRQ_FIRST + 30)
#define BK7258_IRQ_MAC_TXRX_TMR (NVIC_IRQ_FIRST + 31)
#define BK7258_IRQ_MAC_TXRX_MISC (NVIC_IRQ_FIRST + 32)
#define BK7258_IRQ_MAC_RX_TRIG  (NVIC_IRQ_FIRST + 33)
#define BK7258_IRQ_MAC_TX_TRIG  (NVIC_IRQ_FIRST + 34)
#define BK7258_IRQ_MAC_PORT_TRIG (NVIC_IRQ_FIRST + 35)
#define BK7258_IRQ_MAC_GEN      (NVIC_IRQ_FIRST + 36)
#define BK7258_IRQ_HSU          (NVIC_IRQ_FIRST + 37)
#define BK7258_IRQ_MAC_WAKEUP   (NVIC_IRQ_FIRST + 38)
#define BK7258_IRQ_DM           (NVIC_IRQ_FIRST + 39)
#define BK7258_IRQ_BLE          (NVIC_IRQ_FIRST + 40)
#define BK7258_IRQ_BT           (NVIC_IRQ_FIRST + 41)
#define BK7258_IRQ_QSPI1        (NVIC_IRQ_FIRST + 42)
#define BK7258_IRQ_PWM1         (NVIC_IRQ_FIRST + 43)
#define BK7258_IRQ_I2S1         (NVIC_IRQ_FIRST + 44)
#define BK7258_IRQ_I2S2         (NVIC_IRQ_FIRST + 45)
#define BK7258_IRQ_H264         (NVIC_IRQ_FIRST + 46)
#define BK7258_IRQ_SDMADC       (NVIC_IRQ_FIRST + 47)
#define BK7258_IRQ_MBOX0        (NVIC_IRQ_FIRST + 48)
#define BK7258_IRQ_MBOX1        (NVIC_IRQ_FIRST + 49)
#define BK7258_IRQ_BMC64        (NVIC_IRQ_FIRST + 50)
#define BK7258_IRQ_DPLL_UNLOCK  (NVIC_IRQ_FIRST + 51)
#define BK7258_IRQ_TOUCHED      (NVIC_IRQ_FIRST + 52)
#define BK7258_IRQ_USBPLUG      (NVIC_IRQ_FIRST + 53)
#define BK7258_IRQ_RTC          (NVIC_IRQ_FIRST + 54)
#define BK7258_IRQ_GPIO         (NVIC_IRQ_FIRST + 55)
#define BK7258_IRQ_DMA1_SEC     (NVIC_IRQ_FIRST + 56)
#define BK7258_IRQ_DMA1_NSEC    (NVIC_IRQ_FIRST + 57)
#define BK7258_IRQ_YUVB         (NVIC_IRQ_FIRST + 58)
#define BK7258_IRQ_ROTT         (NVIC_IRQ_FIRST + 59)

/* Number of peripheral interrupt sources (INT_ID_MAX in the Beken SDK) */

#define BK7258_IRQ_NEXTINTS     (60)

/* Total number of IRQs: 16 ARMv8-M exceptions + peripheral interrupts */

#define NR_IRQS                 (NVIC_IRQ_FIRST + BK7258_IRQ_NEXTINTS)

/* NVIC priority levels.  The STAR-MC1 core in this part implements three
 * priority bits, bits[7:5] -- __NVIC_PRIO_BITS is 3 in the Beken SDK's
 * cmsis/CMSIS_5/Device/Beken/armstar/armstar.h.  So bits[7:6] carry the
 * group priority and bit[5] the sub-priority.
 *
 * NVIC_SYSH_MAXNORMAL_PRIORITY, NVIC_SYSH_DISABLE_PRIORITY and
 * NVIC_SYSH_SVCALL_PRIORITY are deliberately NOT defined here.  Every other
 * armv8-m chip leaves them to arch/arm_m/nvicpri.h, which derives them from
 * the values below and is pulled in by arch/arm_m/irq.h -- that is, after
 * this file.  Anything defined here would be silently redefined there (the
 * redefinition warning is suppressed because both headers arrive via
 * -isystem), so a local definition looks authoritative while having no
 * effect at all.
 */

#define NVIC_SYSH_PRIORITY_MIN      0xff /* All bits set in minimum priority */
#define NVIC_SYSH_PRIORITY_DEFAULT  0x80 /* Midpoint is the default */
#define NVIC_SYSH_PRIORITY_MAX      0x00 /* Zero is maximum priority */
#define NVIC_SYSH_PRIORITY_STEP     0x40 /* bits[7:6] as group priority */
#define NVIC_SYSH_PRIORITY_SUBSTEP  0x20 /* bit[5] as sub-priority */

#endif /* __BOARD_CONTEST_BOARD_CHIP_INCLUDE_IRQ_H */
