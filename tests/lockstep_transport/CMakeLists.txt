# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

get_filename_component(RDD2_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
list(APPEND DTS_ROOT "${RDD2_ROOT}")
set(DTC_OVERLAY_FILE "${CMAKE_CURRENT_LIST_DIR}/app.overlay")
set(ZEPHYR_EXTRA_MODULES
  "${RDD2_ROOT}/.devenv/state/west/modules/lib/zros"
  "${RDD2_ROOT}/.devenv/state/west/modules/lib/csyn"
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(lockstep_transport)

target_include_directories(app PRIVATE
  "${RDD2_ROOT}/src"
  "${RDD2_ROOT}/subsys/lockstep"
)
target_sources(app PRIVATE
  src/main.c
  "${RDD2_ROOT}/drivers/input/lockstep_rc.c"
  "${RDD2_ROOT}/subsys/lockstep/lockstep_input.c"
  "${RDD2_ROOT}/subsys/lockstep/lockstep_transport.c"
)
