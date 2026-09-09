/****************************************************************************
 * boards/arm/bk7258/contest_board/chip/bk7258_hci.c
 *
 * HCI transport lower half for the closed BK7258 BLE controller.
 *
 * This is the interface openvela documents for a Bluetooth driver: fill in
 * a struct bt_driver_s and hand it to bt_driver_register(), which routes it
 * through drivers/serial/uart_bth4.c and publishes /dev/ttyHCI0.  Everything
 * above the vtable -- H4 framing on the way down, reassembly of writes that
 * split a frame, the receive ring, read(), poll() -- belongs to BTH4 and is
 * deliberately not repeated here.  An earlier version of this file did
 * repeat it, as a hand-written character driver; it worked, but a
 * transport that reimplements the common layer is a transport whose
 * behaviour has to be re-proven against every stack that uses it.
 *
 * What is genuinely board-specific is the two-line contract with the
 * controller archive, settled by disassembling it:
 *
 *   bk_ble_hci_raw_to_controller() reads buf[0] as the H4 packet type and
 *   tail-calls the typed entry with buf+1 / len-1, so the frame it wants is
 *   the type byte followed by the payload.  head_reserve = 1 is what makes
 *   that free: BTH4 hands send() a payload pointer with a byte of headroom
 *   in front, so the type byte goes back in place with no copy.
 *
 *   The receive callback delivers whole frames with the same leading type
 *   byte, which is what BTH4's receive() wants minus that byte.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <syslog.h>

#include <nuttx/clock.h>
#include <nuttx/wireless/bluetooth/bt_driver.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define HCI_PKT_CMD           0x01
#define HCI_PKT_ACL           0x02
#define HCI_PKT_SCO           0x03
#define HCI_PKT_EVT           0x04
#define HCI_PKT_ISO           0x05

#define HCI_TRACE_FRAMES      64

/* Wake code the vendor's own HCI entry point uses; see ble_send_msg(). */

#define BLE_MSG_HCI_CMD       3

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct hci_note_s
{
  uint8_t   dir;                       /* '>' to controller, '<' from */
  uint8_t   type;                      /* H4 packet type byte */
  uint16_t  code;                      /* Opcode for CMD, event code for EVT */
  uint16_t  len;                       /* Whole frame, type byte included */
  uint32_t  ms;                        /* Milliseconds, absolute */
};

