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

if(TARGET ..__modules__lib__zenoh-pico__zephyr)
  target_compile_options(..__modules__lib__zenoh-pico__zephyr PRIVATE
    -Wno-unused-function
    -Wno-unused-variable
  )
endif()

target_include_directories(app PRIVATE
  include
  ${CMAKE_CURRENT_BINARY_DIR}/generated
  ${ZEPHYR_BASE}/../modules/hal/nxp/mcux/mcux-sdk-ng/drivers/flexio
)

add_subdirectory(src)
add_subdirectory(subsys)
add_subdirectory(drivers)
