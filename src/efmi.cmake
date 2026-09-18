set(FETCHCONTENT_BASE_DIR ${PROJECT_BINARY_DIR}/_deps)

function(rdd2_verify_prebuilt_manifest)
  set(_manifest "${RDD2_PREBUILT_EFMI_DIR}/MANIFEST.sha256")
  if(NOT EXISTS "${_manifest}" OR IS_DIRECTORY "${_manifest}")
    message(FATAL_ERROR "Missing prebuilt eFMI manifest: ${_manifest}")
  endif()

  file(STRINGS "${_manifest}" _manifest_lines ENCODING UTF-8)
  if(NOT _manifest_lines)
    message(FATAL_ERROR "Prebuilt eFMI manifest is empty: ${_manifest}")
  endif()
  string(LENGTH "${RDD2_PREBUILT_EFMI_MANIFEST_SHA256}" _manifest_sha256_length)
  if(NOT _manifest_sha256_length EQUAL 64)
    message(FATAL_ERROR
      "RDD2_PREBUILT_EFMI_MANIFEST_SHA256 must contain the 64-digit receipt digest"
    )
  endif()
  file(SHA256 "${_manifest}" _actual_manifest_sha256)
  string(TOLOWER "${RDD2_PREBUILT_EFMI_MANIFEST_SHA256}" _expected_manifest_sha256)
  if(NOT _actual_manifest_sha256 STREQUAL _expected_manifest_sha256)
    message(FATAL_ERROR
      "Prebuilt eFMI manifest SHA-256 mismatch: expected "
      "${_expected_manifest_sha256}, got ${_actual_manifest_sha256}"
    )
  endif()

  set(_verified_files)
  set(_verified_sha256)
  foreach(_line IN LISTS _manifest_lines)
    if(NOT _line MATCHES "^([0-9A-Fa-f]+)[ \t]+\\*?(.+)$")
      message(FATAL_ERROR "Malformed prebuilt eFMI manifest entry: ${_line}")
    endif()
    set(_expected_sha256 "${CMAKE_MATCH_1}")
    set(_relative_path "${CMAKE_MATCH_2}")
    string(LENGTH "${_expected_sha256}" _sha256_length)
    if(NOT _sha256_length EQUAL 64)
      message(FATAL_ERROR "Invalid SHA-256 in prebuilt eFMI manifest: ${_line}")
    endif()
    string(REGEX REPLACE "^\\./" "" _relative_path "${_relative_path}")
    get_filename_component(_artifact
      "${RDD2_PREBUILT_EFMI_DIR}/${_relative_path}" REALPATH
    )
    string(FIND "${_artifact}" "${RDD2_PREBUILT_EFMI_DIR}/" _root_prefix)
    if(NOT _root_prefix EQUAL 0 OR
       NOT EXISTS "${_artifact}" OR IS_DIRECTORY "${_artifact}")
      message(FATAL_ERROR "Missing prebuilt eFMI manifest artifact: ${_artifact}")
    endif()

    file(SHA256 "${_artifact}" _actual_sha256)
    string(TOLOWER "${_expected_sha256}" _expected_sha256)
    if(NOT _actual_sha256 STREQUAL _expected_sha256)
      message(FATAL_ERROR
        "Prebuilt eFMI SHA-256 mismatch for ${_relative_path}: "
        "expected ${_expected_sha256}, got ${_actual_sha256}"
      )
    endif()
    list(APPEND _verified_files "${_artifact}")
    list(APPEND _verified_sha256 "${_actual_sha256}")
  endforeach()

  set(RDD2_PREBUILT_EFMI_VERIFIED_FILES "${_verified_files}" PARENT_SCOPE)
  set(RDD2_PREBUILT_EFMI_VERIFIED_SHA256 "${_verified_sha256}" PARENT_SCOPE)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_manifest}")
endfunction()

