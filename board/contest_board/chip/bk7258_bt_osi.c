/****************************************************************************
 * boards/arm/bk7258/contest_board/chip/bk7258_bt_osi.c
 *
 * OS abstraction table for the closed Beken bluetooth controller.
 *
 * The controller archive owns no OS code: everything it needs -- queues,
 * threads, timers, locks, power and clock gates, the interrupt hookup and
 * the PHY calibration entry points -- arrives through one function-pointer
 * table handed to bt_os_adapter_init().  The library reads that table by
 * memory layout, so struct bt_osi_funcs_t below is a byte-for-byte replica
 * of bk_idk components/bk_bluetooth/include/private/bt_os_adapter.h; the
 * SDK header itself is deliberately not included, because this port does
 * not carry the Beken header tree.  Field order and types must not be
 * touched, and the version/size pair is the handshake the library checks
 * before it accepts the table.
 *
 * The implementations are re-derived from the vendor's FreeRTOS reference
 * (bk_idk components/bk_bluetooth/soc/bk7236/bluetooth.c) onto NuttX
 * primitives.  Where the vendor calls into an SDK subsystem this port does
 * not have -- power management, coexistence, the UART passthrough used by
 * production test -- the field is a documented stub rather than a guess.
 *
 * Nothing here runs unless bk7258_bt_osi_init() is called; it is not wired
 * into board bring-up yet.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/clock.h>
#include <nuttx/irq.h>
#include <nuttx/kmalloc.h>
#include <nuttx/kthread.h>
#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>
#include <nuttx/wqueue.h>

#include "arm_internal.h"
#include "chip.h"
#include "bk7258_gpio.h"
#include "bk7258_memorymap.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Handshake the library validates before it uses the table at all. */

#define BT_OSI_VERSION              0x00010001

/* Vendor timeout encoding (bk_idk include/os/os.h). */

#define BT_OSI_NO_WAIT              (0)
#define BT_OSI_WAIT_FOREVER         (0xffffffffu)

/* Beken/FreeRTOS priorities run the opposite way from NuttX: the vendor
 * layer converts with BK_PRIORITY_TO_NATIVE_PRIORITY(p), which is
 * (configMAX_PRIORITIES - 1) - p with configMAX_PRIORITIES == 10, so
 * beken 0 is the most urgent and beken 9 the least.  NuttX counts up
 * instead, so the mapping is a reflection plus an offset.
 *
 * The offset places the whole band above every application thread this
 * board runs (nsh and the system work queue sit at 100..110) and below
 * CONFIG_SCHED_HPWORKPRIORITY (224), which is where the OSI timer
 * callbacks below execute.  Keeping the timers above the controller
 * threads reproduces the FreeRTOS arrangement, where the timer daemon
 * preempts the stack tasks, and keeping the controller below HPWORK means
 * a spinning controller thread cannot starve NuttX's own deferred work.
 */

#define BT_OSI_BEKEN_PRIO_MAX       9
#define BT_OSI_PRIO_BASE            200

/* System block gates.  Power bits are inverted: 0 powers the domain up. */

#define BT_OSI_SYS_POWER            (BK7258_SYS_BASE + 0x40)
#define BT_OSI_PWD_BTSP             (1u << 8)
#define BT_OSI_PWD_WIFP_PHY         (1u << 10)

#define BT_OSI_SYS_CLK_EN           (BK7258_SYS_BASE + 0x30)
#define BT_OSI_CKEN_BTDM            (1u << 24)
#define BT_OSI_CKEN_XVR             (1u << 25)

/* Registers the library asks for by address so it can poke them itself. */

#define BT_OSI_SYS_DEBUG_CFG0       (BK7258_SYS_BASE + (0x38 << 2))
#define BT_OSI_SYS_DEBUG_CFG1       (BK7258_SYS_BASE + (0x39 << 2))
#define BT_OSI_AON_PMU_CHIPID       (BK7258_AON_PMU_BASE + (0x7c << 2))

/* Chip identity constants, copied from the SDK's bk7258 sys_types.h. */

#define BT_OSI_CHIP_ID_MASK         0xffff0000ul
#define BT_OSI_CHIP_ID_MPW_V4       0x22c20010ul

/* RF arbitration bits (bk_idk components/bk_phy/include/bk_rf_internal.h) */

#define BT_OSI_RF_BY_BLE_BIT        (1u << 1)
#define BT_OSI_RF_BY_ATE_BT_BIT     (1u << 4)

/* The rate index the vendor files BLE calibration results under. */

#define BT_OSI_EVM_BLE_RATE         158

/* Replicated from bt_os_adapter.h -- the library and this file must agree
 * on these encodings even though the header is not included.
 */

#define BLUETOOTH_RF_PLL_WIFI       0x1
#define BLUETOOTH_RF_PLL_MASK       0xf
#define BLUETOOTH_RF_MODE_POLAR     0x10
#define BLUETOOTH_RF_MODE_MASK      0xf0

#define BLUETOOTH_CLK_32K           32000
#define BLUETOOTH_CLK_32768         32768

enum
{
  BLUETOOTH_INT_SRC_BTDM = 0x00,
  BLUETOOTH_INT_SRC_BLE  = 0x01,
  BLUETOOTH_INT_SRC_BT   = 0x02,
};

enum
{
  BT_LPO_SRC_DIVD = 0,          /* 32K divided down from the 26 MHz xtal */
  BT_LPO_SRC_X32K,              /* External 32.768 kHz crystal          */
  BT_LPO_SRC_ROSC,              /* 32K from the internal RC oscillator  */
  BT_LPO_SRC_DEFAULT
};

enum
{
  BT_RF_MODE_WIFI = 1,
  BT_RF_MODE_POLAR,
};

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Verbatim replica of struct bt_osi_funcs_t.  Do not reorder or retype. */

struct bt_osi_funcs_t
{
  uint32_t _version;
  uint32_t size;

  int (*_init_queue)(void **queue, const char *name, uint32_t message_size,
                     uint32_t number_of_messages);
  int (*_deinit_queue)(void **queue);
  int (*_pop_from_queue)(void **queue, void *message, uint32_t timeout_ms);
  int (*_push_to_queue)(void **queue, void *message, uint32_t timeout_ms);
  int (*_create_thread)(void **thread, uint8_t priority, const char *name,
                        void *function, uint32_t stack_size, void *arg);
  int (*_delete_thread)(void **thread);
  int (*_thread_join)(void **thread);

  int32_t (*_init_mutex)(void **mutex);
  int32_t (*_lock_mutex)(void **mutex);
  int32_t (*_unlock_mutex)(void **mutex);
  int32_t (*_deinit_mutex)(void **mutex);

  int32_t (*_init_semaphore)(void **semaphore, int32_t max_count);
  int32_t (*_set_semaphore)(void **semaphore);
  int32_t (*_get_semaphore)(void **semaphore, uint32_t timeout_ms);
  int32_t (*_deinit_semaphore)(void **semaphore);

  int32_t (*_init_timer)(void **timer, uint32_t time_ms, void *function,
                         void *arg);
  int32_t (*_init_timer_ext)(void **timer, uint32_t time_ms, void *function,
                             void *arg, bool oneshot);
  int32_t (*_deinit_timer)(void *timer);
  int32_t (*_stop_timer)(void *timer);

  int32_t (*_timer_change_period)(void *timer, uint32_t time_ms);
  bool (*_is_timer_init)(void *timer);
  int32_t (*_start_timer)(void *timer);
  bool (*_is_timer_running)(void *timer);

  void (*_log)(int level, char *tag, const char *fmt, ...);
  uint32_t (*_get_time)(void);
  int (*_delay_milliseconds)(uint32_t num_ms);

  void *(*_malloc)(unsigned int size);
  void (*_free)(void *p);

  int (*_coex_init)(void);
  int (*_bluetooth_int_isr_register)(uint8_t type, void *isr, void *arg);

