/****************************************************************************
 * board/contest_board/src/bk7258_appinit.c
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

#include <sys/mount.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/board.h>

#include "arm_internal.h"
#include "bk7258_memorymap.h"

#ifdef CONFIG_USERLED_LOWER
#  include <nuttx/leds/userled.h>
#endif
#ifdef CONFIG_INPUT_BUTTONS_LOWER
#  include <nuttx/input/buttons.h>
#endif
#ifdef CONFIG_BK7258_I2C1
#  include <nuttx/i2c/i2c_master.h>
#endif
#ifdef CONFIG_VIDEO_FB
#  include <nuttx/video/fb.h>
#endif
#ifdef CONFIG_MMCSD
#  include <nuttx/sdio.h>
#  include <nuttx/mmcsd.h>
#endif
#ifdef CONFIG_BK7258_RTC
#  include <nuttx/timers/rtc.h>
#  include "bk7258_rtc.h"
#endif
#ifdef CONFIG_BK7258_WDT
#  include "bk7258_wdt.h"
#endif
#ifdef CONFIG_BK7258_TIMER
#  include "bk7258_timer.h"
#endif
#ifdef CONFIG_BK7258_BATTERY
#  include "bk7258_battery.h"
#endif
#ifdef CONFIG_BK7258_FLASH
#  include <nuttx/mtd/mtd.h>
#  include "bk7258_flash.h"
#endif
#ifdef CONFIG_BK7258_WIFI
#  include "bk7258_wifi.h"
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: board_app_initialize
 *
 * Description:
 *   Perform application-level initialisation.  Called by boardctl() with
 *   BOARDIOC_INIT, which NSH issues at start of day.
 *
 ****************************************************************************/

int board_app_initialize(uintptr_t arg)
{
  int ret;

  UNUSED(ret);

#ifdef CONFIG_FS_PROCFS
  ret = nx_mount(NULL, "/proc", "procfs", 0, NULL);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to mount procfs at /proc: %d\n", ret);
    }
#endif

#ifdef CONFIG_FS_TMPFS
  /* A writable scratch filesystem.  Nothing in the demo apps needs one, but
   * anything that writes a temporary file does: both the xTS scanf case and
   * the syscall suite fail at the first open() without it, and the SD card
   * is not a given (it holds the vendor's artwork and may be absent).
   * RAM-backed keeps it out of the way of both.
   */

  ret = nx_mount(NULL, "/tmp", "tmpfs", 0, NULL);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to mount tmpfs at /tmp: %d\n", ret);
    }

#ifdef CONFIG_BLUETOOTH_SERVICE
  /* The Bluetooth service keeps its adapter properties and bond database
   * under a hard-coded /data/misc/bt (service/common/storage.c), and it
   * only mkdir()s the last two components -- without /data the create
   * fails with ENOENT and the daemon carries the failed handle into a
   * bus fault.  RAM-backed is the honest choice here: bonds do not have
   * to survive a power cycle for anything this board does yet, and the
   * SD card is not a given.
   */

  ret = nx_mount(NULL, "/data", "tmpfs", 0, NULL);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to mount tmpfs at /data: %d\n", ret);
    }
#endif
#endif

#ifdef CONFIG_USERLED_LOWER
  ret = userled_lower_initialize("/dev/userleds");
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: userled_lower_initialize: %d\n", ret);
    }
#endif

#ifdef CONFIG_INPUT_BUTTONS_LOWER
  ret = btn_lower_initialize("/dev/buttons");
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: btn_lower_initialize: %d\n", ret);
    }
#endif

#ifdef CONFIG_BK7258_RTC
  /* The counter itself came up in board_late_initialize(); this only
   * publishes the same lower half as a character device.  If that never
   * succeeded there is nothing to publish and saying so is more useful
   * than an empty /dev/rtc0.
   */

    {
      FAR struct rtc_lowerhalf_s *rtclower = bk7258_rtc_lowerhalf();

      if (rtclower == NULL)
        {
          syslog(LOG_WARNING, "rtc: lower half unavailable, no /dev/rtc0\n");
        }
      else
        {
          ret = rtc_initialize(0, rtclower);
          if (ret < 0)
            {
              syslog(LOG_ERR, "ERROR: rtc_initialize: %d\n", ret);
            }
        }
    }
#endif

#ifdef CONFIG_BK7258_WDT
  ret = bk7258_wdt_lowerhalf_initialize("/dev/watchdog0");
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: bk7258_wdt_lowerhalf_initialize: %d\n", ret);
    }
#endif

#ifdef CONFIG_BK7258_WIFI
  ret = bk7258_wifi_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: bk7258_wifi_initialize: %d\n", ret);
    }
#endif

#ifdef CONFIG_BK7258_TIMER
  ret = bk7258_timer_oneshot_register("/dev/oneshot0");
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: bk7258_timer_oneshot_register: %d\n", ret);
    }
#endif

#ifdef CONFIG_BK7258_BATTERY
  ret = bk7258_battery_register("/dev/batt0");
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: bk7258_battery_register: %d\n", ret);
    }
#endif

#ifdef CONFIG_BK7258_FLASH
    {
      FAR struct mtd_dev_s *mtd = bk7258_flash_initialize();

      if (mtd == NULL)
        {
          syslog(LOG_ERR, "ERROR: bk7258_flash_initialize failed\n");
        }
      else
        {
          /* The raw MTD first, then a block device on top of it.  Both are
           * useful: the xTS driver cases talk to /dev/mtd0 directly, while
           * anything wanting a filesystem needs /dev/mtdblock0.
           */

          ret = register_mtddriver("/dev/mtd0", mtd, 0666, NULL);
          if (ret < 0)
            {
              syslog(LOG_ERR, "ERROR: register_mtddriver: %d\n", ret);
            }

          ret = ftl_initialize(0, mtd);
          if (ret < 0)
            {
              syslog(LOG_ERR, "ERROR: ftl_initialize: %d\n", ret);
            }
        }
    }
