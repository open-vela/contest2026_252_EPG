/****************************************************************************
 * board/contest_board/tests/bk7258/src/test_bk7258_pwm.c
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

static int g_motor_init_ret = -1;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: pwm_duty_pct
 *
 * Description:
 *   Recover the programmed duty from CCR4.  The driver puts the high pulse
 *   at the tail of the period, so the compare value counts down from the
 *   period: CCR4 = period - (period / 100) * duty.
 *
 ****************************************************************************/

static uint32_t pwm_duty_pct(void)
{
  uint32_t ccr4 = tb_getreg32(TB_PWM_CCR4);

  if (ccr4 >= TB_MOTOR_PERIOD)
    {
      return 0;
    }

  return (TB_MOTOR_PERIOD - ccr4) * 100u / TB_MOTOR_PERIOD;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: test_bk7258_pwm_setup
 *
 * Description:
 *   Bring up PWM unit 0 and park the output disabled.  Every case here
 *   starts with the motor silent whatever the case before it did, which
 *   matters more than usual: the block under test drives a vibration
 *   motor, so a leaked enable bit is audible for the rest of the run.
 *
 ****************************************************************************/

int test_bk7258_pwm_setup(FAR void **state)
{
  UNUSED(state);

  g_motor_init_ret = bk7258_motor_init();
  bk7258_motor_off();
  return 0;
}

/****************************************************************************
 * Name: test_bk7258_pwm_teardown
 ****************************************************************************/

int test_bk7258_pwm_teardown(FAR void **state)
{
  UNUSED(state);

  bk7258_motor_off();
  return 0;
}

/****************************************************************************
 * Name: test_bk7258_pwm_init
 *
 * Description:
 *   The bring-up reports success and the timer it configured reads back:
 *   the auto-reload register holds one period less than the 26 MHz-derived
 *   1 kHz count, and the TIM2 counter enable is set.
 *
 *   The register read-back is the assertion that can actually fail.  The
 *   initialiser returns OK unconditionally once its static flag is set, so
 *   on its own the return code proves nothing; but every register in this
 *   block reads back as zero while the module clock gate is shut or the
 *   soft reset is still held, which is precisely the failure mode a
 *   clock-tree change introduces.
 *
 ****************************************************************************/

void test_bk7258_pwm_init(FAR void **state)
{
  UNUSED(state);

  assert_int_equal(g_motor_init_ret, 0);
  assert_int_equal(tb_getreg32(TB_PWM_TIM2_ARR), TB_MOTOR_PERIOD - 1);
  assert_int_equal(tb_getreg32(TB_PWM_CR1) & TB_PWM_CR1_CEN2,
                   TB_PWM_CR1_CEN2);
}

/****************************************************************************
 * Name: test_bk7258_pwm_output_gate
 *
 * Description:
 *   Switching the motor on raises the channel-4 output enable in CCMR and
 *   programs the requested duty; switching it off drops the enable again.
 *
 *   Both halves are falsifiable against the pad, not just against a
 *   variable: CCMR is the register the pin driver looks at, so a driver
 *   that forgot the enable, set the wrong channel's enable, or cleared the
 *   whole word instead of one bit fails here.  The duty read-back catches
 *   the neighbouring mistake of enabling the output while leaving the
 *   compare value at its parked "never toggles" setting, which looks
 *   enabled and produces no vibration.
 *
 ****************************************************************************/

void test_bk7258_pwm_output_gate(FAR void **state)
{
  uint32_t ccmr;

  UNUSED(state);

  assert_int_equal(bk7258_motor_on(30), 0);

  ccmr = tb_getreg32(TB_PWM_CCMR);
  assert_int_equal(ccmr & TB_PWM_CCMR_CH4E, TB_PWM_CCMR_CH4E);
  assert_int_equal(pwm_duty_pct(), 30);

  assert_int_equal(bk7258_motor_off(), 0);

  ccmr = tb_getreg32(TB_PWM_CCMR);
  assert_int_equal(ccmr & TB_PWM_CCMR_CH4E, 0);
}

/****************************************************************************
 * Name: test_bk7258_pwm_duty_clamp
 *
 * Description:
 *   An over-range request is clamped rather than passed through.  The
 *   clamp is a hardware-protection rule, not a style preference: an ERM
 *   motor driven past roughly 60% sees close to DC, so a driver that let
 *   200 through would be programming a compare value below zero -- which
 *   in an unsigned register wraps to an enormous one and parks the output
 *   permanently high.
 *
 *   The case asserts on the register the pad follows, so it fails whether
 *   the clamp is removed, moved after the register write, or applied to
 *   the wrong variable.  The lower bound in the range check keeps it from
 *   passing when the request is silently dropped to zero instead.
 *
 ****************************************************************************/

void test_bk7258_pwm_duty_clamp(FAR void **state)
{
  uint32_t ccr4;

  UNUSED(state);

  assert_int_equal(bk7258_motor_on(200), 0);

  ccr4 = tb_getreg32(TB_PWM_CCR4);
  assert_in_range(ccr4,
                  TB_MOTOR_PERIOD -
                  (TB_MOTOR_PERIOD / 100) * TB_MOTOR_DUTY_MAX,
                  TB_MOTOR_PERIOD - 1);
  assert_in_range(pwm_duty_pct(), 1, TB_MOTOR_DUTY_MAX);

  bk7258_motor_off();
}