  void (*_controller_mem_init)(void);
  int (*_bluetooth_deep_sleep_register)(void *enter_config_cb);
  int (*_bluetooth_extern32k_register)(void *switch_cb);
  int (*_bluetooth_extern32k_unregister)(void);
  uint8_t (*_clk_32k_customer_config_get)(void);
  int (*_lpo_src_set)(uint8_t lpo_src);
  uint8_t (*_lpo_src_get)(void);
  int (*_bluetooth_power_ctrl)(uint8_t power_state);
  int (*_phy_power_ctrl)(uint8_t power_state);
  int (*_bt_mac_clock_ctrl)(uint8_t clock_state);
  int (*_bt_phy_clock_ctrl)(uint8_t clock_state);
  void (*_btdm_interrupt_ctrl)(bool en);
  void (*_ble_interrupt_ctrl)(bool en);
  void (*_bt_interrupt_ctrl)(bool en);
  bool (*_ate_is_enabled)(void);
  bool (*_cp_test_is_enabled)(void);
  int (*_get_bluetooth_mac)(uint8_t *mac);
  int (*_uart_write_byte)(uint8_t id, uint8_t data);
  void (*_register_ble_dump_hook)(void *ble_func);
  uint8_t (*_get_ble_pwr_idx)(uint8_t channel);
  void (*_ble_cal_set_txpwr)(uint8_t idx);
  void (*_ble_cal_recover_txpwr)(void);
  void (*_ble_cal_enter_txpwr)(void);
  int (*_gpio_dev_unmap)(uint32_t gpio_id);
  int (*_gpio_dev_map)(uint32_t gpio_id, uint32_t func);
  void (*_set_printf_enable)(uint8_t enable);
  void (*_evm_stop_bypass_mac)(void);
  void (*_rs_deinit)(void);
  void (*_phy_enable_rx_switch)(void);
  int (*_uart_enable_rx_interrupt)(uint8_t id);
  int (*_uart_take_rx_isr)(uint8_t id, void *isr, void *param);
  void (*_ble_vote_rf_ctrl)(uint8_t cmd);
  void (*_ble_ate_vote_rf_ctrl)(uint8_t cmd);

  uint32_t (*_disable_int)(void);
  void (*_enable_int)(uint32_t int_level);
  int (*_gpio_disable_pull)(uint32_t gpio_id);
  int (*_gpio_enable_output)(uint32_t gpio_id);
  int (*_gpio_set_output_high)(uint32_t gpio_id);
  int (*_gpio_set_output_low)(uint32_t gpio_id);
  int (*_gpio_pull_down)(uint32_t gpio_id);
  int (*_gpio_pull_up)(uint32_t gpio_id);
  int (*_get_printf_port)(void);
  void (*_uart_enable)(uint8_t uart_id, uint8_t enable, uint32_t band);
  void (*_enable_debug_gpio)(void);

  void (*_manual_cal_save_ble_txpwr)(uint32_t channel, uint32_t pwr_gain);

  uint32_t (*_bt_rf_pll_ctrl)(uint32_t set);
  void (*_reboot)(void);
  int (*_uart_read_byte_ex)(uint8_t id, uint8_t *ch);
  int (*_bt_vote_sleep_ctrl)(uint32_t sleep_state, uint32_t sleep_time);
  void (*_coexist_check_large_signal)(uint8_t switch2bt);
  void (*_bt_ext_wakeup_ctrl)(uint8_t enable);
  void (*_btsnoop)(uint8_t uart_id, uint8_t pkt_type, uint8_t is_rxed,
                   uint8_t *pkt, uint16_t pkt_len, uint8_t method);

  size_t (*_get_sys_debug_config_addr)(uint32_t index);
  uint32_t (*_get_chipid_mask)(void);
  uint32_t (*_get_chipid)(uint32_t ver);
  uint32_t (*_get_current_chipid)(void);
  uint32_t (*_get_test_rfconfig)(void);
  uint8_t (*_get_rf_mode)(void);

  int (*_bkreg_run_command)(const char *content, int cnt);

  uint32_t (*_manual_cal_txpwr_tab_ready_in_flash)(void);
  uint32_t (*_manual_cal_is_in_cali_mode)(void);
  double (*_bk_driver_get_rc32k_freq)(void);
  void (*_flush_dcache)(void);

  void (*_ble_enter_dut)(void);
  void (*_ble_exit_dut)(void);
  uint8_t (*_set_bluetooth_power_level)(float pwr_gain);
};

/* Ring buffer behind the OSI queue API.  NuttX message queues need a file
 * system name and a POSIX descriptor, which buys nothing here and makes
 * the timeout encoding awkward, so the queue is open-coded: the ring state
 * is protected by an interrupt lock (the only contention is the BLE ISR
 * against the controller threads), 'fill' counts queued messages for
 * blocked readers, and 'space' is a wakeup signal for blocked writers.
 */

struct bt_osi_queue_s
{
  uint8_t *buf;
  uint32_t msgsize;
  uint32_t capacity;
  uint32_t head;
  uint32_t tail;
  uint32_t used;
  sem_t    fill;
  sem_t    space;
};

/* The vendor entry point is void(*)(void *) in both the periodic and the
 * one-shot case; the one-shot flavour reaches it through a two-argument
 * shim that only forwards.  'exited' lets _thread_join() block on a plain
 * kernel thread, which NuttX otherwise offers no way to wait for.
 */

struct bt_osi_thread_s
{
  pid_t  pid;
  void (*entry)(void *arg);
  void  *arg;
  sem_t  exited;
};

/* OSI timers run on the high-priority work queue, not on a watchdog.
 * The vendor's timers are FreeRTOS software timers, so their callbacks
 * execute in the timer daemon *task*: the controller is free to take a
 * mutex or push to a queue with a timeout from inside one.  A NuttX wdog
 * callback runs in interrupt context, where all of that either asserts or
 * deadlocks, so wd_start() would silently change the contract.  HPWORK
 * keeps the callback in a schedulable context and is enabled on this
 * board (CONFIG_SCHED_HPWORK).
 */

