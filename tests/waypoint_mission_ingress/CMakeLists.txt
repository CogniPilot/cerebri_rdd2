# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

set(RDD2_TEST_EFMI_ROOT "" CACHE PATH
  "Root containing generated eFMI control directories"
)
if(NOT RDD2_TEST_EFMI_ROOT AND DEFINED ENV{RDD2_TEST_EFMI_ROOT})
  set(RDD2_TEST_EFMI_ROOT "$ENV{RDD2_TEST_EFMI_ROOT}")
endif()
if(NOT RDD2_TEST_EFMI_ROOT)
  message(FATAL_ERROR "RDD2_TEST_EFMI_ROOT is required")
endif()

get_filename_component(RDD2_TEST_EFMI_ROOT "${RDD2_TEST_EFMI_ROOT}" ABSOLUTE)
set(_planner_header
  "${RDD2_TEST_EFMI_ROOT}/Planning_Bezier_WaypointTrajectoryPlanner/ProductionCode/Planning_Bezier_WaypointTrajectoryPlanner.h"
)
set(_planner_source
  "${RDD2_TEST_EFMI_ROOT}/Planning_Bezier_WaypointTrajectoryPlanner/ProductionCode/Planning_Bezier_WaypointTrajectoryPlanner.c"
)
set(_kernel_header
  "${RDD2_TEST_EFMI_ROOT}/Planning_Bezier_WaypointTrajectoryPlanner/ProductionCode/rumoca_galec_kernels.h"
)
set(_kernel_source
  "${RDD2_TEST_EFMI_ROOT}/Planning_Bezier_WaypointTrajectoryPlanner/ProductionCode/rumoca_galec_kernels.c"
)
foreach(_generated_file IN ITEMS
    "${_planner_header}"
    "${_planner_source}"
    "${_kernel_header}"
    "${_kernel_source}"
)
  if(NOT EXISTS "${_generated_file}")
    message(FATAL_ERROR "Missing generated planner file: ${_generated_file}")
  endif()
endforeach()
get_filename_component(_planner_include_dir "${_planner_header}" DIRECTORY)

include("${CMAKE_CURRENT_LIST_DIR}/../rdd2_test_environment.cmake")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(waypoint_mission_ingress)

target_include_directories(app PRIVATE
  "${RDD2_TEST_APP_ROOT}/src"
  "${RDD2_TEST_APP_ROOT}/src/processes"
  "${RDD2_TEST_APP_ROOT}/subsys/gnss_source"
  "${RDD2_ZROS_ROOT}/include"
  "${_planner_include_dir}"
)
target_sources(app PRIVATE
  src/log_module.c
  src/main.c
  src/generated_real_test.c
  "${_planner_source}"
  "${_kernel_source}"
)
