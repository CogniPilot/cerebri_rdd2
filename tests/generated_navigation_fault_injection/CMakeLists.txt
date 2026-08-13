# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

set(RDD2_TEST_EFMI_ROOT "" CACHE PATH
  "Root containing manifest-verified staged eFMI ProductionCode directories"
)
if(NOT RDD2_TEST_EFMI_ROOT AND DEFINED ENV{RDD2_TEST_EFMI_ROOT})
  set(RDD2_TEST_EFMI_ROOT "$ENV{RDD2_TEST_EFMI_ROOT}")
endif()
if(NOT RDD2_TEST_EFMI_ROOT)
  message(FATAL_ERROR "RDD2_TEST_EFMI_ROOT is required")
endif()

get_filename_component(RDD2_TEST_EFMI_ROOT "${RDD2_TEST_EFMI_ROOT}" REALPATH)
set(_navigation_production_dir
  "${RDD2_TEST_EFMI_ROOT}/Vehicles_Rdd2_NavigationEstimator/ProductionCode"
)
set(_navigation_source
  "${_navigation_production_dir}/Vehicles_Rdd2_NavigationEstimator.c"
)
set(_navigation_header
  "${_navigation_production_dir}/Vehicles_Rdd2_NavigationEstimator.h"
)
foreach(_generated_file IN ITEMS "${_navigation_source}" "${_navigation_header}")
  if(NOT EXISTS "${_generated_file}" OR IS_DIRECTORY "${_generated_file}")
    message(FATAL_ERROR "Missing staged generated estimator file: ${_generated_file}")
  endif()
endforeach()

include("${CMAKE_CURRENT_LIST_DIR}/../rdd2_test_environment.cmake")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(generated_navigation_fault_injection)

target_include_directories(app PRIVATE
  "${RDD2_TEST_APP_ROOT}/src"
  "${RDD2_TEST_APP_ROOT}/src/processes"
  "${RDD2_ZROS_ROOT}/include"
  "${_navigation_production_dir}"
)
target_sources(app PRIVATE
  src/log_module.c
  src/main.c
  "${RDD2_TEST_APP_ROOT}/src/processes/navigation_gps.c"
  "${_navigation_source}"
)
target_compile_definitions(app PRIVATE CONFIG_RDD2_LOCKSTEP=1)
