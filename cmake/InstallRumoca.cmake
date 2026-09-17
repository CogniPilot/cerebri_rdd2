cmake_minimum_required(VERSION 3.20)

foreach(_var
    RDD2_RUMOCA_LOCK_FILE
    RDD2_RUMOCA_BIN_DIR
)
  if(NOT DEFINED ${_var} OR "${${_var}}" STREQUAL "")
    message(FATAL_ERROR "${_var} must be set")
  endif()
endforeach()

if(NOT EXISTS "${RDD2_RUMOCA_LOCK_FILE}")
  message(FATAL_ERROR "Rumoca lock file does not exist: ${RDD2_RUMOCA_LOCK_FILE}")
endif()
include("${RDD2_RUMOCA_LOCK_FILE}")

string(REGEX REPLACE "^v" "" RDD2_RUMOCA_VERSION_NUMBER "${RDD2_RUMOCA_VERSION}")
set(_rumoca_executable "${RDD2_RUMOCA_BIN_DIR}/rumoca")

# A pinned source revision replaces the release binaries: cargo installs it into
# the per-user cache directory the caller names, outside any build directory, so
# a clean rebuild reuses it.
if(RDD2_RUMOCA_SOURCE_REV)
  if(NOT EXISTS "${_rumoca_executable}")
    find_program(CARGO_EXECUTABLE cargo
      HINTS "$ENV{CARGO_HOME}/bin" "$ENV{HOME}/.cargo/bin"
      REQUIRED
    )
    get_filename_component(_cargo_root "${RDD2_RUMOCA_BIN_DIR}" DIRECTORY)
    execute_process(
      COMMAND "${CARGO_EXECUTABLE}" install rumoca
              --git https://github.com/CogniPilot/rumoca
              --rev "${RDD2_RUMOCA_SOURCE_REV}"
              --locked
              --root "${_cargo_root}"
      RESULT_VARIABLE _cargo_result
    )
    if(NOT _cargo_result EQUAL 0)
      message(FATAL_ERROR
        "cargo install of Rumoca ${RDD2_RUMOCA_SOURCE_REV} failed (${_cargo_result})"
      )
    endif()
  endif()
else()

  if(RDD2_RUMOCA_INSTALL_SCRIPT_SHA256 STREQUAL "")
    message(FATAL_ERROR
      "Rumoca ${RDD2_RUMOCA_VERSION} has no published release to install; "
      "set RDD2_RUMOCA_EXECUTABLE to a Rumoca built at revision "
      "${RDD2_RUMOCA_REVISION} (the flake input does this) or keep "
      "RDD2_RUMOCA_SOURCE_REV in the lock so cargo builds it"
    )
  endif()

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
      set(_rumoca_expected_sha256 "${RDD2_RUMOCA_LINUX_X86_64_SHA256}")
    elseif(_host_system_processor MATCHES "^(aarch64|arm64|ARM64)$")
      set(_rumoca_expected_sha256 "${RDD2_RUMOCA_LINUX_AARCH64_SHA256}")
    endif()
  elseif(_host_system_name STREQUAL "Darwin")
    if(_host_system_processor MATCHES "^(x86_64|amd64|AMD64)$")
      set(_rumoca_expected_sha256 "${RDD2_RUMOCA_DARWIN_X86_64_SHA256}")
    elseif(_host_system_processor MATCHES "^(aarch64|arm64|ARM64)$")
      set(_rumoca_expected_sha256 "${RDD2_RUMOCA_DARWIN_AARCH64_SHA256}")
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

  file(SHA256 "${_rumoca_executable}" _rumoca_actual_sha256)
  if(NOT _rumoca_actual_sha256 STREQUAL _rumoca_expected_sha256)
    message(FATAL_ERROR
      "Rumoca binary hash mismatch for ${_rumoca_executable}: "
      "expected ${_rumoca_expected_sha256}, got ${_rumoca_actual_sha256}"
    )
  endif()

endif()

if(NOT EXISTS "${_rumoca_executable}")
  message(FATAL_ERROR "Rumoca install did not produce ${_rumoca_executable}")
endif()

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
if(NOT _rumoca_version_output STREQUAL "rumoca ${RDD2_RUMOCA_VERSION_NUMBER}")
  message(FATAL_ERROR
    "Unexpected Rumoca version: expected rumoca ${RDD2_RUMOCA_VERSION_NUMBER}, "
    "got ${_rumoca_version_output}"
  )
endif()
