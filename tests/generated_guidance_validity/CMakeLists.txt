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
set(_guidance_production_dir
  "${RDD2_TEST_EFMI_ROOT}/Vehicles_Rdd2_GuidanceController/ProductionCode"
)
set(_guidance_source
  "${_guidance_production_dir}/Vehicles_Rdd2_GuidanceController.c"
)
set(_guidance_header
  "${_guidance_production_dir}/Vehicles_Rdd2_GuidanceController.h"
)
set(_kernel_source "${_guidance_production_dir}/rumoca_galec_kernels.c")
set(_kernel_header "${_guidance_production_dir}/rumoca_galec_kernels.h")
foreach(_generated_file IN ITEMS
    "${_guidance_source}"
    "${_guidance_header}"
    "${_kernel_source}"
    "${_kernel_header}"
)
  if(NOT EXISTS "${_generated_file}" OR IS_DIRECTORY "${_generated_file}")
    message(FATAL_ERROR "Missing staged generated Guidance file: ${_generated_file}")
  endif()
endforeach()

include("${CMAKE_CURRENT_LIST_DIR}/../rdd2_test_environment.cmake")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(generated_guidance_validity)

target_include_directories(app PRIVATE "${_guidance_production_dir}")
target_sources(app PRIVATE
  src/main.c
  "${_guidance_source}"
  "${_kernel_source}"
)
