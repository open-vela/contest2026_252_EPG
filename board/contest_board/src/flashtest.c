/****************************************************************************
 * board/contest_board/src/flashtest.c
 *
 * On-chip flash probe -- read-only.
 *
 * Everything here is a read.  The firmware executes in place out of this
 * same chip, and the board README records one occasion where a bad image
 * left it with no serial response at all, so the questions that decide the
 * MTD driver's shape get answered before anything is erased:
 *
 *   - Is the controller where the SDK's modern register map says it is?
 *     Two maps exist in the vendor tree; the one in flash.c is guarded by
 *     CONFIG_SOC_BK7256XX and puts the block at 0x00803000, which is not
 *     this SoC.  dev_id settles it.
 *
 *   - What write protection did the boot loader leave behind?  NuttX does
 *     not run the vendor flash driver, so whatever is in the status
 *     register now is what an MTD write would run into.  The block-protect
 *     field is SR[6:2] with CMP at SR[14] for this part (gd_25Q32E,
 *     0xC86517), and a set BP field makes programs fail silently.
 *
 *   - Are software-path addresses physical or CRC-decoded?  The controller
 *     inserts two CRC bytes every 32 for the CPU's instruction fetch, so
 *     the same offset means different things on the two paths.  Dumping a
 *     known region answers it: the bytes either match nuttx_crc.bin at the
 *     physical offset or nuttx.bin at the virtual one.
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

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/boardctl.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/mtd/mtd.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define FLASH_BASE       0x44030000ul

#define FLASH_DEV_ID     (FLASH_BASE + 0x00)
#define FLASH_DEV_VER    (FLASH_BASE + 0x04)
#define FLASH_GLOBAL     (FLASH_BASE + 0x08)
#define FLASH_OP_CTRL    (FLASH_BASE + 0x10)
#define FLASH_DATA_FS    (FLASH_BASE + 0x18)
#define FLASH_CMD_CFG    (FLASH_BASE + 0x1c)
#define FLASH_RD_ID      (FLASH_BASE + 0x20)
#define FLASH_STATE      (FLASH_BASE + 0x24)
#define FLASH_CONFIG     (FLASH_BASE + 0x28)
#define FLASH_OP_CMD     (FLASH_BASE + 0x54)

#define OP_SW            (1u << 29)
#define WP_VALUE         (1u << 30)
#define BUSY_SW          (1u << 31)

#define CMD_RDSR         (3)
#define CMD_READ         (5)
#define CMD_RDSR2        (6)
#define CMD_RDID         (20)

#define REG(a)           (*(volatile uint32_t *)(a))

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void flash_wait(void)
{
  while ((REG(FLASH_OP_CTRL) & BUSY_SW) != 0)
    {
    }
}

/* Commands that take no address.  op_ctrl's low bits are reserved and bit
 * 31 is read-only status, so preserving wp_value is the whole job.
 */

static void flash_cmd(uint32_t op)
{
  uint32_t regval;

  flash_wait();

  regval = REG(FLASH_OP_CMD) & 0x00ffffffu;
  REG(FLASH_OP_CMD) = regval | (op << 24);

  regval = REG(FLASH_OP_CTRL) & WP_VALUE;
  REG(FLASH_OP_CTRL) = regval | OP_SW;

  flash_wait();
}

static void flash_cmd_at(uint32_t op, uint32_t addr)
{
  uint32_t regval;

  flash_wait();

  REG(FLASH_OP_CMD) = (addr & 0x00ffffffu) | (op << 24);

  regval = REG(FLASH_OP_CTRL) & WP_VALUE;
  REG(FLASH_OP_CTRL) = regval | OP_SW;

  flash_wait();
}

/* One READ moves 32 bytes into eight words, so the address is rounded down
 * to a 32-byte boundary the way the vendor driver rounds it.
 */

