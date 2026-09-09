/****************************************************************************
 * board/contest_board/tests/bk7258/include/test_bk7258.h
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

#ifndef __TESTS_BK7258_INCLUDE_TEST_BK7258_H
#define __TESTS_BK7258_INCLUDE_TEST_BK7258_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>

#include <nuttx/config.h>

#include <stdbool.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Register windows the cases read back.  The drivers under test are bare
 * register drivers with no query interface, so "the write landed" can only
 * be shown by reading the hardware -- and reading it from here rather than
 * from a driver accessor is the point: a driver that silently stopped
 * writing would still report success through its own getter.
 *
 * Every address below is the one the driver itself uses; the comment names
 * the driver constant it mirrors so the two cannot drift apart unnoticed.
 */

/* chip/bk7258_audio.c */

#define TB_AUD_BASE            0x47800000ul  /* AUD_BASE                   */
#define TB_AUD_DEVICEID        (TB_AUD_BASE + 0x00)
#define TB_AUD_ADC_CONFIG0     (TB_AUD_BASE + 0x10)
#define TB_AUD_DAC_CONFIG0     (TB_AUD_BASE + 0x1c)
#define TB_AUD_CONFIG          (TB_AUD_BASE + 0xc0)
#define TB_AUD_ID_VALUE        0x00617564ul  /* AUD_ID_VALUE, "aud"        */

#define TB_SYS_POWER           0x44010040ul  /* SYS_POWER,  bit6 pwd_audp  */
#define TB_SYS_POWER_AUDP      (1u << 6)     /* 0 = domain powered         */
#define TB_SYS_CLK_EN          0x44010030ul  /* SYS_CLK_EN, bit30 aud_cken */
#define TB_SYS_CLK_EN_AUD      (1u << 30)

/* chip/bk7258_pwm.c */

#define TB_PWM0_BASE           0x458a0000ul  /* PWM0_BASE                  */
#define TB_PWM_CR1             (TB_PWM0_BASE + 0x10)
#define TB_PWM_CCMR            (TB_PWM0_BASE + 0x28)
#define TB_PWM_TIM2_ARR        (TB_PWM0_BASE + 0x40)
#define TB_PWM_CCR4            (TB_PWM0_BASE + 0x60)
#define TB_PWM_CCMR_CH4E       (1u << 15)    /* CCMR_CH4E, output enable   */
#define TB_PWM_CR1_CEN2        (1u << 1)     /* TIM2 counter running       */
#define TB_MOTOR_PERIOD        26000         /* MOTOR_PERIOD, 26 MHz / 1k  */
#define TB_MOTOR_DUTY_MAX      60            /* hard clamp in _motor_on()  */

/* chip/bk7258_gpio.h, one 32-bit word per pin */

#define TB_GPIO_CFG(n)         (0x44000400ul + ((n) << 2))
#define TB_GPIO_CFG_INPUT      (1u << 0)
#define TB_GPIO_CFG_OUTPUT     (1u << 1)
#define TB_GPIO_CFG_INPUT_EN   (1u << 2)
#define TB_GPIO_CFG_OUTPUT_DIS (1u << 3)
#define TB_GPIO_CFG_PULL_UP    (1u << 4)
#define TB_GPIO_CFG_PULL_EN    (1u << 5)
#define TB_GPIO_CFG_FUNC_EN    (1u << 6)

/* S3 and S2 on this board, the two pins face_main.c already polls as
 * pulled-up inputs.  Nothing else on the board drives them, so a case may
 * reconfigure them as long as it puts the word back.
 */

#define TB_GPIO_PIN_S3         8
#define TB_GPIO_PIN_S2         12

/* chip/bk7258_saradc.c, through the recipe chip/bk7258_phy_osi.c uses:
 * continuous mode on the 26 MHz crystal, saturation mode 2, longest
 * settle, and the vendor's ten-sample batch with the first five dropped.
 */

#define TB_ADC_MODE_CONTINUOUS 3
#define TB_ADC_SAT_MODE_2      3
#define TB_ADC_CLK_HZ          203125
#define TB_ADC_STEADY          7
#define TB_ADC_TIMEOUT_MS      100
#define TB_ADC_BATCH           10
#define TB_ADC_SKIP            5
#define TB_ADC_SAMPLE_INVALID  2048
#define TB_ADC_TEMP_CHANNEL    7
#define TB_ADC_VOLT_CHANNEL    0
#define TB_ADC_TEMP_SHIFT      2
#define TB_ADC_VAL_MIN         10             /* PHY_ADC_TEMP_VAL_MIN      */
#define TB_ADC_VAL_MAX         1365           /* PHY_ADC_TEMP_VAL_MAX      */

/* Frame geometry the JPEG decoder and the DMA2D splitter agree on. */

#define TB_PANEL_W             160
#define TB_PANEL_H             160

