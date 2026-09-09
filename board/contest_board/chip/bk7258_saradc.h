/****************************************************************************
 * boards/arm/bk7258/contest_board/chip/bk7258_saradc.h
 *
 * Public interface of the SARADC driver.
 *
 * The converter was brought up for the closed RF library, which samples the
 * transmit power detector, the die temperature and the supply during
 * calibration; bk7258_phy_osi.c declared these prototypes locally because
 * it was the only caller.  The battery gauge is the second caller, so the
 * declarations move here rather than being copied.
 *
 * The converter is a single shared resource with no arbitration of its own:
 * bk7258_saradc_start() answers -EBUSY while a conversion is in flight, and
 * callers are expected to cope rather than wait.
 *
 ****************************************************************************/

#ifndef __BOARDS_ARM_BK7258_CONTEST_BOARD_CHIP_BK7258_SARADC_H
#define __BOARDS_ARM_BK7258_CONTEST_BOARD_CHIP_BK7258_SARADC_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Conversion modes.  Mode 0 powers the converter down and is rejected by
 * bk7258_saradc_start().
 */

#define SARADC_MODE_SINGLE_STEP  1
#define SARADC_MODE_SOFTWARE     2
#define SARADC_MODE_CONTINUOUS   3

/* Analog channels that work without pad muxing.  Channel 0 carries the
 * supply, which on this board is the cell: the vendor's battery monitor
 * reads it there.  A digital channel would additionally need its pin
 * switched to the converter, which the driver does not do.
 */

#define SARADC_CHAN_SUPPLY       0
#define SARADC_CHAN_TEMPERATURE  7
#define SARADC_CHAN_TXPOWER      8

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_saradc_div
 *
 * Description:
 *   Translate a wanted converter clock in Hz into the divider field.
 *
 ****************************************************************************/

uint32_t bk7258_saradc_div(uint32_t adc_clk);

/****************************************************************************
 * Name: bk7258_saradc_pwrup
 *
 * Description:
 *   Raise the converter's power and clock.  Idempotent.
 *
 ****************************************************************************/

int bk7258_saradc_pwrup(void);

/****************************************************************************
 * Name: bk7258_saradc_start
 *
 * Description:
 *   Configure the converter and start it.  See the definition in
 *   bk7258_saradc.c for what each argument means; the calibration result
 *   correction is bypassed, matching every one of the vendor's own callers.
 *
 * Returned Value:
 *   OK, -EINVAL on an out-of-range argument, -EBUSY if a conversion is
 *   already in flight or the converter never went idle.
 *
 ****************************************************************************/

int bk7258_saradc_start(uint8_t channel, uint8_t mode, uint32_t div,
                        uint8_t saturate, uint8_t steady, uint8_t rate,
                        uint8_t filter);

/****************************************************************************
 * Name: bk7258_saradc_stop
 ****************************************************************************/

int bk7258_saradc_stop(void);

/****************************************************************************
 * Name: bk7258_saradc_read
 *
 * Description:
 *   Collect size raw samples, waiting at most timeout_ms for them.
 *
 ****************************************************************************/

int bk7258_saradc_read(uint16_t *buf, uint32_t size, uint32_t timeout_ms);

/****************************************************************************
 * Name: bk7258_saradc_tempsensor
 *
 * Description:
 *   Gate the on-die temperature sensor that feeds channel 7.
 *
 ****************************************************************************/

void bk7258_saradc_tempsensor(bool enable);

#endif /* __BOARDS_ARM_BK7258_CONTEST_BOARD_CHIP_BK7258_SARADC_H */
