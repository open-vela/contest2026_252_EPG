/****************************************************************************
 * boards/arm/bk7258/contest_board/src/bk7258_battery.c
 *
 * Battery gauge for the Agora ConvoAI Kit R1.
 *
 * The board charges through an ETA4322 linear charger, which runs on its
 * own: plugging USB in charges the cell whether or not this code exists,
 * and the green LED next to the connector is driven by the charger, not by
 * the MCU.  So there is nothing here that starts or stops charging -- the
 * part exposes no control interface to us.  What it does expose is two
 * status pins, and the cell voltage reaches the converter on channel 0.
 *
 * ---------------------------------------------------------------------------
 * WHAT IS AND IS NOT ESTABLISHED ON THIS BOARD (2026-08-29)
 *
 * Everything below was taken from the vendor's battery monitor and then
 * checked on the board.  The check did not go well, so read this before
 * trusting any number this driver reports.
 *
 *   - The two status pins are NOT driven here.  Forcing a pull-down reads
 *     0 and a pull-up reads 1 on both GPIO51 and GPIO26, which is the
 *     signature of a pin nobody drives.  The vendor's GPIO numbers are
 *     #defines at the top of its own file and evidently describe its
 *     reference board, not this one.  The driver repeats that measurement
 *     at registration and reports BATTERY_UNKNOWN for as long as it fails:
 *     a floating pin that happens to sit high once read as "charging",
 *     which is worse than admitting we cannot tell.
 *
 *   - The channel is unconfirmed.  Channel 0 is the vendor's choice and
 *     this port's SARADC header calls it "supply"; on a board whose
 *     charger regulates the system rail those are not the same node.  A
 *     sweep found no channel reading anything like a charged cell
 *     (chan 0 3213 mV, 1 3555, 3 3705, 4 3332, 9 3656 by the vendor's
 *     formula), so either the cell is deeply discharged or none of these
 *     is the cell.  `batttest probe` re-runs both measurements.
 *
 *   - The conversion and the curve are therefore uncalibrated.  Closing
 *     this needs one multimeter reading of the cell taken at the same
 *     moment as a `batttest` run; until then voltage and capacity are
 *     indicative only.
 * ---------------------------------------------------------------------------
 *
 * The vendor's constants, kept because they are the only documented
 * starting point (bk_avdk_smp/ap/components/bk_batt_monitor/bat_monitor.c):
 *
 *   - cell voltage on SARADC channel 0, continuous mode, ADC clock
 *     203125 Hz, saturation mode 4, settle code 7, calibration correction
 *     bypassed (bk_adc_enable_bypass_clalibration);
 *   - ten raw samples per reading, the first five discarded, and any
 *     sample reading exactly 0 or 2048 dropped as invalid;
 *   - millivolts = raw * 667 / 1000 + 40;
 *   - GPIO51 charge status, GPIO26 full status.
 *
 * The vendor's file carries a second, calibration-based conversion behind
 * an #else that its own build never takes; this port follows the branch
 * that is actually compiled.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/kmalloc.h>
#include <nuttx/mutex.h>
#include <nuttx/power/battery_gauge.h>
#include <nuttx/power/battery_ioctl.h>

#include "bk7258_gpio.h"
#include "bk7258_saradc.h"

#ifdef CONFIG_BK7258_BATTERY

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Status pins, from the vendor's file.  Measured floating on this board --
 * see the header.  The truth table is the vendor's: CHARGE high with FULL
 * high means charging, CHARGE high with FULL low means the charger has
 * terminated, CHARGE low means nothing is plugged in.  It is only applied
 * when the pins prove to be driven.
 */

#define BATTERY_GPIO_CHARGE     51
#define BATTERY_GPIO_FULL       26

/* Converter setup, all four values from the vendor's adc_config_t. */

#define BATTERY_ADC_CHANNEL     0
#define BATTERY_ADC_CLK         203125
#define BATTERY_ADC_SATURATE    4
#define BATTERY_ADC_STEADY      7

/* Ten samples, the first five thrown away.  The vendor discards the first
 * five because the front end has not settled; keeping them drags the
 * average down by tens of millivolts.
 */

#define BATTERY_ADC_SAMPLES     10
#define BATTERY_ADC_SKIP        5

/* A sample of exactly 0 or exactly 2048 is the converter reporting nothing
 * rather than a reading of zero volts or mid-scale.
 */

#define BATTERY_ADC_INVALID_LO  0
#define BATTERY_ADC_INVALID_HI  2048

#define BATTERY_ADC_TIMEOUT_MS  1000

/* millivolts = raw * 667 / 1000 + 40 */