struct bt_osi_timer_s
{
  struct work_s work;
  void        (*cb)(void *arg);
  void         *arg;
  uint32_t      ms;
  bool          oneshot;
  bool          running;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/* Closed PHY/calibration entries.  Every one of these was confirmed
 * present in bk_idk components/bk_libs/bk7258/libs/libbk_phy.a (and in
 * libcom_phy.a) with arm-none-eabi-nm before being declared here; a
 * missing symbol is a link failure, so nothing is declared on faith.
 */

extern uint8_t manual_cal_get_ble_pwr_idx(uint8_t channel);

static uint8_t bt_osi_ble_pwr_idx(uint8_t channel);
extern void ble_cal_set_txpwr(uint8_t idx);
extern void ble_cal_recover_txpwr(void);
extern void ble_cal_enter_txpwr(void);
extern void ble_cal_enter_dut(void);
extern void ble_cal_exit_dut(void);
extern uint8_t get_ble_txpwr_table_size(void);
extern void rf_module_vote_ctrl(uint8_t cmd, uint32_t module);
extern void manual_cal_save_txpwr(uint32_t rate, uint32_t channel,
                                  uint32_t pwr_gain);
extern uint32_t manual_cal_txpwr_tab_ready_in_flash(void);
extern uint32_t manual_cal_is_in_rftest_mode(void);
extern int manual_cal_set_tx_power(int standard, float power_dbm);

/* test_rfconfig is a 4-byte object; rwnx_rfconfig is only 2 (checked with
 * nm -S).  The vendor's bluetooth.c declares the latter uint32_t and so
 * reads two bytes past it -- harmless on their layout, but there is no
 * reason to copy the mistake.
 */

extern uint32_t test_rfconfig;
extern uint16_t rwnx_rfconfig;

/* Already implemented for the closed libraries in bk7258_ble_shim.c; the
 * OSI table reuses them rather than defining a second copy.
 */

extern uint32_t rtos_disable_int(void);
extern void rtos_enable_int(uint32_t flags);
extern int rtos_init_mutex(void **mutex);
extern int rtos_lock_mutex(void **mutex);
extern int rtos_unlock_mutex(void **mutex);

/* The controller's own entry point. */

extern int bt_os_adapter_init(void *osi_funcs);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bt_osi_wait_sem
 *
 * Description:
 *   Apply the vendor timeout encoding to a NuttX semaphore: 0 never
 *   blocks, 0xffffffff blocks forever, anything else is milliseconds.
 *   MSEC2TICK rounds up, so a millisecond count shorter than the 10 ms
 *   tick of this board still waits one tick instead of degenerating into
 *   a poll.  Waits are uninterruptible because the controller has no way
 *   to report or recover from EINTR.
 *
 ****************************************************************************/

static int bt_osi_wait_sem(sem_t *sem, uint32_t timeout_ms)
{
  if (timeout_ms == BT_OSI_NO_WAIT)
    {
      return nxsem_trywait(sem);
    }

  if (timeout_ms == BT_OSI_WAIT_FOREVER)
    {
      return nxsem_wait_uninterruptible(sem);
    }

  return nxsem_tickwait_uninterruptible(sem, MSEC2TICK(timeout_ms));
}

/****************************************************************************
 * Name: bt_osi_init_queue / bt_osi_deinit_queue
 ****************************************************************************/

static int bt_osi_init_queue(void **queue, const char *name,
                             uint32_t message_size,
                             uint32_t number_of_messages)
{
  struct bt_osi_queue_s *q;

  UNUSED(name);

  if (queue == NULL || message_size == 0 || number_of_messages == 0)
    {
      return -1;
    }

  q = kmm_zalloc(sizeof(struct bt_osi_queue_s));
  if (q == NULL)
    {
      return -1;
    }

  q->buf = kmm_malloc(message_size * number_of_messages);
  if (q->buf == NULL)
    {
      kmm_free(q);
      return -1;
    }

  q->msgsize  = message_size;
  q->capacity = number_of_messages;

  nxsem_init(&q->fill, 0, 0);
  nxsem_init(&q->space, 0, number_of_messages);

  *queue = q;
  return 0;
}

static int bt_osi_deinit_queue(void **queue)
{
  struct bt_osi_queue_s *q;

  if (queue == NULL || *queue == NULL)
    {
      return -1;
    }

  q = *queue;
  *queue = NULL;

  nxsem_destroy(&q->fill);
  nxsem_destroy(&q->space);
  kmm_free(q->buf);
  kmm_free(q);
  return 0;
}

/****************************************************************************
 * Name: bt_osi_push_to_queue
 *
 * Description:
 *   The BLE interrupt pushes link-layer events into these queues, and the
 *   vendor layer switches to xQueueSendToBackFromISR there, dropping the
 *   timeout entirely.  Mirror that: from interrupt context this never
 *   blocks and reports failure on a full ring.
 *
 *   'space' is only a hint that a slot was freed -- an interrupt-side push
 *   consumes a slot without consuming a token -- so the loop always
 *   re-checks the ring under the lock and a stale token costs one extra
 *   iteration, never a lost message.
 *
 ****************************************************************************/

static int bt_osi_push_to_queue(void **queue, void *message,
                                uint32_t timeout_ms)
{
  struct bt_osi_queue_s *q;
  irqstate_t flags;
  clock_t start;
  clock_t total;
  bool blocking;

  if (queue == NULL || *queue == NULL || message == NULL)
    {
      return -1;
    }

  q = *queue;
  blocking = timeout_ms != BT_OSI_NO_WAIT && !up_interrupt_context();
  start = clock_systime_ticks();
  total = timeout_ms == BT_OSI_WAIT_FOREVER ? 0 : MSEC2TICK(timeout_ms);

  for (; ; )
    {
      flags = up_irq_save();
      if (q->used < q->capacity)
        {
          memcpy(q->buf + q->tail * q->msgsize, message, q->msgsize);
          q->tail = (q->tail + 1) % q->capacity;
          q->used++;
          up_irq_restore(flags);

          nxsem_post(&q->fill);
          return 0;
        }

      up_irq_restore(flags);

      if (!blocking)
        {
          return -1;
        }

      if (timeout_ms == BT_OSI_WAIT_FOREVER)
        {
          if (nxsem_wait_uninterruptible(&q->space) < 0)
            {
              return -1;
            }
        }
      else
        {
          clock_t elapsed = clock_systime_ticks() - start;

          if (elapsed >= total ||
              nxsem_tickwait_uninterruptible(&q->space,
                                             total - elapsed) < 0)
            {
              return -1;
            }
        }
    }
}

/****************************************************************************
 * Name: bt_osi_pop_from_queue
 *
 * Description:
 *   Every queued message has posted exactly one 'fill' token, so once the
 *   wait succeeds the ring is guaranteed non-empty.  The vendor never pops
 *   from interrupt context and NuttX's semaphore waits assert against it,
 *   so that case is refused outright rather than half-supported.
 *
 ****************************************************************************/

static int bt_osi_pop_from_queue(void **queue, void *message,
                                 uint32_t timeout_ms)
{
  struct bt_osi_queue_s *q;
  irqstate_t flags;
  int sval;

  if (queue == NULL || *queue == NULL || message == NULL ||
      up_interrupt_context())
    {
      return -1;
    }

  q = *queue;

  if (bt_osi_wait_sem(&q->fill, timeout_ms) < 0)
    {
      return -1;
    }

  flags = up_irq_save();
  memcpy(message, q->buf + q->head * q->msgsize, q->msgsize);
  q->head = (q->head + 1) % q->capacity;
  q->used--;
  up_irq_restore(flags);

  /* Top the writer hint back up, capped so it cannot grow without bound
   * over the lifetime of a queue nobody ever blocks on.
   */

  if (nxsem_get_value(&q->space, &sval) >= 0 && sval < (int)q->capacity)
    {
      nxsem_post(&q->space);
    }

  return 0;
}

/****************************************************************************
 * Name: bt_osi_thread_entry
 *
 * Description:
 *   NuttX kernel threads take (argc, argv), the vendor's take a void *.
 *   The control block address travels as a hex string in argv[1] --
 *   argv[0] is the task name -- because NuttX copies argument strings onto
 *   the new thread's stack, which makes the handoff race-free without a
 *   global.
 *
 ****************************************************************************/

static int bt_osi_thread_entry(int argc, char *argv[])
{
  struct bt_osi_thread_s *t;

  if (argc < 2)
    {
      return EXIT_FAILURE;
    }

  t = (struct bt_osi_thread_s *)(uintptr_t)strtoul(argv[1], NULL, 16);
  if (t == NULL || t->entry == NULL)
    {
      return EXIT_FAILURE;
    }

  t->entry(t->arg);
  nxsem_post(&t->exited);
  return EXIT_SUCCESS;
}

/****************************************************************************
 * Name: bt_osi_create_thread
 ****************************************************************************/

static int bt_osi_create_thread(void **thread, uint8_t priority,
                                const char *name, void *function,
                                uint32_t stack_size, void *arg)
{
  struct bt_osi_thread_s *t;
  char handoff[24];
  char *argv[2];
  int prio;

  if (thread == NULL || function == NULL)
    {
      return -1;
    }

  t = kmm_zalloc(sizeof(struct bt_osi_thread_s));
  if (t == NULL)
    {
      return -1;
    }

  t->entry = (void (*)(void *))function;
  t->arg   = arg;
  nxsem_init(&t->exited, 0, 0);

  if (priority > BT_OSI_BEKEN_PRIO_MAX)
    {
      priority = BT_OSI_BEKEN_PRIO_MAX;
    }

  prio = BT_OSI_PRIO_BASE + (BT_OSI_BEKEN_PRIO_MAX - priority);

  snprintf(handoff, sizeof(handoff), "%p", t);
  argv[0] = handoff;
  argv[1] = NULL;

  t->pid = kthread_create(name != NULL ? name : "bt_osi", prio,
                          (int)stack_size, bt_osi_thread_entry, argv);
  if (t->pid < 0)
    {
      nxsem_destroy(&t->exited);
      kmm_free(t);
      return -1;
    }

  *thread = t;
  return 0;
}

/****************************************************************************
 * Name: bt_osi_delete_thread
 *
 * Description:
 *   A NULL handle means "delete the caller" in the vendor API.  Otherwise
 *   the control block outlives the thread so that a concurrent
 *   _thread_join() cannot land on freed memory; the join releases it.  A
 *   thread that is deleted and never joined leaks its control block, which
 *   matches how the vendor's own layer treats the case and is bounded by
 *   the number of controller threads.
 *
 ****************************************************************************/

static int bt_osi_delete_thread(void **thread)
{
  struct bt_osi_thread_s *t;

  if (thread == NULL)
    {
      kthread_delete(0);
      return 0;
    }

  t = *thread;
  if (t == NULL)
    {
      return -1;
    }

  kthread_delete(t->pid);
  nxsem_post(&t->exited);
  return 0;
}

static int bt_osi_thread_join(void **thread)
{
  struct bt_osi_thread_s *t;

  if (thread == NULL || *thread == NULL)
    {
      return -1;
    }

  t = *thread;
  nxsem_wait_uninterruptible(&t->exited);
  *thread = NULL;

  nxsem_destroy(&t->exited);
  kmm_free(t);
  return 0;
}

/****************************************************************************
 * Name: bt_osi_init_mutex / lock / unlock / deinit
 *
 * Description:
 *   The lock primitives already exist for the closed libraries' direct
 *   symbol references in bk7258_ble_shim.c; these are the thin adapters
 *   that give them the table's int32_t signature, which is a distinct type
 *   from int on this toolchain.
 *
 ****************************************************************************/

static int32_t bt_osi_init_mutex(void **mutex)
{
  return rtos_init_mutex(mutex);
}

static int32_t bt_osi_lock_mutex(void **mutex)
{
  return rtos_lock_mutex(mutex);
}

static int32_t bt_osi_unlock_mutex(void **mutex)
{
  return rtos_unlock_mutex(mutex);
}

static int32_t bt_osi_deinit_mutex(void **mutex)
{
  mutex_t *m;

  if (mutex == NULL || *mutex == NULL)
    {
      return -1;
    }

  m = *mutex;
  *mutex = NULL;

  nxmutex_destroy(m);
  kmm_free(m);
  return 0;
}

/****************************************************************************
 * Name: bt_osi_init_semaphore / set / get / deinit
 *
 * Description:
 *   max_count is ignored, as it is in the vendor layer: these are pure
 *   signalling semaphores and NuttX counting semaphores have no ceiling.
 *
 ****************************************************************************/

static int32_t bt_osi_init_semaphore(void **semaphore, int32_t max_count)
{
  sem_t *s;

  UNUSED(max_count);

  if (semaphore == NULL)
    {
      return -1;
    }

  s = kmm_malloc(sizeof(sem_t));
  if (s == NULL)
    {
      return -1;
    }

  nxsem_init(s, 0, 0);
  *semaphore = s;
  return 0;
}

static int32_t bt_osi_set_semaphore(void **semaphore)
{
  if (semaphore == NULL || *semaphore == NULL)
    {
      return -1;
    }

  return nxsem_post((sem_t *)*semaphore) < 0 ? -1 : 0;
}

static int32_t bt_osi_get_semaphore(void **semaphore, uint32_t timeout_ms)
{
  if (semaphore == NULL || *semaphore == NULL || up_interrupt_context())
    {
      return -1;
    }

  return bt_osi_wait_sem((sem_t *)*semaphore, timeout_ms) < 0 ? -1 : 0;
}

static int32_t bt_osi_deinit_semaphore(void **semaphore)
{
  sem_t *s;

  if (semaphore == NULL || *semaphore == NULL)
    {
      return -1;
    }

  s = *semaphore;
  *semaphore = NULL;

  nxsem_destroy(s);
  kmm_free(s);
  return 0;
}

/****************************************************************************
 * Name: bt_osi_timer_worker
 *
 * Description:
 *   The vendor's periodic timer re-arms itself from inside its own
 *   callback (timer_callback1 calls rtos_reload_timer before invoking the
 *   handler) rather than relying on FreeRTOS auto-reload, so re-arming
 *   first here reproduces the original phase behaviour: the period is
 *   measured from callback entry, not from callback return.
 *
 ****************************************************************************/

static void bt_osi_timer_worker(void *arg)
{
  struct bt_osi_timer_s *t = arg;

  if (!t->oneshot && t->running)
    {
      work_queue(HPWORK, &t->work, bt_osi_timer_worker, t,
                 MSEC2TICK(t->ms));
    }
  else
    {
      t->running = false;
    }

  if (t->cb != NULL)
    {
      t->cb(t->arg);
    }
}

/****************************************************************************
 * Name: bt_osi_init_timer_ext / init_timer / start / stop / deinit
 ****************************************************************************/

static int32_t bt_osi_init_timer_ext(void **timer, uint32_t time_ms,
                                     void *function, void *arg,
                                     bool oneshot)
{
  struct bt_osi_timer_s *t;

  if (timer == NULL)
    {
      return -1;
    }

  t = kmm_zalloc(sizeof(struct bt_osi_timer_s));
  if (t == NULL)
    {
      return -1;
    }

  t->cb      = (void (*)(void *))function;
  t->arg     = arg;
  t->ms      = time_ms;
  t->oneshot = oneshot;

  *timer = t;
  return 0;
}

static int32_t bt_osi_init_timer(void **timer, uint32_t time_ms,
                                 void *function, void *arg)
{
  return bt_osi_init_timer_ext(timer, time_ms, function, arg, false);
}

static int32_t bt_osi_start_timer(void *timer)
{
  struct bt_osi_timer_s *t = timer;

  if (t == NULL)
    {
      return -1;
    }

  t->running = true;
  return work_queue(HPWORK, &t->work, bt_osi_timer_worker, t,
                    MSEC2TICK(t->ms)) < 0 ? -1 : 0;
}

static int32_t bt_osi_stop_timer(void *timer)
{
  struct bt_osi_timer_s *t = timer;

  if (t == NULL)
    {
      return -1;
    }

  t->running = false;
  work_cancel(HPWORK, &t->work);
  return 0;
}

static int32_t bt_osi_deinit_timer(void *timer)
{
  struct bt_osi_timer_s *t = timer;

  if (t == NULL)
    {
      return -1;
    }

  t->running = false;

  /* Synchronous cancel, so the worker cannot still be mid-callback on a
   * block that is about to be freed.  Not from interrupt context though:
   * the sync path blocks on a semaphore, and an interrupt that landed on
   * the work thread while it ran this very callback would wait on itself.
   * There the plain cancel is the lesser evil -- a torn callback beats a
   * hang -- and the vendor's FreeRTOS timer has the same hole.
   */

  if (up_interrupt_context())
    {
      work_cancel(HPWORK, &t->work);
    }
  else
    {
      work_cancel_sync(HPWORK, &t->work);
    }

  kmm_free(t);
  return 0;
}

static int32_t bt_osi_timer_change_period(void *timer, uint32_t time_ms)
{
  struct bt_osi_timer_s *t = timer;

  if (t == NULL)
    {
      return -1;
    }

  t->ms = time_ms;

  /* The vendor's one-shot reload restarts the timer with the new period;
   * a running periodic timer picks the new period up on its next re-arm.
   */

  if (t->running)
    {
      return work_queue(HPWORK, &t->work, bt_osi_timer_worker, t,
                        MSEC2TICK(time_ms)) < 0 ? -1 : 0;
    }

  return 0;
}

static bool bt_osi_is_timer_init(void *timer)
{
  return timer != NULL;
}

static bool bt_osi_is_timer_running(void *timer)
{
  struct bt_osi_timer_s *t = timer;

  return t != NULL && t->running;
}

/****************************************************************************
 * Name: bt_osi_log
 *
 * Description:
 *   The vendor points this at bk_printf_ext, whose level values follow
 *   components/log.h (1 error .. 4 debug).  The tag is dropped rather than
 *   composed into a scratch buffer: this can be reached from the BLE
 *   interrupt, where a stack buffer large enough for a formatted line is
 *   not something to spend.
 *
 ****************************************************************************/

static void bt_osi_log(int level, char *tag, const char *fmt, ...)
{
  va_list ap;
  int priority;

  UNUSED(tag);

  switch (level)
    {
      case 1:
        priority = LOG_ERR;
        break;

      case 2:
        priority = LOG_WARNING;
        break;

      case 3:
        priority = LOG_INFO;
        break;

      default:
        priority = LOG_DEBUG;
        break;
    }

  va_start(ap, fmt);
  vsyslog(priority, fmt, ap);
  va_end(ap);
}

/****************************************************************************
 * Name: bt_osi_get_time / delay_milliseconds / malloc / free
 ****************************************************************************/

static uint32_t bt_osi_get_time(void)
{
  return (uint32_t)TICK2MSEC(clock_systime_ticks());
}

/* A busy wait rather than nxsig_usleep: the controller calls this from
 * power and clock bring-up paths where it may already hold an interrupt
 * lock, and a sleep there would either assert or never wake.  Interrupts
 * stay enabled, so the cost is CPU time, not latency.
 */

static int bt_osi_delay_milliseconds(uint32_t num_ms)
{
  up_mdelay(num_ms);
  return 0;
}

static void *bt_osi_malloc(unsigned int size)
{
  return kmm_malloc(size);
}

static void bt_osi_free(void *p)
{
  kmm_free(p);
}

/****************************************************************************
 * Name: bt_osi_coex_init
 *
 * Description:
 *   Stub: Wi-Fi is not built into this image, so there is no coexistence
 *   arbiter to bring up.  The vendor compiles its body out under the same
 *   condition and returns success.
 *
 ****************************************************************************/

static int bt_osi_coex_init(void)
{
  return 0;
}

/****************************************************************************
 * Name: bt_osi_isr_handler
 *
 * Description:
 *   The controller hands over an int_group_isr_t, which takes no
 *   arguments; NuttX wants an xcpt_t.  The library's handler travels as
 *   the attach argument so no dispatch table is needed.
 *
 ****************************************************************************/

volatile uint32_t g_bt_isr_hits;

static int bt_osi_isr_handler(int irq, void *context, void *arg)
{
  void (*isr)(void) = (void (*)(void))arg;

  UNUSED(irq);
  UNUSED(context);

  g_bt_isr_hits++;

  if (isr != NULL)
    {
      isr();
    }

  return OK;
}

/****************************************************************************
 * Name: bt_osi_type_to_irq
 *
 * Description:
 *   Map the table's interrupt source enumeration onto this port's IRQ
 *   numbers.  NuttX counts from the start of the exception vector, so the
 *   NVIC line numbers 39/40/41 appear as NVIC_IRQ_FIRST + n, exactly as
 *   include/irq.h already spells them.
 *
 ****************************************************************************/

static int bt_osi_type_to_irq(uint8_t type)
{
  switch (type)
    {
      case BLUETOOTH_INT_SRC_BTDM:
        return BK7258_IRQ_DM;

      case BLUETOOTH_INT_SRC_BLE:
        return BK7258_IRQ_BLE;

      case BLUETOOTH_INT_SRC_BT:
        return BK7258_IRQ_BT;

      default:
        return -1;
    }
}

/****************************************************************************
 * Name: bt_osi_int_isr_register
 *
 * Description:
 *   Attach and enable, because that is what the vendor's Cortex-M path
 *   does: bk_int_isr_register lands in arch_interrupt_register_int, which
 *   installs the vector and calls NVIC_EnableIRQ.  up_enable_irq() also
 *   sets the SoC routing bit for the line -- which on this part gates the
 *   interrupt ahead of the NVIC -- so the line becomes live slightly
 *   earlier than on the vendor, but only after the handler is in place.
 *
 ****************************************************************************/

static int bt_osi_int_isr_register(uint8_t type, void *isr, void *arg)
{
  int irq = bt_osi_type_to_irq(type);

  UNUSED(arg);

  if (irq < 0 || isr == NULL)
    {
      return -1;
    }

  if (irq_attach(irq, bt_osi_isr_handler, isr) < 0)
    {
      return -1;
    }

  up_enable_irq(irq);
  return 0;
}

/****************************************************************************
 * Name: bt_osi_controller_mem_init
 *
 * Description:
 *   Stub: the vendor clears a _bt_data_start.._bt_data_end region, but
 *   only when the controller is configured to reuse the media memory pool
 *   (CONFIG_BT_REUSE_MEDIA_MEMORY).  This port defines no such linker
 *   region, so there is nothing to clear and clearing anything else would
 *   be a guess at another subsystem's memory.
 *
 ****************************************************************************/

static void bt_osi_controller_mem_init(void)
{
}

/****************************************************************************
 * Name: bt_osi_deep_sleep_register / extern32k_register / unregister
 *
 * Description:
 *   Stubs reporting success.  All three register callbacks with the SDK
 *   power manager, which this port does not have; reporting failure would
 *   make the controller treat an absent feature as a broken one.  Since
 *   bt_osi_vote_sleep_ctrl never votes for sleep either, the callbacks
 *   would have nothing to fire on.
 *
 ****************************************************************************/

static int bt_osi_deep_sleep_register(void *enter_config_cb)
{
  UNUSED(enter_config_cb);
  return 0;
}

static int bt_osi_extern32k_register(void *switch_cb)
{
  UNUSED(switch_cb);
  return 0;
}

static int bt_osi_extern32k_unregister(void)
{
  return 0;
}

/****************************************************************************
 * Name: bt_osi_clk_32k_config_get / lpo_src_set / lpo_src_get
 *
 * Description:
 *   Report the 32 kHz sleep clock as derived from the 26 MHz crystal,
 *   which is the one source with an exactly known rate (32000 Hz) and no
 *   calibration loop behind it.  The silicon default is actually the
 *   internal RC oscillator with software calibration, which this port
 *   cannot measure -- see bt_osi_get_rc32k_freq.  Nothing depends on it
 *   while BLE sleep stays off, but it has to be reconciled before the
 *   controller is allowed to sleep.
 *
 ****************************************************************************/

static uint8_t bt_osi_clk_32k_config_get(void)
{
  return BT_LPO_SRC_DIVD;
}

static int bt_osi_lpo_src_set(uint8_t lpo_src)
{
  UNUSED(lpo_src);
  return 0;
}

static uint8_t bt_osi_lpo_src_get(void)
{
  return BT_LPO_SRC_DIVD;
}

/****************************************************************************
 * Name: bt_osi_bluetooth_power_ctrl / phy_power_ctrl
 *
 * Description:
 *   The vendor routes these through a reference-counted power manager; on
 *   this port CPU0 is the secure master and owns the system block
 *   outright, so the domain bits are written directly.  Both bits are
 *   inverted -- clearing powers the domain up.
 *
 ****************************************************************************/

static int bt_osi_bluetooth_power_ctrl(uint8_t power_state)
{
  modifyreg32(BT_OSI_SYS_POWER,
              power_state ? BT_OSI_PWD_BTSP : 0,
              power_state ? 0 : BT_OSI_PWD_BTSP);
  return 0;
}

static int bt_osi_phy_power_ctrl(uint8_t power_state)
{
  modifyreg32(BT_OSI_SYS_POWER,
              power_state ? BT_OSI_PWD_WIFP_PHY : 0,
              power_state ? 0 : BT_OSI_PWD_WIFP_PHY);
  return 0;
}

/****************************************************************************
 * Name: bt_osi_bt_mac_clock_ctrl / bt_phy_clock_ctrl
 ****************************************************************************/

static int bt_osi_bt_mac_clock_ctrl(uint8_t clock_state)
{
  modifyreg32(BT_OSI_SYS_CLK_EN,
              clock_state ? 0 : BT_OSI_CKEN_BTDM,
              clock_state ? BT_OSI_CKEN_BTDM : 0);
  return 0;
}

static int bt_osi_bt_phy_clock_ctrl(uint8_t clock_state)
{
  modifyreg32(BT_OSI_SYS_CLK_EN,
              clock_state ? 0 : BT_OSI_CKEN_XVR,
              clock_state ? BT_OSI_CKEN_XVR : 0);
  return 0;
}

/****************************************************************************
 * Name: bt_osi_btdm_interrupt_ctrl / ble_interrupt_ctrl / bt_interrupt_ctrl
 *
 * Description:
 *   The vendor toggles the per-CPU routing bit for the line (bits 7/8/9 of
 *   cpu0_int_32_63_en).  up_enable_irq()/up_disable_irq() on this port set
 *   and clear that same bit together with the NVIC enable, so they gate
 *   the line the same way and stay symmetric with each other.
 *
 ****************************************************************************/

static void bt_osi_btdm_interrupt_ctrl(bool en)
{
  if (en)
    {
      up_enable_irq(BK7258_IRQ_DM);
    }
  else
    {
      up_disable_irq(BK7258_IRQ_DM);
    }
}

static void bt_osi_ble_interrupt_ctrl(bool en)
{
  if (en)
    {
      up_enable_irq(BK7258_IRQ_BLE);
    }
  else
    {
      up_disable_irq(BK7258_IRQ_BLE);
    }
}

static void bt_osi_bt_interrupt_ctrl(bool en)
{
  if (en)
    {
      up_enable_irq(BK7258_IRQ_BT);
    }
  else
    {
      up_disable_irq(BK7258_IRQ_BT);
    }
}

/****************************************************************************
 * Name: bt_osi_ate_is_enabled / cp_test_is_enabled
 *
 * Description:
 *   Stubs reporting "off".  Both gate production-test entry points that
 *   reconfigure the RF away from normal operation; the vendor's real
 *   implementations read a boot pin latch this port does not sample, and
 *   reporting off is the state a shipped board is in.
 *
 ****************************************************************************/

static bool bt_osi_ate_is_enabled(void)
{
  return false;
}

static bool bt_osi_cp_test_is_enabled(void)
{
  return false;
}

/****************************************************************************
 * Name: bt_osi_get_bluetooth_mac
 *
 * Description:
 *   Placeholder address, OUI first.  A production implementation must read
 *   the per-unit address from OTP bank 1, falling back to the flash
 *   sys_net partition, the way bk_get_mac(MAC_TYPE_BLUETOOTH) does; until
 *   then every board built from this tree advertises the same address, so
 *   two of them cannot be told apart or connected to at once.
 *
 ****************************************************************************/

static int bt_osi_get_bluetooth_mac(uint8_t *mac)
{
  static const uint8_t placeholder[6] =
  {
    0xc8, 0x47, 0x8c, 0x25, 0x20, 0x26
  };

  if (mac == NULL)
    {
      return -1;
    }

  memcpy(mac, placeholder, sizeof(placeholder));
  return 0;
}

/****************************************************************************
 * Name: bt_osi_uart_* / bt_osi_get_printf_port
 *
 * Description:
 *   Stubs.  These exist for the vendor's HCI-over-UART passthrough and
 *   bkreg console, where the controller takes a UART away from the
 *   application and drives it byte by byte.  This port keeps UART0 as the
 *   NuttX console; letting the controller seize it would take the shell
 *   down, and interleaving raw HCI into the console stream would corrupt
 *   both.  The read/write entries report failure so a caller that ignores
 *   the port query still cannot get half a transfer.
 *
 ****************************************************************************/

static int bt_osi_uart_write_byte(uint8_t id, uint8_t data)
{
  UNUSED(id);
  UNUSED(data);
  return -1;
}

static int bt_osi_uart_read_byte_ex(uint8_t id, uint8_t *ch)
{
  UNUSED(id);
  UNUSED(ch);
  return -1;
}

static int bt_osi_uart_enable_rx_interrupt(uint8_t id)
{
  UNUSED(id);
  return -1;
}

static int bt_osi_uart_take_rx_isr(uint8_t id, void *isr, void *param)
{
  UNUSED(id);
  UNUSED(isr);
  UNUSED(param);
  return -1;
}

static void bt_osi_uart_enable(uint8_t uart_id, uint8_t enable,
                               uint32_t band)
{
  UNUSED(uart_id);
  UNUSED(enable);
  UNUSED(band);
}

static int bt_osi_get_printf_port(void)
{
  return 0;
}

/****************************************************************************
 * Name: bt_osi_register_ble_dump_hook
 *
 * Description:
 *   Stub: the vendor only installs this hook on its RISC-V parts, where
 *   the exception dumper calls back into the controller for link state.
 *   The Cortex-M build leaves it empty too.
 *
 ****************************************************************************/

static void bt_osi_register_ble_dump_hook(void *ble_func)
{
  UNUSED(ble_func);
}

/****************************************************************************
 * Name: bt_osi_gpio_dev_unmap / bt_osi_gpio_dev_map
 *
 * Description:
 *   unmap takes a pin back from whatever peripheral held it and parks it
 *   as a plain input.  map cannot be implemented: its 'func' argument is a
 *   vendor GPIO_DEV_* device identifier, and turning that into this part's
 *   per-pin alternate-function index needs the SDK's gpio_map table, which
 *   this port does not carry.  Failing is the honest answer -- picking an
 *   alternate function at random would route a live peripheral onto an
 *   arbitrary pad.
 *
 ****************************************************************************/

static int bt_osi_gpio_dev_unmap(uint32_t gpio_id)
{
  if (gpio_id >= BK7258_NGPIOS)
    {
      return -1;
    }

  bk7258_gpio_config((int)gpio_id, false, false, false);
  return 0;
}

static int bt_osi_gpio_dev_map(uint32_t gpio_id, uint32_t func)
{
  UNUSED(gpio_id);
  UNUSED(func);
  return -1;
}

/****************************************************************************
 * Name: bt_osi_gpio_*
 *
 * Description:
 *   Pull control touches only the pull bits: the controller uses these to
 *   adjust a pad it has already configured, so forcing a direction here
 *   would undo that.
 *
 ****************************************************************************/

static int bt_osi_gpio_disable_pull(uint32_t gpio_id)
{
  if (gpio_id >= BK7258_NGPIOS)
    {
      return -1;
    }

  modifyreg32(BK7258_GPIO_CFG(gpio_id),
              GPIO_CFG_PULL_EN | GPIO_CFG_PULL_UP, 0);
  return 0;
}

static int bt_osi_gpio_pull_up(uint32_t gpio_id)
{
  if (gpio_id >= BK7258_NGPIOS)
    {
      return -1;
    }

  modifyreg32(BK7258_GPIO_CFG(gpio_id), 0,
              GPIO_CFG_PULL_EN | GPIO_CFG_PULL_UP);
  return 0;
}

static int bt_osi_gpio_pull_down(uint32_t gpio_id)
{
  if (gpio_id >= BK7258_NGPIOS)
    {
      return -1;
    }

  modifyreg32(BK7258_GPIO_CFG(gpio_id), GPIO_CFG_PULL_UP,
              GPIO_CFG_PULL_EN);
  return 0;
}

static int bt_osi_gpio_enable_output(uint32_t gpio_id)
{
  if (gpio_id >= BK7258_NGPIOS)
    {
      return -1;
    }

  bk7258_gpio_config((int)gpio_id, true, false, false);
  return 0;
}

static int bt_osi_gpio_set_output_high(uint32_t gpio_id)
{
  if (gpio_id >= BK7258_NGPIOS)
    {
      return -1;
    }

  bk7258_gpio_write((int)gpio_id, true);
  return 0;
}

static int bt_osi_gpio_set_output_low(uint32_t gpio_id)
{
  if (gpio_id >= BK7258_NGPIOS)
    {
      return -1;
    }

  bk7258_gpio_write((int)gpio_id, false);
  return 0;
}

/****************************************************************************
 * Name: bt_osi_set_printf_enable / enable_debug_gpio
 *
 * Description:
 *   Stubs.  The first mutes the vendor console, which this port does not
 *   route through the controller anyway.  The second remaps roughly thirty
 *   pads to internal debug signals; on this board those pads carry the
 *   camera, display and audio, so honouring it would take the product
 *   apart to service a debug probe nobody attached.
 *
 ****************************************************************************/

static void bt_osi_set_printf_enable(uint8_t enable)
{
  UNUSED(enable);
}

static void bt_osi_enable_debug_gpio(void)
{
}

/****************************************************************************
 * Name: bt_osi_evm_stop_bypass_mac / bt_osi_rs_deinit
 *
 * Description:
 *   Stubs, matching the vendor with Wi-Fi disabled: both tear down Wi-Fi
 *   MAC state before the RF is handed to bluetooth, and there is no Wi-Fi
 *   MAC in this image to tear down.
 *
 ****************************************************************************/

static void bt_osi_evm_stop_bypass_mac(void)
{
}

static void bt_osi_rs_deinit(void)
{
}

/****************************************************************************
 * Name: bt_osi_ble_vote_rf_ctrl / bt_osi_ble_ate_vote_rf_ctrl
 *
 * Description:
 *   Forwarded to the closed RF arbiter with the caller's role bit, exactly
 *   as the vendor does.
 *
 ****************************************************************************/

static void bt_osi_ble_vote_rf_ctrl(uint8_t cmd)
{
  rf_module_vote_ctrl(cmd, BT_OSI_RF_BY_BLE_BIT);
}

static void bt_osi_ble_ate_vote_rf_ctrl(uint8_t cmd)
{
  rf_module_vote_ctrl(cmd, BT_OSI_RF_BY_ATE_BT_BIT);
}

/****************************************************************************
 * Name: bt_osi_manual_cal_save_ble_txpwr
 *
 * Description:
 *   The calibration table is indexed by rate; the vendor files every BLE
 *   result under the same synthetic rate index 158.
 *
 ****************************************************************************/

static void bt_osi_manual_cal_save_ble_txpwr(uint32_t channel,
                                             uint32_t pwr_gain)
{
  manual_cal_save_txpwr(BT_OSI_EVM_BLE_RATE, channel, pwr_gain);
}

/****************************************************************************
 * Name: bt_osi_bt_rf_pll_ctrl
 *
 * Description:
 *   Stub returning success, matching the vendor with Wi-Fi disabled.  The
 *   call asks the arbiter to hold the Wi-Fi PLL on behalf of bluetooth;
 *   with no Wi-Fi in the image the BT PLL is the only one running and
 *   there is nothing to hold.
 *
 ****************************************************************************/

static uint32_t bt_osi_bt_rf_pll_ctrl(uint32_t set)
{
  UNUSED(set);
  return 0;
}

/****************************************************************************
 * Name: bt_osi_reboot
 *
 * Description:
 *   Stub that only complains.  The controller reaches for this on
 *   unrecoverable internal errors; during bring-up a silent reset would
 *   destroy the evidence, and the log line survives it.  Wire this to
 *   up_systemreset() once the stack is trusted.
 *
 ****************************************************************************/

static void bt_osi_reboot(void)
{
  syslog(LOG_ERR, "bt_osi: controller requested reboot, ignored\n");
}

/****************************************************************************
 * Name: bt_osi_bt_vote_sleep_ctrl / bt_ext_wakeup_ctrl
 *
 * Description:
 *   Stubs.  Both talk to the SDK power manager: the first votes the
 *   bluetooth domain into or out of low power, the second arms the
 *   external wakeup path out of it.  This port never lowers the domain, so
 *   reporting success keeps the controller's own state machine consistent
 *   without anything actually going to sleep.
 *
 ****************************************************************************/

static int bt_osi_bt_vote_sleep_ctrl(uint32_t sleep_state,
                                     uint32_t sleep_time)
{
  UNUSED(sleep_state);
  UNUSED(sleep_time);
  return 0;
}

static void bt_osi_bt_ext_wakeup_ctrl(uint8_t enable)
{
  UNUSED(enable);
}

/****************************************************************************
 * Name: bt_osi_btsnoop
 *
 * Description:
 *   Stub: the vendor dumps every HCI packet out a UART, which this port
 *   does not lend to the controller (see bt_osi_uart_write_byte).
 *
 ****************************************************************************/

static void bt_osi_btsnoop(uint8_t uart_id, uint8_t pkt_type,
                           uint8_t is_rxed, uint8_t *pkt, uint16_t pkt_len,
                           uint8_t method)
{
  UNUSED(uart_id);
  UNUSED(pkt_type);
  UNUSED(is_rxed);
  UNUSED(pkt);
  UNUSED(pkt_len);
  UNUSED(method);
}

/****************************************************************************
 * Name: bt_osi_get_sys_debug_config_addr / chip identity
 *
 * Description:
 *   Addresses and constants copied from the SDK's bk7258 sys_reg.h and
 *   sys_types.h.  _get_chipid answers with the only revision the vendor
 *   table knows; _get_current_chipid reads the AON PMU identity register
 *   that aon_pmu_hal_get_chipid() reads.
 *
 ****************************************************************************/

static size_t bt_osi_get_sys_debug_config_addr(uint32_t index)
{
  return index == 0 ? BT_OSI_SYS_DEBUG_CFG0 : BT_OSI_SYS_DEBUG_CFG1;
}

static uint32_t bt_osi_get_chipid_mask(void)
{
  return BT_OSI_CHIP_ID_MASK;
}

static uint32_t bt_osi_get_chipid(uint32_t ver)
{
  UNUSED(ver);
  return BT_OSI_CHIP_ID_MPW_V4;
}

static uint32_t bt_osi_get_current_chipid(void)
{
  return getreg32(BT_OSI_AON_PMU_CHIPID);
}

/****************************************************************************
 * Name: bt_osi_get_test_rfconfig / bt_osi_get_rf_mode
 *
 * Description:
 *   Both variables live in the closed PHY archive and are set by its own
 *   configuration path; the table only reads them back.
 *
 ****************************************************************************/

static uint32_t bt_osi_get_test_rfconfig(void)
{
  return test_rfconfig;
}

static uint8_t bt_osi_get_rf_mode(void)
{
  uint32_t cfg = rwnx_rfconfig;

  if ((cfg & BLUETOOTH_RF_PLL_MASK) == BLUETOOTH_RF_PLL_WIFI)
    {
      return BT_RF_MODE_WIFI;
    }

  if ((cfg & BLUETOOTH_RF_MODE_MASK) == BLUETOOTH_RF_MODE_POLAR)
    {
      return BT_RF_MODE_POLAR;
    }

  return 0;
}

/****************************************************************************
 * Name: bt_osi_bkreg_run_command
 *
 * Description:
 *   Stub reporting "nothing consumed".  bkreg is the vendor's register
 *   poke console, gated behind CONFIG_CLI && CONFIG_BKREG in their build
 *   and absent here.
 *
 ****************************************************************************/

static int bt_osi_bkreg_run_command(const char *content, int cnt)
{
  UNUSED(content);
  UNUSED(cnt);
  return 0;
}

/****************************************************************************
 * Name: bt_osi_manual_cal_is_in_cali_mode
 *
 * Description:
 *   The table's name and the archive's name disagree: the closed library
 *   exports manual_cal_is_in_rftest_mode, which is what the vendor's own
 *   wrapper calls too.
 *
 ****************************************************************************/

static uint32_t bt_osi_manual_cal_is_in_cali_mode(void)
{
  return manual_cal_is_in_rftest_mode();
}

/****************************************************************************
 * Name: bt_osi_get_rc32k_freq
 *
 * Description:
 *   Returns the exact rate implied by bt_osi_clk_32k_config_get()'s
 *   answer.  The vendor measures the RC oscillator with the CKMN block
 *   when the sleep clock comes from it; this port has no CKMN driver, so
 *   it reports the source it can state exactly instead of a measurement it
 *   cannot make.  The X32K arm is kept for the day the board gains a
 *   32.768 kHz crystal.
 *
 ****************************************************************************/

static double bt_osi_get_rc32k_freq(void)
{
  if (bt_osi_lpo_src_get() == BT_LPO_SRC_X32K)
    {
      return BLUETOOTH_CLK_32768;
    }

  return BLUETOOTH_CLK_32K;
}

/****************************************************************************
 * Name: bt_osi_flush_dcache
 *
 * Description:
 *   Stub: the data cache is disabled on this port, so there is never a
 *   dirty line between the controller and the DMA engines it shares
 *   buffers with.
 *
 ****************************************************************************/

static void bt_osi_flush_dcache(void)
{
}

/****************************************************************************
 * Name: bt_osi_ble_enter_dut / bt_osi_ble_exit_dut
 *
 * Description:
 *   Forwarded to the closed calibration library, whose entry points were
 *   confirmed present.  They only run when the controller is put into
 *   device-under-test mode, which bt_osi_ate_is_enabled() declines.
 *
 ****************************************************************************/

static void bt_osi_ble_enter_dut(void)
{
  ble_cal_enter_dut();
}

static void bt_osi_ble_exit_dut(void)
{
  ble_cal_exit_dut();
}

/****************************************************************************
 * Name: bt_osi_set_bluetooth_power_level
 *
 * Description:
 *   Reproduces the vendor sequence.  bk_ble_set_tx_power() is SDK glue and
 *   is not in any archive here, so its one-line body is inlined: it is
 *   manual_cal_set_tx_power() with the "not a Wi-Fi standard" selector,
 *   which is how the calibration layer spells BLE.  The index reported
 *   back is clamped to the table, but -- as in the vendor -- the
 *   unclamped index is the one programmed.
 *
 ****************************************************************************/

static uint8_t bt_osi_set_bluetooth_power_level(float pwr_gain)
{
  uint8_t max_index;
  uint8_t index;

  manual_cal_set_tx_power(0, pwr_gain);

  index = manual_cal_get_ble_pwr_idx(19);
  max_index = get_ble_txpwr_table_size() - 1;
  ble_cal_set_txpwr(index);

  return index > max_index ? max_index : index;
}

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Must stay out of .rodata.  The vendor learned the hard way that a const
 * table costs enough extra cycles on every indirect call to make the audio
 * paths miss their deadline and trip the watchdog, and the library writes
 * nothing here, so there is no protection being given up.
 *
 * _phy_enable_rx_switch and _coexist_check_large_signal are left NULL on
 * purpose, exactly as the vendor's own table leaves them: both are
 * feature probes as much as callbacks -- an external RX switch and a
 * Wi-Fi coexistence arbiter, neither of which this board has -- and an
 * empty stub would answer "present" to a question whose honest answer is
 * "absent".
 */

struct bt_osi_funcs_t g_bt_osi_funcs =
{
  ._version                             = BT_OSI_VERSION,
  .size                                 = sizeof(struct bt_osi_funcs_t),