function(rdd2_verified_prebuilt_sha256 artifact output_variable)
  get_filename_component(_artifact "${artifact}" REALPATH)
  list(FIND RDD2_PREBUILT_EFMI_VERIFIED_FILES "${_artifact}" _verified_index)
  if(_verified_index EQUAL -1)
    message(FATAL_ERROR "Prebuilt eFMI artifact is not receipted: ${_artifact}")
  endif()
  list(GET RDD2_PREBUILT_EFMI_VERIFIED_SHA256 ${_verified_index} _sha256)
  set(${output_variable} "${_sha256}" PARENT_SCOPE)
endfunction()

if(RDD2_PREBUILT_EFMI_DIR)
  rdd2_verify_prebuilt_manifest()
else()
  set(RDD2_RUMOCA_LOCK_FILE "${CMAKE_CURRENT_LIST_DIR}/../cmake/RumocaLock.cmake")
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
      ${PROJECT_BINARY_DIR}/tools/rumoca/${RDD2_RUMOCA_VERSION}
    )
    set(RDD2_RUMOCA_EXECUTABLE ${RDD2_RUMOCA_BIN_DIR}/rumoca)

    add_custom_command(
      OUTPUT ${RDD2_RUMOCA_EXECUTABLE}
      COMMAND ${CMAKE_COMMAND}
              -DRDD2_RUMOCA_LOCK_FILE=${RDD2_RUMOCA_LOCK_FILE}
              -DRDD2_RUMOCA_BIN_DIR=${RDD2_RUMOCA_BIN_DIR}
              -P ${CMAKE_CURRENT_LIST_DIR}/../cmake/InstallRumoca.cmake
      COMMENT "Installing pinned Rumoca ${RDD2_RUMOCA_VERSION}"
      VERBATIM
    )
  endif()

  add_custom_target(rdd2_rumoca_tool
    DEPENDS ${RDD2_RUMOCA_EXECUTABLE}
  )
endif()

set(RDD2_RUMOCA_GENERATED_DIR ${PROJECT_BINARY_DIR}/generated/rumoca)
set(RDD2_EFMI_CONTROL_STAMPS)
set(RDD2_EFMI_CONTROL_SOURCES)
set(RDD2_EFMI_CONTROL_INCLUDE_DIRS)

if(NOT RDD2_PREBUILT_EFMI_DIR)
  file(GLOB_RECURSE RDD2_MODELICA_SOURCES CONFIGURE_DEPENDS
    "${RDD2_MODELICA_MODELS_ROOT}/*.mo"
  )
endif()

