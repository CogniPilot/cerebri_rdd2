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
set(_rate_header
  "${RDD2_TEST_EFMI_ROOT}/RateControlAllocator/Vehicles_Rdd2_RateControlAllocator/ProductionCode/Vehicles_Rdd2_RateControlAllocator.h"
)
set(_navigation_header
  "${RDD2_TEST_EFMI_ROOT}/NavigationEstimator/Vehicles_Rdd2_NavigationEstimator/ProductionCode/Vehicles_Rdd2_NavigationEstimator.h"
)
set(_guidance_header
  "${RDD2_TEST_EFMI_ROOT}/GuidanceController/Vehicles_Rdd2_GuidanceController/ProductionCode/Vehicles_Rdd2_GuidanceController.h"
)
get_filename_component(_rate_include_dir "${_rate_header}" DIRECTORY)
get_filename_component(_navigation_include_dir "${_navigation_header}" DIRECTORY)
get_filename_component(_guidance_include_dir "${_guidance_header}" DIRECTORY)
foreach(_generated_header IN ITEMS
    "${_rate_header}"
    "${_navigation_header}"
    "${_guidance_header}"
)
  if(NOT EXISTS "${_generated_header}")
    message(FATAL_ERROR "Missing frozen generated header: ${_generated_header}")
  endif()
endforeach()

get_filename_component(_workspace_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(ZEPHYR_EXTRA_MODULES
  "${_workspace_root}/.devenv/state/west/modules/lib/zros"
  "${_workspace_root}/.devenv/state/west/modules/lib/csyn"
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(process_wrapper_fault_injection)

target_include_directories(app PRIVATE
  "${_workspace_root}/src"
  "${_workspace_root}/src/processes"
  "${_workspace_root}/subsys/gnss_source"
  "${_workspace_root}/.devenv/state/west/modules/lib/zros/include"
  "${_rate_include_dir}"
  "${_navigation_include_dir}"
  "${_guidance_include_dir}"
)
target_sources(app PRIVATE
  src/log_module.c
  src/rate_wrapper_test.c
  src/data_test.c
  src/navigation_wrapper_test.c
  src/navigation_gps_test.c
  src/guidance_wrapper_test.c
  "${_workspace_root}/src/processes/navigation_gps.c"
)
target_compile_definitions(app PRIVATE CONFIG_RDD2_LOCKSTEP=1)
