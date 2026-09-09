/****************************************************************************
 * board/contest_board/chip/bk7258_start.c
 *
 * BK7258 reset entry point.
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

#include <nuttx/arch.h>
#include <nuttx/board.h>
#include <nuttx/init.h>

#include "arm_internal.h"
#include "nvic.h"

#include "bk7258_lowputc.h"
#include "bk7258_clockconfig.h"
#include "bk7258_gpio.h"
#include "bk7258_wdt.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define HEAP_BASE  ((uintptr_t)_ebss + CONFIG_IDLETHREAD_STACKSIZE)

#ifdef CONFIG_DEBUG_FEATURES
#  define showprogress(c) arm_lowputc(c)
#else
#  define showprogress(c)
#endif

/* Bring-up marker.
 *
 * These push a byte straight into the UART0 TX FIFO.
 *
 * Do not expect them to work before bk7258_lowsetup() has run.  Disassembling
 * the Beken bootloader shows it tears UART0 down on the way out: the call at
 * 0x02000ac2, reached unconditionally just before the jump, ends up in a
 * routine that writes both the UART0 block (0x44820000) and the system clock
 * registers (0x44010000).  An earlier version of this port assumed the
 * bootloader left the console running and put markers at the very top of
 * __start(); those markers can only have written into an ungated peripheral.
 *
 * Nothing here may touch .data or .bss: both are still uninitialised when the
 * first marker runs.
 */

#ifdef CONFIG_BK7258_EARLY_MARKERS
#  define earlymark_at(base, c) \
     do \
       { \
         volatile uint32_t *_st = \
           (volatile uint32_t *)((base) + BK7258_UART_FIFO_STATUS_OFFSET); \
         volatile uint32_t *_tx = \
           (volatile uint32_t *)((base) + BK7258_UART_FIFO_PORT_OFFSET); \
         uint32_t _spin = 100000; \
         while ((*_st & UART_FIFO_STATUS_WR_READY) == 0 && --_spin); \
         *_tx = (uint32_t)(unsigned char)(c); \
       } \
     while (0)
#  define earlymark(c) earlymark_at(BK7258_UART0_BASE, c)
#else
#  define earlymark_at(base, c)
#  define earlymark(c)
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

#ifdef CONFIG_BK7258_TXPIN_TRACE

/****************************************************************************
 * Name: bk7258_txpin_mark
 *
 * Description:
 *   Emit one burst on GPIO11 and then hold the line idle, so a host reading
 *   the port sees a run of bytes followed by a clear gap.  Called at each
 *   step of the boot, the number of bursts that arrive says how far the boot
 *   got: five bursts then silence means the fifth step is where it died.
 *
 *   UART idle is high, so parking the pad high between bursts is what makes
 *   the gaps read as silence rather than as more framing errors.
 *
 ****************************************************************************/

void bk7258_txpin_mark(void)
{
  volatile uint32_t *tx = (volatile uint32_t *)BK7258_GPIO_CFG(11);
  volatile uint32_t spin;
  uint32_t i;

  for (i = 0; i < 200; i++)
    {
      *tx = (i & 1) ? GPIO_CFG_OUTPUT : 0;

      for (spin = 0; spin < 2000; spin++)
        {
        }
    }

  *tx = GPIO_CFG_OUTPUT;

  for (i = 0; i < 500; i++)
    {
      for (spin = 0; spin < 2000; spin++)
        {
        }
    }
}
#  define txmark() bk7258_txpin_mark()
#else
#  define txmark()
#endif

#ifdef CONFIG_BK7258_UART_DUMP

/****************************************************************************
 * Name: bk7258_txpin_word
 *
 * Description:
 *   Shift a 32-bit value out on GPIO11, most significant bit first, as long
 *   bursts for ones and short bursts for zeros.
 *
 *   This exists because the console cannot report on itself.  When
 *   bk7258_lowsetup() leaves a UART that never asserts write-ready,
 *   arm_lowputc() spins forever and the boot stops with nothing to show for
 *   it; reading the block's own registers back out through a mechanism that
 *   does not depend on the block is the way to see what state it is in.
 *
 ****************************************************************************/