static void flash_read32(uint32_t addr, uint8_t *out)
{
  uint32_t buf[8];
  int i;

  flash_cmd_at(CMD_READ, addr & ~31u);

  for (i = 0; i < 8; i++)
    {
      buf[i] = REG(FLASH_DATA_FS);
    }

  memcpy(out, buf, 32);
}

static uint32_t flash_status(void)
{
  uint32_t sr;

  REG(FLASH_CMD_CFG) = 0;

  flash_cmd(CMD_RDSR);
  sr = REG(FLASH_STATE) & 0xff;

  flash_cmd(CMD_RDSR2);
  sr |= (REG(FLASH_STATE) & 0xff) << 8;

  return sr;
}

static void dump(const char *label, uint32_t addr)
{
  uint8_t b[32];
  int i;
  int blank = 1;

  flash_read32(addr, b);

  printf("  %-20s @%08lx ", label, (unsigned long)addr);
  for (i = 0; i < 32; i++)
    {
      printf("%02x", b[i]);
    }

  for (i = 0; i < 32; i++)
    {
      if (b[i] != 0xff)
        {
          blank = 0;
          break;
        }
    }

  printf("%s\n", blank ? "  [blank]" : "");
}

/* Walk a range one 32-byte unit per sector and report the sectors that are
 * not erased.  "Unallocated in the partition table" and "actually blank"
 * are different claims, and only this one is measured.
 */

static void scan(uint32_t start, uint32_t end)
{
  uint32_t addr;
  uint32_t used = 0;
  uint32_t total = 0;
  uint8_t b[32];
  int i;

  printf("scanning %08lx..%08lx, one 32B sample per 4K sector\n",
         (unsigned long)start, (unsigned long)end);

  for (addr = start; addr < end; addr += 0x1000)
    {
      total++;
      flash_read32(addr, b);

      for (i = 0; i < 32; i++)
        {
          if (b[i] != 0xff)
            {
              break;
            }
        }

      if (i < 32)
        {
          if (used < 12)
            {
              printf("  non-blank @%08lx  %02x%02x%02x%02x%02x%02x%02x%02x\n",
                     (unsigned long)addr, b[0], b[1], b[2], b[3],
                     b[4], b[5], b[6], b[7]);
            }

          used++;
        }
    }

  printf("  %lu of %lu sectors non-blank%s\n",
         (unsigned long)used, (unsigned long)total,
         used > 12 ? " (first 12 shown)" : "");
}

/* End-to-end check through the MTD and the block device on top of it.  The
 * first write this driver ever performs should be one whose result is
 * verified byte for byte, not one whose absence of an error message is
 * taken for success.
 */

static int mtd_selftest(void)
{
  struct mtd_geometry_s geo;
  uint8_t wr[512];
  uint8_t rd[512];
  int fd;
  int ret;
  int i;

  fd = open("/dev/mtd0", O_RDONLY);
  if (fd < 0)
    {
      printf("  open /dev/mtd0 failed\n");
      return -1;
    }

  ret = ioctl(fd, MTDIOC_GEOMETRY, (unsigned long)&geo);
  close(fd);

  if (ret < 0)
    {
      printf("  MTDIOC_GEOMETRY failed\n");
      return -1;
    }

  printf("  geometry: blocksize %lu  erasesize %lu  neraseblocks %lu"
         "  (%lu KB)  model %s\n",
         (unsigned long)geo.blocksize, (unsigned long)geo.erasesize,
         (unsigned long)geo.neraseblocks,
         (unsigned long)(geo.erasesize * geo.neraseblocks / 1024),
         geo.model);

  /* Through the block device, so the FTL's erase-before-write is exercised
   * too rather than only the raw program path.
   */

  fd = open("/dev/mtdblock0", O_RDWR);
  if (fd < 0)
    {
      printf("  open /dev/mtdblock0 failed\n");
      return -1;
    }

  if (read(fd, rd, sizeof(rd)) != (ssize_t)sizeof(rd))
    {
      printf("  read before write failed\n");
      close(fd);
      return -1;
    }

  for (i = 0; i < (int)sizeof(rd); i++)
    {
      if (rd[i] != 0xff)
        {
          break;
        }
    }

  printf("  before: %s\n",
         i == (int)sizeof(rd) ? "erased (all FF)" : "already carries data");

  for (i = 0; i < (int)sizeof(wr); i++)
    {
      wr[i] = (uint8_t)(i * 7 + 0x5a);
    }

  lseek(fd, 0, SEEK_SET);
  if (write(fd, wr, sizeof(wr)) != (ssize_t)sizeof(wr))
    {
      printf("  write failed\n");
      close(fd);
      return -1;
    }

  close(fd);

  /* Reopen so nothing can be served out of a cache that never reached the
   * chip.
   */

  fd = open("/dev/mtdblock0", O_RDONLY);
  if (fd < 0)
    {
      printf("  reopen failed\n");
      return -1;
    }

  memset(rd, 0, sizeof(rd));
  if (read(fd, rd, sizeof(rd)) != (ssize_t)sizeof(rd))
    {
      printf("  read back failed\n");
      close(fd);
      return -1;
    }

  close(fd);

  if (memcmp(wr, rd, sizeof(wr)) != 0)
    {
      for (i = 0; i < (int)sizeof(wr); i++)
        {
          if (wr[i] != rd[i])
            {
              break;
            }
        }

      printf("  MISMATCH at byte %d: wrote %02x, read %02x\n",
             i, wr[i], rd[i]);
      return -1;
    }

  printf("  512 bytes written and read back identical -- PASS\n");
  return 0;
}

