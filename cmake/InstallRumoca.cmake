cmake_minimum_required(VERSION 3.20)

foreach(_var
    RDD2_RUMOCA_VERSION
    RDD2_RUMOCA_BIN_DIR
    RDD2_RUMOCA_INSTALL_SCRIPT_URL
    RDD2_RUMOCA_INSTALL_SCRIPT_SHA256)
  if(NOT DEFINED ${_var} OR "${${_var}}" STREQUAL "")
    message(FATAL_ERROR "${_var} must be set")
  endif()
endforeach()

if(CMAKE_HOST_WIN32)
  message(FATAL_ERROR "Rumoca install.sh is not supported on Windows hosts")
endif()

set(_host_system_name "${CMAKE_HOST_SYSTEM_NAME}")
set(_host_system_processor "${CMAKE_HOST_SYSTEM_PROCESSOR}")
if(_host_system_name STREQUAL "")
  execute_process(
    COMMAND uname -s
    OUTPUT_VARIABLE _host_system_name
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
endif()
if(_host_system_processor STREQUAL "")
  execute_process(
    COMMAND uname -m
    OUTPUT_VARIABLE _host_system_processor
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
endif()

set(_rumoca_expected_sha256 "")
if(_host_system_name STREQUAL "Linux")
  if(_host_system_processor MATCHES "^(x86_64|amd64|AMD64)$")
    set(_rumoca_expected_sha256 "1d93c0341f59630a38c86a1dcfd9ab72afea1cc65696556a9efbac9e7b10a1b0")
  elseif(_host_system_processor MATCHES "^(aarch64|arm64|ARM64)$")
    set(_rumoca_expected_sha256 "635dd577484e9f17ee3e200edc8f55e330ee3f1bcf034f9970a1c08192af711b")
  endif()
elseif(_host_system_name STREQUAL "Darwin")
  if(_host_system_processor MATCHES "^(x86_64|amd64|AMD64)$")
    set(_rumoca_expected_sha256 "16ceafc2ebf9b87ba73e02572ed39e136fdfb33c3545c6df23bd3ffb850323ed")
  elseif(_host_system_processor MATCHES "^(aarch64|arm64|ARM64)$")
    set(_rumoca_expected_sha256 "0555158fa1091d6a66179d4c2f88449049f4d1cb8cc006e6c994dd9dd49ff623")
  endif()
endif()

if(_rumoca_expected_sha256 STREQUAL "")
  message(FATAL_ERROR
    "Unsupported Rumoca host platform: ${_host_system_name}/${_host_system_processor}"
  )
endif()

find_program(BASH_EXECUTABLE bash REQUIRED)
find_program(CURL_EXECUTABLE curl REQUIRED)

file(MAKE_DIRECTORY "${RDD2_RUMOCA_BIN_DIR}")
set(_install_script "${RDD2_RUMOCA_BIN_DIR}/install-rumoca-${RDD2_RUMOCA_VERSION}.sh")
file(DOWNLOAD
  "${RDD2_RUMOCA_INSTALL_SCRIPT_URL}"
  "${_install_script}"
  EXPECTED_HASH "SHA256=${RDD2_RUMOCA_INSTALL_SCRIPT_SHA256}"
  TLS_VERIFY ON
  STATUS _download_status
)
list(GET _download_status 0 _download_code)
list(GET _download_status 1 _download_message)
if(NOT _download_code EQUAL 0)
  message(FATAL_ERROR
    "Failed to download Rumoca installer from ${RDD2_RUMOCA_INSTALL_SCRIPT_URL}: "
    "${_download_message}"
  )
endif()

execute_process(
  COMMAND "${BASH_EXECUTABLE}" "${_install_script}"
          --version "${RDD2_RUMOCA_VERSION}"
          --bin-dir "${RDD2_RUMOCA_BIN_DIR}"
  RESULT_VARIABLE _install_result
  OUTPUT_VARIABLE _install_output
  ERROR_VARIABLE _install_error
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_STRIP_TRAILING_WHITESPACE
)
if(NOT _install_result EQUAL 0)
  message(FATAL_ERROR
    "Rumoca ${RDD2_RUMOCA_VERSION} installer failed\n"
    "stdout:\n${_install_output}\n"
    "stderr:\n${_install_error}"
  )
endif()

set(_rumoca_executable "${RDD2_RUMOCA_BIN_DIR}/rumoca")
if(NOT EXISTS "${_rumoca_executable}")
  message(FATAL_ERROR "Rumoca installer did not produce ${_rumoca_executable}")
endif()

file(SHA256 "${_rumoca_executable}" _rumoca_actual_sha256)
if(NOT _rumoca_actual_sha256 STREQUAL _rumoca_expected_sha256)
  message(FATAL_ERROR
    "Rumoca binary hash mismatch for ${_rumoca_executable}: "
    "expected ${_rumoca_expected_sha256}, got ${_rumoca_actual_sha256}"
  )
endif()

string(REGEX REPLACE "^v" "" _rumoca_version_number "${RDD2_RUMOCA_VERSION}")
execute_process(
  COMMAND "${_rumoca_executable}" --version
  OUTPUT_VARIABLE _rumoca_version_output
  ERROR_VARIABLE _rumoca_version_error
  RESULT_VARIABLE _rumoca_version_result
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_STRIP_TRAILING_WHITESPACE
)
if(NOT _rumoca_version_result EQUAL 0)
  message(FATAL_ERROR "Could not run ${_rumoca_executable} --version: ${_rumoca_version_error}")
endif()
if(NOT _rumoca_version_output STREQUAL "rumoca ${_rumoca_version_number}")
  message(FATAL_ERROR
    "Unexpected Rumoca version: expected rumoca ${_rumoca_version_number}, "
    "got ${_rumoca_version_output}"
  )
endif()
