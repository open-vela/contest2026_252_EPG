/****************************************************************************
 * board/contest_board/chip/bk7258_irq.c
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
#include <assert.h>
#include <debug.h>

#include <nuttx/irq.h>
#include <nuttx/arch.h>

#include "arm_internal.h"
#include "nvic.h"
#include "ram_vectors.h"

#include "chip.h"
#include "bk7258_memorymap.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Default priority applied to every interrupt at start of day */

#define DEFPRIORITY32 \
  (NVIC_SYSH_PRIORITY_DEFAULT << 24 | NVIC_SYSH_PRIORITY_DEFAULT << 16 | \
   NVIC_SYSH_PRIORITY_DEFAULT << 8  | NVIC_SYSH_PRIORITY_DEFAULT)

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_prioritize_syscall
 *
 * Description:
 *   Give SVCall its own priority.  Needed even when prioritised interrupt
 *   support is not configured, because BASEPRI masking relies on SVCall
 *   sitting above the masked range.
 *
 ****************************************************************************/

static inline void bk7258_prioritize_syscall(int priority)
{
  uint32_t regval;

  /* SVCall is system handler 11 */

  regval  = getreg32(NVIC_SYSH8_11_PRIORITY);
  regval &= ~NVIC_SYSH_PRIORITY_PR11_MASK;
  regval |= (uint32_t)priority << NVIC_SYSH_PRIORITY_PR11_SHIFT;
  putreg32(regval, NVIC_SYSH8_11_PRIORITY);
}

/****************************************************************************
 * Name: bk7258_exception_bit
 *
 * Description:
 *   Map one of the maskable processor exceptions onto the register and bit
 *   that controls it.  Returns ERROR for exceptions that cannot be masked.
 *
 ****************************************************************************/