struct bk7258_btdrv_s
{
  struct bt_driver_s drv;
  uint16_t trace;                      /* Frames recorded so far */
  uint32_t rxdrop;                     /* Frames BTH4 had no room for */
  struct hci_note_s notes[HCI_TRACE_FRAMES];
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int  bk7258_bt_open(FAR struct bt_driver_s *btdev);
static int  bk7258_bt_send(FAR struct bt_driver_s *btdev,
                           enum bt_buf_type_e type,
                           FAR void *data, size_t len);
static void bk7258_bt_close(FAR struct bt_driver_s *btdev);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct bk7258_btdrv_s g_bkbt =
{
  .drv =
  {
    .head_reserve = 1,                 /* The H4 packet type byte */
    .open         = bk7258_bt_open,
    .send         = bk7258_bt_send,
    .close        = bk7258_bt_close,

    /* .receive is filled in by bt_driver_register(); the documentation is
     * explicit that a lower half must not set it.
     */
  }
};

/* Closed controller, controller-only archive. */

extern int bk_ble_hci_raw_to_controller(uint8_t *buf, uint16_t len);
extern int bk_ble_reg_hci_raw_recv_callback(int (*cb)(uint8_t *buf,
                                                      uint16_t len));

/* Match the vendor's own HCI entry point.
 *
 * bk_ble_hci_raw_to_controller() ends in hci_cmd_received(), which
 * allocates a kernel message and pushes it on a list; kernel_msg_send()
 * and kernel_event_set() touch nothing but that list and the interrupt
 * mask, so the caller is what tells the controller task to look.  The
 * vendor's UART HCI path, uart_rx_cmd_handler(), ends with
 * ble_send_msg(3), and this is the same kind of entry point, so it does
 * too.
 *
 * It is not a latency fix, and was added while chasing one: an idle
 * controller already answers in under 50 ms without it, and the
 * before/after measurements were identical to the millisecond.  Kept
 * because following the vendor flow at the same boundary is worth more
 * than one call per command costs.
 */

extern void ble_send_msg(uint32_t msg);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: hci_trace
 *
 * Description:
 *   Bounded record of the opening HCI dialogue, stored rather than
 *   printed.  A host bring-up is a fixed sequence of a few dozen frames,
 *   and when it stalls the only question worth answering is which command
 *   did not come back and when.
 *
 *   Nothing here may block.  The receive side runs in the controller's own
 *   callback thread, and an earlier version that called syslog() from
 *   there moved the stall around between runs -- 115200 baud of console
 *   inside the controller's context is enough to lose the events the trace
 *   exists to observe.  So this records four fields and returns;
 *   bk7258_hci_trace_dump() prints them later from a shell task.
 *
 ****************************************************************************/

static void hci_trace(char dir, FAR const uint8_t *buf, uint16_t len)
{
  FAR struct hci_note_s *n;

  if (g_bkbt.trace >= HCI_TRACE_FRAMES)
    {
      return;
    }

  n = &g_bkbt.notes[g_bkbt.trace++];

  n->dir  = (uint8_t)dir;
  n->type = buf[0];
  n->len  = len;
  n->ms   = TICK2MSEC(clock_systime_ticks());

  if (buf[0] == HCI_PKT_CMD && len >= 3)
    {
      n->code = (uint16_t)(buf[1] | (buf[2] << 8));
    }
  else if (buf[0] == HCI_PKT_EVT && len >= 2)
    {
      n->code = buf[1];
    }
  else
    {
      n->code = 0;
    }
}

/****************************************************************************
 * Name: bk7258_hci_recv
 *
 * Description:
 *   Controller callback, one whole H4 frame per call, running in the
 *   controller's own thread.  Hand the payload up to BTH4, which owns the
 *   ring and the poll wakeup.
 *
 ****************************************************************************/

static int bk7258_hci_recv(FAR uint8_t *buf, uint16_t len)
{
  enum bt_buf_type_e type;
  int ret;

  if (buf == NULL || len < 2)
    {
      return 0;
    }

  switch (buf[0])
    {
      case HCI_PKT_EVT:
        type = BT_EVT;
        break;

      case HCI_PKT_ACL:
        type = BT_ACL_IN;
        break;

      case HCI_PKT_ISO:
        type = BT_ISO_IN;
        break;

      default:

        /* SCO has no BTH4 receive type and this controller carries no
         * voice, so an SCO frame here would mean the stream lost sync.
         */

        return 0;
    }

  hci_trace('<', buf, len);

  ret = bt_netdev_receive(&g_bkbt.drv, type, buf + 1, (size_t)(len - 1));
  if (ret < 0)
    {
      /* BTH4's ring was full.  Losing whole frames shows up as missing
       * advertising reports rather than as an error anywhere, so count
       * them: bk7258_hci_trace_dump() reports the total.
       */

      g_bkbt.rxdrop++;
    }

  return 0;
}

/****************************************************************************
 * Name: bk7258_bt_open / bk7258_bt_send / bk7258_bt_close
 ****************************************************************************/

static int bk7258_bt_open(FAR struct bt_driver_s *btdev)
{
  UNUSED(btdev);

  /* The controller itself is brought up separately -- it owns the radio,
   * and powering it here would tie the radio's lifetime to whoever
   * happens to open the node.  This only attaches the frame path.
   */

  return bk_ble_reg_hci_raw_recv_callback(bk7258_hci_recv) == 0 ? 0 : -EIO;
}

static int bk7258_bt_send(FAR struct bt_driver_s *btdev,
                          enum bt_buf_type_e type,
                          FAR void *data, size_t len)
{
  FAR uint8_t *frame = (FAR uint8_t *)data - 1;
  int ret;

  UNUSED(btdev);

  switch (type)
    {
      case BT_CMD:
        frame[0] = HCI_PKT_CMD;
        break;

      case BT_ACL_OUT:
        frame[0] = HCI_PKT_ACL;
        break;

      case BT_ISO_OUT:
        frame[0] = HCI_PKT_ISO;
        break;

      default:
        return -EINVAL;
    }

  hci_trace('>', frame, (uint16_t)(len + 1));

  ret = bk_ble_hci_raw_to_controller(frame, (uint16_t)(len + 1));
  if (ret != 0)
    {
      /* A refusal and a silent controller look identical from the host
       * side -- both end as a command that never completes -- so say
       * which one happened.
       */

      syslog(LOG_ERR, "hci ! cmd %04x refused %d\n",
             (unsigned)(frame[1] | (frame[2] << 8)), ret);
      return -EIO;
    }

  ble_send_msg(BLE_MSG_HCI_CMD);
  return (int)len;
}

static void bk7258_bt_close(FAR struct bt_driver_s *btdev)
{
  UNUSED(btdev);

  bk_ble_reg_hci_raw_recv_callback(NULL);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_hci_register
 *
 * Description:
 *   Publish the transport.  bt_driver_register() names the node
 *   /dev/ttyHCI<CONFIG_BLUETOOTH_DEVICE_ID>, which is what the Bluetooth
 *   service opens by default.
 *
 ****************************************************************************/

int bk7258_hci_register(FAR const char *path)
{
  UNUSED(path);

  return bt_driver_register(&g_bkbt.drv);
}

/****************************************************************************
 * Name: bk7258_hci_trace_dump
 *
 * Description:
 *   Print what hci_trace() recorded, with the time each frame crossed the
 *   boundary.  Called from a shell task, where blocking on the console
 *   costs nothing.
 *
 ****************************************************************************/

void bk7258_hci_trace_dump(void)
{
  uint16_t i;

  syslog(LOG_INFO, "hci: %lu dropped, %u frames traced\n",
         (unsigned long)g_bkbt.rxdrop, g_bkbt.trace);

  for (i = 0; i < g_bkbt.trace; i++)
    {
      FAR struct hci_note_s *n = &g_bkbt.notes[i];
      unsigned long dt = (unsigned long)(n->ms - g_bkbt.notes[0].ms);

      if (n->type == HCI_PKT_CMD)
        {
          syslog(LOG_INFO, "  +%lums %c cmd %04x len %u\n",
                 dt, n->dir, n->code, n->len);
        }
      else if (n->type == HCI_PKT_EVT)
        {
          syslog(LOG_INFO, "  +%lums %c evt %02x len %u\n",
                 dt, n->dir, n->code, n->len);
        }
      else
        {
          syslog(LOG_INFO, "  +%lums %c type %02x len %u\n",
                 dt, n->dir, n->type, n->len);
        }
    }
}
