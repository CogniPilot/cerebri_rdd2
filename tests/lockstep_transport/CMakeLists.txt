# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

include("${CMAKE_CURRENT_LIST_DIR}/../rdd2_test_environment.cmake")
list(APPEND DTS_ROOT "${RDD2_TEST_APP_ROOT}")
set(DTC_OVERLAY_FILE "${CMAKE_CURRENT_LIST_DIR}/app.overlay")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(lockstep_transport)

target_include_directories(app PRIVATE
  "${RDD2_TEST_APP_ROOT}/src"
  "${RDD2_TEST_APP_ROOT}/subsys/gnss_source"
  "${RDD2_TEST_APP_ROOT}/subsys/lockstep"
)
target_sources(app PRIVATE
  src/main.c
  "${RDD2_TEST_APP_ROOT}/drivers/input/lockstep_rc.c"
  "${RDD2_TEST_APP_ROOT}/subsys/gnss_source/gnss_lockstep.c"
  "${RDD2_TEST_APP_ROOT}/subsys/lockstep/lockstep_input.c"
  "${RDD2_TEST_APP_ROOT}/subsys/lockstep/lockstep_transport.c"
)
