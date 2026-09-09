/****************************************************************************
 * board/contest_board/chip/bk7258_serial.c
 *
 * BK7258 UART character driver.
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

#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/fs/ioctl.h>
#include <termios.h>

#include <nuttx/serial/serial.h>
#include <nuttx/kthread.h>
#include <nuttx/signal.h>

#include "arm_internal.h"
#include "bk7258_lowputc.h"
#include "bk7258_wdt.h"
#include "bk7258_uart.h"
#include "chip.h"

#ifdef USE_SERIALDRIVER

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Decide which UART becomes /dev/console and how the others are numbered.
 * The console always takes ttyS0 so that it keeps a stable name.
 */

#if defined(CONFIG_UART0_SERIAL_CONSOLE) && defined(CONFIG_BK7258_UART0)
#  define CONSOLE_DEV     g_uart0port
#  define TTYS0_DEV       g_uart0port
#  define UART0_ASSIGNED  1
#elif defined(CONFIG_UART1_SERIAL_CONSOLE) && defined(CONFIG_BK7258_UART1)
#  define CONSOLE_DEV     g_uart1port
#  define TTYS0_DEV       g_uart1port
#  define UART1_ASSIGNED  1
#elif defined(CONFIG_UART2_SERIAL_CONSOLE) && defined(CONFIG_BK7258_UART2)
#  define CONSOLE_DEV     g_uart2port
#  define TTYS0_DEV       g_uart2port
#  define UART2_ASSIGNED  1
#elif defined(CONFIG_BK7258_UART0)
#  define TTYS0_DEV       g_uart0port
#  define UART0_ASSIGNED  1
#elif defined(CONFIG_BK7258_UART1)
#  define TTYS0_DEV       g_uart1port
#  define UART1_ASSIGNED  1
#elif defined(CONFIG_BK7258_UART2)
#  define TTYS0_DEV       g_uart2port
#  define UART2_ASSIGNED  1
#endif

#if defined(CONFIG_BK7258_UART0) && !defined(UART0_ASSIGNED)
#  define TTYS1_DEV       g_uart0port
#  define UART0_ASSIGNED  1
#elif defined(CONFIG_BK7258_UART1) && !defined(UART1_ASSIGNED)
#  define TTYS1_DEV       g_uart1port
#  define UART1_ASSIGNED  1
#elif defined(CONFIG_BK7258_UART2) && !defined(UART2_ASSIGNED)
#  define TTYS1_DEV       g_uart2port
#  define UART2_ASSIGNED  1
#endif

