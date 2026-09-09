/****************************************************************************
 * board/contest_board/src/rxsens.c
 *
 * Beken's own receiver-sensitivity test, exposed to NSH.
 *
 * The vendor registers this as the "rxsens" console command from bk_cli,
 * which this port does not build -- so the test sat in the image
 * (do_rx_sensitivity is linked) with no way to reach it.  This is the
 * missing three lines.
 *
 * It exists because the receiver currently reports zero frames on all
 * thirteen scan channels and takes no RX-trigger interrupt at all, and the
 * question "does the radio hear anything" deserves a direct measurement
 * rather than more disassembly of closed code.
 *
 * Usage, from the library's own help string:
 *
 *   rxsens [-b bandwidth]  0:20M (default)  1:40M
 *          [-c channel]    wifi 1..14, ble 2400..2527
 *          [-d duration]   0: stop timer, d: start timer with d interval
 *
 * Requires the vendor MAC to be up -- run "ifup wlan0" first, or let
 * netinit do it.  Its output comes from the closed PHY library and is
 * gated by _bk_feature_phy_log_enable in bk7258_phy_osi.c.
 *
 * RUN IT IN THE BACKGROUND:  rxsens -c 6 -d 2000 &
 *
 * It does not return.  Run in the foreground it occupies NSH, and since it
 * prints nothing after "[RS]reset_mm" the console looks dead -- which is
 * how this was first, wrongly, written up as "hangs the board".  It does
 * not: with "&" the shell stays responsive and ps shows the task alive.
 *
 * What ps actually shows is the useful part.  The task sits in state Ready,
 * not Waiting, with its stack usage identical across samples minutes apart:
 * a tight spin at a fixed call depth, not a blocked wait.  Interrupts are
 * never masked either -- a traced run recorded zero rtos_disable_int calls.
 * So the test is polling for something that never arrives.
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

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdio.h>

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/* components/bk_wifi/src/phy.c, compiled into this image with the vendor's
 * flags.  Declared rather than included: its header pulls in the vendor
 * include path, which this file is not compiled against.
 */

extern void rx_sens_cmd_test(char *buf, int len, int argc, char **argv);

/* libwifi.a.  Bring the MAC out of doze before touching it.
 *
 * The command reaches the MAC outside the core thread:
 *
 *   do_rx_sensitivity -> rs_test -> [g_phy_funcs_t+24] -> rs_init
 *                     -> hal_machw_stop
 *
 * and hal_machw_stop sets the MAC soft-reset bit and polls it until the
 * hardware clears it.  rwnx_intf_init() calls rwnxl_sleep() at the tail of
 * wifi_init(), and core_thread_main undoes that for every message it pops --
 * but the NSH task never goes through core_thread_main, so the MAC is still
 * asleep when this command reaches it.
 *
 * Two narrower attempts came first and are recorded because each was wrong in
 * an instructive way.  hal_machw_enable_maccore_clk() alone opens the clock
 * gate -- bkreg confirmed core_clk=running -- and the soft-reset poll still
 * never terminated.  Adding rf_module_vote_ctrl(RF_OPEN, RF_BY_WIFI_BIT) then
 * powered and clocked the modem too (phytrace showed rf_vote(202,0) and
 * modem_clk(27,1)), and instead of hanging the board reset -- with the
 * vendor's own assert finally saying what was actually wrong:
 *
 *     wifi: ASSERT MAC is in doze, open maccore and phy clock
 *     !!!ASSERT at nxmac_current_state_getf:2001.
 *
 * That is the guard at the top of every nxmac accessor: it reads the doze
 * byte at 0x2803d94c+1 and asserts if it is set.  The MAC was never merely
 * clock-gated -- it is logically in doze, rwnxl_sleep set that flag, and no
 * amount of poking clocks clears it.  A dozing MAC does not run its soft
 * reset, which is why the poll never finished.
 *
 * So wake it properly.  rwnxl_wakeup() clears the doze flag, clears the core
 * clock gate (the same bic that hal_machw_enable_maccore_clk does), and takes
 * the Wi-Fi RF vote itself -- which is why neither of the two calls above is
 * needed any more.
 *
 * That gets the test through its whole init sequence for the first time --
 * [RS]reset_mm, config_me, config_me_channel, start_mm, and seven ISRs
 * registered -- so the soft-reset poll really was blocked on doze.
 *
 * It is still not enough to run the test, and the way it now fails is the
 * next thing to fix rather than a mystery.  About four seconds in, the NMI
 * watchdog bites with core_thread as the current task, and its stack is
 *
 *     core_thread_main+0x102 -> mac_sleep_check+0x110 -> rwnxl_sleep+0x104
 *
 * i.e. the core thread was putting the MAC back to sleep while this task had
 * just woken it.  The wake/sleep pair belongs to the core thread and is not
 * safe to drive from here; core_thread_main runs mac_sleep_check after every
 * message it pops, so any wake done from the console is undone underneath it.
 *
 * The real fix is to stop the stack parking the MAC at all for the duration
 * of a radio test, which is exactly what the vendor's build does: rxsens is
 * an ATE-mode tool, rwnx_intf_init only calls rwnxl_sleep() on the
 * !ate_is_enabled() branch, and this port's ate_is_enabled() returns false.
 * Making that answer true (or runtime-switchable) is the next step, and it is
 * worth trying against the zero-receive problem generally, not just here --
 * ps_env_set_ps_on(true) is set on the same branch.
 */

extern void rwnxl_wakeup(void);

/* Temporary: arms the critical-section trace in bk7258_ble_shim.c while the
 * rxsens hang is being located.
 */

extern void bk7258_int_trace(bool on);

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  if (argc < 2)
    {
      printf("rxsens [-b bandwidth]  0:20M (default)  1:40M\n"
             "       [-c channel]    wifi 1..14, ble 2400..2527\n"
             "       [-d duration]   0: stop timer, d: start with interval\n"
             "\n"
             "Bring the interface up first: ifup wlan0\n");
      return 1;
    }

  /* The vendor parses argv itself, U-Boot style, and expects argv[0] to be
   * the command name -- which is exactly what NSH hands us.
   */

  rwnxl_wakeup();

  bk7258_int_trace(true);
  rx_sens_cmd_test(NULL, 0, argc, argv);
  bk7258_int_trace(false);
  return 0;
}