#define BATTERY_MV_NUM          667
#define BATTERY_MV_DEN          1000
#define BATTERY_MV_OFFSET       40

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Discharge curve.  These are the vendor's points, and the vendor's own
 * comment says they are an example rather than a measured curve for this
 * cell -- so treat the percentage as indicative until someone runs the
 * board down against a bench supply and replaces the table.  What is not
 * indicative is the top of the range: full is decided by the charger's
 * FULL pin, not by voltage, which is why the table stops at 99%.
 */

struct battery_lut_s
{
  uint16_t mv;
  uint8_t  percent;
};

static const struct battery_lut_s g_battery_lut[] =
{
  { 3000,   0 },
  { 3400,  10 },
  { 3450,  20 },
  { 3500,  30 },
  { 3550,  40 },
  { 3590,  50 },
  { 3650,  60 },
  { 3750,  70 },
  { 3880,  80 },
  { 3980,  90 },
  { 4100,  99 },
};

#define BATTERY_LUT_LEN (sizeof(g_battery_lut) / sizeof(g_battery_lut[0]))

struct bk7258_battery_dev_s
{
  /* The upper half's device.  Must be first: battery_gauge_register()
   * stores this pointer as the driver's private data and the upper half
   * casts it back, so anything of ours has to sit behind it.  Embedding
   * the whole struct rather than restating its fields means a change up
   * there cannot silently misalign what is down here.
   */

  struct battery_gauge_dev_s dev;

  /* Ours. */

  mutex_t adclock;

  /* False when the status pins measured floating at registration, which
   * makes every state derived from them meaningless.
   */