#if defined(CONFIG_BK7258_UART1) && !defined(UART1_ASSIGNED)
#  define TTYS2_DEV       g_uart1port
#  define UART1_ASSIGNED  1
#elif defined(CONFIG_BK7258_UART2) && !defined(UART2_ASSIGNED)
#  define TTYS2_DEV       g_uart2port
#  define UART2_ASSIGNED  1
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct bk7258_dev_s
{
  uintptr_t    base;      /* Base address of the UART registers */
  uint32_t     baud;      /* Configured baud rate */
  uint8_t      irq;       /* IRQ number of this UART */
  uint8_t      uart;      /* UART index, 0-2 */
  uint8_t      databits;  /* Number of data bits */
  uint8_t      parity;    /* 0=none, 1=odd, 2=even */
  bool         stop2;     /* true: two stop bits */
  uint32_t     im;        /* Shadow copy of the interrupt enable register */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int  bk7258_setup(struct uart_dev_s *dev);
static void bk7258_shutdown(struct uart_dev_s *dev);
static int  bk7258_attach(struct uart_dev_s *dev);
static void bk7258_detach(struct uart_dev_s *dev);
static int  bk7258_interrupt(int irq, void *context, void *arg);
static int  bk7258_ioctl(struct file *filep, int cmd, unsigned long arg);
static int  bk7258_receive(struct uart_dev_s *dev, unsigned int *status);
static void bk7258_rxint(struct uart_dev_s *dev, bool enable);
static bool bk7258_rxavailable(struct uart_dev_s *dev);
static void bk7258_send(struct uart_dev_s *dev, int ch);
static void bk7258_txint(struct uart_dev_s *dev, bool enable);
static bool bk7258_txready(struct uart_dev_s *dev);
static bool bk7258_txempty(struct uart_dev_s *dev);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct uart_ops_s g_uart_ops =
{
  .setup       = bk7258_setup,
  .shutdown    = bk7258_shutdown,
  .attach      = bk7258_attach,
  .detach      = bk7258_detach,
  .ioctl       = bk7258_ioctl,
  .receive     = bk7258_receive,
  .rxint       = bk7258_rxint,
  .rxavailable = bk7258_rxavailable,
  .send        = bk7258_send,
  .txint       = bk7258_txint,
  .txready     = bk7258_txready,
  .txempty     = bk7258_txempty,
};

#ifdef CONFIG_BK7258_UART0
static char g_uart0rxbuffer[CONFIG_UART0_RXBUFSIZE];
static char g_uart0txbuffer[CONFIG_UART0_TXBUFSIZE];

static struct bk7258_dev_s g_uart0priv =
{
  .base     = BK7258_UART0_BASE,
  .baud     = CONFIG_UART0_BAUD,
  .irq      = BK7258_IRQ_UART0,
  .uart     = 0,
  .databits = CONFIG_UART0_BITS,
  .parity   = CONFIG_UART0_PARITY,
  .stop2    = CONFIG_UART0_2STOP != 0,
};

static struct uart_dev_s g_uart0port =
{
  .recv     =
  {
    .size   = CONFIG_UART0_RXBUFSIZE,
    .buffer = g_uart0rxbuffer,
  },
  .xmit     =
  {
    .size   = CONFIG_UART0_TXBUFSIZE,
    .buffer = g_uart0txbuffer,
  },
  .ops      = &g_uart_ops,
  .priv     = &g_uart0priv,
};
#endif

#ifdef CONFIG_BK7258_UART1
static char g_uart1rxbuffer[CONFIG_UART1_RXBUFSIZE];
static char g_uart1txbuffer[CONFIG_UART1_TXBUFSIZE];

static struct bk7258_dev_s g_uart1priv =
{
  .base     = BK7258_UART1_BASE,
  .baud     = CONFIG_UART1_BAUD,
  .irq      = BK7258_IRQ_UART1,
  .uart     = 1,
  .databits = CONFIG_UART1_BITS,
  .parity   = CONFIG_UART1_PARITY,
  .stop2    = CONFIG_UART1_2STOP != 0,
};

static struct uart_dev_s g_uart1port =
{
  .recv     =
  {
    .size   = CONFIG_UART1_RXBUFSIZE,
    .buffer = g_uart1rxbuffer,
  },
  .xmit     =
  {
    .size   = CONFIG_UART1_TXBUFSIZE,
    .buffer = g_uart1txbuffer,
  },
  .ops      = &g_uart_ops,
  .priv     = &g_uart1priv,
};
#endif

#ifdef CONFIG_BK7258_UART2
static char g_uart2rxbuffer[CONFIG_UART2_RXBUFSIZE];
static char g_uart2txbuffer[CONFIG_UART2_TXBUFSIZE];

static struct bk7258_dev_s g_uart2priv =
{
  .base     = BK7258_UART2_BASE,
  .baud     = CONFIG_UART2_BAUD,
  .irq      = BK7258_IRQ_UART2,
  .uart     = 2,
  .databits = CONFIG_UART2_BITS,
  .parity   = CONFIG_UART2_PARITY,
  .stop2    = CONFIG_UART2_2STOP != 0,
};

static struct uart_dev_s g_uart2port =
{
  .recv     =
  {
    .size   = CONFIG_UART2_RXBUFSIZE,
    .buffer = g_uart2rxbuffer,
  },
  .xmit     =
  {
    .size   = CONFIG_UART2_TXBUFSIZE,
    .buffer = g_uart2txbuffer,
  },
  .ops      = &g_uart_ops,
  .priv     = &g_uart2priv,
};
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline uint32_t bk7258_serialin(struct bk7258_dev_s *priv,
                                       unsigned int offset)
{
  return getreg32(priv->base + offset);
}

static inline void bk7258_serialout(struct bk7258_dev_s *priv,
                                    unsigned int offset, uint32_t value)
{
  putreg32(value, priv->base + offset);
}

/* Black-box breadcrumbs.
 *
 * The wedge this port is chasing dies inside this interrupt handler with
 * interrupts masked and nothing dispatched, so nothing can report from the
 * inside.  These notes go just above the linked SRAM region -- outside
 * everything this image links or heaps -- and survive the watchdog reset
 * that follows; __start() prints them on the way back up.  Tags: 0x11
 * handler entry with int_status, 0x22 rx budget chosen, 0x33 handler exit
 * with round count.
 *
 * The address comes from the link script (_bbnote), not from a constant: a
 * constant picked to clear the region once stopped clearing it when
 * LENGTH(sram) grew, and the notes landed in live heap.
 */

extern uint32_t _bbnote[];

#define BB_BASE  ((volatile uint32_t *)_bbnote)

static inline void bb_note(uint32_t word)
{
  volatile uint32_t *bb = BB_BASE;
  uint32_t seq;

  if (bb[0] != 0xb1acb0c5)
    {
      bb[0] = 0xb1acb0c5;
      bb[1] = 0;
    }

  seq = bb[1] + 1;
  bb[1] = seq;
  bb[2 + (seq % 29)] = word;
}

/****************************************************************************
 * Name: bk7258_fifo_status_stable
 *
 * Description:
 *   Read the FIFO status register until two consecutive reads agree.
 *
 *   The register mixes live TX state (count in bits 0-7) and RX state
 *   (count in bits 8-15, flags at 16+) in one word, and both sides update
 *   asynchronously to the bus clock.  While the transmitter is draining --
 *   which on a console means during every echo -- a single read can return
 *   a torn value, and a torn RX count that reads high is what sent
 *   uart_recvchars after a byte that was never there: popping an empty FIFO
 *   underflows the hardware count, after which "data available" is true
 *   forever and the interrupt handler never comes home.  The console died
 *   on exactly the second echoed character every run because that is the
 *   first moment RX status reads overlap TX drain activity.
 *
 *   Two identical consecutive reads cannot both be torn by the same
 *   in-flight update, so agreement is taken as the true value.  The loop is
 *   bounded; persistent disagreement falls back to the latest read, which
 *   at worst delays a byte to the next interrupt.
 *
 ****************************************************************************/

static uint32_t bk7258_fifo_status_stable(struct bk7258_dev_s *priv)
{
  uint32_t prev;
  uint32_t curr;
  int tries;

  prev = bk7258_serialin(priv, BK7258_UART_FIFO_STATUS_OFFSET);

  for (tries = 0; tries < 8; tries++)
    {
      curr = bk7258_serialin(priv, BK7258_UART_FIFO_STATUS_OFFSET);

      if (curr == prev)
        {
          break;
        }

      prev = curr;
    }

  return curr;
}

/****************************************************************************
 * Name: bk7258_setup
 *
 * Description:
 *   Configure the UART baud, bits, parity and stop bits.  Interrupts are
 *   not enabled here; that happens in bk7258_attach().
 *
 ****************************************************************************/

static int bk7258_setup(struct uart_dev_s *dev)
{
  struct bk7258_dev_s *priv = dev->priv;

#ifndef CONFIG_SUPPRESS_UART_CONFIG
  bk7258_uart_configure(priv->base, priv->uart, priv->baud, priv->databits,
                        priv->parity, priv->stop2);
#endif

  priv->im = 0;
  bk7258_serialout(priv, BK7258_UART_INT_ENABLE_OFFSET, 0);

  return OK;
}

/****************************************************************************
 * Name: bk7258_shutdown
 ****************************************************************************/

static void bk7258_shutdown(struct uart_dev_s *dev)
{
  struct bk7258_dev_s *priv = dev->priv;

  /* Mask every source and stop the transceiver. */

  priv->im = 0;
  bk7258_serialout(priv, BK7258_UART_INT_ENABLE_OFFSET, 0);

  modifyreg32(priv->base + BK7258_UART_CONFIG_OFFSET,
              UART_CONFIG_TX_ENABLE | UART_CONFIG_RX_ENABLE, 0);
}

/****************************************************************************
 * Name: bk7258_attach
 ****************************************************************************/

static int bk7258_attach(struct uart_dev_s *dev)
{
  struct bk7258_dev_s *priv = dev->priv;
  int ret;

  ret = irq_attach(priv->irq, bk7258_interrupt, dev);
  if (ret == OK)
    {
      up_enable_irq(priv->irq);
    }

  return ret;
}

/****************************************************************************
 * Name: bk7258_detach
 ****************************************************************************/

static void bk7258_detach(struct uart_dev_s *dev)
{
  struct bk7258_dev_s *priv = dev->priv;

  up_disable_irq(priv->irq);
  irq_detach(priv->irq);
}

/****************************************************************************
 * Name: bk7258_interrupt
 ****************************************************************************/

static int bk7258_interrupt(int irq, void *context, void *arg)
{
  struct uart_dev_s *dev = (struct uart_dev_s *)arg;
  struct bk7258_dev_s *priv;
  uint32_t status;

  DEBUGASSERT(dev != NULL && dev->priv != NULL);
  priv = dev->priv;

  /* Keep draining until nothing we enabled is still pending.
   *
   * A single read-clear-handle pass loses events on this block: a byte that
   * lands after the write-1-to-clear but before exception return latches the
   * status bit while the NVIC line is already active, and depending on how
   * the routing matrix forwards it, no new edge may ever arrive.  The first
   * interactive test showed exactly that shape -- two characters echoed,
   * then RX went permanently quiet.
   *
   * The loop is bounded, and running out of the bound is treated as a stuck
   * source: everything is masked so the system stays alive and observable
   * rather than wedged inside this handler.  A console that answers with RX
   * dead is evidence; a hung board is not.
   */

  int rounds;

  for (rounds = 0; ; rounds++)
    {
      status  = bk7258_serialin(priv, BK7258_UART_INT_STATUS_OFFSET);

      if ((status & priv->im) == 0 || rounds >= 32)
        {
          break;
        }

      bb_note(0x11000000 | (status & 0xffff));

      bk7258_serialout(priv, BK7258_UART_INT_STATUS_OFFSET, status);

      if ((status & priv->im &
           (UART_INT_RX_NEED_READ | UART_INT_RX_FINISH)) != 0)
        {
          bb_note(0x22000000);
          uart_recvchars(dev);
        }

      if ((status & priv->im & UART_INT_TX_NEED_WRITE) != 0)
        {
          uart_xmitchars(dev);
        }
    }

  /* Sweep stragglers.  A byte can land after the last status clear without
   * re-asserting need_read -- the interrupt condition is computed from the
   * same lying count field -- and a byte the interrupt never announces
   * would otherwise sit in the FIFO until the next unrelated interrupt,
   * which is exactly the two-character-late delivery this port debugged on
   * hardware.  rd_ready is checked directly on the way out.
   */

  if ((bk7258_fifo_status_stable(priv) & UART_FIFO_STATUS_RD_READY) != 0)
    {
      uart_recvchars(dev);
    }

  bb_note(0x33000000 | (uint32_t)rounds);

  if (rounds >= 32)
    {
      priv->im = 0;
      bk7258_serialout(priv, BK7258_UART_INT_ENABLE_OFFSET, 0);
      bk7258_serialout(priv, BK7258_UART_INT_STATUS_OFFSET, 0xffffffff);
    }

  return OK;
}

/****************************************************************************
 * Name: bk7258_ioctl
 ****************************************************************************/

static int bk7258_ioctl(struct file *filep, int cmd, unsigned long arg)
{
#ifdef CONFIG_SERIAL_TERMIOS
  struct inode *inode = filep->f_inode;
  struct uart_dev_s *dev = inode->i_private;
  struct bk7258_dev_s *priv = dev->priv;
#endif
  int ret = OK;

  switch (cmd)
    {
#ifdef CONFIG_SERIAL_TERMIOS
      case TCGETS:
        {
          struct termios *termiosp = (struct termios *)arg;

          if (termiosp == NULL)
            {
              return -EINVAL;
            }

          termiosp->c_cflag = 0;

          if (priv->parity == 1)
            {
              termiosp->c_cflag |= PARENB | PARODD;
            }
          else if (priv->parity == 2)
            {
              termiosp->c_cflag |= PARENB;
            }

          if (priv->stop2)
            {
              termiosp->c_cflag |= CSTOPB;
            }

          termiosp->c_cflag |= CS5 + (priv->databits - 5);

          cfsetispeed(termiosp, priv->baud);
          cfsetospeed(termiosp, priv->baud);
        }
        break;

      case TCSETS:
        {
          struct termios *termiosp = (struct termios *)arg;

          if (termiosp == NULL)
            {
              return -EINVAL;
            }

          switch (termiosp->c_cflag & CSIZE)
            {
              case CS5:
                priv->databits = 5;
                break;

              case CS6:
                priv->databits = 6;
                break;

              case CS7:
                priv->databits = 7;
                break;

              default:
                priv->databits = 8;
                break;
            }

          if ((termiosp->c_cflag & PARENB) == 0)
            {
              priv->parity = 0;
            }
          else
            {
              priv->parity = (termiosp->c_cflag & PARODD) != 0 ? 1 : 2;
            }

          priv->stop2 = (termiosp->c_cflag & CSTOPB) != 0;
          priv->baud  = cfgetispeed(termiosp);

          /* Re-apply the line settings.  Interrupt enables survive because
           * bk7258_uart_configure() masks them and we restore our shadow.
           */

          bk7258_uart_configure(priv->base, priv->uart, priv->baud,
                                priv->databits, priv->parity, priv->stop2);
          bk7258_serialout(priv, BK7258_UART_INT_ENABLE_OFFSET, priv->im);
        }
        break;
#endif /* CONFIG_SERIAL_TERMIOS */

      default:
        ret = -ENOTTY;
        break;
    }

  return ret;
}

/****************************************************************************
 * Name: bk7258_receive
 ****************************************************************************/

static int bk7258_receive(struct uart_dev_s *dev, unsigned int *status)
{
  struct bk7258_dev_s *priv = dev->priv;
  uint32_t regval;

  if (status != NULL)
    {
      *status = bk7258_serialin(priv, BK7258_UART_FIFO_STATUS_OFFSET);
    }

  regval = bk7258_serialin(priv, BK7258_UART_FIFO_PORT_OFFSET);
  return (int)((regval & UART_FIFO_PORT_RXDATA_MASK) >>
               UART_FIFO_PORT_RXDATA_SHIFT);
}

/****************************************************************************
 * Name: bk7258_rxint
 ****************************************************************************/

static void bk7258_rxint(struct uart_dev_s *dev, bool enable)
{
  struct bk7258_dev_s *priv = dev->priv;
  irqstate_t flags;

  flags = enter_critical_section();

  if (enable)
    {
      priv->im |= UART_INT_RX_ALL;
    }
  else
    {
      priv->im &= ~UART_INT_RX_ALL;
    }

  bk7258_serialout(priv, BK7258_UART_INT_ENABLE_OFFSET, priv->im);
  leave_critical_section(flags);
}

/****************************************************************************
 * Name: bk7258_rxavailable
 ****************************************************************************/

static bool bk7258_rxavailable(struct uart_dev_s *dev)
{
  struct bk7258_dev_s *priv = dev->priv;

  /* Trust rd_ready and nothing else.
   *
   * The count and empty fields of fifo_status lie under load, in both
   * directions.  With the transmitter active -- every console echo -- the
   * empty flag first read "not empty" forever and sent the original driver
   * into an unbounded drain of a drained FIFO; guarded by the count field
   * instead, it read "empty" while two received bytes sat in the FIFO, and
   * those bytes only surfaced when the next line's CR arrived.  Both
   * failures were watched live on hardware (fs=0x003a0000: rd_ready set,
   * empty set, count zero -- with data demonstrably present).
   *
   * rd_ready tracked the truth through all of it, as does its transmit twin
   * wr_ready, which this driver has trusted from the start without a single
   * glitch.
   */

  return (bk7258_fifo_status_stable(priv) & UART_FIFO_STATUS_RD_READY) != 0;
}

/****************************************************************************
 * Name: bk7258_send
 ****************************************************************************/

static void bk7258_send(struct uart_dev_s *dev, int ch)
{
  struct bk7258_dev_s *priv = dev->priv;

  bk7258_serialout(priv, BK7258_UART_FIFO_PORT_OFFSET,
                   (uint32_t)(ch & 0xff));
}

/****************************************************************************
 * Name: bk7258_txint
 ****************************************************************************/

static void bk7258_txint(struct uart_dev_s *dev, bool enable)
{
  struct bk7258_dev_s *priv = dev->priv;
  irqstate_t flags;

  flags = enter_critical_section();

  if (enable)
    {
      priv->im |= UART_INT_TX_NEED_WRITE;
      bk7258_serialout(priv, BK7258_UART_INT_ENABLE_OFFSET, priv->im);

      /* Prime the pump: the FIFO may already be below the threshold, in
       * which case no fresh edge would ever arrive.
       */

      uart_xmitchars(dev);
    }
  else
    {
      priv->im &= ~UART_INT_TX_NEED_WRITE;
      bk7258_serialout(priv, BK7258_UART_INT_ENABLE_OFFSET, priv->im);
    }

  leave_critical_section(flags);
}

/****************************************************************************
 * Name: bk7258_txready
 ****************************************************************************/

static bool bk7258_txready(struct uart_dev_s *dev)
{
  struct bk7258_dev_s *priv = dev->priv;

  return (bk7258_fifo_status_stable(priv) &
          UART_FIFO_STATUS_WR_READY) != 0;
}

/****************************************************************************
 * Name: bk7258_txempty
 ****************************************************************************/

static bool bk7258_txempty(struct uart_dev_s *dev)
{
  struct bk7258_dev_s *priv = dev->priv;

  return (bk7258_serialin(priv, BK7258_UART_FIFO_STATUS_OFFSET) &
          UART_FIFO_STATUS_TX_EMPTY) != 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: arm_earlyserialinit
 *
 * Description:
 *   Bring up the console UART early so that debug output works before the
 *   full serial driver is registered.
 *
 ****************************************************************************/

void arm_earlyserialinit(void)
{
#ifdef CONSOLE_DEV
  CONSOLE_DEV.isconsole = true;
  bk7258_setup(&CONSOLE_DEV);
#endif
}

/****************************************************************************
 * Name: arm_serialinit
 *
 * Description:
 *   Register /dev/console and /dev/ttyS[n].
 *
 ****************************************************************************/

void arm_serialinit(void)
{
#ifdef CONSOLE_DEV
  uart_register("/dev/console", &CONSOLE_DEV);

#ifdef CONFIG_TTY_SIGINT
  /* Turn on ISIG for the console, which is what gates the Ctrl-C path:
   * uart_recvchars() -> uart_check_special() returns immediately unless this
   * bit is set, so without it the interrupt character is just another byte
   * in the receive buffer and no runaway task can ever be stopped.
   *
   * uart_register() would normally do this itself, but only for a device
   * whose isconsole flag is set at registration time, and it sets ECHO and
   * ICANON in the same breath.  This port never reaches that path -- its
   * early console comes up through bk7258_lowputc() rather than
   * arm_earlyserialinit(), which nothing calls -- and enabling driver-side
   * echo and canonical mode underneath NSH's own line editing would be a
   * behaviour change nobody asked for.  So set the one bit that matters.
   */

  CONSOLE_DEV.tc_lflag |= ISIG;
#endif
#endif
#ifdef TTYS0_DEV
  uart_register("/dev/ttyS0", &TTYS0_DEV);
#endif
#ifdef TTYS1_DEV
  uart_register("/dev/ttyS1", &TTYS1_DEV);
#endif
#ifdef TTYS2_DEV
  uart_register("/dev/ttyS2", &TTYS2_DEV);
#endif
}

/****************************************************************************
 * Name: up_putc
 *
 * Description:
 *   Write one character to the console, bypassing the driver.  Used by the
 *   low-level debug and syslog paths.
 *
 ****************************************************************************/

void up_putc(int ch)
{
#ifdef HAVE_CONSOLE
  arm_lowputc((char)ch);
#endif
}

#endif /* USE_SERIALDRIVER */

/****************************************************************************
 * Name: bk7258_console_monitor
 *
 * Description:
 *   Print the console's interrupt-chain state every few seconds, through
 *   up_putc rather than through the serial driver, so it keeps reporting
 *   when the driver is exactly what broke.
 *
 *   This exists because the port's remaining bug is an RX path that dies
 *   after the first couple of received characters while everything else --
 *   SysTick, the scheduler, transmit -- stays healthy.  A system that
 *   healthy keeps feeding the watchdog, so the failure defeats both the
 *   watchdog and the `reboot` command (which would need working RX to be
 *   typed).  The monitor closes that hole two ways: it shows which link of
 *   the chain went dark (UART int_status vs NVIC pending vs enables), and
 *   if the black-box sequence freezes while the hardware says receive data
 *   is pending, it concludes the RX path is dead, dumps everything, and
 *   reboots through the watchdog -- making even this half-dead state
 *   self-recovering.
 *
 ****************************************************************************/

static int bk7258_console_monitor(int argc, char **argv)
{
  struct bk7258_dev_s *priv = &g_uart0priv;
  volatile uint32_t *bb = BB_BASE;
  uint32_t last_seq = 0;
  int frozen = 0;

  for (; ; )
    {
      uint32_t seq = (bb[0] == 0xb1acb0c5) ? bb[1] : 0;
      uint32_t ie  = bk7258_serialin(priv, BK7258_UART_INT_ENABLE_OFFSET);
      uint32_t is  = bk7258_serialin(priv, BK7258_UART_INT_STATUS_OFFSET);
      uint32_t fs  = bk7258_serialin(priv, BK7258_UART_FIFO_STATUS_OFFSET);
      uint32_t en  = getreg32(0xe000e100);      /* NVIC ISER0 */
      uint32_t pnd = getreg32(0xe000e200);      /* NVIC ISPR0 */
      uint32_t act = getreg32(0xe000e300);      /* NVIC IABR0 */
      uint32_t mtx = getreg32(BK7258_SYS_CPU0_INT_EN(4));
      bool rx_pending = (fs & UART_FIFO_STATUS_RD_READY) != 0 &&
                        (ie & UART_INT_RX_ALL) != 0;

      /* Stay quiet while healthy: a diagnostic that prints into the middle
       * of whatever the user is typing is itself a console defect.  Speak
       * only once something looks wrong.
       */

      if (frozen > 0)
        {
          _alert("conmon seq=%lu im=%02lx ie=%02lx is=%02lx fs=%08lx "
                 "nvic e/p/a=%d/%d/%d mtx=%d froz=%d\n",
                 (unsigned long)seq, (unsigned long)priv->im,
                 (unsigned long)ie, (unsigned long)is, (unsigned long)fs,
                 (int)((en >> 4) & 1), (int)((pnd >> 4) & 1),
                 (int)((act >> 4) & 1), (int)((mtx >> 4) & 1), frozen);
        }

      if (seq == last_seq && rx_pending)
        {
          frozen++;
        }
      else
        {
          frozen = 0;
        }

      last_seq = seq;

      if (frozen >= 3)
        {
          _alert("conmon: RX interrupt chain dead with data pending; "
                 "rebooting\n");
          bk7258_wdt_reboot();
        }

      nxsig_usleep(3000000);
    }

  return 0;
}

/****************************************************************************
 * Name: bk7258_serial_monitor_start
 ****************************************************************************/

void bk7258_serial_monitor_start(void)
{
  kthread_create("conmon", 100, 2048, bk7258_console_monitor, NULL);
}