static void bk7258_txpin_answer(int yes)
{
  volatile uint32_t *tx = (volatile uint32_t *)BK7258_GPIO_CFG(11);
  volatile uint32_t spin;
  /* Answer by duration, not by byte count.  The host groups the stream by
   * silence and measures how long the noise lasted: a couple of seconds means
   * yes, a fifth of a second means no.  Counting bytes instead does not work
   * -- the receiver delivers a long burst in chunks, and any grouping rule
   * fine enough to separate answers also splits one answer into several.
   */

  uint32_t toggles = yes ? 600 : 100;
  uint32_t i;

  for (i = 0; i < toggles; i++)
    {
      *tx = (i & 1) ? GPIO_CFG_OUTPUT : 0;

      for (spin = 0; spin < 1500; spin++)
        {
        }
    }

  *tx = GPIO_CFG_OUTPUT;

  for (i = 0; i < 400; i++)
    {
      for (spin = 0; spin < 1500; spin++)
        {
        }
    }
}
#endif

#if defined(CONFIG_BK7258_TXPIN_PROBE) || defined(CONFIG_BK7258_TXPIN_PROBE_LATE)

/****************************************************************************
 * Name: bk7258_txpin_forever
 *
 * Description:
 *   Bit-bang GPIO11 -- UART0 TX, wired to the CH340 bridge -- and never
 *   return.  Each level lasts long enough for the host receiver to frame the
 *   transition as a byte, so any traffic on the port means execution reached
 *   the call site.
 *
 *   The pin is reclaimed from whatever peripheral owns it by writing the
 *   config register outright: bit 6 clear hands it back to the GPIO block,
 *   and bit 3 -- output disable, low active -- must stay clear for the pad to
 *   drive at all.
 *
 ****************************************************************************/

static void bk7258_txpin_forever(void)
{
  volatile uint32_t *tx = (volatile uint32_t *)BK7258_GPIO_CFG(11);
  uint32_t phase = 0;

  for (; ; )
    {
      volatile uint32_t spin;

      *tx = phase ? GPIO_CFG_OUTPUT : 0;

      for (spin = 0; spin < 2000; spin++)
        {
        }

      phase ^= 1;
    }
}
#endif

/****************************************************************************
 * Public Data
 ****************************************************************************/

/* The IDLE thread stack sits immediately above .bss; the heap starts above
 * it.  up_allocate_heap() in bk7258_allocateheap.c uses this value.
 */

const uintptr_t g_idle_topstack = HEAP_BASE;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: __start
 *
 * Description:
 *   This is the reset entry point.  It is reached from the reset vector in
 *   arch/arm/src/arm_m/arm_vectors.c after the stack pointers have been
 *   initialised.
 *
 *   The BK7258 boot ROM and the Beken second-stage bootloader have already
 *   brought up the flash controller and mapped this image for
 *   execute-in-place before control arrives here, so the only memory setup
 *   still required is the usual .data/.bss preparation.
 *
 ****************************************************************************/

