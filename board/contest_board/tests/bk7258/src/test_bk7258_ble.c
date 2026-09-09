/****************************************************************************
 * board/contest_board/tests/bk7258/src/test_bk7258_ble.c
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

#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include "test_bk7258.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* How long the controller gets before it is declared hung.  The vendor's
 * own bring-up completes in well under a second; ten is the figure the
 * board's face application already uses for the same watchdog.
 */

#define TB_BLE_CTRL_TIMEOUT_MS  10000
#define TB_BLE_CTRL_POLL_MS     100
#define TB_BLE_CTRL_STACKSIZE   8192
#define TB_BLE_CTRL_PRIORITY    90

/* Advertisers are everywhere; five seconds of passive listening on all
 * three primary channels hears a great many of them.  A receive chain that
 * is actually working cannot come back with none.
 */

#define TB_BLE_SCAN_SECONDS     5

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* What each stage of the one-time bring-up returned.  The cases assert on
 * these; nothing here re-runs the bring-up.
 */

struct tb_ble_result_s
{
  bool ran;                 /* The group fixture reached this file        */
  bool controller_returned; /* false means the closed init never came back */
  int  osi;
  int  feature;
  int  phy;
  int  rf;
  int  controller;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct tb_ble_result_s g_ble;
static volatile int g_ctrl_done;
static volatile int g_ctrl_ret;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: tb_ble_ctrl_thread
 *
 * Description:
 *   Run the closed controller's initialiser off to one side.  It is the
 *   first stage that touches the radio and the only one that has ever been
 *   observed not to return; running it on the test thread would take the
 *   whole program down with it and report nothing, which is the opposite
 *   of what a test is for.
 *
 ****************************************************************************/

static FAR void *tb_ble_ctrl_thread(FAR void *arg)
{
  UNUSED(arg);

  g_ctrl_ret = bk7258_bt_controller_init();
  g_ctrl_done = 1;
  return NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_ble_stack_bringup
 *
 * Description:
 *   The staged bring-up, run exactly once per boot from the group fixture.
 *
 *   Once is not a preference.  Re-registering the OSI table, or handing
 *   the PHY and RF adapters their tables a second time, over a controller
 *   that is already live leaves the radio deaf for the rest of the boot --
 *   and the symptom is a scan that returns zero, which reads exactly like
 *   broken hardware.  That is why the bring-up is here and not in a
 *   per-case setup: cmocka runs a per-case setup once per case, and three
 *   BLE cases would mean three bring-ups.
 *
 *   Order matters too.  PHY before RF, because the radio arbiter's error
 *   path dereferences the PHY table's logger and would fault instead of
 *   reporting; and the synthesiser handover before the controller, which
 *   reads the RF configuration as it brings the transceiver up.
 *
 ****************************************************************************/

void bk7258_ble_stack_bringup(void)
{
  pthread_attr_t attr;
  struct sched_param sp;
  pthread_t tid;
  int waited;

  if (g_ble.ran)
    {
      return;
    }

  g_ble.ran = true;

  g_ble.osi = bk7258_bt_osi_init();
  if (g_ble.osi != 0)
    {
      return;
    }

  g_ble.feature = bk7258_bt_feature_init();
  if (g_ble.feature != 0)
    {
      return;
    }

  g_ble.phy = bk7258_phy_adapter_init();
  g_ble.rf = bk7258_rf_adapter_init();
  if (g_ble.phy != 0 || g_ble.rf != 0)
    {
      return;
    }

  bk7258_ble_use_bt_pll();

  g_ctrl_done = 0;
  g_ctrl_ret = -1;
  g_ble.controller = -1;

  pthread_attr_init(&attr);
  sp.sched_priority = TB_BLE_CTRL_PRIORITY;
  pthread_attr_setschedparam(&attr, &sp);
  pthread_attr_setstacksize(&attr, TB_BLE_CTRL_STACKSIZE);

  if (pthread_create(&tid, &attr, tb_ble_ctrl_thread, NULL) != 0)
    {
      return;
    }

  for (waited = 0;
       waited < TB_BLE_CTRL_TIMEOUT_MS / TB_BLE_CTRL_POLL_MS &&
       g_ctrl_done == 0;
       waited++)
    {
      usleep(TB_BLE_CTRL_POLL_MS * 1000);
    }

  if (g_ctrl_done == 0)
    {
      /* Leave the thread where it is.  Cancelling a task that is inside
       * the closed initialiser would be a worse outcome than a reported
       * failure, and the case below turns this into one.
       */

      return;
    }

  pthread_join(tid, NULL);

  g_ble.controller_returned = true;
  g_ble.controller = g_ctrl_ret;
}

/****************************************************************************
 * Name: test_bk7258_ble_setup / test_bk7258_ble_teardown
 *
 * Description:
 *   Deliberately empty of hardware.  The BLE block has no per-case reset
 *   that is safe to perform on a live controller, so the isolation these
 *   cases get comes from doing the bring-up once, before any of them, and
 *   never touching it again.
 *
 ****************************************************************************/

int test_bk7258_ble_setup(FAR void **state)
{
  UNUSED(state);
  return 0;
}

int test_bk7258_ble_teardown(FAR void **state)
{
  UNUSED(state);
  return 0;
}

/****************************************************************************
 * Name: test_bk7258_ble_stack_init
 *
 * Description:
 *   Every stage of the bring-up was accepted.
 *
 *   Each of these is a handshake with a closed archive that checks what it
 *   is given.  The OSI stage fails when the table's layout no longer
 *   matches the ABI the controller was built against -- a real event on
 *   this port, and one that no compiler diagnostic reports.  The feature
 *   stage fails on a feature block of the wrong size.  The PHY and RF
 *   stages fail when the radio library rejects its adapter tables.  The
 *   controller stage is the one that powers the transceiver and is the
 *   only stage that can hang rather than fail, which is why it is reported
 *   separately: a controller that never returned is a distinct outcome
 *   from one that returned an error, and they have different causes.
 *
 ****************************************************************************/

void test_bk7258_ble_stack_init(FAR void **state)
{
  UNUSED(state);

  assert_true(g_ble.ran);
  assert_int_equal(g_ble.osi, 0);
  assert_int_equal(g_ble.feature, 0);
  assert_int_equal(g_ble.phy, 0);
  assert_int_equal(g_ble.rf, 0);
  assert_true(g_ble.controller_returned);
  assert_int_equal(g_ble.controller, 0);
}

/****************************************************************************
 * Name: test_bk7258_ble_scan
 *
 * Description:
 *   Passive scanning hears at least one advertiser.
 *
 *   This is the only case in the suite that measures the radio rather than
 *   the register interface to it, and it is the one that made the rest
 *   worth writing.  Every stage above can report success on a part whose
 *   receive chain produces nothing: the controller initialises, accepts
 *   the scan parameters, accepts the enable, and then simply never raises
 *   an advertising report.  Counting reports is what distinguishes a radio
 *   that works from a radio that merely answers.
 *
 *   It assumes the board is somewhere with BLE traffic, which in practice
 *   means anywhere with a phone or a laptop in it.  A zero here on a bench
 *   that is genuinely empty of advertisers is the one false negative this
 *   case admits, and it is cheap to rule out.
 *
 ****************************************************************************/

void test_bk7258_ble_scan(FAR void **state)
{
  int reports;

  UNUSED(state);

  /* Scanning a controller that never came up would report a transport
   * error rather than a radio one, so say which it is.
   */

  assert_true(g_ble.controller_returned);
  assert_int_equal(g_ble.controller, 0);

  reports = bk7258_ble_scan(TB_BLE_SCAN_SECONDS);
  assert_true(reports > 0);
}
