/****************************************************************************
 * board/contest_board/tests/bk7258/src/test_bk7258_audio.c
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

/* The fixture runs the two initialisers and keeps what they returned; the
 * cases assert on those values rather than calling the initialisers
 * themselves.  Both are idempotent behind a "already done" flag, so a case
 * that called them a second time would be asserting on a cached OK and
 * would keep passing after the hardware sequence broke.
 */

static int g_dac_init_ret = -1;
static int g_adc_init_ret = -1;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: test_bk7258_audio_setup
 *
 * Description:
 *   Bring the AUD block to the state every audio case expects: domain
 *   powered, clock gated on, DAC and ADC configured for 16 kHz, hardware
 *   loopback off, neither converter running.
 *
 ****************************************************************************/

int test_bk7258_audio_setup(FAR void **state)
{
  UNUSED(state);

  g_dac_init_ret = bk7258_audio_dac_init(true);
  g_adc_init_ret = bk7258_audio_adc_init(true);

  /* The silicon loopback bit survives a stop, so clear it here as well as
   * in the teardown: a case must not inherit a mic-to-speaker path that
   * some earlier run of this program left set.
   */

  bk7258_audio_loopback(false);
  return 0;
}

/****************************************************************************
 * Name: test_bk7258_audio_teardown
 *
 * Description:
 *   Mute and stop both converters and drop the power amplifier, so the
 *   next case starts from silence whatever this one did.
 *
 ****************************************************************************/

int test_bk7258_audio_teardown(FAR void **state)
{
  UNUSED(state);

  bk7258_audio_loopback(false);
  bk7258_audio_adc_stop();
  bk7258_audio_dac_stop();
  return 0;
}

/****************************************************************************
 * Name: test_bk7258_audio_deviceid
 *
 * Description:
 *   The AUD block identifies itself in its first register.  The value is
 *   the measured one -- lowercase "aud" -- not the one the documentation
 *   prints, and it only reads back once the AUDP domain is powered and the
 *   module clock is gated on, which is what the fixture arranged.
 *
 *   This fails whenever the domain is left unpowered or the clock gate
 *   shut: the bus returns zero (or the last bus value) instead of the tag.
 *   It is the cheapest proof that everything the rest of the audio cases
 *   write is actually reaching the block.
 *
 ****************************************************************************/

void test_bk7258_audio_deviceid(FAR void **state)
{
  UNUSED(state);

  assert_int_equal(tb_getreg32(TB_AUD_DEVICEID), TB_AUD_ID_VALUE);
}

/****************************************************************************
 * Name: test_bk7258_audio_dac_init
 *
 * Description:
 *   The DAC bring-up reports success, and the two system-level gates it is
 *   responsible for are actually in the state it claims: the AUDP domain
 *   powered (the power bit is low active) and the audio module clock
 *   enabled.  Checking the gates rather than only the return code is the
 *   point -- the initialiser returns OK from its cached flag on every call
 *   after the first, so the return code alone stops meaning anything as
 *   soon as something else powers the domain back down.
 *
 *   The digital DAC word is checked too: the driver programs a fixed
 *   configuration (HPF bypassed, digital gain 0x20) and a 16 kHz rate
 *   field, so a block that is not accepting writes reads back zero here.
 *
 ****************************************************************************/

void test_bk7258_audio_dac_init(FAR void **state)
{
  uint32_t regval;

  UNUSED(state);

  assert_int_equal(g_dac_init_ret, 0);

  regval = tb_getreg32(TB_SYS_POWER);
  assert_int_equal(regval & TB_SYS_POWER_AUDP, 0);

  regval = tb_getreg32(TB_SYS_CLK_EN);
  assert_int_equal(regval & TB_SYS_CLK_EN_AUD, TB_SYS_CLK_EN_AUD);

  /* Bits 23:16 are the fields the driver owns in this word (HPF bypass
   * plus digital gain 0x20).  Comparing the field rather than the whole
   * register keeps the case honest about what it knows: the reserved bits
   * belong to no one here, and asserting on them would be asserting on
   * silicon behaviour this port has never characterised.
   */

  regval = tb_getreg32(TB_AUD_DAC_CONFIG0);
  assert_int_equal((regval >> 16) & 0xffu, 0x83u);

  /* Rate field bits 7:6 select 8 k (0) or 16 k (1); the fixture asked for
   * 16 k.  Bit 8 is the APLL select the common bring-up sets, and the ADC
   * only ever produces samples with it set.
   */

  regval = tb_getreg32(TB_AUD_CONFIG);
  assert_int_equal((regval >> 6) & 3u, 1u);
  assert_int_equal(regval & (1u << 8), 1u << 8);
}

/****************************************************************************
 * Name: test_bk7258_audio_adc_init
 *
 * Description:
 *   The capture path reports success and its digital configuration word
 *   reads back as programmed: gain field 0x33 and both high-pass filters
 *   bypassed with the analog microphone selected.  A block whose clock
 *   never came up returns zero for that word, which is the failure this
 *   catches; the same read also proves the ADC configuration was not
 *   clobbered by the DAC bring-up, which shares AUD_CONFIG with it.
 *
 ****************************************************************************/

void test_bk7258_audio_adc_init(FAR void **state)
{
  uint32_t regval;

  UNUSED(state);

  assert_int_equal(g_adc_init_ret, 0);

  regval = tb_getreg32(TB_AUD_ADC_CONFIG0);
  assert_int_equal((regval >> 18) & 0x3fu, 0x33u);
  assert_int_equal((regval >> 16) & 0x3u, 0x3u);

  /* The ADC's own rate field is bits 1:0 of the shared word.  The DAC
   * bring-up writes the same register, so this is where a read-modify-
   * write that turned into a plain store would show up.
   */

  regval = tb_getreg32(TB_AUD_CONFIG);
  assert_int_equal(regval & 3u, 1u);
}