void __start(void)
{
  const uint32_t *src;
  uint32_t *dest;

  /* No probe here.
   *
   * This used to write one character to each of UART0's two aliases, to find
   * out which security state the bootloader handed over in.  That question is
   * settled -- the vendor SoC config builds the application for the secure
   * environment, so peripherals live at 0x44xxxxxx and this port addresses
   * them correctly -- and the probe itself was never sound: it touched a
   * peripheral before this file configures anything, including the alias the
   * answer said we do not use.
   */


#ifdef CONFIG_BK7258_LED_PROBE
  /* Bring-up probe: does the bootloader actually hand control to this image?
   *
   * Drives three pins the board schematic ties to things a person can see or
   * feel, then loops.  GPIO9 is the one that removes all guesswork: the motor
   * driver on sheet 4 is wired to a net literally named "P9", so there is no
   * inference between net label and pin number.  GPIO40/GPIO41 are the likely
   * LED1/LED2 pins, but that mapping is read off label ordering rather than an
   * explicit pin number, so it is the weaker signal of the two.
   *
   * Uses only the always-on GPIO block: no clock gate, no pin mux, no UART,
   * and no assumption about whether SYSRESETREQ reboots this SoC.  All three
   * pins drive passive loads (LED resistors, a transistor base), so nothing
   * here can contend with another device.
   */

  {
    volatile uint32_t *motor = (volatile uint32_t *)BK7258_GPIO_CFG(9);
    volatile uint32_t *led1  = (volatile uint32_t *)BK7258_GPIO_CFG(40);
    volatile uint32_t *led2  = (volatile uint32_t *)BK7258_GPIO_CFG(41);
    uint32_t phase = 0;

    /* Release any deep-sleep GPIO retention lock first: a locked pad ignores
     * its config register completely.  A cold boot should leave this clear,
     * but this probe has already been fooled twice by pins that could not
     * move, so it is not worth assuming.  Bit 31 of AON_PMU r0 is the lock,
     * and it only takes effect after the two-word commit key, which is what
     * the vendor's sys_hal_gpio_state_switch() writes.
     */

    putreg32(getreg32(BK7258_AON_PMU_BASE) & ~(1u << 31), BK7258_AON_PMU_BASE);
    putreg32(0x424b55aa, BK7258_AON_PMU_BASE + 0x94);
    putreg32(0xbdb4aa55, BK7258_AON_PMU_BASE + 0x94);

    /* Power the 3.3V rail before expecting the motor to move: GPIO52 is net
     * LDO33_EN (pin 6 = P52), and the motor shares that rail with the LCD and
     * the SD NAND.  The two LEDs sit on VIO and do not need it, which is why
     * they stay the more trustworthy half of this probe.
     */

    putreg32(GPIO_CFG_OUTPUT, BK7258_GPIO_CFG(52));

    for (; ; )
      {
        volatile uint32_t spin;

        /* GPIO_CFG_OUTPUT_DIS is low active, so a driven pad needs it clear.
         * Setting it -- which is what an earlier version of this probe did,
         * under the name GPIO_CFG_OUTPUT_EN -- parks the pad in high-Z, and a
         * high-Z pad cannot light an LED or turn the motor transistor on no
         * matter how correctly the rest of this function runs.
         */

        uint32_t hi = GPIO_CFG_OUTPUT;
        uint32_t lo = 0;

        *motor = phase ? hi : lo;
        *led1  = phase ? hi : lo;
        *led2  = phase ? lo : hi;

        for (spin = 0; spin < 2000000; spin++)
          {
          }

        phase ^= 1;
      }
  }
#endif

#ifdef CONFIG_BK7258_TXPIN_PROBE
  /* Bring-up probe: does the bootloader actually hand control to this image?
   *
   * Bit-bangs GPIO11, which the datasheet and the schematic agree is UART0 TX
   * and therefore wired to the CH340 bridge.  Each level lasts about a
   * millisecond, which is roughly a hundred bit times at 115200, so the host
   * receiver frames it as a stream of bytes rather than staying idle.  Any
   * byte arriving on the port proves this code executes.
   *
   * This is the probe with the fewest assumptions behind it: it needs no
   * clock gate, no pin mux, no UART peripheral, no power rail, and nobody
   * watching the board.  What it does need is the right polarity on bit 3 --
   * see GPIO_CFG_OUTPUT_DIS -- which is what defeated every earlier attempt.
   */

  bk7258_txpin_forever();
#endif

#ifdef CONFIG_BK7258_SELFRESET_PROBE
  /* Bring-up probe: does the bootloader actually hand control to this image?
   *
   * Nothing that depends on a working UART can answer that, because a silent
   * console is exactly the symptom under investigation.  Instead, sit for
   * roughly half a second and then reset.  If this code runs, the board loops
   * through the bootloader about twice a second, so its UART download window
   * reopens continuously and tools/bk_flash.py links up without anyone
   * touching the RST button.  If a manual reset is still required, this code
   * never ran.
   *
   * The reset goes through the watchdog, not through ARMv8-M's SYSRESETREQ.
   * Nothing in the vendor SDK ever writes AIRCR -- its own bk_reboot() arms
   * the watchdog with a short period and spins -- and the Beken bootloader
   * reboots the same way, so SYSRESETREQ evidently does not reset this SoC.
   * An earlier version of this probe used it and read the resulting silence
   * as "the image never ran".
   *
   * Both watchdogs are armed the way the bootloader does it: unlock with 0x5A
   * in the top byte, commit with 0xA5, period in the low bits.  No pin is
   * driven, so there is no way for this to fight another device on the board.
   */

  {
    volatile uint32_t spin;
    uint32_t regval;

    for (spin = 0; spin < 3000000; spin++)
      {
      }

    regval = getreg32(BK7258_AON_PMU_BASE + 8);
    putreg32((regval & ~0x3f) | 0x26, BK7258_AON_PMU_BASE + 8);

    putreg32(0x5a0000 | 6, BK7258_WDT_BASE + 0x10);
    putreg32(0xa50000 | 6, BK7258_WDT_BASE + 0x10);
    putreg32(0x5a0000 | 6, BK7258_AON_WDT_BASE);
    putreg32(0xa50000 | 6, BK7258_AON_WDT_BASE);

    for (; ; );
  }
#endif

  txmark();   /* trace 1: reached __start */

  /* Keep interrupts masked until nx_start() is ready to take them. */

  __asm__ __volatile__ ("cpsid i" : : : "memory");

  /* Point the vector table at our own vectors.  The bootloader will have
   * left VTOR pointing at its own table.
   */

  putreg32((uint32_t)_vectors, NVIC_VECTAB);

  /* The STAR-MC1 core carries real architectural L1 caches and the boot
   * chain leaves them OFF -- discovered when JPEG decode would not scale
   * with the core clock (XIP instruction fetches were the wall, ~2 s per
   * frame at any frequency).  Enable the ICACHE: invalidate-all, then
   * CCR.IC.  The DCACHE stays off until every DMA consumer does proper
   * maintenance.
   */

  putreg32(0, 0xe000ef50);                          /* ICIALLU */
  __asm__ __volatile__ ("dsb\n isb" : : : "memory");
  putreg32(getreg32(0xe000ed14) | (1u << 17), 0xe000ed14);
  __asm__ __volatile__ ("dsb\n isb" : : : "memory");

  earlymark('2');

  txmark();   /* trace 1b: VTOR set, FPU not yet touched */

  /* Enable the FPU before any code that might touch floating point. */

#ifdef CONFIG_ARCH_FPU
  arm_fpuconfig();
#endif

  earlymark('3');

  txmark();   /* trace 2: VTOR set, FPU on */

  /* Clear .bss.  The linker guarantees both bounds are 4-byte aligned. */

  for (dest = (uint32_t *)_sbss; dest < (uint32_t *)_ebss; )
    {
      *dest++ = 0;
    }

  /* Copy .data from its load address in flash into SRAM. */

  for (src = (const uint32_t *)_eronly, dest = (uint32_t *)_sdata;
       dest < (uint32_t *)_edata;
      )
    {
      *dest++ = *src++;
    }

  earlymark('4');

  txmark();   /* trace 3: .bss cleared, .data copied */

  /* Bring up the clocks we depend on, then the debug console so that any
   * failure after this point is visible on the serial port.
   */

  bk7258_clockconfig();

  earlymark('5');

  txmark();   /* trace 4: bk7258_clockconfig() returned */

  bk7258_lowsetup();

  earlymark('6');

  /* Arm the watchdog only now that the console is up and the '6' marker is
   * out.  The first attempt armed it at the very top of __start(); the board
   * then went completely silent, and with no marker before the arming there
   * was no telling whether the write wedged the bus or the period ran out
   * before the SysTick feeder started.  Arming after the console gives every
   * future life a visible '6' first -- a silent board now means death before
   * this line, a rebooting board means the dog fired after it.
   */

  bk7258_wdt_arm(BK7258_WDT_PERIOD_RUN);

  earlymark('W');

  /* Black-box playback.  The serial interrupt handler drops breadcrumbs
   * just above the linked SRAM region -- outside everything this image
   * links or heaps, and preserved across a watchdog reset.  If the previous
   * life ended in the wedge this port is chasing, its last moments are
   * still there; print them before this run overwrites anything.
   */

  {
    extern uint32_t _bbnote[];
    volatile uint32_t *bb = (volatile uint32_t *)_bbnote;

    if (bb[0] == 0xb1acb0c5)
      {
        static const char hex[] = "0123456789abcdef";
        uint32_t seq = bb[1];
        int i;
        int k;

        arm_lowputc('B');
        arm_lowputc('B');
        arm_lowputc('=');

        for (k = 28; k >= 0; k--)
          {
            uint32_t w = bb[2 + ((seq - k) % 29)];

            for (i = 28; i >= 0; i -= 4)
              {
                arm_lowputc(hex[(w >> i) & 0xf]);
              }

            arm_lowputc(k == 0 ? '\n' : ' ');
          }
      }

    bb[0] = 0xb1acb0c5;
    bb[1] = 0;
  }

#ifdef CONFIG_BK7258_UART_DUMP
  /* Ask the console UART four yes/no questions and shift the answers out on
   * GPIO11 -- long burst for yes, short for no -- forever.
   *
   * An earlier version tried to shift whole registers out bit by bit.  The
   * channel could not carry it: a short burst makes too few framing errors to
   * register, and neighbouring bits ran together, so the recovered words were
   * noise dressed up as data.  Four well-separated answers survive the trip.
   */

  for (; ; )
    {
      volatile uint32_t spin;
      uint32_t cfg = getreg32(BK7258_CONSOLE_BASE + BK7258_UART_CONFIG_OFFSET);
      uint32_t sts = getreg32(BK7258_CONSOLE_BASE +
                              BK7258_UART_FIFO_STATUS_OFFSET);
      uint32_t i;

      /* 1: does the block answer at all?  A zero id means no clock. */

      bk7258_txpin_answer(getreg32(BK7258_CONSOLE_BASE) != 0);

      /* 2: did our transmit-enable stick? */

      bk7258_txpin_answer((cfg & UART_CONFIG_TX_ENABLE) != 0);

      /* 3: did our baud divisor stick? */

      bk7258_txpin_answer(((cfg & UART_CONFIG_CLKDIV_MASK) >>
                           UART_CONFIG_CLKDIV_SHIFT) ==
                          UART_CLKDIV(BK7258_CONSOLE_BAUD));

      /* 4: will the FIFO take a byte?  This is what arm_lowputc() waits on. */

      bk7258_txpin_answer((sts & UART_FIFO_STATUS_WR_READY) != 0);

      for (i = 0; i < 1200; i++)
        {
          for (spin = 0; spin < 1500; spin++)
            {
            }
        }
    }
#endif

  /* No marker past this point: bk7258_lowsetup() has handed GPIO11 to the
   * UART, and the bit-bang marker reclaims the pad by clearing the pin's
   * peripheral-function bit.  Marking here would silence the console it is
   * supposed to be reporting on.  Use CONFIG_BK7258_EARLY_MARKERS instead,
   * which writes through the UART rather than around it.
   */

  showprogress('A');

  /* Perform board-specific initialisation that has to happen before the
   * NuttX subsystems come up.
   */

#ifdef CONFIG_BOARD_EARLY_INITIALIZE
  board_early_initialize();
#endif


  showprogress('B');

#ifdef CONFIG_BK7258_TXPIN_PROBE_LATE
  /* Bring-up probe: did everything this file does survive?
   *
   * Sits between the last chip-level setup and nx_start(), so it splits the
   * boot in two.  Traffic on the port means the clocks, the console setup and
   * board_early_initialize() all completed and the fault is inside NuttX;
   * silence means it is still in this file, above here.
   */

  bk7258_txpin_forever();
#endif

  /* Start NuttX.  This does not return. */

  nx_start();

  showprogress('X');

  for (; ; );
}
