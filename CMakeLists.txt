cmake_minimum_required(VERSION 3.20.0)

list(APPEND DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

get_filename_component(WORKSPACE_ROOT ${CMAKE_CURRENT_SOURCE_DIR} DIRECTORY)
set(ZEPHYR_MODULES
  ${WORKSPACE_ROOT}/modules/hal/cmsis
  ${WORKSPACE_ROOT}/modules/hal/cmsis_6
  ${WORKSPACE_ROOT}/modules/hal/nxp
  ${WORKSPACE_ROOT}/modules/lib/cmsis-dsp
  ${WORKSPACE_ROOT}/modules/lib/zenoh-pico
  ${WORKSPACE_ROOT}/modules/lib/zros
  ${WORKSPACE_ROOT}/modules/lib/zephyr_boards
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(rdd2 LANGUAGES C)

include(FetchContent)
set(FETCHCONTENT_BASE_DIR ${CMAKE_CURRENT_BINARY_DIR}/_deps)

FetchContent_Declare(synapse_fbs_c
  URL https://github.com/CogniPilot/synapse_fbs/releases/download/v0.1.2/synapse_fbs-c.tar.gz
  URL_HASH SHA256=be2cd865e82838d74aac0ca41b97971210ed55ecf012e76880128a6ad074e089
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(synapse_fbs_c)

set(RDD2_FLATBUFFER_SCHEMA_DIR
  ${synapse_fbs_c_SOURCE_DIR}/fbs
)
set(RDD2_FLATBUFFER_BFBS_DIR
  ${synapse_fbs_c_SOURCE_DIR}/bfbs
)
set(RDD2_FLATBUFFER_SCHEMA
  ${RDD2_FLATBUFFER_SCHEMA_DIR}/synapse_topics.fbs
)
set(RDD2_FLATBUFFER_LOG_SCHEMA
  ${RDD2_FLATBUFFER_SCHEMA_DIR}/synapse_log.fbs
)
set(RDD2_FLATBUFFER_SIL_SCHEMA
  ${RDD2_FLATBUFFER_SCHEMA_DIR}/synapse_sil.fbs
)
set(RDD2_FLATBUFFER_TOPIC_BFBS
  ${RDD2_FLATBUFFER_BFBS_DIR}/synapse_topics.bfbs
)
set(RDD2_FLATBUFFER_SIL_BFBS
  ${RDD2_FLATBUFFER_BFBS_DIR}/synapse_sil.bfbs
)
set(RDD2_FLATBUFFER_GENERATED_DIR
  ${CMAKE_CURRENT_BINARY_DIR}/generated/flatbuffers
)
set(RDD2_FLATBUFFER_STAGE_STAMP
  ${RDD2_FLATBUFFER_GENERATED_DIR}/.schema-copy.stamp
)

if(NOT EXISTS ${RDD2_FLATBUFFER_SCHEMA})
  message(FATAL_ERROR
    "Missing FlatBuffer topic schema in synapse_fbs release: ${RDD2_FLATBUFFER_SCHEMA}."
  )
endif()

if(NOT EXISTS ${RDD2_FLATBUFFER_LOG_SCHEMA})
  message(FATAL_ERROR
    "Missing FlatBuffer log schema in synapse_fbs release: ${RDD2_FLATBUFFER_LOG_SCHEMA}."
  )
endif()

if(NOT EXISTS ${RDD2_FLATBUFFER_SIL_SCHEMA})
  message(FATAL_ERROR
    "Missing FlatBuffer SITL schema in synapse_fbs release: ${RDD2_FLATBUFFER_SIL_SCHEMA}."
  )
endif()

if(NOT EXISTS ${RDD2_FLATBUFFER_TOPIC_BFBS})
  message(FATAL_ERROR
    "Missing FlatBuffer topic reflection schema in synapse_fbs release: ${RDD2_FLATBUFFER_TOPIC_BFBS}."
  )
endif()

if(NOT EXISTS ${RDD2_FLATBUFFER_SIL_BFBS})
  message(FATAL_ERROR
    "Missing FlatBuffer SITL reflection schema in synapse_fbs release: ${RDD2_FLATBUFFER_SIL_BFBS}."
  )
endif()

add_custom_command(
  OUTPUT ${RDD2_FLATBUFFER_STAGE_STAMP}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${RDD2_FLATBUFFER_GENERATED_DIR}
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${RDD2_FLATBUFFER_SCHEMA}
          ${RDD2_FLATBUFFER_GENERATED_DIR}/synapse_topics.fbs
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${RDD2_FLATBUFFER_LOG_SCHEMA}
          ${RDD2_FLATBUFFER_GENERATED_DIR}/synapse_log.fbs
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${RDD2_FLATBUFFER_SIL_SCHEMA}
          ${RDD2_FLATBUFFER_GENERATED_DIR}/synapse_sil.fbs
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${RDD2_FLATBUFFER_TOPIC_BFBS}
          ${RDD2_FLATBUFFER_GENERATED_DIR}/synapse_topics.bfbs
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${RDD2_FLATBUFFER_SIL_BFBS}
          ${RDD2_FLATBUFFER_GENERATED_DIR}/synapse_sil.bfbs
  COMMAND ${CMAKE_COMMAND} -E touch ${RDD2_FLATBUFFER_STAGE_STAMP}
  DEPENDS
    ${RDD2_FLATBUFFER_SCHEMA}
    ${RDD2_FLATBUFFER_LOG_SCHEMA}
    ${RDD2_FLATBUFFER_SIL_SCHEMA}
    ${RDD2_FLATBUFFER_TOPIC_BFBS}
    ${RDD2_FLATBUFFER_SIL_BFBS}
  COMMENT "Staging FlatBuffer schemas into ${RDD2_FLATBUFFER_GENERATED_DIR}"
  VERBATIM
)

add_custom_target(rdd2_flatbuffer_schema_stage
  DEPENDS ${RDD2_FLATBUFFER_STAGE_STAMP}
)

add_dependencies(app rdd2_flatbuffer_schema_stage)
target_link_libraries(app PRIVATE synapse_fbs::c)

target_include_directories(app PRIVATE
  include
  ${ZEPHYR_BASE}/../modules/hal/nxp/mcux/mcux-sdk-ng/drivers/flexio
)

add_subdirectory(src)
add_subdirectory(subsys)
add_subdirectory(drivers)