/* Comparison tolerances.
 *
 * TB_TOL_YUV covers the difference between the hardware IDCT and the host
 * reference decoder on the same coefficients; a component that lands
 * further out than this is not rounding, it is a different picture.
 *
 * TB_TOL_RGB additionally absorbs the RGB565 quantisation the panel format
 * imposes (up to 4 counts on red and blue, 2 on green after expansion) on
 * top of the colour-space matrix rounding.  It is deliberately far below
 * the error a swapped Cb/Cr pair produces on these saturated colours,
 * which is over 100 counts.
 */

#define TB_TOL_YUV             16
#define TB_TOL_RGB             24

/* A byte pattern no decode or conversion result can legitimately be.  The
 * output buffers are stamped with it before every run so "the block never
 * wrote anything" fails loudly instead of passing on stale content.
 *
 * The value is not arbitrary: 0xa8 sits at least 32 counts away from every
 * reference component in test_bk7258_image.h and, read as an RGB565 pair,
 * at least 100 counts away from every reference colour.  A poison byte
 * that happened to land inside a tolerance would make an untouched buffer
 * pass, which is the one thing it exists to prevent.
 */

#define TB_POISON              0xa8

/****************************************************************************
 * Inline Functions
 ****************************************************************************/

/****************************************************************************
 * Name: tb_getreg32 / tb_putreg32
 *
 * Description:
 *   Peripheral register access from the test task.  arm_internal.h is
 *   private to the arch build, so the accessors are restated here rather
 *   than included; this is a flat build, so the mapping is the same one
 *   the drivers see.
 *
 ****************************************************************************/

static inline uint32_t tb_getreg32(uintptr_t addr)
{
  return *(volatile uint32_t *)addr;
}

static inline void tb_putreg32(uint32_t value, uintptr_t addr)
{
  *(volatile uint32_t *)addr = value;
}

/****************************************************************************
 * Name: tb_absdiff
 *
 * Description:
 *   Unsigned distance between two samples.  assert_in_range() casts to an
 *   unsigned type, so a signed difference has to be folded before it is
 *   handed over or a negative error would read as an enormous one.
 *
 ****************************************************************************/

static inline unsigned int tb_absdiff(int a, int b)
{
  return (unsigned int)(a > b ? a - b : b - a);
}

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* The chip layer under test.  These live in the board's chip/ directory
 * and no public header exports them -- the board's own applications
 * declare them at the point of use for the same reason.  Restating them in
 * one place means a signature that changes breaks the build here instead
 * of corrupting the stack at run time.
 */

int  bk7258_audio_dac_init(bool rate16k);
int  bk7258_audio_dac_start(void);
int  bk7258_audio_dac_stop(void);
int  bk7258_audio_dac_write(FAR const int16_t *pcm, int nsamples);
int  bk7258_audio_adc_init(bool rate16k);
int  bk7258_audio_adc_start(void);
int  bk7258_audio_adc_stop(void);
int  bk7258_audio_adc_read(FAR int16_t *pcm, int nsamples);
int  bk7258_audio_loopback(bool on);

uint32_t bk7258_saradc_div(uint32_t adc_clk);
int  bk7258_saradc_pwrup(void);
int  bk7258_saradc_start(uint8_t channel, uint8_t mode, uint32_t div,
                         uint8_t saturate, uint8_t steady, uint8_t rate,
                         uint8_t filter);
int  bk7258_saradc_stop(void);
int  bk7258_saradc_read(FAR uint16_t *buf, uint32_t size,
                        uint32_t timeout_ms);
void bk7258_saradc_tempsensor(bool enable);

int  bk7258_motor_init(void);
int  bk7258_motor_on(int duty_pct);
int  bk7258_motor_off(void);

void bk7258_gpio_config(int pin, bool output, bool pullup, bool pulldown);
void bk7258_gpio_write(int pin, bool value);
bool bk7258_gpio_read(int pin);

#ifdef CONFIG_TESTING_BK7258_VIDEO
int  bk7258_jpegdec_decode(FAR const uint8_t *jpg, size_t len,
                           FAR uint8_t *out, int width, int height);
int  bk7258_dma2d_split(FAR const void *src, FAR void *dstl, FAR void *dstr);
void bk7258_dma2d_set_fgcfg(uint32_t fmt2, int reve);
#endif

int  bk7258_bt_osi_init(void);
int  bk7258_bt_feature_init(void);
int  bk7258_phy_adapter_init(void);
int  bk7258_rf_adapter_init(void);
int  bk7258_ble_use_bt_pll(void);
int  bk7258_bt_controller_init(void);
int  bk7258_ble_scan(int seconds);

/* Group fixture.  The BLE stack is brought up here and nowhere else: the
 * closed controller does not survive being initialised twice, and a
 * per-case setup that re-registered the OSI table left the radio deaf for
 * the rest of the boot.  Everything else the group fixture does is
 * bookkeeping, and it always reports success so that one dead block
 * cannot fail cases that never touch it.
 */

int  bk7258_group_setup(FAR void **state);
int  bk7258_group_teardown(FAR void **state);

/* Per-module fixtures: each one parks its own block in a known state on
 * the way in and on the way out, so no case can inherit a running
 * converter, an enabled PWM output or a reconfigured pin from the case
 * before it.
 */

