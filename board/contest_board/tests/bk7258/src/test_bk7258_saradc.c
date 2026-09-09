/****************************************************************************
 * board/contest_board/tests/bk7258/src/test_bk7258_saradc.c
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

#include <string.h>

#include "test_bk7258.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: saradc_batch
 *
 * Description:
 *   One acquisition on a channel, using the configuration the closed RF
 *   library asks this driver for (chip/bk7258_phy_osi.c phy_osi_sense):
 *   continuous mode off the crystal at ~203 kHz, saturation mode 2, the
 *   longest settle code, and a ten-sample batch.  Raw samples land in buf.
 *
 * Returned Value:
 *   OK, or the negated errno the driver reported.  The converter is
 *   stopped either way, so a failed acquisition cannot leave it running
 *   for the next case.
 *
 ****************************************************************************/

static int saradc_batch(uint8_t channel, FAR uint16_t *buf, uint32_t size)
{
  int ret;

  ret = bk7258_saradc_start(channel, TB_ADC_MODE_CONTINUOUS,
                            bk7258_saradc_div(TB_ADC_CLK_HZ),
                            TB_ADC_SAT_MODE_2, TB_ADC_STEADY, 0, 0);
  if (ret < 0)
    {
      return ret;
    }

  ret = bk7258_saradc_read(buf, size, TB_ADC_TIMEOUT_MS);
  bk7258_saradc_stop();
  return ret;
}

/****************************************************************************
 * Name: saradc_average
 *
 * Description:
 *   The vendor's reduction: drop the leading samples taken while the input
 *   was still settling, drop the two codes the converter emits when it has
 *   nothing to say (zero and mid-scale), average the rest.
 *
 * Returned Value:
 *   The averaged code, or 0 when the batch produced nothing usable --
 *   which is exactly how the vendor's own accessor signals the same thing,
 *   and which the callers below treat as a failure because 0 is outside
 *   every validity window.
 *
 ****************************************************************************/

static uint32_t saradc_average(FAR const uint16_t *buf, uint32_t size)
{
  uint32_t count = 0;
  uint32_t sum = 0;
  uint32_t i;

  for (i = TB_ADC_SKIP; i < size; i++)
    {
      if (buf[i] != 0 && buf[i] != TB_ADC_SAMPLE_INVALID)
        {
          sum += buf[i];
          count++;
        }
    }

  return count == 0 ? 0 : sum / count;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: test_bk7258_saradc_setup
 *
 * Description:
 *   Force the converter idle before every case.  The driver carries a
 *   "conversion in flight" flag, and a case that left it set would make
 *   the next bk7258_saradc_start() return -EBUSY -- a failure caused
 *   entirely by the previous case.  Stopping an already-stopped converter
 *   is defined behaviour, which is what makes this safe as a fixture.
 *
 ****************************************************************************/

int test_bk7258_saradc_setup(FAR void **state)
{
  UNUSED(state);

  bk7258_saradc_tempsensor(false);
  bk7258_saradc_stop();
  return 0;
}

/****************************************************************************
 * Name: test_bk7258_saradc_teardown
 ****************************************************************************/

int test_bk7258_saradc_teardown(FAR void **state)
{
  UNUSED(state);

  /* Drop the temperature sensor bias whether or not this case raised it:
   * the vendor brackets every temperature acquisition rather than leaving
   * the sensor biased, and the analog shadow is shared with the RF path.
   */

  bk7258_saradc_tempsensor(false);
  bk7258_saradc_stop();
  return 0;
}

/****************************************************************************
 * Name: test_bk7258_saradc_tempsensor
 *
 * Description:
 *   Read the on-die temperature sensor on channel 7 and require the code
 *   to land inside the vendor's validity window (10 < code < 1365 after
 *   the divide-by-four this part uses).  That window is what the closed RF
 *   library checks before it will trust a reading, so a code outside it is
 *   the same event that makes calibration skip its temperature correction.
 *
 *   Genuine ways this fails: the sensor bias never reaches the analog
 *   island (shadow-register handshake broken) so the input floats and the
 *   code pins at zero or full scale; the converter never starts and the
 *   read times out; the channel field is mis-programmed and the reading is
 *   actually the supply, which sits far above the window.
 *
 ****************************************************************************/

void test_bk7258_saradc_tempsensor(FAR void **state)
{
  uint16_t raw[TB_ADC_BATCH];
  uint32_t code;
  int ret;

  UNUSED(state);

  memset(raw, 0, sizeof(raw));

  bk7258_saradc_tempsensor(true);
  ret = saradc_batch(TB_ADC_TEMP_CHANNEL, raw, TB_ADC_BATCH);
  bk7258_saradc_tempsensor(false);

  assert_int_equal(ret, 0);

  code = saradc_average(raw, TB_ADC_BATCH) >> TB_ADC_TEMP_SHIFT;
  assert_in_range(code, TB_ADC_VAL_MIN + 1, TB_ADC_VAL_MAX - 1);
}

/****************************************************************************
 * Name: test_bk7258_saradc_voltage
 *
 * Description:
 *   Read the supply on channel 0.  The vendor accepts any code above 10
 *   here and falls back to a nominal constant below it, so ">10" is the
 *   real acceptance threshold rather than an invented one.
 *
 *   This fails when the front end is not biased (every sample reads zero,
 *   the average reduces to zero and zero is not above ten) or when the
 *   FIFO never fills and the read times out.  A board whose supply had
 *   actually collapsed would not be running this program, so a low code
 *   here means the converter, not the rail.
 *
 ****************************************************************************/

void test_bk7258_saradc_voltage(FAR void **state)
{
  uint16_t raw[TB_ADC_BATCH];
  uint32_t code;
  int ret;

  UNUSED(state);

  memset(raw, 0, sizeof(raw));

  ret = saradc_batch(TB_ADC_VOLT_CHANNEL, raw, TB_ADC_BATCH);
  assert_int_equal(ret, 0);

  code = saradc_average(raw, TB_ADC_BATCH);
  assert_true(code > TB_ADC_VAL_MIN);
}

/****************************************************************************
 * Name: test_bk7258_saradc_fifo_advances
 *
 * Description:
 *   Eight consecutive samples off one channel must not all carry the same
 *   code.  A converter that is running produces a least-significant bit of
 *   noise on every acquisition; a FIFO that is not advancing hands the
 *   same latched word back on every read, and every other assertion in
 *   this file would still pass while it did -- an average of one repeated
 *   plausible code is a plausible average.  That is the specific failure
 *   this case exists to catch.
 *
 *   Two neighbouring samples can legitimately be identical, which is why
 *   the assertion is over the whole batch rather than over a pair.
 *
 ****************************************************************************/

void test_bk7258_saradc_fifo_advances(FAR void **state)
{
  uint16_t raw[8];
  bool moved = false;
  int ret;
  int i;

  UNUSED(state);

  memset(raw, 0, sizeof(raw));

  ret = saradc_batch(TB_ADC_VOLT_CHANNEL, raw, 8);
  assert_int_equal(ret, 0);

  for (i = 1; i < 8; i++)
    {
      if (raw[i] != raw[0])
        {
          moved = true;
          break;
        }
    }

  assert_true(moved);
}