  ._init_queue                          = bt_osi_init_queue,
  ._deinit_queue                        = bt_osi_deinit_queue,
  ._pop_from_queue                      = bt_osi_pop_from_queue,
  ._push_to_queue                       = bt_osi_push_to_queue,
  ._create_thread                       = bt_osi_create_thread,
  ._delete_thread                       = bt_osi_delete_thread,
  ._thread_join                         = bt_osi_thread_join,

  ._init_mutex                          = bt_osi_init_mutex,
  ._lock_mutex                          = bt_osi_lock_mutex,
  ._unlock_mutex                        = bt_osi_unlock_mutex,
  ._deinit_mutex                        = bt_osi_deinit_mutex,

  ._init_semaphore                      = bt_osi_init_semaphore,
  ._set_semaphore                       = bt_osi_set_semaphore,
  ._get_semaphore                       = bt_osi_get_semaphore,
  ._deinit_semaphore                    = bt_osi_deinit_semaphore,

  ._init_timer                          = bt_osi_init_timer,
  ._init_timer_ext                      = bt_osi_init_timer_ext,
  ._deinit_timer                        = bt_osi_deinit_timer,
  ._stop_timer                          = bt_osi_stop_timer,

  ._timer_change_period                 = bt_osi_timer_change_period,
  ._is_timer_init                       = bt_osi_is_timer_init,
  ._start_timer                         = bt_osi_start_timer,
  ._is_timer_running                    = bt_osi_is_timer_running,

