cmake_minimum_required(VERSION 3.20.0)

list(APPEND DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

get_filename_component(WORKSPACE_ROOT ${CMAKE_CURRENT_SOURCE_DIR} DIRECTORY)
set(ZEPHYR_MODULES
  ${WORKSPACE_ROOT}/modules/hal/cmsis
  ${WORKSPACE_ROOT}/modules/hal/cmsis_6
  ${WORKSPACE_ROOT}/modules/hal/nxp
  ${WORKSPACE_ROOT}/modules/fs/fatfs
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
  URL https://github.com/CogniPilot/synapse_fbs/releases/download/v0.1.3/synapse_fbs-c.tar.gz
  URL_HASH SHA256=7dd5d389154750290a2497609a0a69bbba0071d3d7bc576529c73ee7f2f38103
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(synapse_fbs_c)

set(RDD2_FLATBUFFER_SCHEMA_DIR
  ${synapse_fbs_c_SOURCE_DIR}/fbs
)
set(RDD2_FLATBUFFER_BFBS_DIR
  ${synapse_fbs_c_SOURCE_DIR}/bfbs
)
set(RDD2_FLATBUFFER_SCHEMA_SHA256
  ${synapse_fbs_c_SOURCE_DIR}/schema.sha256
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
set(RDD2_FLATBUFFER_MOCAP_SCHEMA
  ${RDD2_FLATBUFFER_SCHEMA_DIR}/synapse_mocap.fbs
)
set(RDD2_FLATBUFFER_TOPIC_BFBS
  ${RDD2_FLATBUFFER_BFBS_DIR}/synapse_topics.bfbs
)
set(RDD2_FLATBUFFER_SIL_BFBS
  ${RDD2_FLATBUFFER_BFBS_DIR}/synapse_sil.bfbs
)
set(RDD2_FLATBUFFER_LOG_BFBS
  ${RDD2_FLATBUFFER_BFBS_DIR}/synapse_log.bfbs
)
set(RDD2_FLATBUFFER_MOCAP_BFBS
  ${RDD2_FLATBUFFER_BFBS_DIR}/synapse_mocap.bfbs
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

if(NOT EXISTS ${RDD2_FLATBUFFER_MOCAP_SCHEMA})
  message(FATAL_ERROR
    "Missing FlatBuffer mocap schema in synapse_fbs release: ${RDD2_FLATBUFFER_MOCAP_SCHEMA}."
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

if(NOT EXISTS ${RDD2_FLATBUFFER_LOG_BFBS})
  message(FATAL_ERROR
    "Missing FlatBuffer log reflection schema in synapse_fbs release: ${RDD2_FLATBUFFER_LOG_BFBS}."
  )
endif()

if(NOT EXISTS ${RDD2_FLATBUFFER_MOCAP_BFBS})
  message(FATAL_ERROR
    "Missing FlatBuffer mocap reflection schema in synapse_fbs release: ${RDD2_FLATBUFFER_MOCAP_BFBS}."
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
          ${RDD2_FLATBUFFER_MOCAP_SCHEMA}
          ${RDD2_FLATBUFFER_GENERATED_DIR}/synapse_mocap.fbs
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${RDD2_FLATBUFFER_TOPIC_BFBS}
          ${RDD2_FLATBUFFER_GENERATED_DIR}/synapse_topics.bfbs
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${RDD2_FLATBUFFER_LOG_BFBS}
          ${RDD2_FLATBUFFER_GENERATED_DIR}/synapse_log.bfbs
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${RDD2_FLATBUFFER_SIL_BFBS}
          ${RDD2_FLATBUFFER_GENERATED_DIR}/synapse_sil.bfbs
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${RDD2_FLATBUFFER_MOCAP_BFBS}
          ${RDD2_FLATBUFFER_GENERATED_DIR}/synapse_mocap.bfbs
  COMMAND ${CMAKE_COMMAND} -E touch ${RDD2_FLATBUFFER_STAGE_STAMP}
  DEPENDS
    ${RDD2_FLATBUFFER_SCHEMA}
    ${RDD2_FLATBUFFER_LOG_SCHEMA}
    ${RDD2_FLATBUFFER_SIL_SCHEMA}
    ${RDD2_FLATBUFFER_MOCAP_SCHEMA}
    ${RDD2_FLATBUFFER_TOPIC_BFBS}
    ${RDD2_FLATBUFFER_LOG_BFBS}
    ${RDD2_FLATBUFFER_SIL_BFBS}
    ${RDD2_FLATBUFFER_MOCAP_BFBS}
  COMMENT "Staging FlatBuffer schemas into ${RDD2_FLATBUFFER_GENERATED_DIR}"
  VERBATIM
)

add_custom_target(rdd2_flatbuffer_schema_stage
  DEPENDS ${RDD2_FLATBUFFER_STAGE_STAMP}
)

add_dependencies(app rdd2_flatbuffer_schema_stage)
target_link_libraries(app PRIVATE synapse_fbs::c)

set(RDD2_LOG_SCHEMA_BLOBS_C ${CMAKE_CURRENT_BINARY_DIR}/generated/rdd2_log_schema_blobs.c)
set(RDD2_LOG_SCHEMA_BLOBS_H ${CMAKE_CURRENT_BINARY_DIR}/generated/rdd2_log_schema_blobs.h)

function(rdd2_schema_sha256 output_var schema_relpath)
  file(STRINGS ${RDD2_FLATBUFFER_SCHEMA_SHA256} _schema_hash_lines REGEX "  ${schema_relpath}$")
  list(LENGTH _schema_hash_lines _schema_hash_line_count)
  if(NOT _schema_hash_line_count EQUAL 1)
    message(FATAL_ERROR "Could not find sha256 for ${schema_relpath} in ${RDD2_FLATBUFFER_SCHEMA_SHA256}")
  endif()
  list(GET _schema_hash_lines 0 _schema_hash_line)
  string(REGEX REPLACE "^([0-9a-f]+)  .*$" "\\1" _schema_hash "${_schema_hash_line}")
  set(${output_var} "${_schema_hash}" PARENT_SCOPE)
endfunction()

function(rdd2_append_embedded_file output_file symbol input_file)
  file(READ ${input_file} _hex HEX)
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _bytes "${_hex}")
  file(APPEND ${output_file}
    "const uint8_t ${symbol}[] = {${_bytes}};\n"
    "const size_t ${symbol}_len = sizeof(${symbol});\n\n"
  )
endfunction()

rdd2_schema_sha256(RDD2_LOG_SCHEMA_HASH "fbs/synapse_log.fbs")
rdd2_schema_sha256(RDD2_TOPIC_SCHEMA_HASH "fbs/synapse_topics.fbs")
rdd2_schema_sha256(RDD2_MOCAP_SCHEMA_HASH "fbs/synapse_mocap.fbs")

file(SIZE ${RDD2_FLATBUFFER_LOG_SCHEMA} RDD2_LOG_SCHEMA_FBS_SIZE)
file(SIZE ${RDD2_FLATBUFFER_LOG_BFBS} RDD2_LOG_SCHEMA_BFBS_SIZE)
file(SIZE ${RDD2_FLATBUFFER_SCHEMA} RDD2_TOPIC_SCHEMA_FBS_SIZE)
file(SIZE ${RDD2_FLATBUFFER_TOPIC_BFBS} RDD2_TOPIC_SCHEMA_BFBS_SIZE)
file(SIZE ${RDD2_FLATBUFFER_MOCAP_SCHEMA} RDD2_MOCAP_SCHEMA_FBS_SIZE)
file(SIZE ${RDD2_FLATBUFFER_MOCAP_BFBS} RDD2_MOCAP_SCHEMA_BFBS_SIZE)

file(WRITE ${RDD2_LOG_SCHEMA_BLOBS_H}
  "#ifndef RDD2_LOG_SCHEMA_BLOBS_H_\n"
  "#define RDD2_LOG_SCHEMA_BLOBS_H_\n\n"
  "#include <stddef.h>\n"
  "#include <stdint.h>\n\n"
  "extern const uint8_t rdd2_schema_synapse_log_fbs[];\n"
  "extern const size_t rdd2_schema_synapse_log_fbs_len;\n"
  "extern const uint8_t rdd2_schema_synapse_log_bfbs[];\n"
  "extern const size_t rdd2_schema_synapse_log_bfbs_len;\n"
  "extern const uint8_t rdd2_schema_synapse_topics_fbs[];\n"
  "extern const size_t rdd2_schema_synapse_topics_fbs_len;\n"
  "extern const uint8_t rdd2_schema_synapse_topics_bfbs[];\n"
  "extern const size_t rdd2_schema_synapse_topics_bfbs_len;\n"
  "extern const uint8_t rdd2_schema_synapse_mocap_fbs[];\n"
  "extern const size_t rdd2_schema_synapse_mocap_fbs_len;\n"
  "extern const uint8_t rdd2_schema_synapse_mocap_bfbs[];\n"
  "extern const size_t rdd2_schema_synapse_mocap_bfbs_len;\n\n"
  "#define RDD2_SCHEMA_SYNAPSE_LOG_FBS_LEN ${RDD2_LOG_SCHEMA_FBS_SIZE}U\n"
  "#define RDD2_SCHEMA_SYNAPSE_LOG_BFBS_LEN ${RDD2_LOG_SCHEMA_BFBS_SIZE}U\n"
  "#define RDD2_SCHEMA_SYNAPSE_TOPICS_FBS_LEN ${RDD2_TOPIC_SCHEMA_FBS_SIZE}U\n"
  "#define RDD2_SCHEMA_SYNAPSE_TOPICS_BFBS_LEN ${RDD2_TOPIC_SCHEMA_BFBS_SIZE}U\n"
  "#define RDD2_SCHEMA_SYNAPSE_MOCAP_FBS_LEN ${RDD2_MOCAP_SCHEMA_FBS_SIZE}U\n"
  "#define RDD2_SCHEMA_SYNAPSE_MOCAP_BFBS_LEN ${RDD2_MOCAP_SCHEMA_BFBS_SIZE}U\n\n"
  "#define RDD2_SCHEMA_SYNAPSE_LOG_FBS_SHA256 \"${RDD2_LOG_SCHEMA_HASH}\"\n"
  "#define RDD2_SCHEMA_SYNAPSE_TOPICS_FBS_SHA256 \"${RDD2_TOPIC_SCHEMA_HASH}\"\n"
  "#define RDD2_SCHEMA_SYNAPSE_MOCAP_FBS_SHA256 \"${RDD2_MOCAP_SCHEMA_HASH}\"\n\n"
  "#endif\n"
)

file(WRITE ${RDD2_LOG_SCHEMA_BLOBS_C}
  "#include \"rdd2_log_schema_blobs.h\"\n\n"
)
rdd2_append_embedded_file(${RDD2_LOG_SCHEMA_BLOBS_C} rdd2_schema_synapse_log_fbs ${RDD2_FLATBUFFER_LOG_SCHEMA})
rdd2_append_embedded_file(${RDD2_LOG_SCHEMA_BLOBS_C} rdd2_schema_synapse_log_bfbs ${RDD2_FLATBUFFER_LOG_BFBS})
rdd2_append_embedded_file(${RDD2_LOG_SCHEMA_BLOBS_C} rdd2_schema_synapse_topics_fbs ${RDD2_FLATBUFFER_SCHEMA})
rdd2_append_embedded_file(${RDD2_LOG_SCHEMA_BLOBS_C} rdd2_schema_synapse_topics_bfbs ${RDD2_FLATBUFFER_TOPIC_BFBS})
rdd2_append_embedded_file(${RDD2_LOG_SCHEMA_BLOBS_C} rdd2_schema_synapse_mocap_fbs ${RDD2_FLATBUFFER_MOCAP_SCHEMA})
rdd2_append_embedded_file(${RDD2_LOG_SCHEMA_BLOBS_C} rdd2_schema_synapse_mocap_bfbs ${RDD2_FLATBUFFER_MOCAP_BFBS})

target_sources_ifdef(CONFIG_RDD2_LOG_SDCARD app PRIVATE
  ${RDD2_LOG_SCHEMA_BLOBS_C}
  ${synapse_fbs_c_SOURCE_DIR}/src/flatcc-runtime/builder.c
  ${synapse_fbs_c_SOURCE_DIR}/src/flatcc-runtime/emitter.c
  ${synapse_fbs_c_SOURCE_DIR}/src/flatcc-runtime/json_parser.c
  ${synapse_fbs_c_SOURCE_DIR}/src/flatcc-runtime/json_printer.c
  ${synapse_fbs_c_SOURCE_DIR}/src/flatcc-runtime/refmap.c
  ${synapse_fbs_c_SOURCE_DIR}/src/flatcc-runtime/verifier.c
)

target_include_directories(app PRIVATE
  include
  ${CMAKE_CURRENT_BINARY_DIR}/generated
  ${ZEPHYR_BASE}/../modules/hal/nxp/mcux/mcux-sdk-ng/drivers/flexio
)

add_subdirectory(src)
add_subdirectory(subsys)
add_subdirectory(drivers)
