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
set(_rate_header
  "${RDD2_TEST_EFMI_ROOT}/Vehicles_Rdd2_RateControlAllocator/ProductionCode/Vehicles_Rdd2_RateControlAllocator.h"
)
set(_navigation_header
  "${RDD2_TEST_EFMI_ROOT}/Vehicles_Rdd2_NavigationEstimator/ProductionCode/Vehicles_Rdd2_NavigationEstimator.h"
)
set(_guidance_header
  "${RDD2_TEST_EFMI_ROOT}/Vehicles_Rdd2_GuidanceController/ProductionCode/Vehicles_Rdd2_GuidanceController.h"
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
    message(FATAL_ERROR "Missing generated header: ${_generated_header}")
  endif()
endforeach()

include("${CMAKE_CURRENT_LIST_DIR}/../rdd2_test_environment.cmake")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(process_wrapper_fault_injection)

target_include_directories(app PRIVATE
  "${RDD2_TEST_APP_ROOT}/src"
  "${RDD2_TEST_APP_ROOT}/src/processes"
  "${RDD2_TEST_APP_ROOT}/subsys/gnss_source"
  "${RDD2_ZROS_ROOT}/include"
  "${_rate_include_dir}"
  "${_navigation_include_dir}"
  "${_guidance_include_dir}"
)
target_sources(app PRIVATE
  src/log_module.c
  src/rate_wrapper_test.c
  src/data_test.c
  src/imu_preintegration_test.c
  src/navigation_wrapper_test.c
  src/navigation_gps_test.c
  src/guidance_wrapper_test.c
  "${RDD2_TEST_APP_ROOT}/src/processes/navigation_gps.c"
  "${RDD2_TEST_APP_ROOT}/src/processes/imu_preintegration.c"
)
target_compile_definitions(app PRIVATE CONFIG_RDD2_LOCKSTEP=1)