static int bk7258_exception_bit(int irq, uintptr_t *regaddr, uint32_t *bit)
{
  *regaddr = NVIC_SYSHCON;

  switch (irq)
    {
      case NVIC_IRQ_MEMFAULT:
        *bit = NVIC_SYSHCON_MEMFAULTENA;
        break;

      case NVIC_IRQ_BUSFAULT:
        *bit = NVIC_SYSHCON_BUSFAULTENA;
        break;

      case NVIC_IRQ_USAGEFAULT:
        *bit = NVIC_SYSHCON_USGFAULTENA;
        break;

      case NVIC_IRQ_SYSTICK:
        *regaddr = NVIC_SYSTICK_CTRL;
        *bit     = NVIC_SYSTICK_CTRL_ENABLE;
        break;

      default:
        return ERROR;
    }

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: up_irqinitialize
 ****************************************************************************/

void up_irqinitialize(void)
{
  int num_priority_registers;
  uintptr_t regaddr;
  int i;

  /* Disable and clear every peripheral interrupt, at both gates.  Clearing
   * the SoC routing matrix as well as the NVIC matters because the Beken
   * bootloader hands over with its own lines still routed -- UART0 at least
   * -- and up_disable_irq() clears both, so an init that only cleared the
   * NVIC would leave the two gates disagreeing for every line we never
   * touch.  Nothing is lost by clearing it: up_enable_irq() sets the matrix
   * bit back when a driver claims the line.
   */

  for (i = 0; i < BK7258_IRQ_NEXTINTS; i += 32)
    {
      putreg32(0xffffffff, NVIC_IRQ_CLEAR(i));
      putreg32(0xffffffff, NVIC_IRQ_CLRPEND(i));
      putreg32(0, BK7258_SYS_CPU0_INT_EN(i));
    }

  /* Point the NVIC at our vector table. */

  putreg32((uint32_t)_vectors, NVIC_VECTAB);

#ifdef CONFIG_ARCH_RAMVECTORS
  up_ramvec_initialize();
#endif

  /* Give the configurable system exceptions the default priority. */

  putreg32(DEFPRIORITY32, NVIC_SYSH4_7_PRIORITY);
  putreg32(DEFPRIORITY32, NVIC_SYSH8_11_PRIORITY);
  putreg32(DEFPRIORITY32, NVIC_SYSH12_15_PRIORITY);

  /* NVIC_ICTR bits 0-4 report how many 32-line blocks the NVIC implements,
   * which is eight priority registers per block.
   */

  num_priority_registers = (getreg32(NVIC_ICTR) + 1) * 8;

  regaddr = NVIC_IRQ0_3_PRIORITY;
  while (num_priority_registers--)
    {
      putreg32(DEFPRIORITY32, regaddr);
      regaddr += 4;
    }

  /* Catch memory management faults so a bad access reports instead of
   * silently escalating to a hard fault.
   */

#ifdef CONFIG_ARM_MPU
  up_enable_irq(NVIC_IRQ_MEMFAULT);
#endif

#ifdef CONFIG_DEBUG_FEATURES
  modifyreg32(NVIC_SYSHCON, 0,
              NVIC_SYSHCON_USGFAULTENA | NVIC_SYSHCON_BUSFAULTENA |
              NVIC_SYSHCON_MEMFAULTENA);
#endif

  /* Attach the SVCall and hard fault handlers.  Setting the SVCall priority
   * below is not enough on its own: without these two attaches the vector
   * slots still hold irq_unexpected_isr, and the very first context switch
   * raises SVCall.  The symptom of leaving them out is a shell that prints
   * its banner and then ignores every keystroke -- boot-time code runs, but
   * any wakeup that needs the scheduler dies in "ERROR irq: 11".
   */

  irq_attach(NVIC_IRQ_SVCALL, arm_svcall, NULL);
  irq_attach(NVIC_IRQ_HARDFAULT, arm_hardfault, NULL);

  /* SVCall has to stay above the BASEPRI mask level.  Both values come from
   * arch/arm_m/nvicpri.h: SVCall lands one step below the default (0x40)
   * while up_irq_save() raises BASEPRI to NVIC_SYSH_DISABLE_PRIORITY, which
   * is the default itself (0x80).  Every peripheral interrupt is left at the
   * default by the loop above, so a critical section masks all of them and
   * still lets a system call through.
   */

  bk7258_prioritize_syscall(NVIC_SYSH_SVCALL_PRIORITY);

#ifndef CONFIG_SUPPRESS_INTERRUPTS
  up_irq_enable();
#endif
}

/****************************************************************************
 * Name: up_disable_irq
 ****************************************************************************/

void up_disable_irq(int irq)
{
  uintptr_t regaddr;
  uint32_t regval;
  uint32_t bit;
  int n;

  DEBUGASSERT(irq >= NVIC_IRQ_NMI && irq < NR_IRQS);

  if (irq >= NVIC_IRQ_FIRST)
    {
      /* The clear-enable registers are write-1-to-clear. */

      n = irq - NVIC_IRQ_FIRST;
      putreg32((uint32_t)1 << (n & 0x1f), NVIC_IRQ_CLEAR(n));

      /* Take the line out of the SoC routing matrix as well; see
       * up_enable_irq() for why the matrix exists.
       */

      modifyreg32(BK7258_SYS_CPU0_INT_EN(n), (uint32_t)1 << (n & 0x1f), 0);
    }
  else if (bk7258_exception_bit(irq, &regaddr, &bit) == OK)
    {
      regval  = getreg32(regaddr);
      regval &= ~bit;
      putreg32(regval, regaddr);
    }
}

/****************************************************************************
 * Name: up_enable_irq
 ****************************************************************************/

void up_enable_irq(int irq)
{
  uintptr_t regaddr;
  uint32_t regval;
  uint32_t bit;
  int n;

  DEBUGASSERT(irq >= NVIC_IRQ_NMI && irq < NR_IRQS);

  if (irq >= NVIC_IRQ_FIRST)
    {
      /* The set-enable registers are write-1-to-set. */

      n = irq - NVIC_IRQ_FIRST;
      putreg32((uint32_t)1 << (n & 0x1f), NVIC_IRQ_ENABLE(n));

      /* The NVIC is not the only gate on this SoC.  Interrupt lines pass
       * through a per-CPU routing matrix in the system block first --
       * cpu0_int_0_31_en / cpu0_int_32_63_en -- and a line whose matrix bit
       * is clear never reaches the NVIC at all, no matter what the NVIC
       * enable says.  The Beken bootloader sets bit 4 (UART0) there as the
       * last step of bringing its console up and clears it again on the way
       * out, which is how this port booted to a shell whose transmit worked
       * while every received byte vanished: polled TX needs no interrupt,
       * RX does.  The bit index matches the NVIC line number.
       */

      modifyreg32(BK7258_SYS_CPU0_INT_EN(n), 0, (uint32_t)1 << (n & 0x1f));
    }
  else if (bk7258_exception_bit(irq, &regaddr, &bit) == OK)
    {
      regval  = getreg32(regaddr);
      regval |= bit;
      putreg32(regval, regaddr);
    }
}

/****************************************************************************
 * Name: arm_ack_irq
 *
 * Description:
 *   Acknowledge the interrupt.  The Cortex-M NVIC clears the pending bit
 *   automatically when the handler is entered, so there is nothing to do.
 *
 ****************************************************************************/

void arm_ack_irq(int irq)
{
}

/****************************************************************************
 * Name: up_prioritize_irq
 ****************************************************************************/

#ifdef CONFIG_ARCH_IRQPRIO
int up_prioritize_irq(int irq, int priority)
{
  uintptr_t regaddr;
  uint32_t regval;
  int shift;

  DEBUGASSERT(irq >= NVIC_IRQ_MEMFAULT && irq < NR_IRQS &&
              (unsigned)priority <= NVIC_SYSH_PRIORITY_MIN);

  if (irq < NVIC_IRQ_FIRST)
    {
      /* The system handler priority registers start at exception 4. */

      regaddr = NVIC_SYSH_PRIORITY(irq);
      irq    -= 4;
    }
  else
    {
      irq    -= NVIC_IRQ_FIRST;
      regaddr = NVIC_IRQ_PRIORITY(irq);
    }

  shift   = ((irq & 3) << 3);
  regval  = getreg32(regaddr);
  regval &= ~((uint32_t)0xff << shift);
  regval |= ((uint32_t)priority << shift);
  putreg32(regval, regaddr);

  return OK;
}
#endif /* CONFIG_ARCH_IRQPRIO */
