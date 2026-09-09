/****************************************************************************
 * boards/arm/bk7258/contest_board/chip/bk7258_ble_shim.c
 *
 * The closed Beken BLE/PHY libraries link against a handful of direct
 * symbols (everything else is injected through function-pointer tables
 * at runtime).  These are their NuttX implementations: the beken rtos_*
 * critical-section and mutex primitives, a microsecond delay, and two
 * harmless vendor hooks.
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/mutex.h>
#include <nuttx/kmalloc.h>
#include <stdint.h>

#include "arm_internal.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* Beken semantics: disable returns the saved state, enable restores it. */

/****************************************************************************
 * Critical-section trace
 *
 * Temporary, for the rxsens hang: the board goes silent after
 * "[RS]reset_mm" with the console and the scheduler both dead, which is
 * what a spin with interrupts masked looks like.  This says whether the
 * disable/enable calls balance, and where the last unmatched one came from.
 *
 * Output goes through up_putc(), not syslog().  syslog needs the character
 * driver and therefore interrupts; up_putc is the OS's polled low-level
 * path and is the only thing that still works inside the section being
 * investigated.
 *
 * Armed by bk7258_int_trace(true) from the rxsens command, so boot does not
 * spend the budget.
 ****************************************************************************/

static int  g_bk_int_depth;
static int  g_bk_int_budget;

static void bk_trace_word(char tag, uintptr_t v)
{
  static const char hex[] = "0123456789abcdef";
  int i;

  up_putc(tag);
  for (i = 28; i >= 0; i -= 4)
    {
      up_putc(hex[(v >> i) & 0xf]);
    }

  up_putc('\r');
  up_putc('\n');
}

void bk7258_int_trace(bool on)
{
  g_bk_int_budget = on ? 120 : 0;
  g_bk_int_depth  = 0;
}

uint32_t rtos_disable_int(void)
{
  if (g_bk_int_budget > 0)
    {
      g_bk_int_budget--;
      g_bk_int_depth++;
      bk_trace_word('D', (uintptr_t)__builtin_return_address(0));
      bk_trace_word('d', (uintptr_t)g_bk_int_depth);
    }

  return (uint32_t)up_irq_save();
}

void rtos_enable_int(uint32_t flags)
{
  if (g_bk_int_budget > 0)
    {
      g_bk_int_budget--;
      g_bk_int_depth--;
      bk_trace_word('E', (uintptr_t)__builtin_return_address(0));
    }

  up_irq_restore((irqstate_t)flags);
}

/* beken_mutex_t is an opaque void *; init allocates, lock/unlock operate.
 * Return convention: 0 = ok (bk_err_t).
 */

int rtos_init_mutex(void **mutex)
{
  mutex_t *m = kmm_malloc(sizeof(mutex_t));

  if (m == NULL)
    {
      return -1;
    }

  nxmutex_init(m);
  *mutex = m;
  return 0;
}

int rtos_lock_mutex(void **mutex)
{
  return nxmutex_lock((mutex_t *)*mutex);
}

int rtos_unlock_mutex(void **mutex)
{
  return nxmutex_unlock((mutex_t *)*mutex);
}

/* Only the WiFi stack tears mutexes down; BLE holds its own for the life of
 * the image.  It belongs here anyway, next to the allocation it undoes.
 */

int rtos_deinit_mutex(void **mutex)
{
  if (mutex == NULL || *mutex == NULL)
    {
      return -1;
    }

  nxmutex_destroy((mutex_t *)*mutex);
  kmm_free(*mutex);
  *mutex = NULL;
  return 0;
}

void delay_us(uint32_t us)
{
  up_udelay(us);
}

/* Vendor hooks the controller calls directly; nothing to do on NuttX.
 * bk_uart_recover_rx_isr re-arms the DUT UART after HCI passthrough;
 * bk_set_printf_sync toggles synchronous logging.
 */

void bk_uart_recover_rx_isr(void)
{
}

void bk_set_printf_sync(uint8_t enable)
{
  (void)enable;
}

/****************************************************************************
 * Name: sys_drv_module_power_state_get
 *
 * Description:
 *   Report whether a power domain is switched off.  The closed PHY's
 *   rfconfig switch calls this directly; the module number is the bit
 *   index in the sleep/wakeup word, and the bit reads 1 when the domain
 *   is powered down, matching the vendor's HAL.
 *
 ****************************************************************************/

int sys_drv_module_power_state_get(uint32_t module)
{
  return (int)((getreg32(0x44010040ul) >> module) & 1u);
}
