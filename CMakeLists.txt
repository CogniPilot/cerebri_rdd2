cmake_minimum_required(VERSION 3.20.0)

# clangd, Neovim, and VS Code consume Zephyr's exact generated include paths
# and compile definitions from this database.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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

function(rdd2_cache_path variable fallback description)
  if(DEFINED ENV{${variable}})
    set(${variable} "$ENV{${variable}}" CACHE PATH "${description}" FORCE)
  else()
    set(${variable} "${fallback}" CACHE PATH "${description}")
  endif()
endfunction()

rdd2_cache_path(RDD2_CEREBRI_MODULES_ROOT
  "${WORKSPACE_ROOT}/modules/lib/cerebri_lockstep"
  "Cerebri Zephyr module source"
)
rdd2_cache_path(RDD2_ZROS_ROOT
  "${WORKSPACE_ROOT}/modules/lib/zros"
  "ZROS Zephyr module source"
)
rdd2_cache_path(RDD2_CSYN_ROOT
  "${WORKSPACE_ROOT}/modules/lib/csyn"
  "CSyn Zephyr module source"
)
rdd2_cache_path(RDD2_MODELICA_MODELS_ROOT
  "${WORKSPACE_ROOT}/models/modelica_models"
  "Shared CogniPilot Modelica model library"
)

set(ZEPHYR_EXTRA_MODULES
  ${RDD2_CEREBRI_MODULES_ROOT}
  ${RDD2_ZROS_ROOT}
  ${RDD2_CSYN_ROOT}
)

