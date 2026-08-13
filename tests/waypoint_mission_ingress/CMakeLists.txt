# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

set(RDD2_TEST_EFMI_ROOT "" CACHE PATH
  "Root containing frozen generated eFMI ProductionCode directories"
)
if(NOT RDD2_TEST_EFMI_ROOT AND DEFINED ENV{RDD2_TEST_EFMI_ROOT})
  set(RDD2_TEST_EFMI_ROOT "$ENV{RDD2_TEST_EFMI_ROOT}")
endif()
if(NOT RDD2_TEST_EFMI_ROOT)
  message(FATAL_ERROR "RDD2_TEST_EFMI_ROOT is required")
endif()

get_filename_component(RDD2_TEST_EFMI_ROOT "${RDD2_TEST_EFMI_ROOT}" ABSOLUTE)
set(_planner_header
  "${RDD2_TEST_EFMI_ROOT}/WaypointTrajectoryPlanner/Planning_Bezier_WaypointTrajectoryPlanner/ProductionCode/Planning_Bezier_WaypointTrajectoryPlanner.h"
)
set(_planner_source
  "${RDD2_TEST_EFMI_ROOT}/WaypointTrajectoryPlanner/Planning_Bezier_WaypointTrajectoryPlanner/ProductionCode/Planning_Bezier_WaypointTrajectoryPlanner.c"
)
foreach(_generated_file IN ITEMS "${_planner_header}" "${_planner_source}")
  if(NOT EXISTS "${_generated_file}")
    message(FATAL_ERROR "Missing frozen generated planner file: ${_generated_file}")
  endif()
endforeach()
get_filename_component(_planner_include_dir "${_planner_header}" DIRECTORY)

get_filename_component(_workspace_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(ZEPHYR_EXTRA_MODULES
  "${_workspace_root}/.devenv/state/west/modules/lib/zros"
  "${_workspace_root}/.devenv/state/west/modules/lib/csyn"
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(waypoint_mission_ingress)

target_include_directories(app PRIVATE
  "${_workspace_root}/src"
  "${_workspace_root}/src/processes"
  "${_workspace_root}/subsys/gnss_source"
  "${_workspace_root}/.devenv/state/west/modules/lib/zros/include"
  "${_planner_include_dir}"
)
target_sources(app PRIVATE
  src/log_module.c
  src/main.c
  src/generated_real_test.c
  "${_planner_source}"
)
