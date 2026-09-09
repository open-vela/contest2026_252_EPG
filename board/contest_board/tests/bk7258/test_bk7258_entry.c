/****************************************************************************
 * board/contest_board/tests/bk7258/test_bk7258_entry.c
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
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_group_setup
 *
 * Description:
 *   Runs once, before any case in the suite.
 *
 *   Its only job is the BLE bring-up, which must happen exactly once per
 *   boot: the closed controller does not survive being initialised twice,
 *   and a per-case setup that re-registered the OSI table leaves the radio
 *   deaf for the rest of the boot.
 *
 *   It always reports success.  A group fixture that failed would mark
 *   every case in the suite as errored, including the audio, converter,
 *   PWM and video cases that never touch the radio -- so a dead radio
 *   would hide the state of everything else.  The bring-up records what
 *   each stage returned instead, and the BLE cases assert on those.
 *
 ****************************************************************************/

int bk7258_group_setup(FAR void **state)
{
  UNUSED(state);

  bk7258_ble_stack_bringup();
  return 0;
}

/****************************************************************************
 * Name: bk7258_group_teardown
 ****************************************************************************/

int bk7258_group_teardown(FAR void **state)
{
  UNUSED(state);

  /* Nothing to undo.  The radio is deliberately left up: tearing the
   * controller down and letting a later run bring it up again is the
   * double-initialisation this suite exists to avoid.
   */

  return 0;
}

/****************************************************************************
 * Name: main
 *
 * Description:
 *   cmocka_bk7258_test: the board self-test suite for the Agora ConvoAI
 *   Kit R1 (Beken BK7258).  Every case drives a bare register driver in
 *   this board's chip/ directory and asserts against hardware state, not
 *   against the driver's own bookkeeping.
 *
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  /* Add Test Cases */

  const struct CMUnitTest Bk7258TestSuite[] =
  {
    CM_BK7258_TESTCASES
  };

  UNUSED(argc);
  UNUSED(argv);

  /* Run Test cases */

  cmocka_run_group_tests(Bk7258TestSuite, bk7258_group_setup,
                         bk7258_group_teardown);
  return 0;
}