  ._log                                 = bt_osi_log,
  ._get_time                            = bt_osi_get_time,
  ._delay_milliseconds                  = bt_osi_delay_milliseconds,

  ._malloc                              = bt_osi_malloc,
  ._free                                = bt_osi_free,

  ._coex_init                           = bt_osi_coex_init,
  ._bluetooth_int_isr_register          = bt_osi_int_isr_register,

  ._controller_mem_init                 = bt_osi_controller_mem_init,
  ._bluetooth_deep_sleep_register       = bt_osi_deep_sleep_register,
  ._bluetooth_extern32k_register        = bt_osi_extern32k_register,
  ._bluetooth_extern32k_unregister      = bt_osi_extern32k_unregister,
  ._clk_32k_customer_config_get         = bt_osi_clk_32k_config_get,
  ._lpo_src_set                         = bt_osi_lpo_src_set,
  ._lpo_src_get                         = bt_osi_lpo_src_get,
  ._bluetooth_power_ctrl                = bt_osi_bluetooth_power_ctrl,
  ._phy_power_ctrl                      = bt_osi_phy_power_ctrl,
  ._bt_mac_clock_ctrl                   = bt_osi_bt_mac_clock_ctrl,
  ._bt_phy_clock_ctrl                   = bt_osi_bt_phy_clock_ctrl,
  ._btdm_interrupt_ctrl                 = bt_osi_btdm_interrupt_ctrl,
  ._ble_interrupt_ctrl                  = bt_osi_ble_interrupt_ctrl,
  ._bt_interrupt_ctrl                   = bt_osi_bt_interrupt_ctrl,
  ._ate_is_enabled                      = bt_osi_ate_is_enabled,
  ._cp_test_is_enabled                  = bt_osi_cp_test_is_enabled,
  ._get_bluetooth_mac                   = bt_osi_get_bluetooth_mac,
  ._uart_write_byte                     = bt_osi_uart_write_byte,
  ._register_ble_dump_hook              = bt_osi_register_ble_dump_hook,
  ._get_ble_pwr_idx                     = bt_osi_ble_pwr_idx,
  ._ble_cal_set_txpwr                   = ble_cal_set_txpwr,
  ._ble_cal_recover_txpwr               = ble_cal_recover_txpwr,
  ._ble_cal_enter_txpwr                 = ble_cal_enter_txpwr,
  ._gpio_dev_unmap                      = bt_osi_gpio_dev_unmap,
  ._gpio_dev_map                        = bt_osi_gpio_dev_map,
  ._set_printf_enable                   = bt_osi_set_printf_enable,
  ._evm_stop_bypass_mac                 = bt_osi_evm_stop_bypass_mac,
  ._rs_deinit                           = bt_osi_rs_deinit,
  ._phy_enable_rx_switch                = NULL,
  ._uart_enable_rx_interrupt            = bt_osi_uart_enable_rx_interrupt,
  ._uart_take_rx_isr                    = bt_osi_uart_take_rx_isr,
  ._ble_vote_rf_ctrl                    = bt_osi_ble_vote_rf_ctrl,
  ._ble_ate_vote_rf_ctrl                = bt_osi_ble_ate_vote_rf_ctrl,

