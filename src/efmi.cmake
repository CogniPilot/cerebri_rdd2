set(FETCHCONTENT_BASE_DIR ${PROJECT_BINARY_DIR}/_deps)

if(NOT RDD2_RUMOCA_EXECUTABLE AND DEFINED ENV{RDD2_RUMOCA_EXECUTABLE} AND
   NOT "$ENV{RDD2_RUMOCA_EXECUTABLE}" STREQUAL "")
  set(RDD2_RUMOCA_EXECUTABLE "$ENV{RDD2_RUMOCA_EXECUTABLE}" CACHE FILEPATH
    "Exact Rumoca executable selected by the build environment" FORCE
  )
elseif(NOT DEFINED RDD2_RUMOCA_EXECUTABLE)
  set(RDD2_RUMOCA_EXECUTABLE "" CACHE FILEPATH
    "Exact Rumoca executable selected by the build environment"
  )
endif()
if(NOT RDD2_RUMOCA_EXECUTABLE_SHA256 AND DEFINED ENV{RDD2_RUMOCA_EXECUTABLE_SHA256} AND
   NOT "$ENV{RDD2_RUMOCA_EXECUTABLE_SHA256}" STREQUAL "")
  set(RDD2_RUMOCA_EXECUTABLE_SHA256
    "$ENV{RDD2_RUMOCA_EXECUTABLE_SHA256}" CACHE STRING
    "Expected SHA-256 of the exact Rumoca executable" FORCE
  )
elseif(NOT DEFINED RDD2_RUMOCA_EXECUTABLE_SHA256)
  set(RDD2_RUMOCA_EXECUTABLE_SHA256 "" CACHE STRING
    "Expected SHA-256 of the exact Rumoca executable"
  )
endif()
if(NOT RDD2_RUMOCA_EXECUTABLE)
  message(FATAL_ERROR
    "RDD2_RUMOCA_EXECUTABLE is required.\n"
    "With Nix, the repository commands supply the pinned compiler:\n"
    "  nix run .#build-native-sim\n"
    "Without Nix, build or obtain Rumoca at the revision this repository pins "
    "and pass it explicitly:\n"
    "  west build ... -- -DRDD2_RUMOCA_EXECUTABLE=/path/to/rumoca "
    "-DRDD2_RUMOCA_EXECUTABLE_SHA256=<sha256 of that file>\n"
    "Any executable is accepted; the digest is what fixes which one was used."
  )
endif()
if(NOT EXISTS "${RDD2_RUMOCA_EXECUTABLE}" OR
   IS_DIRECTORY "${RDD2_RUMOCA_EXECUTABLE}")
  message(FATAL_ERROR
    "RDD2_RUMOCA_EXECUTABLE is not a file: ${RDD2_RUMOCA_EXECUTABLE}"
  )
endif()
string(LENGTH "${RDD2_RUMOCA_EXECUTABLE_SHA256}"
  _rdd2_rumoca_expected_sha256_length
)
if(NOT _rdd2_rumoca_expected_sha256_length EQUAL 64 OR
   NOT RDD2_RUMOCA_EXECUTABLE_SHA256 MATCHES "^[0-9A-Fa-f]+$")
  message(FATAL_ERROR
    "RDD2_RUMOCA_EXECUTABLE_SHA256 must contain the exact 64-digit digest"
  )
endif()
file(SHA256 "${RDD2_RUMOCA_EXECUTABLE}" _rdd2_rumoca_actual_sha256)
string(TOLOWER "${RDD2_RUMOCA_EXECUTABLE_SHA256}"
  _rdd2_rumoca_expected_sha256
)
if(NOT _rdd2_rumoca_actual_sha256 STREQUAL _rdd2_rumoca_expected_sha256)
  message(FATAL_ERROR
    "Rumoca executable SHA-256 mismatch: expected "
    "${_rdd2_rumoca_expected_sha256}, got ${_rdd2_rumoca_actual_sha256}"
  )
endif()