function(rdd2_add_efmi_control_model model_file model_name generated_name)
  set(_model_file ${RDD2_MODELICA_MODELS_ROOT}/${model_file})
  set(_efmu_dir ${RDD2_RUMOCA_GENERATED_DIR}/${generated_name})
  set(_efmu_file ${RDD2_RUMOCA_GENERATED_DIR}/${generated_name}.efmu)
  set(_stamp ${RDD2_RUMOCA_GENERATED_DIR}/${generated_name}.galec-production.stamp)
  set(_source ${_efmu_dir}/ProductionCode/${generated_name}.c)
  set(_header ${_efmu_dir}/ProductionCode/${generated_name}.h)
  set(_algorithm ${_efmu_dir}/AlgorithmCode/${generated_name}.alg)
  # Rumoca emits its whole-array kernels (copy, fill, dot, scaled add) as one
  # model-independent translation unit beside every block. Every block's copy
  # is identical for one compiler build, and each generated source checks the
  # kernel header's version at compile time, so the application compiles the
  # unit once (see the end of this file) rather than once per block.
  set(_kernels_source ${_efmu_dir}/ProductionCode/rumoca_galec_kernels.c)
  set(_kernels_header ${_efmu_dir}/ProductionCode/rumoca_galec_kernels.h)

  if(RDD2_PREBUILT_EFMI_DIR)
    set(_prebuilt_dir "${RDD2_PREBUILT_EFMI_DIR}/${generated_name}")
    if(NOT IS_DIRECTORY "${_prebuilt_dir}")
      file(GLOB _efmu_dir_candidates LIST_DIRECTORIES true
        "${RDD2_PREBUILT_EFMI_DIR}/*/${generated_name}"
      )
      list(LENGTH _efmu_dir_candidates _efmu_dir_candidate_count)
      if(NOT _efmu_dir_candidate_count EQUAL 1)
        message(FATAL_ERROR
          "Expected exactly one prebuilt ${generated_name} directory under "
          "${RDD2_PREBUILT_EFMI_DIR}; found ${_efmu_dir_candidate_count}"
        )
      endif()
      list(GET _efmu_dir_candidates 0 _prebuilt_dir)
    endif()

    set(_prebuilt_source ${_prebuilt_dir}/ProductionCode/${generated_name}.c)
    set(_prebuilt_header ${_prebuilt_dir}/ProductionCode/${generated_name}.h)
    set(_prebuilt_algorithm ${_prebuilt_dir}/AlgorithmCode/${generated_name}.alg)
    rdd2_verified_prebuilt_sha256("${_prebuilt_source}" _source_sha256)
    rdd2_verified_prebuilt_sha256("${_prebuilt_header}" _header_sha256)
    rdd2_verified_prebuilt_sha256("${_prebuilt_algorithm}" _algorithm_sha256)

    set(_prebuilt_kernels_source ${_prebuilt_dir}/ProductionCode/rumoca_galec_kernels.c)
    set(_prebuilt_kernels_header ${_prebuilt_dir}/ProductionCode/rumoca_galec_kernels.h)
    set(_prebuilt_kernel_byproducts)
    set(_prebuilt_kernel_commands)
    set(_prebuilt_kernel_depends)
    if(EXISTS "${_prebuilt_kernels_source}" AND EXISTS "${_prebuilt_kernels_header}")
      rdd2_verified_prebuilt_sha256("${_prebuilt_kernels_source}" _kernels_source_sha256)
      rdd2_verified_prebuilt_sha256("${_prebuilt_kernels_header}" _kernels_header_sha256)
      set(_prebuilt_kernel_byproducts ${_kernels_source} ${_kernels_header})
      set(_prebuilt_kernel_commands
        COMMAND ${CMAKE_COMMAND}
                -DINPUT=${_prebuilt_kernels_source}
                -DOUTPUT=${_kernels_source}
                -DEXPECTED_SHA256=${_kernels_source_sha256}
                -P ${CMAKE_CURRENT_LIST_DIR}/verify_prebuilt_efmi.cmake
        COMMAND ${CMAKE_COMMAND}
                -DINPUT=${_prebuilt_kernels_header}
                -DOUTPUT=${_kernels_header}
                -DEXPECTED_SHA256=${_kernels_header_sha256}
                -P ${CMAKE_CURRENT_LIST_DIR}/verify_prebuilt_efmi.cmake
      )
      set(_prebuilt_kernel_depends ${_prebuilt_kernels_source} ${_prebuilt_kernels_header})
      list(APPEND RDD2_EFMI_KERNEL_SOURCES ${_kernels_source})
    elseif(EXISTS "${_prebuilt_kernels_source}" OR EXISTS "${_prebuilt_kernels_header}")
      message(FATAL_ERROR
        "Prebuilt ${generated_name} must provide both rumoca kernel files or neither"
      )
    endif()

    add_custom_command(
      OUTPUT ${_stamp}
      BYPRODUCTS ${_source} ${_header} ${_algorithm} ${_prebuilt_kernel_byproducts}
      COMMAND ${CMAKE_COMMAND} -E make_directory
              ${_efmu_dir}/ProductionCode
              ${_efmu_dir}/AlgorithmCode
      COMMAND ${CMAKE_COMMAND}
              -DINPUT=${_prebuilt_source}
              -DOUTPUT=${_source}
              -DEXPECTED_SHA256=${_source_sha256}
              -P ${CMAKE_CURRENT_LIST_DIR}/verify_prebuilt_efmi.cmake
      COMMAND ${CMAKE_COMMAND}
              -DINPUT=${_prebuilt_header}
              -DOUTPUT=${_header}
              -DEXPECTED_SHA256=${_header_sha256}
              -P ${CMAKE_CURRENT_LIST_DIR}/verify_prebuilt_efmi.cmake
      COMMAND ${CMAKE_COMMAND}
              -DINPUT=${_prebuilt_algorithm}
              -DOUTPUT=${_algorithm}
              -DEXPECTED_SHA256=${_algorithm_sha256}
              -P ${CMAKE_CURRENT_LIST_DIR}/verify_prebuilt_efmi.cmake
      ${_prebuilt_kernel_commands}
      COMMAND ${CMAKE_COMMAND} -E touch
              ${_source} ${_header} ${_algorithm} ${_prebuilt_kernel_byproducts} ${_stamp}
      DEPENDS
        ${RDD2_PREBUILT_EFMI_DIR}/MANIFEST.sha256
        ${CMAKE_CURRENT_LIST_DIR}/verify_prebuilt_efmi.cmake
        ${_prebuilt_source}
        ${_prebuilt_header}
        ${_prebuilt_algorithm}
        ${_prebuilt_kernel_depends}
      COMMENT "Staging prebuilt ${model_name} eFMI Production Code"
      VERBATIM
    )
  else()
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
        ${_algorithm}
        ${_efmu_dir}/AlgorithmCode/manifest.xml
        ${_source}
        ${_header}
        ${_kernels_source}
        ${_kernels_header}
        ${_efmu_dir}/ProductionCode/manifest.xml
        ${_efmu_dir}/__content.xml
      COMMAND ${CMAKE_COMMAND} -E rm -rf
              ${_efmu_dir}
              ${_efmu_file}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${RDD2_RUMOCA_GENERATED_DIR}
      COMMAND ${_rumoca_command}
              --cache-dir ${PROJECT_BINARY_DIR}/.rumoca-cache
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
    list(APPEND RDD2_EFMI_KERNEL_SOURCES ${_kernels_source})
  endif()

  list(APPEND RDD2_EFMI_CONTROL_STAMPS ${_stamp})
  list(APPEND RDD2_EFMI_CONTROL_SOURCES ${_source})
  list(APPEND RDD2_EFMI_CONTROL_INCLUDE_DIRS ${_efmu_dir}/ProductionCode)
  set(RDD2_EFMI_CONTROL_STAMPS ${RDD2_EFMI_CONTROL_STAMPS} PARENT_SCOPE)
  set(RDD2_EFMI_CONTROL_SOURCES ${RDD2_EFMI_CONTROL_SOURCES} PARENT_SCOPE)
  set(RDD2_EFMI_CONTROL_INCLUDE_DIRS ${RDD2_EFMI_CONTROL_INCLUDE_DIRS} PARENT_SCOPE)
  set(RDD2_EFMI_KERNEL_SOURCES ${RDD2_EFMI_KERNEL_SOURCES} PARENT_SCOPE)
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

# One copy of the shared kernel unit serves every block: the copies are
# byte-identical for one compiler build, and a block generated by a different
# build fails to compile against the header it would otherwise share.
if(RDD2_EFMI_KERNEL_SOURCES)
  list(GET RDD2_EFMI_KERNEL_SOURCES 0 _rdd2_efmi_kernels_source)
  list(APPEND RDD2_EFMI_CONTROL_SOURCES ${_rdd2_efmi_kernels_source})
endif()

add_custom_target(rdd2_efmi_control_codegen
  DEPENDS ${RDD2_EFMI_CONTROL_STAMPS}
)

add_dependencies(app rdd2_efmi_control_codegen)
target_link_libraries(app PRIVATE synapse_fbs::c)

set_source_files_properties(${RDD2_EFMI_CONTROL_SOURCES}
  PROPERTIES GENERATED TRUE
)

target_sources(app PRIVATE ${RDD2_EFMI_CONTROL_SOURCES})
target_include_directories(app PRIVATE ${RDD2_EFMI_CONTROL_INCLUDE_DIRS})
