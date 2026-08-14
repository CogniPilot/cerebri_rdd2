foreach(_kind IN ITEMS SOURCE HEADER)
  set(_reference_sha256 "")
  foreach(_index RANGE 0 3)
    set(_variable "${_kind}_${_index}")
    if(NOT DEFINED ${_variable} OR
       NOT EXISTS "${${_variable}}" OR
       IS_DIRECTORY "${${_variable}}")
      message(FATAL_ERROR "Missing generated eFMI kernel ${_kind}: ${${_variable}}")
    endif()
    file(SHA256 "${${_variable}}" _sha256)
    if(_index EQUAL 0)
      set(_reference_sha256 "${_sha256}")
    elseif(NOT _sha256 STREQUAL _reference_sha256)
      message(FATAL_ERROR
        "Generated eFMI kernels differ: ${${_variable}} has ${_sha256}, "
        "expected ${_reference_sha256}"
      )
    endif()
  endforeach()
endforeach()

if(NOT DEFINED STAMP OR STAMP STREQUAL "")
  message(FATAL_ERROR "STAMP is required")
endif()
file(TOUCH "${STAMP}")
