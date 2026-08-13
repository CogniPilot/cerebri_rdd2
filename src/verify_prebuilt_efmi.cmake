foreach(_required_variable IN ITEMS INPUT OUTPUT EXPECTED_SHA256)
  if(NOT DEFINED ${_required_variable} OR "${${_required_variable}}" STREQUAL "")
    message(FATAL_ERROR "Missing ${_required_variable} for prebuilt eFMI staging")
  endif()
endforeach()

if(NOT EXISTS "${INPUT}" OR IS_DIRECTORY "${INPUT}")
  message(FATAL_ERROR "Missing prebuilt eFMI artifact: ${INPUT}")
endif()

configure_file("${INPUT}" "${OUTPUT}" COPYONLY)

file(SHA256 "${OUTPUT}" _actual_sha256)
string(TOLOWER "${EXPECTED_SHA256}" _expected_sha256)
if(NOT _actual_sha256 STREQUAL _expected_sha256)
  file(REMOVE "${OUTPUT}")
  message(FATAL_ERROR
    "Staged prebuilt eFMI SHA-256 mismatch for ${INPUT}: "
    "expected ${_expected_sha256}, got ${_actual_sha256}"
  )
endif()
