cmake_minimum_required(VERSION 3.20.0)

list(APPEND DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

set(RDD2_WORKSPACE_ROOT "" CACHE PATH "West workspace root containing Zephyr and module checkouts")
if(NOT RDD2_WORKSPACE_ROOT AND DEFINED ENV{RDD2_WORKSPACE_ROOT})
  set(RDD2_WORKSPACE_ROOT "$ENV{RDD2_WORKSPACE_ROOT}")
endif()

if(RDD2_WORKSPACE_ROOT)
  get_filename_component(WORKSPACE_ROOT ${RDD2_WORKSPACE_ROOT} ABSOLUTE)
else()
  get_filename_component(WORKSPACE_ROOT ${CMAKE_CURRENT_SOURCE_DIR} DIRECTORY)
endif()

set(ZEPHYR_MODULES
  ${WORKSPACE_ROOT}/modules/hal/cmsis
  ${WORKSPACE_ROOT}/modules/hal/cmsis_6
  ${WORKSPACE_ROOT}/modules/hal/nxp
  ${WORKSPACE_ROOT}/modules/fs/fatfs
  ${WORKSPACE_ROOT}/modules/lib/cmsis-dsp
  ${WORKSPACE_ROOT}/modules/lib/zenoh-pico
  ${WORKSPACE_ROOT}/modules/lib/cerebri_lockstep
  ${WORKSPACE_ROOT}/modules/lib/zros
  ${WORKSPACE_ROOT}/modules/lib/csyn
  ${WORKSPACE_ROOT}/modules/lib/zephyr_boards
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(rdd2 LANGUAGES C)

if(TARGET native_simulator)
  target_sources(native_simulator INTERFACE subsys/lockstep/lockstep_host.c)
endif()

set(FETCHCONTENT_BASE_DIR ${CMAKE_CURRENT_BINARY_DIR}/_deps)

set(RDD2_RUMOCA_VERSION "v0.9.11"
  CACHE STRING "Pinned Rumoca version used for eFMI control code generation"
)
set(RDD2_RUMOCA_INSTALL_SCRIPT_SHA256
  "ac15aa5ec90cb974387610fae7594d2704f2a3bd2e080cfe2517e896d30eb161"
  CACHE STRING "SHA256 of the pinned Rumoca binary install script"
)
set(RDD2_RUMOCA_INSTALL_SCRIPT_URL
  "https://raw.githubusercontent.com/CogniPilot/rumoca/${RDD2_RUMOCA_VERSION}/infra/install/install.sh"
  CACHE STRING "Pinned Rumoca binary install script URL"
)
set(RDD2_RUMOCA_EXECUTABLE "" CACHE FILEPATH
  "Existing Rumoca executable; skips the pinned binary download when set"
)

if(RDD2_RUMOCA_EXECUTABLE)
  if(NOT EXISTS "${RDD2_RUMOCA_EXECUTABLE}")
    message(FATAL_ERROR
      "RDD2_RUMOCA_EXECUTABLE does not exist: ${RDD2_RUMOCA_EXECUTABLE}"
    )
  endif()
else()
  set(RDD2_RUMOCA_BIN_DIR
    ${CMAKE_CURRENT_BINARY_DIR}/tools/rumoca/${RDD2_RUMOCA_VERSION}
  )
  set(RDD2_RUMOCA_EXECUTABLE ${RDD2_RUMOCA_BIN_DIR}/rumoca)

  add_custom_command(
    OUTPUT ${RDD2_RUMOCA_EXECUTABLE}
    COMMAND ${CMAKE_COMMAND}
            -DRDD2_RUMOCA_VERSION=${RDD2_RUMOCA_VERSION}
            -DRDD2_RUMOCA_BIN_DIR=${RDD2_RUMOCA_BIN_DIR}
            -DRDD2_RUMOCA_INSTALL_SCRIPT_URL=${RDD2_RUMOCA_INSTALL_SCRIPT_URL}
            -DRDD2_RUMOCA_INSTALL_SCRIPT_SHA256=${RDD2_RUMOCA_INSTALL_SCRIPT_SHA256}
            -P ${CMAKE_CURRENT_LIST_DIR}/cmake/InstallRumoca.cmake
    COMMENT "Installing pinned Rumoca ${RDD2_RUMOCA_VERSION}"
    VERBATIM
  )
endif()

add_custom_target(rdd2_rumoca_tool
  DEPENDS ${RDD2_RUMOCA_EXECUTABLE}
)

set(RDD2_RUMOCA_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/rumoca)
set(RDD2_EFMI_CONTROL_STAMPS)
set(RDD2_EFMI_CONTROL_SOURCES)
set(RDD2_EFMI_CONTROL_INCLUDE_DIRS)

function(rdd2_add_efmi_control_model model_name)
  set(_model_file ${CMAKE_CURRENT_SOURCE_DIR}/modelica/${model_name}.mo)
  set(_efmu_dir ${RDD2_RUMOCA_GENERATED_DIR}/${model_name})
  set(_efmu_file ${RDD2_RUMOCA_GENERATED_DIR}/${model_name}.efmu)
  set(_stamp ${RDD2_RUMOCA_GENERATED_DIR}/${model_name}.galec-production.stamp)
  set(_source ${_efmu_dir}/ProductionCode/${model_name}.c)
  set(_header ${_efmu_dir}/ProductionCode/${model_name}.h)

  add_custom_command(
    OUTPUT ${_stamp}
    BYPRODUCTS
      ${_efmu_file}
      ${_efmu_dir}/AlgorithmCode/${model_name}.alg
      ${_efmu_dir}/AlgorithmCode/manifest.xml
      ${_source}
      ${_header}
      ${_efmu_dir}/ProductionCode/manifest.xml
      ${_efmu_dir}/__content.xml
    COMMAND ${CMAKE_COMMAND} -E rm -rf
            ${_efmu_dir}
            ${_efmu_file}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${RDD2_RUMOCA_GENERATED_DIR}
    COMMAND ${RDD2_RUMOCA_EXECUTABLE}
            --cache-dir ${CMAKE_CURRENT_BINARY_DIR}/.rumoca-cache
            compile ${_model_file}
            --model ${model_name}
            --target galec-production
            --output ${RDD2_RUMOCA_GENERATED_DIR}
    COMMAND ${CMAKE_COMMAND} -E touch ${_stamp}
    DEPENDS
      ${_model_file}
      ${RDD2_RUMOCA_EXECUTABLE}
    COMMENT "Generating ${model_name} eFMI Production Code with Rumoca ${RDD2_RUMOCA_VERSION}"
    VERBATIM
  )

  list(APPEND RDD2_EFMI_CONTROL_STAMPS ${_stamp})
  list(APPEND RDD2_EFMI_CONTROL_SOURCES ${_source})
  list(APPEND RDD2_EFMI_CONTROL_INCLUDE_DIRS ${_efmu_dir}/ProductionCode)
  set(RDD2_EFMI_CONTROL_STAMPS ${RDD2_EFMI_CONTROL_STAMPS} PARENT_SCOPE)
  set(RDD2_EFMI_CONTROL_SOURCES ${RDD2_EFMI_CONTROL_SOURCES} PARENT_SCOPE)
  set(RDD2_EFMI_CONTROL_INCLUDE_DIRS ${RDD2_EFMI_CONTROL_INCLUDE_DIRS} PARENT_SCOPE)
endfunction()

rdd2_add_efmi_control_model(Quadrotor)

add_custom_target(rdd2_efmi_control_codegen
  DEPENDS ${RDD2_EFMI_CONTROL_STAMPS}
)

add_dependencies(app rdd2_efmi_control_codegen)
target_link_libraries(app PRIVATE synapse_fbs::c)

set_source_files_properties(${RDD2_EFMI_CONTROL_SOURCES}
  PROPERTIES GENERATED TRUE
)

target_sources(app PRIVATE
  ${RDD2_EFMI_CONTROL_SOURCES}
)

if(TARGET ..__modules__lib__zenoh-pico__zephyr)
  target_compile_options(..__modules__lib__zenoh-pico__zephyr PRIVATE
    -Wno-unused-function
    -Wno-unused-variable
  )
endif()

target_include_directories(app PRIVATE
  include
  ${CMAKE_CURRENT_BINARY_DIR}/generated
  ${RDD2_EFMI_CONTROL_INCLUDE_DIRS}
  ${ZEPHYR_BASE}/../modules/hal/nxp/mcux/mcux-sdk-ng/drivers/flexio
)

add_subdirectory(src)
add_subdirectory(subsys)
add_subdirectory(drivers)