  ._disable_int                         = rtos_disable_int,
  ._enable_int                          = rtos_enable_int,
  ._gpio_disable_pull                   = bt_osi_gpio_disable_pull,
  ._gpio_enable_output                  = bt_osi_gpio_enable_output,
  ._gpio_set_output_high                = bt_osi_gpio_set_output_high,
  ._gpio_set_output_low                 = bt_osi_gpio_set_output_low,
  ._gpio_pull_down                      = bt_osi_gpio_pull_down,
  ._gpio_pull_up                        = bt_osi_gpio_pull_up,
  ._get_printf_port                     = bt_osi_get_printf_port,
  ._uart_enable                         = bt_osi_uart_enable,
  ._enable_debug_gpio                   = bt_osi_enable_debug_gpio,

  ._manual_cal_save_ble_txpwr           = bt_osi_manual_cal_save_ble_txpwr,

  ._bt_rf_pll_ctrl                      = bt_osi_bt_rf_pll_ctrl,
  ._reboot                              = bt_osi_reboot,
  ._uart_read_byte_ex                   = bt_osi_uart_read_byte_ex,
  ._bt_vote_sleep_ctrl                  = bt_osi_bt_vote_sleep_ctrl,
  ._coexist_check_large_signal          = NULL,
  ._bt_ext_wakeup_ctrl                  = bt_osi_bt_ext_wakeup_ctrl,
  ._btsnoop                             = bt_osi_btsnoop,