#endif

#if defined(CONFIG_BK7258_I2C1) && defined(CONFIG_I2C_DRIVER)
    {
      extern struct i2c_master_s *bk7258_i2cbus_initialize(int port);
      extern void bk7258_gpio_setaf(int pin, uint8_t af, bool input);
      extern void bk7258_gpio_config(int pin, bool output, bool pullup,
                                     bool pulldown);
      extern void bk7258_gpio_write(int pin, bool value);
      struct i2c_master_s *i2c;
      int port;

      /* Camera stage 1 (sensor = GalaxyCore GC2145, proven by chip ID
       * 0x2145 at address 0x3c): power rail on GPIO49, 24 MHz MCLK out
       * of GPIO27 via CLK_AUXS_CIS, SCCB on the bit-banged bus at
       * GPIO42/43.  No reset/PWDN line exists in the vendor flow.  The
       * pad is handed over peripheral-style (function 1, output stage
       * off); note the pad input latch cannot observe peripheral-driven
       * clocks -- the sensor ACKing is the only valid MCLK detector.
       */

      bk7258_gpio_config(49, true, false, false);
      bk7258_gpio_write(49, true);
      up_mdelay(5);

      modifyreg32(BK7258_SYS_BASE + (0x10 << 2), 1u << 7, 0);
      modifyreg32(BK7258_SYS_BASE + (0x0c << 2), 0,
                  (1u << 28) | (1u << 23));
      modifyreg32(BK7258_SYS_BASE + (0x0d << 2), 0,
                  (1u << 0) | (1u << 3) | (1u << 8) | (1u << 9));
      modifyreg32(BK7258_SYS_BASE + (0x08 << 2),
                  (0xfu << 26) | (1u << 30),
                  (1u << 26) | (1u << 30));
      modifyreg32(BK7258_SYS_BASE + (0x0a << 2),
                  (3u << 15) | (0x1fu << 17),
                  (3u << 15) | (19u << 17));

      putreg32(0, 0x48020008);
      putreg32(3, 0x48020008);
      modifyreg32(0x48020010, 3u << 10, (1u << 0) | (1u << 10));

      bk7258_gpio_setaf(27, 1, false);

      for (port = 0; port <= 1; port++)
        {
          i2c = bk7258_i2cbus_initialize(port);

          if (i2c == NULL)
            {
              syslog(LOG_ERR, "ERROR: i2c port %d init failed\n", port);
            }
          else
            {
              ret = i2c_register(i2c, port);
              if (ret < 0)
                {
                  syslog(LOG_ERR, "ERROR: i2c_register(%d): %d\n",
                         port, ret);
                }

              /* Sensor-as-MCLK-detector: sweep the SCCB bus once per
               * GPIO28 polarity.  Any ACK simultaneously proves MCLK,
               * power, pin 28's meaning, and the sensor address.
               */

              if (port == 1)
                {
                  struct i2c_msg_s msg;
                  uint8_t zero = 0;
                  int pol;
                  int addr;

                  for (pol = 0; pol < 2; pol++)
                    {
                      bk7258_gpio_write(28, pol != 0);
                      up_mdelay(20);

                      for (addr = 0x08; addr <= 0x77; addr++)
                        {
                          msg.frequency = 100000;
                          msg.addr      = addr;
                          msg.flags     = 0;
                          msg.buffer    = &zero;
                          msg.length    = 1;

                          if (I2C_TRANSFER(i2c, &msg, 1) >= 0)
                            {
                              syslog(LOG_INFO,
                                     "camera: ACK addr=0x%02x gpio28=%d\n",
                                     addr, pol);
                            }
                        }
                    }

                  syslog(LOG_INFO, "camera: sccb sweep done\n");
                }
            }
        }
    }
#endif

#if defined(CONFIG_I2C_BITBANG) && defined(CONFIG_I2C_DRIVER)
    {
      extern struct i2c_master_s *bk7258_i2c_bitbang_initialize(void);
      struct i2c_master_s *i2c = bk7258_i2c_bitbang_initialize();

      if (i2c != NULL)
        {
          ret = i2c_register(i2c, 2);
          if (ret < 0)
            {
              syslog(LOG_ERR, "ERROR: i2c_register(bitbang): %d\n", ret);
            }
        }
    }
#endif

#ifdef CONFIG_VIDEO_FB
  ret = fb_register(0, 0);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: fb_register(0): %d\n", ret);
    }

  ret = fb_register(1, 0);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: fb_register(1): %d\n", ret);
    }
#endif

#ifdef CONFIG_DEV_GPIO
  extern int bk7258_gpiodev_initialize(void);

  ret = bk7258_gpiodev_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: bk7258_gpiodev_initialize: %d\n", ret);
    }
#endif

#ifdef CONFIG_MMCSD
    {
      extern struct sdio_dev_s *bk7258_sdio_initialize(void);
      struct sdio_dev_s *sdio = bk7258_sdio_initialize();

      ret = mmcsd_slotinitialize(0, sdio);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: mmcsd_slotinitialize: %d\n", ret);
        }
    }
#endif

#ifdef CONFIG_BK7258_HCI_TRANSPORT
    {
      /* The H4 transport the openvela Bluetooth service expects to
       * find.  Registering the node costs nothing and touches no
       * radio: the controller has to be brought up separately, and
       * traffic only starts when something opens the device.
       */

      extern int bk7258_hci_register(FAR const char *path);

      ret = bk7258_hci_register(NULL);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: bk7258_hci_register: %d\n", ret);
        }
    }
#endif

  return OK;
}