# Zephyr main resolves snippets only from SNIPPET_ROOT and no longer implies
# the application directory, so this repo's own snippets/ has to be named for
# `-S mocap-gnss` to resolve.
list(APPEND SNIPPET_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(rdd2 LANGUAGES C)

if(TARGET native_simulator)
  target_sources(native_simulator INTERFACE subsys/lockstep/lockstep_host.c)
endif()

set(FETCHCONTENT_BASE_DIR ${CMAKE_CURRENT_BINARY_DIR}/_deps)

set(RDD2_RUMOCA_LOCK_FILE "${CMAKE_CURRENT_LIST_DIR}/cmake/RumocaLock.cmake")
include(${RDD2_RUMOCA_LOCK_FILE})
if(DEFINED ENV{RDD2_RUMOCA_EXECUTABLE} AND
   NOT "$ENV{RDD2_RUMOCA_EXECUTABLE}" STREQUAL "")
  set(RDD2_RUMOCA_EXECUTABLE "$ENV{RDD2_RUMOCA_EXECUTABLE}" CACHE FILEPATH
    "Existing Rumoca executable; skips the pinned binary download when set" FORCE
  )
else()
  set(RDD2_RUMOCA_EXECUTABLE "" CACHE FILEPATH
    "Existing Rumoca executable; skips the pinned binary download when set"
  )
endif()
if(DEFINED ENV{RDD2_RUMOCA_LIBRARY_PATH} AND
   NOT "$ENV{RDD2_RUMOCA_LIBRARY_PATH}" STREQUAL "")
  set(RDD2_RUMOCA_LIBRARY_PATH "$ENV{RDD2_RUMOCA_LIBRARY_PATH}" CACHE PATH
    "Optional runtime library path for a locally built Rumoca executable" FORCE
  )
else()
  set(RDD2_RUMOCA_LIBRARY_PATH "" CACHE PATH
    "Optional runtime library path for a locally built Rumoca executable"
  )
endif()

if(RDD2_RUMOCA_EXECUTABLE)
  if(NOT EXISTS "${RDD2_RUMOCA_EXECUTABLE}")
    message(FATAL_ERROR
      "RDD2_RUMOCA_EXECUTABLE does not exist: ${RDD2_RUMOCA_EXECUTABLE}"
    )
  endif()
  set(_rdd2_rumoca_version_command ${RDD2_RUMOCA_EXECUTABLE} --version)
  if(RDD2_RUMOCA_LIBRARY_PATH)
    list(PREPEND _rdd2_rumoca_version_command
      ${CMAKE_COMMAND} -E env
      "LD_LIBRARY_PATH=${RDD2_RUMOCA_LIBRARY_PATH}"
    )
  endif()
  execute_process(
    COMMAND ${_rdd2_rumoca_version_command}
    OUTPUT_VARIABLE _rdd2_rumoca_version_output
    ERROR_VARIABLE _rdd2_rumoca_version_error
    RESULT_VARIABLE _rdd2_rumoca_version_result
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE
  )
  string(REGEX REPLACE "^v" "" _rdd2_rumoca_version_number "${RDD2_RUMOCA_VERSION}")
  if(NOT _rdd2_rumoca_version_result EQUAL 0 OR
     NOT _rdd2_rumoca_version_output STREQUAL "rumoca ${_rdd2_rumoca_version_number}")
    message(FATAL_ERROR
      "RDD2_RUMOCA_EXECUTABLE does not match ${RDD2_RUMOCA_LOCK_FILE}: "
      "expected rumoca ${_rdd2_rumoca_version_number}, "
      "got '${_rdd2_rumoca_version_output}' (${_rdd2_rumoca_version_error})"
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
            -DRDD2_RUMOCA_LOCK_FILE=${RDD2_RUMOCA_LOCK_FILE}
            -DRDD2_RUMOCA_BIN_DIR=${RDD2_RUMOCA_BIN_DIR}
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

file(GLOB_RECURSE RDD2_MODELICA_SOURCES CONFIGURE_DEPENDS
  "${RDD2_MODELICA_MODELS_ROOT}/*.mo"
)

function(rdd2_add_efmi_control_model model_file model_name generated_name)
  set(_model_file ${RDD2_MODELICA_MODELS_ROOT}/${model_file})
  set(_efmu_dir ${RDD2_RUMOCA_GENERATED_DIR}/${generated_name})
  set(_efmu_file ${RDD2_RUMOCA_GENERATED_DIR}/${generated_name}.efmu)
  set(_stamp ${RDD2_RUMOCA_GENERATED_DIR}/${generated_name}.galec-production.stamp)
  set(_source ${_efmu_dir}/ProductionCode/${generated_name}.c)
  set(_header ${_efmu_dir}/ProductionCode/${generated_name}.h)
  set(_rumoca_command ${RDD2_RUMOCA_EXECUTABLE})
  if(RDD2_RUMOCA_LIBRARY_PATH)
    list(PREPEND _rumoca_command
      ${CMAKE_COMMAND} -E env
      "LD_LIBRARY_PATH=${RDD2_RUMOCA_LIBRARY_PATH}"
    )
  endif()

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
    COMMAND ${_rumoca_command}
            --cache-dir ${CMAKE_CURRENT_BINARY_DIR}/.rumoca-cache
            compile ${_model_file}
            --model ${model_name}
            --source-root ${RDD2_MODELICA_MODELS_ROOT}
            --target galec-production
            --output ${RDD2_RUMOCA_GENERATED_DIR}
    COMMAND ${CMAKE_COMMAND} -E touch ${_stamp}
    DEPENDS
      ${RDD2_MODELICA_SOURCES}
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

rdd2_add_efmi_control_model(
  Planning/Bezier/WaypointTrajectoryPlanner.mo
  Planning.Bezier.WaypointTrajectoryPlanner
  Planning_Bezier_WaypointTrajectoryPlanner
)

rdd2_add_efmi_control_model(
  Vehicles/Rdd2/GuidanceController.mo
  Vehicles.Rdd2.GuidanceController
  Vehicles_Rdd2_GuidanceController
)

rdd2_add_efmi_control_model(
  Vehicles/Rdd2/RateControlAllocator.mo
  Vehicles.Rdd2.RateControlAllocator
  Vehicles_Rdd2_RateControlAllocator
)

rdd2_add_efmi_control_model(
  Vehicles/Rdd2/NavigationEstimator.mo
  Vehicles.Rdd2.NavigationEstimator
  Vehicles_Rdd2_NavigationEstimator
)

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