set(_rdd2_rumoca_version_command ${RDD2_RUMOCA_EXECUTABLE} --version)
execute_process(
  COMMAND ${_rdd2_rumoca_version_command}
  OUTPUT_VARIABLE _rdd2_rumoca_version_output
  ERROR_VARIABLE _rdd2_rumoca_version_error
  RESULT_VARIABLE _rdd2_rumoca_version_result
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_STRIP_TRAILING_WHITESPACE
)
if(NOT _rdd2_rumoca_version_result EQUAL 0 OR
   NOT _rdd2_rumoca_version_output MATCHES "^rumoca [^ ]+$")
  message(FATAL_ERROR
    "Rumoca executable did not report a valid version: "
    "'${_rdd2_rumoca_version_output}' (${_rdd2_rumoca_version_error})"
  )
endif()

# Resolved provenance: the providers this build actually consumed, recorded
# where the build output lives so the selection can be checked afterwards
# against the one that was intended. The digest and version above are already
# verified, so this records established facts rather than declared intent.
set(_rdd2_provenance_content
  "rumoca.executable=${RDD2_RUMOCA_EXECUTABLE}\n"
  "rumoca.sha256=${_rdd2_rumoca_actual_sha256}\n"
  "rumoca.version=${_rdd2_rumoca_version_output}\n"
  "modelica_models.root=${RDD2_MODELICA_MODELS_ROOT}\n"
  "cerebri_modules.root=${RDD2_CEREBRI_MODULES_ROOT}\n"
  "zros.root=${RDD2_ZROS_ROOT}\n"
  "csyn.root=${RDD2_CSYN_ROOT}\n"
  "synapse_fbs_c.root=${FETCHCONTENT_SOURCE_DIR_SYNAPSE_FBS_C}\n"
)
file(WRITE ${PROJECT_BINARY_DIR}/rdd2-resolved-providers.txt
  ${_rdd2_provenance_content}
)
# Under sysbuild this project is one image among several, so the record above
# lands in the image directory while anything inspecting the build looks at the
# top level. Write it there too: the providers are a property of the build, and
# a caller should not have to know whether sysbuild was involved to find them.
if(SYSBUILD)
  get_filename_component(_rdd2_sysbuild_topdir ${PROJECT_BINARY_DIR} DIRECTORY)
  file(WRITE ${_rdd2_sysbuild_topdir}/rdd2-resolved-providers.txt
    ${_rdd2_provenance_content}
  )
endif()
message(STATUS "RDD2 Rumoca provider: ${RDD2_RUMOCA_EXECUTABLE}")

add_custom_target(rdd2_rumoca_tool
  DEPENDS ${RDD2_RUMOCA_EXECUTABLE}
)

set(RDD2_RUMOCA_GENERATED_DIR ${PROJECT_BINARY_DIR}/generated/rumoca)
set(RDD2_EFMI_CONTROL_STAMPS)
set(RDD2_EFMI_CONTROL_SOURCES)
set(RDD2_EFMI_CONTROL_INCLUDE_DIRS)
set(RDD2_EFMI_KERNEL_SOURCES)
set(RDD2_EFMI_KERNEL_HEADERS)

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
  set(_kernel_source ${_efmu_dir}/ProductionCode/rumoca_galec_kernels.c)
  set(_kernel_header ${_efmu_dir}/ProductionCode/rumoca_galec_kernels.h)
  set(_algorithm ${_efmu_dir}/AlgorithmCode/${generated_name}.alg)

  set(_rumoca_command ${RDD2_RUMOCA_EXECUTABLE})

  add_custom_command(
    OUTPUT ${_stamp}
    BYPRODUCTS
      ${_efmu_file}
      ${_algorithm}
      ${_efmu_dir}/AlgorithmCode/manifest.xml
      ${_source}
      ${_header}
      ${_kernel_source}
      ${_kernel_header}
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
    COMMENT "Generating ${model_name} eFMI Production Code with ${_rdd2_rumoca_version_output} (${_rdd2_rumoca_actual_sha256})"
    VERBATIM
  )

  list(APPEND RDD2_EFMI_CONTROL_STAMPS ${_stamp})
  list(APPEND RDD2_EFMI_CONTROL_SOURCES ${_source})
  list(APPEND RDD2_EFMI_CONTROL_INCLUDE_DIRS ${_efmu_dir}/ProductionCode)
  list(APPEND RDD2_EFMI_KERNEL_SOURCES ${_kernel_source})
  list(APPEND RDD2_EFMI_KERNEL_HEADERS ${_kernel_header})
  set(RDD2_EFMI_CONTROL_STAMPS ${RDD2_EFMI_CONTROL_STAMPS} PARENT_SCOPE)
  set(RDD2_EFMI_CONTROL_SOURCES ${RDD2_EFMI_CONTROL_SOURCES} PARENT_SCOPE)
  set(RDD2_EFMI_CONTROL_INCLUDE_DIRS ${RDD2_EFMI_CONTROL_INCLUDE_DIRS} PARENT_SCOPE)
  set(RDD2_EFMI_KERNEL_SOURCES ${RDD2_EFMI_KERNEL_SOURCES} PARENT_SCOPE)
  set(RDD2_EFMI_KERNEL_HEADERS ${RDD2_EFMI_KERNEL_HEADERS} PARENT_SCOPE)
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

