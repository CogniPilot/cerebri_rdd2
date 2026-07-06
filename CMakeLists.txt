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
  ${WORKSPACE_ROOT}/modules/lib/zros
  ${WORKSPACE_ROOT}/modules/lib/zephyr_boards
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(rdd2 LANGUAGES C)

include(FetchContent)
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

FetchContent_Declare(synapse_fbs_c
  URL https://github.com/CogniPilot/synapse_fbs/releases/download/v0.1.5/synapse_fbs-c.tar.gz
  URL_HASH SHA256=f03232831b24527785da1320b731aadabe08ae4615b8a917d4f5a2305c4f8440
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(synapse_fbs_c)

set_property(TARGET synapse_fbs_c APPEND PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
  ${synapse_fbs_c_SOURCE_DIR}/include
)

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

add_dependencies(app rdd2_efmi_control_codegen)
add_dependencies(app rdd2_flatbuffer_schema_stage)
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
  ${RDD2_EFMI_CONTROL_INCLUDE_DIRS}
  ${ZEPHYR_BASE}/../modules/hal/nxp/mcux/mcux-sdk-ng/drivers/flexio
)

add_subdirectory(src)
add_subdirectory(subsys)
add_subdirectory(drivers)
