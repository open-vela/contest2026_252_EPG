/****************************************************************************
 * board/contest_board/chip/bk7258_flash.c
 *
 * On-chip flash MTD for the BK7258.
 *
 * The firmware runs in place out of this same chip, which sets the tone for
 * everything here.  Four facts decided the shape, and three of them were
 * measured on the board with src/flashtest.c rather than read out of a
 * header, because the vendor tree contains enough contradictory material to
 * get each one wrong:
 *
 *   - The register map is the one in middleware/soc/bk7258/soc/flash_struct.h,
 *     at 0x44030000.  The other map, in middleware/driver/flash/flash.c, is
 *     guarded by CONFIG_SOC_BK7256XX and places the block at 0x00803000;
 *     this SoC is the BK7236XX family, so that one is a trap.  dev_id reads
 *     "FLSH" at offset 0, which settles it.
 *
 *   - Software-path addresses are raw physical bytes.  The controller
 *     inserts two CRC bytes every 32 on the CPU's instruction path, so an
 *     XIP address is 32/34 of a physical one -- but that mapping does not
 *     apply here.  Reading physical 0x11020 returns the two CRC bytes of the
 *     app image's first block, byte for byte, which no decoded view would.
 *
 *   - The chip arrives fully write protected.  The boot loader leaves the
 *     block-protect field (SR[6:2]) at 0x1f, and NuttX does not run the
 *     vendor flash driver that would otherwise manage it, so every erase and
 *     program here has to clear it and put it back.  Without that the flash
 *     silently ignores the write and the read-back looks like a dead driver.
 *
 *   - The region is not negotiable.  The vendor partition table fills
 *     exactly 4MB and this part is 8MB, so the top half is unclaimed -- but
 *     a scan found the last six sectors in use, one of them starting with
 *     "TLV", which is factory data this board's RF almost certainly needs.
 *     The default region stops 64KB short of the end for that reason.
 *
 * What an erase costs, measured rather than assumed: one read-modify-erase-
 * write of a 4KB sector through the FTL takes ~81 ms, repeatably (see
 * "flashtest time").  Whether the core keeps fetching instructions while the
 * chip is busy was NOT established -- an earlier version of this comment
 * asserted it did not, on no evidence, and that claim was wrong enough to
 * send a debugging session in the wrong direction.  What is known is that
 * the driver runs, prints and keeps time between operations.  Anything
 * scheduling against this driver should budget for the 81 ms; anything
 * relying on what happens inside it should measure first.
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
#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <nuttx/fs/ioctl.h>
#include <nuttx/irq.h>
#include <nuttx/kmalloc.h>
#include <nuttx/mtd/mtd.h>
#include <nuttx/mutex.h>
#include <nuttx/spinlock.h>

#include "arm_internal.h"

#include "bk7258_flash.h"

#ifdef CONFIG_BK7258_WDT
#  include "bk7258_wdt.h"
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define BK7258_FLASH_REG_BASE       0x44030000ul

#define BK7258_FLASH_DEV_ID         (BK7258_FLASH_REG_BASE + 0x00)
#define BK7258_FLASH_OP_CTRL        (BK7258_FLASH_REG_BASE + 0x10)
#define BK7258_FLASH_DATA_SW        (BK7258_FLASH_REG_BASE + 0x14)
#define BK7258_FLASH_DATA_FS        (BK7258_FLASH_REG_BASE + 0x18)
#define BK7258_FLASH_CMD_CFG        (BK7258_FLASH_REG_BASE + 0x1c)
#define BK7258_FLASH_STATE          (BK7258_FLASH_REG_BASE + 0x24)
#define BK7258_FLASH_CONFIG         (BK7258_FLASH_REG_BASE + 0x28)
#define BK7258_FLASH_OP_CMD         (BK7258_FLASH_REG_BASE + 0x54)

/* op_ctrl.  The low bits are reserved and busy is read-only, so preserving
 * wp_value is the whole job of a read-modify-write here.
 */

#define FLASH_OP_SW                 (1u << 29)
#define FLASH_WP_VALUE              (1u << 30)
#define FLASH_BUSY_SW               (1u << 31)

/* op_cmd: a 24-bit physical address and a 5-bit command in one register. */

#define FLASH_ADDR_MASK             0x00ffffffu
#define FLASH_CMD_SHIFT             24

#define FLASH_CMD_RDSR              3
#define FLASH_CMD_READ              5
#define FLASH_CMD_RDSR2             6
#define FLASH_CMD_WRSR2             7
#define FLASH_CMD_PP                12
#define FLASH_CMD_SE                13