list(LENGTH RDD2_EFMI_KERNEL_SOURCES _kernel_source_count)
list(LENGTH RDD2_EFMI_KERNEL_HEADERS _kernel_header_count)
if(NOT _kernel_source_count EQUAL 4 OR NOT _kernel_header_count EQUAL 4)
  message(FATAL_ERROR "Expected one generated kernel source/header per eFMU")
endif()
list(GET RDD2_EFMI_KERNEL_SOURCES 0 _shared_kernel_source)
list(GET RDD2_EFMI_KERNEL_SOURCES 1 _kernel_source_1)
list(GET RDD2_EFMI_KERNEL_SOURCES 2 _kernel_source_2)
list(GET RDD2_EFMI_KERNEL_SOURCES 3 _kernel_source_3)
list(GET RDD2_EFMI_KERNEL_HEADERS 0 _shared_kernel_header)
list(GET RDD2_EFMI_KERNEL_HEADERS 1 _kernel_header_1)
list(GET RDD2_EFMI_KERNEL_HEADERS 2 _kernel_header_2)
list(GET RDD2_EFMI_KERNEL_HEADERS 3 _kernel_header_3)
set(_shared_kernel_stamp
  ${RDD2_RUMOCA_GENERATED_DIR}/rumoca_galec_kernels.verified.stamp
)

add_custom_command(
  OUTPUT ${_shared_kernel_stamp}
  COMMAND ${CMAKE_COMMAND}
          -DSOURCE_0=${_shared_kernel_source}
          -DSOURCE_1=${_kernel_source_1}
          -DSOURCE_2=${_kernel_source_2}
          -DSOURCE_3=${_kernel_source_3}
          -DHEADER_0=${_shared_kernel_header}
          -DHEADER_1=${_kernel_header_1}
          -DHEADER_2=${_kernel_header_2}
          -DHEADER_3=${_kernel_header_3}
          -DSTAMP=${_shared_kernel_stamp}
          -P ${CMAKE_CURRENT_LIST_DIR}/verify_shared_efmi_kernel.cmake
  DEPENDS
    ${RDD2_EFMI_CONTROL_STAMPS}
    ${RDD2_EFMI_KERNEL_SOURCES}
    ${RDD2_EFMI_KERNEL_HEADERS}
    ${CMAKE_CURRENT_LIST_DIR}/verify_shared_efmi_kernel.cmake
  COMMENT "Verifying the shared generated eFMI kernel"
  VERBATIM
)

add_custom_target(rdd2_efmi_control_codegen
  DEPENDS ${RDD2_EFMI_CONTROL_STAMPS} ${_shared_kernel_stamp}
)

add_dependencies(app rdd2_efmi_control_codegen)
target_link_libraries(app PRIVATE synapse_fbs::c)

set_source_files_properties(${RDD2_EFMI_CONTROL_SOURCES} ${_shared_kernel_source}
  PROPERTIES GENERATED TRUE
)
set_source_files_properties(${_shared_kernel_source}
  PROPERTIES OBJECT_DEPENDS ${_shared_kernel_stamp}
)

target_sources(app PRIVATE ${RDD2_EFMI_CONTROL_SOURCES} ${_shared_kernel_source})
target_include_directories(app PRIVATE ${RDD2_EFMI_CONTROL_INCLUDE_DIRS})