  bool status_valid;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int bk7258_battery_state(FAR struct battery_gauge_dev_s *dev,
                                FAR int *status);
static int bk7258_battery_online(FAR struct battery_gauge_dev_s *dev,
                                 FAR bool *status);
static int bk7258_battery_voltage(FAR struct battery_gauge_dev_s *dev,
                                  FAR int *value);
static int bk7258_battery_capacity(FAR struct battery_gauge_dev_s *dev,
                                   FAR int *value);
static int bk7258_battery_operate(FAR struct battery_gauge_dev_s *dev,
                                  FAR int *param);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct battery_gauge_operations_s g_bk7258_battery_ops =
{
  .state    = bk7258_battery_state,
  .online   = bk7258_battery_online,
  .voltage  = bk7258_battery_voltage,
  .capacity = bk7258_battery_capacity,
  .operate  = bk7258_battery_operate,

  /* No shunt on this board, no cell thermistor, and the charger has no ID
   * register: current, temp and chipid stay NULL so the upper half returns
   * -ENOSYS rather than a made-up number.
   */
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_battery_sample
 *
 * Description:
 *   One conversion run on channel 0, averaged and converted to millivolts.
 *
 *   The converter is shared with the closed RF library, which samples the
 *   transmit power detector and the die temperature during calibration.
 *   bk7258_saradc_start() answers -EBUSY when a conversion is already in
 *   flight, and that is passed straight up: a battery reading is never
 *   worth stalling a calibration for, and the caller can simply ask again.
 *
 ****************************************************************************/

static int bk7258_battery_sample_chan(FAR struct bk7258_battery_dev_s *priv,
                                      uint8_t channel, FAR int *raw_avg,
                                      FAR int *millivolts)
{
  uint16_t raw[BATTERY_ADC_SAMPLES];
  uint32_t sum = 0;
  uint32_t count = 0;
  uint32_t div;
  int ret;
  int i;

  ret = nxmutex_lock(&priv->adclock);
  if (ret < 0)
    {
      return ret;
    }

  ret = bk7258_saradc_pwrup();
  if (ret < 0)
    {
      goto unlock;
    }

  div = bk7258_saradc_div(BATTERY_ADC_CLK);

  ret = bk7258_saradc_start(channel, SARADC_MODE_CONTINUOUS,
                            div, BATTERY_ADC_SATURATE, BATTERY_ADC_STEADY,
                            0, 0);
  if (ret < 0)
    {
      goto unlock;
    }

  memset(raw, 0, sizeof(raw));
  ret = bk7258_saradc_read(raw, BATTERY_ADC_SAMPLES,
                           BATTERY_ADC_TIMEOUT_MS);
  bk7258_saradc_stop();

  if (ret < 0)
    {
      goto unlock;
    }

  for (i = BATTERY_ADC_SKIP; i < BATTERY_ADC_SAMPLES; i++)
    {
      if (raw[i] != BATTERY_ADC_INVALID_LO &&
          raw[i] != BATTERY_ADC_INVALID_HI)
        {
          sum += raw[i];
          count++;
        }
    }

  if (count == 0)
    {
      /* Every surviving sample was one of the two "no reading" values.
       * Reporting zero millivolts here would look like a flat cell, so
       * fail instead.
       */

      ret = -EIO;
      goto unlock;
    }

  if (raw_avg != NULL)
    {
      *raw_avg = (int)(sum / count);
    }

  *millivolts = (int)(((sum / count) * BATTERY_MV_NUM) / BATTERY_MV_DEN) +
                BATTERY_MV_OFFSET;
  ret = OK;

unlock:
  nxmutex_unlock(&priv->adclock);
  return ret;
}

static int bk7258_battery_sample(FAR struct bk7258_battery_dev_s *priv,
                                 FAR int *millivolts)
{
  return bk7258_battery_sample_chan(priv, BATTERY_ADC_CHANNEL, NULL,
                                    millivolts);
}

/****************************************************************************
 * Name: bk7258_battery_operate
 *
 * Description:
 *   Diagnostic hook, reached by BATIOC_OPERATE.  The caller passes an
 *   analog channel number in *param and gets that channel's averaged raw
 *   count back in the same place.
 *
 *   This exists because channel 0 is documented as "supply", and on a
 *   board where the charger regulates the system rail that is not
 *   necessarily the cell.  Sweeping the channels answers which one moves
 *   with the battery, rather than assuming the vendor's board and this one
 *   are wired alike.
 *
 ****************************************************************************/

static int bk7258_battery_operate(FAR struct battery_gauge_dev_s *dev,
                                  FAR int *param)
{
  FAR struct bk7258_battery_dev_s *priv =
    (FAR struct bk7258_battery_dev_s *)dev;
  int raw_avg = 0;
  int millivolts = 0;
  int ret;

  if (*param < 0 || *param > 15)
    {
      return -EINVAL;
    }

  ret = bk7258_battery_sample_chan(priv, (uint8_t)*param, &raw_avg,
                                   &millivolts);
  if (ret < 0)
    {
      return ret;
    }

  *param = raw_avg;
  return OK;
}

/****************************************************************************
 * Name: bk7258_battery_percent
 *
 * Description:
 *   Linear interpolation between the curve's points, clamped at both ends.
 *
 ****************************************************************************/

static int bk7258_battery_percent(int millivolts)
{
  int i;

  if (millivolts <= (int)g_battery_lut[0].mv)
    {
      return g_battery_lut[0].percent;
    }

  if (millivolts >= (int)g_battery_lut[BATTERY_LUT_LEN - 1].mv)
    {
      return g_battery_lut[BATTERY_LUT_LEN - 1].percent;
    }

  for (i = 1; i < (int)BATTERY_LUT_LEN; i++)
    {
      if (millivolts < (int)g_battery_lut[i].mv)
        {
          int lo_mv = g_battery_lut[i - 1].mv;
          int hi_mv = g_battery_lut[i].mv;
          int lo_pc = g_battery_lut[i - 1].percent;
          int hi_pc = g_battery_lut[i].percent;

          return lo_pc + ((millivolts - lo_mv) * (hi_pc - lo_pc)) /
                         (hi_mv - lo_mv);
        }
    }

  return g_battery_lut[BATTERY_LUT_LEN - 1].percent;
}

/****************************************************************************
 * Name: bk7258_battery_state
 ****************************************************************************/

static int bk7258_battery_state(FAR struct battery_gauge_dev_s *dev,
                                FAR int *status)
{
  FAR struct bk7258_battery_dev_s *priv =
    (FAR struct bk7258_battery_dev_s *)dev;
  bool charging;
  bool full;

  if (!priv->status_valid)
    {
      /* The pins are not driven, so any answer derived from them would be
       * an invention.  BATTERY_UNKNOWN is the honest one.
       */

      *status = BATTERY_UNKNOWN;
      return OK;
    }

  charging = bk7258_gpio_read(BATTERY_GPIO_CHARGE);
  full     = bk7258_gpio_read(BATTERY_GPIO_FULL);

  if (!charging)
    {
      *status = BATTERY_DISCHARGING;
    }
  else if (full)
    {
      *status = BATTERY_CHARGING;
    }
  else
    {
      *status = BATTERY_FULL;
    }

  return OK;
}

/****************************************************************************
 * Name: bk7258_battery_online
 *
 * Description:
 *   The vendor's presence check is a compile-time constant -- this board
 *   has no cell-detect line -- so report present and leave it at that
 *   rather than inventing a test.
 *
 ****************************************************************************/

static int bk7258_battery_online(FAR struct battery_gauge_dev_s *dev,
                                 FAR bool *status)
{
  *status = true;
  return OK;
}

/****************************************************************************
 * Name: bk7258_battery_voltage
 ****************************************************************************/

static int bk7258_battery_voltage(FAR struct battery_gauge_dev_s *dev,
                                  FAR int *value)
{
  FAR struct bk7258_battery_dev_s *priv =
    (FAR struct bk7258_battery_dev_s *)dev;

  return bk7258_battery_sample(priv, value);
}

/****************************************************************************
 * Name: bk7258_battery_capacity
 *
 * Description:
 *   Percentage from the voltage curve, except when the charger says the
 *   cell is full: the curve tops out at 99% on purpose, and the charger's
 *   own termination is a better answer than any voltage reading taken
 *   while it is still driving the cell.
 *
 ****************************************************************************/

static int bk7258_battery_capacity(FAR struct battery_gauge_dev_s *dev,
                                   FAR int *value)
{
  FAR struct bk7258_battery_dev_s *priv =
    (FAR struct bk7258_battery_dev_s *)dev;
  int millivolts;
  int ret;

  if (priv->status_valid &&
      bk7258_gpio_read(BATTERY_GPIO_CHARGE) &&
      !bk7258_gpio_read(BATTERY_GPIO_FULL))
    {
      *value = 100;
      return OK;
    }

  ret = bk7258_battery_sample(priv, &millivolts);
  if (ret < 0)
    {
      return ret;
    }

  *value = bk7258_battery_percent(millivolts);
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_battery_pin_driven
 *
 * Description:
 *   Decide whether anything outside the SoC is holding a status pin.
 *
 *   A pin with a real driver on it keeps its level when a weak internal
 *   pull tries to move it the other way; an unconnected one just follows
 *   whichever pull is applied.  So: pull down, read, pull up, read.  Equal
 *   readings mean something else won the contest, which is what "driven"
 *   means here.  Different readings mean the pad is ours alone.
 *
 *   This is the measurement that caught the original mistake.  Both pins
 *   read high with no pull, the vendor's table turned that into "charging",
 *   and it stayed convincing for as long as nobody pulled the other way.
 *
 ****************************************************************************/

static bool bk7258_battery_pin_driven(int pin)
{
  bool low;
  bool high;

  bk7258_gpio_config(pin, false, false, true);
  up_udelay(1000);
  low = bk7258_gpio_read(pin);

  bk7258_gpio_config(pin, false, true, false);
  up_udelay(1000);
  high = bk7258_gpio_read(pin);

  /* Leave the pull-up in place either way.  If the pin is an open-drain
   * status output that is the arrangement it expects, and if it is
   * unconnected a defined level beats a floating input.
   */

  return low == high;
}

/****************************************************************************
 * Name: bk7258_battery_register
 *
 * Description:
 *   Configure the two charger status pins and register the gauge.
 *
 *   The pins are configured as inputs explicitly.  Reading them without
 *   doing so returns whatever the input latch happens to hold, which is
 *   not the pad -- the same trap that produced three hours of false
 *   negatives while bringing the camera clock up (PORTING_NOTES 7.5).
 *   Neither pin gets a pull: the charger drives both.
 *
 ****************************************************************************/

int bk7258_battery_register(FAR const char *devpath)
{
  FAR struct bk7258_battery_dev_s *priv;
  int ret;

  priv = kmm_zalloc(sizeof(struct bk7258_battery_dev_s));
  if (priv == NULL)
    {
      return -ENOMEM;
    }

  /* batlock and flist are initialized by battery_gauge_register(); only
   * the converter lock is ours.
   */

  priv->dev.ops = &g_bk7258_battery_ops;
  nxmutex_init(&priv->adclock);

  priv->status_valid = bk7258_battery_pin_driven(BATTERY_GPIO_CHARGE) &&
                       bk7258_battery_pin_driven(BATTERY_GPIO_FULL);

  if (!priv->status_valid)
    {
      syslog(LOG_WARNING,
             "batt: charger status pins (GPIO%d/%d) are not driven; "
             "state will read unknown\n",
             BATTERY_GPIO_CHARGE, BATTERY_GPIO_FULL);
    }

  ret = battery_gauge_register(devpath,
                               (FAR struct battery_gauge_dev_s *)priv);
  if (ret < 0)
    {
      nxmutex_destroy(&priv->adclock);
      kmm_free(priv);
      return ret;
    }

  return OK;
}

#endif /* CONFIG_BK7258_BATTERY */