/* config.wrsr_data occupies bits 25:10. */

#define FLASH_WRSR_SHIFT            10
#define FLASH_WRSR_MASK             (0xffffu << FLASH_WRSR_SHIFT)

/* Status register of the gd_25Q32E (id 0xc86517): five block-protect bits
 * at SR[6:2] with the complement bit at SR[14].  Both zero means the whole
 * chip is writable, which is the vendor table's "protect none".
 */

#define FLASH_SR_BP_SHIFT           2
#define FLASH_SR_BP_MASK            (0x1fu << FLASH_SR_BP_SHIFT)
#define FLASH_SR_CMP                (1u << 14)
#define FLASH_SR_PROTECT_BITS       (FLASH_SR_BP_MASK | FLASH_SR_CMP)

/* Chip geometry.  The controller moves 32 bytes per software operation,
 * which is unrelated to the flash's own 256-byte program page: a program
 * only has to stay inside one page, and 32 divides it.
 */

#define FLASH_UNIT                  32
#define FLASH_SECTOR                4096

/* Bounded spins.  The busy bit is the flash's own, so an erase legitimately
 * holds it for tens of milliseconds.  The bound exists so a wedged
 * controller cannot hang the system outright; it is not a real timeout, and
 * it is deliberately far larger than any erase this part performs.
 */

