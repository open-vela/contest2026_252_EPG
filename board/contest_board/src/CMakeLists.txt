# ##############################################################################
# board/contest_board/src/CMakeLists.txt
#
# The "board" library target is created by nuttx/boards; we only add sources
# to it.
#
# Licensed to the Apache Software Foundation (ASF) under one or more contributor
# license agreements.  See the NOTICE file distributed with this work for
# additional information regarding copyright ownership.  The ASF licenses this
# file to you under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License.  You may obtain a copy of
# the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
# License for the specific language governing permissions and limitations under
# the License.
#
# ##############################################################################

set(SRCS bk7258_boardinit.c bk7258_reset.c)

if(CONFIG_BOARDCTL)
  list(APPEND SRCS bk7258_appinit.c)
endif()

if(CONFIG_USERLED)
  list(APPEND SRCS bk7258_userleds.c)
endif()

if(CONFIG_ARCH_BUTTONS)
  list(APPEND SRCS bk7258_buttons.c)
endif()

if(CONFIG_DEV_GPIO)
  list(APPEND SRCS bk7258_gpiodev.c)
endif()

if(CONFIG_I2C_BITBANG)
  list(APPEND SRCS bk7258_i2c_bitbang.c)
endif()

if(CONFIG_LCD)
  list(APPEND SRCS bk7258_gc9d01.c bk7258_swspi.c)
endif()

if(CONFIG_BK7258_BATTERY)
  list(APPEND SRCS bk7258_battery.c)
endif()

target_sources(board PRIVATE ${SRCS})

target_include_directories(board PRIVATE ${NUTTX_CHIP_ABS_DIR})

# The /etc ROMFS.  sched/init/nx_bringup.c mounts it before the init task
# runs, which is what puts /etc/1.txt in place for the xTS md5 case, and
# /etc/templates/template9 for the RNG one -- NonOverlappingTemplate reads
# "templates/template<m>" relative to its working directory, and that file
# ships in the NIST archive rather than with the test, so without it the run
# produces 148 rows of 0.000000 that look like a failing generator and are
# really a missing input.  2.6 KB; copy it next to the experiments tree
# before running (see PORTING_NOTES).  rcS and
# rc.sysinit go through RCSRCS (they are preprocessed, so they must stay free
# of '#' comments); everything else is copied verbatim through RCRAWS.

if(CONFIG_ETC_ROMFS)
  nuttx_add_romfs(
    NAME
    etc
    MOUNTPOINT
    etc
    RCSRCS
    etc/init.d/rcS
    etc/init.d/rc.sysinit
    RCRAWS
    etc/group
    etc/1.txt
    etc/templates/template9
    PATH
    ${CMAKE_CURRENT_LIST_DIR}/etc)

  target_link_libraries(board PRIVATE romfs_etc)
endif()

if(CONFIG_TTY_SIGINT)
  # Instrument for the console SIGINT chain (see src/sigtest.c).
  nuttx_add_application(
    NAME
    sigtest
    STACKSIZE
    4096
    PRIORITY
    100
    SRCS
    sigtest.c)
endif()

if(CONFIG_BK7258_FLASH)
  # Instrument for the on-chip flash controller (see src/flashtest.c).
  nuttx_add_application(
    NAME
    flashtest
    STACKSIZE
    4096
    PRIORITY
    100
    SRCS
    flashtest.c)
endif()

if(CONFIG_BK7258_TIMER)
  # Instrument for TIMER group 0 (see src/timertest.c).
  nuttx_add_application(
    NAME
    timertest
    STACKSIZE
    4096
    PRIORITY
    100
    SRCS
    timertest.c)
endif()

if(CONFIG_BK7258_WIFI_VENDOR)
  # Beken's own receiver-sensitivity test.  The vendor reaches it through
  # bk_cli, which this port does not build (see src/rxsens.c).
  nuttx_add_application(
    NAME
    rxsens
    STACKSIZE
    4096
    PRIORITY
    100
    SRCS
    rxsens.c)

  # Register peek, so "is that bit set on the board" stops being answered by
  # reading vendor source (see src/bkreg.c).
  nuttx_add_application(
    NAME
    bkreg
    STACKSIZE
    2048
    PRIORITY
    100
    SRCS
    bkreg.c)
endif()

if(CONFIG_TESTING_NIST_STS)
  # Instrument for the "MAX # OF OPENED FILES" wall the NIST suite hits.
  nuttx_add_application(
    NAME
    fdtest
    STACKSIZE
    4096
    PRIORITY
    100
    SRCS
    fdtest.c)
endif()

if(CONFIG_CRYPTO)
  nuttx_add_application(
    NAME
    md5_test
    STACKSIZE
    4096
    PRIORITY
    100
    SRCS
    md5_test.c)
endif()

if(CONFIG_BK7258_BATTERY)
  # Instrument for the battery gauge (see src/batttest.c).
  nuttx_add_application(
    NAME
    batttest
    STACKSIZE
    2048
    PRIORITY
    100
    SRCS
    batttest.c)
endif()
