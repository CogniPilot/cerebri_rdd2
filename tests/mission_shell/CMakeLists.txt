# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

include("${CMAKE_CURRENT_LIST_DIR}/../rdd2_test_environment.cmake")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(mission_shell)

target_link_libraries(app PRIVATE synapse_fbs::c)

target_include_directories(app PRIVATE
  "${RDD2_TEST_APP_ROOT}/src"
  "${RDD2_TEST_APP_ROOT}/subsys/gnss_source"
  "${RDD2_ZROS_ROOT}/include"
)
target_sources(app PRIVATE src/main.c)