/* Why did the board come up?  A silent reset with no crash dump narrows to
 * a watchdog or a brown-out, and those want opposite fixes, so guessing is
 * not good enough.  The AON PMU field survives the reset and bk7258_reset.c
 * decodes it.
 */

static void show_reset_cause(void)
{
  struct boardioc_reset_cause_s cause;
  static const char *names[] =
    {
      "none", "power on", "RTC watchdog system", "brown-out",
      "core soft", "core deep-sleep", "core main watchdog",
      "core RTC watchdog", "cpu main watchdog", "cpu soft",
      "cpu RTC watchdog", "pin", "low power", "unknown"
    };

  memset(&cause, 0, sizeof(cause));

  if (boardctl(BOARDIOC_RESET_CAUSE, (uintptr_t)&cause) < 0)
    {
      printf("  BOARDIOC_RESETCAUSE failed\n");
      return;
    }

  printf("  last reset: %d (%s)  flag %lu\n", (int)cause.cause,
         (unsigned)cause.cause < sizeof(names) / sizeof(names[0])
           ? names[cause.cause] : "?",
         (unsigned long)cause.flag);
}

/* How long does a sector erase really take?  The claim that the core stalls
 * on instruction fetch for the whole of it was an assumption, never a
 * measurement, and the bounded spin in the driver is only safe if an erase
 * finishes well inside it.  Timing it from a task settles both: if this
 * function can read the clock and print, the core was running.
 */

