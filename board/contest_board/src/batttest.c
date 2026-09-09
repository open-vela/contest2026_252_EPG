/****************************************************************************
 * boards/arm/bk7258/contest_board/src/batttest.c
 *
 * Instrument for the battery gauge (see src/bk7258_battery.c).
 *
 * apps/examples/battery cannot be used here: it asks for BATIOC_HEALTH on
 * the first pass, and the gauge upper half does not implement it -- that
 * ioctl belongs to the charger and monitor classes.  The example treats the
 * ENOTTY as fatal and exits, so nothing after it is ever reached.
 *
 * This reads only what a gauge actually answers, and reports each field
 * separately so that one unsupported call does not hide the others.
 *
 * Usage:
 *   batttest            one reading
 *   batttest <n>        n readings, one per second
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <stdbool.h>
#include <nuttx/power/battery_ioctl.h>

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static const char *state_name(int state)
{
  switch (state)
    {
      case BATTERY_UNKNOWN:     return "unknown";
      case BATTERY_FAULT:       return "fault";
      case BATTERY_IDLE:        return "idle";
      case BATTERY_FULL:        return "full";
      case BATTERY_CHARGING:    return "charging";
      case BATTERY_DISCHARGING: return "discharging";
      default:                  return "?";
    }
}

/****************************************************************************
 * Name: report
 *
 * Description:
 *   One pass.  Each ioctl is reported on its own line with its own errno,
 *   because "the gauge is broken" and "this one field is not implemented"
 *   look identical if the first failure ends the run.
 *
 ****************************************************************************/

static void report(int fd)
{
  int   ivalue;
  bool  bvalue;
  int   ret;

  ret = ioctl(fd, BATIOC_STATE, (unsigned long)((uintptr_t)&ivalue));
  if (ret < 0)
    {
      printf("  state    : failed, errno %d\n", errno);
    }
  else
    {
      printf("  state    : %s\n", state_name(ivalue));
    }

  ret = ioctl(fd, BATIOC_ONLINE, (unsigned long)((uintptr_t)&bvalue));
  if (ret < 0)
    {
      printf("  online   : failed, errno %d\n", errno);
    }
  else
    {
      printf("  online   : %s\n", bvalue ? "yes" : "no");
    }

  ret = ioctl(fd, BATIOC_VOLTAGE, (unsigned long)((uintptr_t)&ivalue));
  if (ret < 0)
    {
      printf("  voltage  : failed, errno %d\n", errno);
    }
  else
    {
      printf("  voltage  : %d mV\n", ivalue);
    }

  ret = ioctl(fd, BATIOC_CAPACITY, (unsigned long)((uintptr_t)&ivalue));
  if (ret < 0)
    {
      printf("  capacity : failed, errno %d\n", errno);
    }
  else
    {
      printf("  capacity : %d %%\n", ivalue);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: probe
 *
 * Description:
 *   Two questions the ordinary reading cannot answer.
 *
 *   1. Are the charger's status pins actually driven?  ETA4322-class parts
 *      use open-drain status outputs, and a pin nobody drives reads
 *      whatever the pad floats to.  Forcing a pull-down and then a pull-up
 *      settles it: a driven pin holds its level against the pull it
 *      opposes, a floating one follows whichever pull is applied.
 *
 *   2. Which analog channel carries the cell?  Channel 0 is documented as
 *      "supply", and on a board whose charger regulates the system rail
 *      that is not automatically the battery.
 *
 ****************************************************************************/

#define AON_GPIO_BASE   0x44000400ul
#define GPIO_CFG(n)     (*(volatile uint32_t *)(AON_GPIO_BASE + ((n) << 2)))
#define GPIO_INPUT_BIT  (1u << 0)
#define GPIO_INPUT_EN   (1u << 2)
#define GPIO_OUTPUT_DIS (1u << 3)
#define GPIO_PULL_UP    (1u << 4)
#define GPIO_PULL_EN    (1u << 5)

static int pin_read_with(int pin, bool pull_en, bool pull_up)
{
  uint32_t save = GPIO_CFG(pin);
  uint32_t cfg;
  int level;
  volatile int spin;

  cfg = save & ~(GPIO_PULL_UP | GPIO_PULL_EN);
  cfg |= GPIO_INPUT_EN | GPIO_OUTPUT_DIS;
  if (pull_en)
    {
      cfg |= GPIO_PULL_EN;
      if (pull_up)
        {
          cfg |= GPIO_PULL_UP;
        }
    }

  GPIO_CFG(pin) = cfg;

  /* Let the pad settle; a weak pull against a cable capacitance is not
   * instantaneous, and reading in the same cycle sees the old level.
   */

  for (spin = 0; spin < 20000; spin++)
    {
    }

  level = (GPIO_CFG(pin) & GPIO_INPUT_BIT) ? 1 : 0;
  GPIO_CFG(pin) = save;
  return level;
}

static void probe_pins(void)
{
  static const struct
  {
    int pin;
    const char *name;
  } pins[] =
  {
    { 51, "CHARGE" },
    { 26, "FULL"   },
  };

  int i;

  printf("status pins:\n");
  for (i = 0; i < 2; i++)
    {
      int down = pin_read_with(pins[i].pin, true, false);
      int up   = pin_read_with(pins[i].pin, true, true);
      int free = pin_read_with(pins[i].pin, false, false);

      printf("  GPIO%-3d %-7s pull-down=%d pull-up=%d no-pull=%d  -> %s\n",
             pins[i].pin, pins[i].name, down, up, free,
             (down == up) ? "DRIVEN" : "floating (reading is meaningless)");
    }
}

static void probe_channels(int fd)
{
  static const int chans[] = { 0, 1, 2, 3, 4, 9, 11 };
  int i;

  printf("analog channels (raw average):\n");
  for (i = 0; i < (int)(sizeof(chans) / sizeof(chans[0])); i++)
    {
      int param = chans[i];
      int ret = ioctl(fd, BATIOC_OPERATE, (unsigned long)((uintptr_t)&param));

      if (ret < 0)
        {
          printf("  chan %-2d : failed, errno %d\n", chans[i], errno);
        }
      else
        {
          printf("  chan %-2d : raw %-6d  (vendor formula -> %d mV)\n",
                 chans[i], param, (param * 667) / 1000 + 40);
        }
    }
}

int main(int argc, FAR char *argv[])
{
  int rounds = 1;
  int fd;
  int i;

  if (argc > 1)
    {
      rounds = atoi(argv[1]);
      if (rounds < 1)
        {
          rounds = 1;
        }
    }

  fd = open("/dev/batt0", O_RDONLY);
  if (fd < 0)
    {
      printf("batttest: cannot open /dev/batt0, errno %d\n", errno);
      return 1;
    }

  if (argc > 1 && strcmp(argv[1], "probe") == 0)
    {
      probe_pins();
      probe_channels(fd);
      close(fd);
      return 0;
    }

  for (i = 0; i < rounds; i++)
    {
      printf("reading %d:\n", i + 1);
      report(fd);

      if (i + 1 < rounds)
        {
          sleep(1);
        }
    }

  close(fd);
  return 0;
}