int  test_bk7258_audio_setup(FAR void **state);
int  test_bk7258_audio_teardown(FAR void **state);
int  test_bk7258_saradc_setup(FAR void **state);
int  test_bk7258_saradc_teardown(FAR void **state);
int  test_bk7258_pwm_setup(FAR void **state);
int  test_bk7258_pwm_teardown(FAR void **state);
int  test_bk7258_gpio_setup(FAR void **state);
int  test_bk7258_gpio_teardown(FAR void **state);
int  test_bk7258_video_setup(FAR void **state);
int  test_bk7258_video_teardown(FAR void **state);
int  test_bk7258_ble_setup(FAR void **state);
int  test_bk7258_ble_teardown(FAR void **state);

/* Called by the group fixture, defined next to the BLE cases. */

void bk7258_ble_stack_bringup(void);

/* TEST CASES FUNCTIONS */

void test_bk7258_audio_deviceid(FAR void **state);
void test_bk7258_audio_dac_init(FAR void **state);
void test_bk7258_audio_adc_init(FAR void **state);

void test_bk7258_saradc_tempsensor(FAR void **state);
void test_bk7258_saradc_voltage(FAR void **state);
void test_bk7258_saradc_fifo_advances(FAR void **state);

void test_bk7258_pwm_init(FAR void **state);
void test_bk7258_pwm_output_gate(FAR void **state);
void test_bk7258_pwm_duty_clamp(FAR void **state);

void test_bk7258_gpio_config(FAR void **state);

#ifdef CONFIG_TESTING_BK7258_VIDEO
void test_bk7258_jpegdec_decode(FAR void **state);
void test_bk7258_dma2d_split(FAR void **state);
#endif

void test_bk7258_ble_stack_init(FAR void **state);
void test_bk7258_ble_scan(FAR void **state);

/****************************************************************************
 * Pre-processor Definitions -- the suite itself
 ****************************************************************************/

#ifdef CONFIG_TESTING_BK7258_VIDEO
#  define CM_BK7258_VIDEO_TESTCASES                                    \
     cmocka_unit_test_setup_teardown(test_bk7258_jpegdec_decode,       \
                                     test_bk7258_video_setup,          \
                                     test_bk7258_video_teardown),      \
     cmocka_unit_test_setup_teardown(test_bk7258_dma2d_split,          \
                                     test_bk7258_video_setup,          \
                                     test_bk7258_video_teardown),
#else
#  define CM_BK7258_VIDEO_TESTCASES
#endif

#define CM_BK7258_TESTCASES                                            \
  cmocka_unit_test_setup_teardown(test_bk7258_audio_deviceid,          \
                                  test_bk7258_audio_setup,             \
                                  test_bk7258_audio_teardown),         \
  cmocka_unit_test_setup_teardown(test_bk7258_audio_dac_init,          \
                                  test_bk7258_audio_setup,             \
                                  test_bk7258_audio_teardown),         \
  cmocka_unit_test_setup_teardown(test_bk7258_audio_adc_init,          \
                                  test_bk7258_audio_setup,             \
                                  test_bk7258_audio_teardown),         \
  cmocka_unit_test_setup_teardown(test_bk7258_saradc_tempsensor,       \
                                  test_bk7258_saradc_setup,            \
                                  test_bk7258_saradc_teardown),        \
  cmocka_unit_test_setup_teardown(test_bk7258_saradc_voltage,          \
                                  test_bk7258_saradc_setup,            \
                                  test_bk7258_saradc_teardown),        \
  cmocka_unit_test_setup_teardown(test_bk7258_saradc_fifo_advances,    \
                                  test_bk7258_saradc_setup,            \
                                  test_bk7258_saradc_teardown),        \
  cmocka_unit_test_setup_teardown(test_bk7258_pwm_init,                \
                                  test_bk7258_pwm_setup,               \
                                  test_bk7258_pwm_teardown),           \
  cmocka_unit_test_setup_teardown(test_bk7258_pwm_output_gate,         \
                                  test_bk7258_pwm_setup,               \
                                  test_bk7258_pwm_teardown),           \
  cmocka_unit_test_setup_teardown(test_bk7258_pwm_duty_clamp,          \
                                  test_bk7258_pwm_setup,               \
                                  test_bk7258_pwm_teardown),           \
  cmocka_unit_test_setup_teardown(test_bk7258_gpio_config,             \
                                  test_bk7258_gpio_setup,              \
                                  test_bk7258_gpio_teardown),          \
  CM_BK7258_VIDEO_TESTCASES                                            \
  cmocka_unit_test_setup_teardown(test_bk7258_ble_stack_init,          \
                                  test_bk7258_ble_setup,               \
                                  test_bk7258_ble_teardown),           \
  cmocka_unit_test_setup_teardown(test_bk7258_ble_scan,                \
                                  test_bk7258_ble_setup,               \
                                  test_bk7258_ble_teardown),

#endif /* __TESTS_BK7258_INCLUDE_TEST_BK7258_H */