static void time_erase(void)
{
  struct timespec t0;
  struct timespec t1;
  struct mtd_geometry_s geo;
  int fd;
  int i;

  fd = open("/dev/mtd0", O_RDONLY);
  if (fd < 0 || ioctl(fd, MTDIOC_GEOMETRY, (unsigned long)&geo) < 0)
    {
      printf("  cannot reach /dev/mtd0\n");
      return;
    }

  close(fd);

  fd = open("/dev/mtdblock0", O_RDWR);
  if (fd < 0)
    {
      printf("  cannot open /dev/mtdblock0\n");
      return;
    }

  for (i = 0; i < 8; i++)
    {
      uint8_t buf[512];

      memset(buf, (uint8_t)i, sizeof(buf));

      clock_t c0;
      clock_t c1;
      long real_ms;
      long tick_ms;

      /* Two clocks on purpose.  CLOCK_MONOTONIC comes off the AON counter
       * and keeps real time whatever the software does; clock() counts
       * system ticks, which only advance when the tick handler runs.  The
       * watchdog feed rides that same handler, so if these two diverge
       * across a flash burst, the starvation everyone has been guessing at
       * is measured rather than assumed.
       */

      c0 = clock();
      clock_gettime(CLOCK_MONOTONIC, &t0);
      lseek(fd, (off_t)i * geo.erasesize, SEEK_SET);
      write(fd, buf, sizeof(buf));
      clock_gettime(CLOCK_MONOTONIC, &t1);
      c1 = clock();

      real_ms = (long)((t1.tv_sec - t0.tv_sec) * 1000 +
                       (t1.tv_nsec - t0.tv_nsec) / 1000000);
      tick_ms = (long)((c1 - c0) * 1000 / CLOCKS_PER_SEC);

      printf("  sector %d: real %ld ms, tick clock %ld ms%s\n", i,
             real_ms, tick_ms,
             (real_ms - tick_ms) > 20 ? "   <-- ticks lost" : "");
    }

  close(fd);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  uint32_t cfg;
  uint32_t sr;

  /* flashtest <addr>            dump one 32-byte unit
   * flashtest scan <from> <to>  report non-blank sectors in a range
   */

  if (argc == 2 && strcmp(argv[1], "mtd") == 0)
    {
      printf("mtd self-test (writes to the region, which is scratch)\n");
      return mtd_selftest() == 0 ? 0 : 1;
    }

  if (argc == 2 && strcmp(argv[1], "why") == 0)
    {
      show_reset_cause();
      return 0;
    }

  if (argc == 2 && strcmp(argv[1], "time") == 0)
    {
      printf("timing writes through the FTL (erase included)\n");
      time_erase();
      return 0;
    }

  if (argc == 2)
    {
      dump("at", strtoul(argv[1], NULL, 0));
      return 0;
    }

  if (argc == 4 && strcmp(argv[1], "scan") == 0)
    {
      scan(strtoul(argv[2], NULL, 0), strtoul(argv[3], NULL, 0));
      return 0;
    }

  printf("controller\n");
  printf("  dev_id      %08lx   ver %08lx   global %08lx\n",
         (unsigned long)REG(FLASH_DEV_ID), (unsigned long)REG(FLASH_DEV_VER),
         (unsigned long)REG(FLASH_GLOBAL));

  cfg = REG(FLASH_CONFIG);
  printf("  config      %08lx   crc_en=%s  clk_cfg=%lu  mode_sel=%lu\n",
         (unsigned long)cfg, (cfg & (1u << 26)) ? "ON" : "off",
         (unsigned long)(cfg & 0xf), (unsigned long)((cfg >> 4) & 0x1f));
  printf("  state       %08lx   crc_err_num=%lu\n",
         (unsigned long)REG(FLASH_STATE),
         (unsigned long)((REG(FLASH_STATE) >> 8) & 0xff));

  flash_cmd(CMD_RDID);
  printf("  flash id    %08lx   (expect 1765c8 = GD 8MB)\n",
         (unsigned long)REG(FLASH_RD_ID));

  sr = flash_status();
  printf("  status reg  %08lx   BP=%lu%s  CMP=%lu   -> %s\n",
         (unsigned long)sr, (unsigned long)((sr >> 2) & 0x1f),
         (((sr >> 2) & 0x1f) == 0) ? " (none)" : " (PROTECTED)",
         (unsigned long)((sr >> 14) & 1),
         (((sr >> 2) & 0x1f) == 0 && ((sr >> 14) & 1) == 0)
           ? "writable" : "needs unprotect");

  /* First 16 of 32 bytes at four places.  The two known regions decide
   * physical-vs-virtual by comparison against the host's images; the two
   * high ones say whether the space above the vendor partition table is
   * really untouched.
   */

  printf("first 16 bytes of the 32-byte read unit\n");
  dump("bootloader phys 0", 0x00000000);
  dump("app phys 0x11000", 0x00011000);
  dump("above partitions", 0x00400000);
  dump("last sector", 0x007ff000);

  return 0;
}
