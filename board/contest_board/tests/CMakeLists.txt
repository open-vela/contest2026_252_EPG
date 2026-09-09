# ##############################################################################
# board/contest_board/tests/CMakeLists.txt
#
# Contest board self tests.  Reached from the apps build through the
# apps/tests symlink, for the same reason apps/ itself is reached through
# vendor/openvela/apps: nuttx_add_subdirectory() and nuttx_generate_kconfig()
# both scan only their immediate children, so a directory has to sit under
# apps/ to be found -- while the tests belong beside the board port they
# exercise, not inside the shipping application set.
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

nuttx_add_subdirectory()
nuttx_generate_kconfig(MENUDESC "Contest board tests")
