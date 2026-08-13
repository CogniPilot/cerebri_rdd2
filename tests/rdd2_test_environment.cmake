# SPDX-License-Identifier: Apache-2.0

foreach(_module_variable IN ITEMS RDD2_ZROS_ROOT RDD2_CSYN_ROOT)
  if(NOT DEFINED ENV{${_module_variable}} OR "$ENV{${_module_variable}}" STREQUAL "")
    message(FATAL_ERROR "${_module_variable} is required")
  endif()

  get_filename_component(${_module_variable}
    "$ENV{${_module_variable}}" REALPATH
  )
  if(NOT EXISTS "${${_module_variable}}/zephyr/module.yml")
    message(FATAL_ERROR
      "${_module_variable} is not a Zephyr module: ${${_module_variable}}"
    )
  endif()
endforeach()

get_filename_component(RDD2_TEST_APP_ROOT
  "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE
)
set(ZEPHYR_EXTRA_MODULES
  "${RDD2_ZROS_ROOT}"
  "${RDD2_CSYN_ROOT}"
)