#define FLASH_SPINS                 100000000

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct bk7258_flash_dev_s
{
  struct mtd_dev_s mtd;         /* Must be first: we cast between them */
  mutex_t          lock;        /* One operation on the controller at a time */
  uint32_t         base;        /* Physical start of the region we own */
  uint32_t         size;        /* Its length in bytes */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int bk7258_flash_erase(FAR struct mtd_dev_s *dev, off_t startblock,
                              size_t nblocks);
static ssize_t bk7258_flash_bread(FAR struct mtd_dev_s *dev, off_t startblock,
                                  size_t nblocks, FAR uint8_t *buffer);
static ssize_t bk7258_flash_bwrite(FAR struct mtd_dev_s *dev,
                                   off_t startblock, size_t nblocks,
                                   FAR const uint8_t *buffer);
static ssize_t bk7258_flash_read(FAR struct mtd_dev_s *dev, off_t offset,
                                 size_t nbytes, FAR uint8_t *buffer);
static int bk7258_flash_ioctl(FAR struct mtd_dev_s *dev, int cmd,
                              unsigned long arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct bk7258_flash_dev_s g_bk7258_flash =
{
  .mtd =
    {
      .erase  = bk7258_flash_erase,
      .bread  = bk7258_flash_bread,
      .bwrite = bk7258_flash_bwrite,
      .read   = bk7258_flash_read,
      .ioctl  = bk7258_flash_ioctl,
      .name   = "bk7258_flash",
    },
  .lock = NXMUTEX_INITIALIZER,
  .base = CONFIG_BK7258_FLASH_OFFSET,
  .size = CONFIG_BK7258_FLASH_SIZE,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_flash_wait
 ****************************************************************************/

static void bk7258_flash_wait(void)
{
  uint32_t i;

  for (i = 0; i < FLASH_SPINS; i++)
    {
      if ((getreg32(BK7258_FLASH_OP_CTRL) & FLASH_BUSY_SW) == 0)
        {
          return;
        }
    }

  ferr("ERROR: flash controller stayed busy\n");
}

/****************************************************************************
 * Name: bk7258_flash_op
 *
 * Description:
 *   Issue one controller command.  Address and opcode share op_cmd; the
 *   trigger lives in op_ctrl, whose wp_value has to survive the write.
 *
 ****************************************************************************/

static void bk7258_flash_op(uint32_t cmd, uint32_t addr)
{
  uint32_t regval;

  bk7258_flash_wait();

  putreg32((addr & FLASH_ADDR_MASK) | (cmd << FLASH_CMD_SHIFT),
           BK7258_FLASH_OP_CMD);

  regval = getreg32(BK7258_FLASH_OP_CTRL) & FLASH_WP_VALUE;
  putreg32(regval | FLASH_OP_SW, BK7258_FLASH_OP_CTRL);

  bk7258_flash_wait();
}

/****************************************************************************
 * Name: bk7258_flash_read_sr / bk7258_flash_write_sr
 *
 * Description:
 *   The part has a 16-bit status register read as two bytes and written as
 *   one WRSR2.  wp_value has to be asserted across the write; the vendor HAL
 *   raises it before the command and drops it after, and so do we.
 *
 ****************************************************************************/

static uint32_t bk7258_flash_read_sr(void)
{
  uint32_t sr;

  putreg32(0, BK7258_FLASH_CMD_CFG);

  bk7258_flash_op(FLASH_CMD_RDSR, 0);
  sr = getreg32(BK7258_FLASH_STATE) & 0xff;

  bk7258_flash_op(FLASH_CMD_RDSR2, 0);
  sr |= (getreg32(BK7258_FLASH_STATE) & 0xff) << 8;

  return sr;
}

static void bk7258_flash_write_sr(uint32_t sr)
{
  uint32_t regval;

  bk7258_flash_wait();

  putreg32(0, BK7258_FLASH_CMD_CFG);

  regval = getreg32(BK7258_FLASH_CONFIG) & ~FLASH_WRSR_MASK;
  putreg32(regval | ((sr & 0xffff) << FLASH_WRSR_SHIFT), BK7258_FLASH_CONFIG);

  regval = getreg32(BK7258_FLASH_OP_CTRL) & FLASH_WP_VALUE;
  putreg32(regval | FLASH_WP_VALUE, BK7258_FLASH_OP_CTRL);

  bk7258_flash_op(FLASH_CMD_WRSR2, 0);

  regval = getreg32(BK7258_FLASH_OP_CTRL) & ~FLASH_WP_VALUE;
  putreg32(regval, BK7258_FLASH_OP_CTRL);
}

/****************************************************************************
 * Name: bk7258_flash_unprotect / bk7258_flash_reprotect
 *
 * Description:
 *   Clear the block-protect field for the duration of a batch and put the
 *   original value back afterwards.  Done per batch rather than per unit:
 *   each status-register write is a flash write cycle, so doing it around
 *   every 32 bytes would be both slow and pointless wear.
 *
 *   Returns the value to hand back to bk7258_flash_reprotect().
 *
 ****************************************************************************/

static uint32_t bk7258_flash_unprotect(void)
{
  uint32_t sr = bk7258_flash_read_sr();

  if ((sr & FLASH_SR_PROTECT_BITS) != 0)
    {
      bk7258_flash_write_sr(sr & ~FLASH_SR_PROTECT_BITS);
    }

  return sr;
}

static void bk7258_flash_reprotect(uint32_t sr)
{
  if ((sr & FLASH_SR_PROTECT_BITS) != 0)
    {
      bk7258_flash_write_sr(sr);
    }
}

/****************************************************************************
 * Name: bk7258_flash_read_unit / bk7258_flash_write_unit
 *
 * Description:
 *   Move one 32-byte unit.  Note the asymmetry, which is the controller's
 *   and not a mistake: a read issues the command and then drains eight words
 *   out of the data register, while a write fills eight words in first and
 *   issues the command last.
 *
 ****************************************************************************/

static void bk7258_flash_read_unit(uint32_t addr, FAR uint8_t *out)
{
  uint32_t buf[8];
  irqstate_t flags;
  int i;

  flags = enter_critical_section();

  bk7258_flash_op(FLASH_CMD_READ, addr & ~(FLASH_UNIT - 1));

  for (i = 0; i < 8; i++)
    {
      buf[i] = getreg32(BK7258_FLASH_DATA_FS);
    }

  leave_critical_section(flags);

  memcpy(out, buf, FLASH_UNIT);
}

static void bk7258_flash_write_unit(uint32_t addr, FAR const uint8_t *in)
{
  uint32_t buf[8];
  irqstate_t flags;
  int i;

  memcpy(buf, in, FLASH_UNIT);

  flags = enter_critical_section();

  bk7258_flash_wait();

  for (i = 0; i < 8; i++)
    {
      putreg32(buf[i], BK7258_FLASH_DATA_SW);
    }

  bk7258_flash_op(FLASH_CMD_PP, addr & ~(FLASH_UNIT - 1));

  leave_critical_section(flags);
}

/****************************************************************************
 * Name: bk7258_flash_kick_wdt
 *
 * Description:
 *   Feed the watchdog from inside a long batch.  This is load bearing, and
 *   it was measured rather than reasoned: with these calls removed, the
 *   block stress test does not survive its first case -- the watchdog bites
 *   with the test task on the CPU.  With them, the test passes.
 *
 *   The mechanism is only partly pinned down, and the honest version is
 *   worth keeping.  Heavy flash traffic starves the normal feed, which
 *   rides the RTC alarm callback (bk7258_rtc.c, paced by counter time).
 *   Exactly how it starves it is NOT established: an earlier comment here
 *   claimed the core cannot fetch instructions while the chip is busy, which
 *   was asserted on no evidence and cost a debugging session.  What is
 *   measured is the starvation itself, and that a non-flash load of similar
 *   weight (ostest) never provokes it.
 *
 *   bk7258_wdt_service() keeps its own semantics: if userspace has claimed
 *   the watchdog and let its deadline pass, this still lets the bite
 *   through rather than papering over it.
 *
 ****************************************************************************/

static void bk7258_flash_kick_wdt(void)
{
#ifdef CONFIG_BK7258_WDT
  bk7258_wdt_service();
#endif
}

/****************************************************************************
 * Name: bk7258_flash_inrange
 *
 * Description:
 *   The guard.  Every path that can reach a program or an erase comes
 *   through here first, because the cost of an off-by-one is the boot
 *   loader, another core's image, or the factory calibration block.
 *
 ****************************************************************************/

static bool bk7258_flash_inrange(FAR struct bk7258_flash_dev_s *priv,
                                 uint32_t offset, uint32_t len)
{
  return len <= priv->size && offset <= priv->size - len;
}

/****************************************************************************
 * Name: bk7258_flash_erase
 ****************************************************************************/

static int bk7258_flash_erase(FAR struct mtd_dev_s *dev, off_t startblock,
                              size_t nblocks)
{
  FAR struct bk7258_flash_dev_s *priv =
    (FAR struct bk7258_flash_dev_s *)dev;
  uint32_t offset = (uint32_t)startblock * FLASH_SECTOR;
  uint32_t len    = (uint32_t)nblocks * FLASH_SECTOR;
  uint32_t sr;
  uint32_t i;
  int ret;

  if (!bk7258_flash_inrange(priv, offset, len))
    {
      ferr("ERROR: erase %" PRIu32 "+%" PRIu32 " outside the region\n",
           offset, len);
      return -EFAULT;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  sr = bk7258_flash_unprotect();

  for (i = 0; i < nblocks; i++)
    {
      bk7258_flash_kick_wdt();
      bk7258_flash_op(FLASH_CMD_SE,
                      priv->base + offset + i * FLASH_SECTOR);
    }

  bk7258_flash_reprotect(sr);
  nxmutex_unlock(&priv->lock);

  return (int)nblocks;
}

/****************************************************************************
 * Name: bk7258_flash_read
 ****************************************************************************/

static ssize_t bk7258_flash_read(FAR struct mtd_dev_s *dev, off_t offset,
                                 size_t nbytes, FAR uint8_t *buffer)
{
  FAR struct bk7258_flash_dev_s *priv =
    (FAR struct bk7258_flash_dev_s *)dev;
  uint8_t unit[FLASH_UNIT];
  uint32_t addr;
  size_t done = 0;
  int ret;

  if (!bk7258_flash_inrange(priv, (uint32_t)offset, (uint32_t)nbytes))
    {
      return -EFAULT;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  addr = priv->base + (uint32_t)offset;

  while (done < nbytes)
    {
      uint32_t skip = addr & (FLASH_UNIT - 1);
      size_t   take = FLASH_UNIT - skip;

      if (take > nbytes - done)
        {
          take = nbytes - done;
        }

      bk7258_flash_read_unit(addr, unit);
      memcpy(buffer + done, unit + skip, take);

      addr += take;
      done += take;
    }

  nxmutex_unlock(&priv->lock);
  return (ssize_t)nbytes;
}

/****************************************************************************
 * Name: bk7258_flash_bread
 ****************************************************************************/

static ssize_t bk7258_flash_bread(FAR struct mtd_dev_s *dev, off_t startblock,
                                  size_t nblocks, FAR uint8_t *buffer)
{
  ssize_t ret = bk7258_flash_read(dev,
                                  startblock * CONFIG_BK7258_FLASH_BLOCKSIZE,
                                  nblocks * CONFIG_BK7258_FLASH_BLOCKSIZE,
                                  buffer);

  return ret < 0 ? ret : (ssize_t)nblocks;
}

/****************************************************************************
 * Name: bk7258_flash_bwrite
 ****************************************************************************/

static ssize_t bk7258_flash_bwrite(FAR struct mtd_dev_s *dev,
                                   off_t startblock, size_t nblocks,
                                   FAR const uint8_t *buffer)
{
  FAR struct bk7258_flash_dev_s *priv =
    (FAR struct bk7258_flash_dev_s *)dev;
  uint32_t offset = (uint32_t)startblock * CONFIG_BK7258_FLASH_BLOCKSIZE;
  uint32_t len    = (uint32_t)nblocks * CONFIG_BK7258_FLASH_BLOCKSIZE;
  uint32_t addr;
  uint32_t sr;
  uint32_t i;
  int ret;

  if (!bk7258_flash_inrange(priv, offset, len))
    {
      ferr("ERROR: write %" PRIu32 "+%" PRIu32 " outside the region\n",
           offset, len);
      return -EFAULT;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  /* The block size is a multiple of the 32-byte unit, so every unit is
   * whole and none of the read-modify-write dance a partial one would need
   * arises here.
   */

  addr = priv->base + offset;
  sr   = bk7258_flash_unprotect();

  for (i = 0; i < len; i += FLASH_UNIT)
    {
      /* Once a sector's worth: programming is far quicker than erasing. */

      if ((i % FLASH_SECTOR) == 0)
        {
          bk7258_flash_kick_wdt();
        }

      bk7258_flash_write_unit(addr + i, buffer + i);
    }

  bk7258_flash_reprotect(sr);
  nxmutex_unlock(&priv->lock);

  return (ssize_t)nblocks;
}

/****************************************************************************
 * Name: bk7258_flash_ioctl
 ****************************************************************************/

static int bk7258_flash_ioctl(FAR struct mtd_dev_s *dev, int cmd,
                              unsigned long arg)
{
  FAR struct bk7258_flash_dev_s *priv =
    (FAR struct bk7258_flash_dev_s *)dev;
  int ret = -ENOTTY;

  switch (cmd)
    {
      case MTDIOC_GEOMETRY:
        {
          FAR struct mtd_geometry_s *geo =
            (FAR struct mtd_geometry_s *)arg;

          if (geo != NULL)
            {
              memset(geo, 0, sizeof(*geo));
              geo->blocksize    = CONFIG_BK7258_FLASH_BLOCKSIZE;
              geo->erasesize    = FLASH_SECTOR;
              geo->neraseblocks = priv->size / FLASH_SECTOR;
              strlcpy(geo->model, "bk7258-flash", sizeof(geo->model));
              ret = OK;
            }
          else
            {
              ret = -EINVAL;
            }
        }
        break;

      case BIOC_PARTINFO:
        {
          FAR struct partition_info_s *info =
            (FAR struct partition_info_s *)arg;

          if (info != NULL)
            {
              info->numsectors  = priv->size / CONFIG_BK7258_FLASH_BLOCKSIZE;
              info->sectorsize  = CONFIG_BK7258_FLASH_BLOCKSIZE;
              info->startsector = 0;
              strlcpy(info->parent, "bk7258_flash", sizeof(info->parent));
              ret = OK;
            }
          else
            {
              ret = -EINVAL;
            }
        }
        break;

      case MTDIOC_ERASESTATE:
        {
          FAR uint8_t *state = (FAR uint8_t *)arg;

          if (state != NULL)
            {
              *state = 0xff;
              ret = OK;
            }
          else
            {
              ret = -EINVAL;
            }
        }
        break;

      /* No MTDIOC_XIPBASE.  The CPU's view of this flash is CRC-decoded and
       * offset by the 34/32 ratio, so the bytes this driver writes are not
       * executable at any address and claiming an XIP base would be a lie.
       */

      default:
        break;
    }

  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_flash_initialize
 *
 * Description:
 *   See bk7258_flash.h.
 *
 ****************************************************************************/

FAR struct mtd_dev_s *bk7258_flash_initialize(void)
{
  FAR struct bk7258_flash_dev_s *priv = &g_bk7258_flash;

  /* The region is compile-time, but a bad pair of Kconfig values would only
   * show up as corruption somewhere else on the chip, so check it here.
   */

  if ((priv->base % FLASH_SECTOR) != 0 || (priv->size % FLASH_SECTOR) != 0 ||
      priv->size == 0 ||
      (uint64_t)priv->base + priv->size > BK7258_FLASH_CHIP_SIZE)
    {
      ferr("ERROR: region 0x%" PRIx32 "+0x%" PRIx32 " is not usable\n",
           priv->base, priv->size);
      return NULL;
    }

  finfo("on-chip flash MTD: 0x%" PRIx32 "..0x%" PRIx32 ", %" PRIu32 " KB\n",
        priv->base, priv->base + priv->size, priv->size / 1024);

  return &priv->mtd;
}
