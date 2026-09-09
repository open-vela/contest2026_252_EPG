/****************************************************************************
 * board/contest_board/chip/bk7258_wifi.h
 *
 * WiFi network device for the BK7258.
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

#ifndef __BOARD_CONTEST_BOARD_CHIP_BK7258_WIFI_H
#define __BOARD_CONTEST_BOARD_CHIP_BK7258_WIFI_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifndef __ASSEMBLY__

#undef EXTERN
#if defined(__cplusplus)
#define EXTERN extern "C"
extern "C"
{
#else
#define EXTERN extern
#endif

/****************************************************************************
 * Name: bk7258_wifi_initialize
 *
 * Description:
 *   Register the WiFi interface with the network stack, as an 802.11
 *   netdev lower half with wireless extensions.
 *
 *   The interface it registers cannot carry traffic yet.  The vendor MAC is
 *   not built, because two internal headers it needs -- sm_task.h and ps.h,
 *   which between them define struct vif_info_tag -- are in none of Beken's
 *   published SDKs and never have been.  ifup() and the radio parameters
 *   answer -ENOSYS until that is resolved; the association parameters
 *   (essid, bssid, passwd, mode, auth) are implemented and do work, so the
 *   wapi interface can be exercised against this driver as it stands.
 *
 *   See PORTING_NOTES chapter 21 for the measurements and for what to ask
 *   Beken for.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int bk7258_wifi_initialize(void);

#undef EXTERN
#if defined(__cplusplus)
}
#endif

#endif /* __ASSEMBLY__ */
#endif /* __BOARD_CONTEST_BOARD_CHIP_BK7258_WIFI_H */
