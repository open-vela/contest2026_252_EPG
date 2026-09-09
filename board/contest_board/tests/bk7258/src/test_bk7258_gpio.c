/****************************************************************************
 * board/contest_board/tests/bk7258/src/test_bk7258_gpio.c
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

#include "test_bk7258.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* The pin words as they were before the case ran.  Restoring them byte for
 * byte -- rather than reconfiguring to what they are believed to have been
 * -- is what makes this fixture leak-free even for the bits it does not
 * know about, such as the interrupt enable the button driver owns.
 */

static uint32_t g_saved_s2;
static uint32_t g_saved_s3;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: test_bk7258_gpio_setup
 ****************************************************************************/

int test_bk7258_gpio_setup(FAR void **state)
{
  UNUSED(state);

  g_saved_s2 = tb_getreg32(TB_GPIO_CFG(TB_GPIO_PIN_S2));
  g_saved_s3 = tb_getreg32(TB_GPIO_CFG(TB_GPIO_PIN_S3));
  return 0;
}

/****************************************************************************
 * Name: test_bk7258_gpio_teardown
 ****************************************************************************/

int test_bk7258_gpio_teardown(FAR void **state)
{
  UNUSED(state);

  tb_putreg32(g_saved_s2, TB_GPIO_CFG(TB_GPIO_PIN_S2));
  tb_putreg32(g_saved_s3, TB_GPIO_CFG(TB_GPIO_PIN_S3));
  return 0;
}

/****************************************************************************
 * Name: test_bk7258_gpio_config
 *
 * Description:
 *   Pin configuration lands in the pad register with the polarity the part
 *   actually uses, and an input reads what its pull resistor says.
 *
 *   The output-disable bit is the reason this case exists.  It is low
 *   active -- the pad drives only while it reads zero -- and the port has
 *   already once left every output in high impedance by treating it as an
 *   ordinary enable.  A regression that flips it back reads as "output
 *   configured" everywhere in the driver and produces nothing at the pin,
 *   which no return code reports; here it fails immediately, because an
 *   input pin must have both io_mode bits set.
 *
 *   The read assertion is a live one: both switch pins carry pull-ups and
 *   nothing else drives them, so a pin that has genuinely been configured
 *   as a pulled-up input reads high.  It requires only that neither switch
 *   is being held down while the suite runs.  A pull that never reaches
 *   the pad leaves the input floating, and a floating pin next to a
 *   switched rail does not reliably read high.
 *
 *   bk7258_gpio_write() is exercised against the register only, never
 *   against the pad: the pins available for this on the board are switch
 *   inputs, and driving one high while its switch is closed would be a
 *   short.  Setting the output latch of a pin whose driver is disabled
 *   changes the register and nothing else.
 *
 ****************************************************************************/

void test_bk7258_gpio_config(FAR void **state)
{
  uint32_t regval;

  UNUSED(state);

  /* Input with a pull-up: both io_mode bits set, pull enabled and up,
   * peripheral function released.
   */

  bk7258_gpio_config(TB_GPIO_PIN_S2, false, true, false);

  regval = tb_getreg32(TB_GPIO_CFG(TB_GPIO_PIN_S2));
  assert_int_equal(regval & TB_GPIO_CFG_INPUT_EN, TB_GPIO_CFG_INPUT_EN);
  assert_int_equal(regval & TB_GPIO_CFG_OUTPUT_DIS, TB_GPIO_CFG_OUTPUT_DIS);
  assert_int_equal(regval & TB_GPIO_CFG_PULL_EN, TB_GPIO_CFG_PULL_EN);
  assert_int_equal(regval & TB_GPIO_CFG_PULL_UP, TB_GPIO_CFG_PULL_UP);
  assert_int_equal(regval & TB_GPIO_CFG_FUNC_EN, 0);

  /* Same pin, pull-down: the enable stays, the direction bit drops. */

  bk7258_gpio_config(TB_GPIO_PIN_S2, false, false, true);

  regval = tb_getreg32(TB_GPIO_CFG(TB_GPIO_PIN_S2));
  assert_int_equal(regval & TB_GPIO_CFG_PULL_EN, TB_GPIO_CFG_PULL_EN);
  assert_int_equal(regval & TB_GPIO_CFG_PULL_UP, 0);

  /* The output latch is writable independently of the pad driver. */

  bk7258_gpio_write(TB_GPIO_PIN_S2, true);
  regval = tb_getreg32(TB_GPIO_CFG(TB_GPIO_PIN_S2));
  assert_int_equal(regval & TB_GPIO_CFG_OUTPUT, TB_GPIO_CFG_OUTPUT);

  bk7258_gpio_write(TB_GPIO_PIN_S2, false);
  regval = tb_getreg32(TB_GPIO_CFG(TB_GPIO_PIN_S2));
  assert_int_equal(regval & TB_GPIO_CFG_OUTPUT, 0);

  /* Live read-back on both switch pins, pulled up and released. */

  bk7258_gpio_config(TB_GPIO_PIN_S2, false, true, false);
  bk7258_gpio_config(TB_GPIO_PIN_S3, false, true, false);

  assert_true(bk7258_gpio_read(TB_GPIO_PIN_S2));
  assert_true(bk7258_gpio_read(TB_GPIO_PIN_S3));

  /* An out-of-range pin is rejected rather than walked off the end of the
   * register file; the driver reports it by reading false.
   */

  assert_false(bk7258_gpio_read(-1));
  assert_false(bk7258_gpio_read(64));
}