  ._get_sys_debug_config_addr           = bt_osi_get_sys_debug_config_addr,
  ._get_chipid_mask                     = bt_osi_get_chipid_mask,
  ._get_chipid                          = bt_osi_get_chipid,
  ._get_current_chipid                  = bt_osi_get_current_chipid,
  ._get_test_rfconfig                   = bt_osi_get_test_rfconfig,
  ._get_rf_mode                         = bt_osi_get_rf_mode,

  ._bkreg_run_command                   = bt_osi_bkreg_run_command,

  ._manual_cal_txpwr_tab_ready_in_flash =
                                    manual_cal_txpwr_tab_ready_in_flash,
  ._manual_cal_is_in_cali_mode          =
                                    bt_osi_manual_cal_is_in_cali_mode,
  ._bk_driver_get_rc32k_freq            = bt_osi_get_rc32k_freq,
  ._flush_dcache                        = bt_osi_flush_dcache,

  ._ble_enter_dut                       = bt_osi_ble_enter_dut,
  ._ble_exit_dut                        = bt_osi_ble_exit_dut,
  ._set_bluetooth_power_level           = bt_osi_set_bluetooth_power_level,
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_bt_osi_init
 *
 * Description:
 *   Hand the OS abstraction table to the closed bluetooth controller.
 *   Must run before any other controller entry point; the library's
 *   return value is passed through unchanged, and a non-zero value means
 *   it rejected the table (version or size mismatch).
 *
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_bt_osi_diag
 *
 * Description:
 *   Report whether the radio's interrupts actually reach the CPU: the
 *   hit count, the SoC routing matrix word that gates lines 32..63
 *   ahead of the NVIC, and the NVIC enable word covering the same
 *   lines.  Bits 7/8/9 of the routing word are DM/BLE/BT.
 *
 ****************************************************************************/

/****************************************************************************
 * Name: bt_osi_ble_pwr_idx
 *
 * Description:
 *   The controller asks for a transmit level per channel and applies
 *   the answer itself, so this callback is the only place a level
 *   actually sticks -- setting one before advertising starts is
 *   overwritten on the first event, which is why an earlier override
 *   proved nothing.  The library's own answer comes from a calibration
 *   that had no factory record and read TSSI with the transmitter
 *   possibly unkeyed, so a plausible-looking index can still mean no
 *   output.  Clamp upward while the receiver works and nothing hears
 *   the transmitter; if this is what carries, the calibrated value is
 *   the thing to fix rather than this floor.
 *
 ****************************************************************************/

static uint8_t bt_osi_ble_pwr_idx(uint8_t channel)
{
  uint8_t idx = manual_cal_get_ble_pwr_idx(channel);

  return idx < 60 ? 60 : idx;
}

void bk7258_bt_rf_diag(void)
{
  syslog(LOG_INFO, "bt: rwnx_rfconfig %08lx test_rfconfig %08lx "
                   "rf_mode %u\n",
         (unsigned long)rwnx_rfconfig, (unsigned long)test_rfconfig,
         bt_osi_get_rf_mode());
}

void bk7258_bt_osi_diag(void)
{
  syslog(LOG_INFO,
         "bt: isr %lu route %08lx pwr %08lx clk %08lx lpo %08lx\n",
         (unsigned long)g_bt_isr_hits,
         (unsigned long)getreg32(0x44010084ul),
         (unsigned long)getreg32(0x44010040ul),
         (unsigned long)getreg32(0x44010030ul),
         (unsigned long)getreg32(0x44000104ul));
}

int bk7258_bt_osi_init(void)
{
  return bt_os_adapter_init(&g_bt_osi_funcs);
}
