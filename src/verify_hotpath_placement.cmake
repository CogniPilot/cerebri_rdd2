# Assert that the periodic control path actually landed in tightly-coupled
# memory, and fail the build loudly when it did not.
#
# LEDGER ITEM R-12: hotpath_placement.cmake selects the hot translation units
# by name, which reconstructs knowledge only the code generator holds. A
# rename or a translation-unit split would place nothing, and the only
# symptom would be a slower image. This check turns that silent miss into a
# build failure. It stays useful after the emitter-side RUMOCA_GALEC_HOT
# macros replace the name matching, because it verifies the linked result
# rather than the request.

set(_map "${MAP_DIR}/zephyr_final.map")
if(NOT EXISTS "${_map}")
  set(_map "${MAP_DIR}/zephyr.map")
endif()
if(NOT EXISTS "${_map}")
  message(FATAL_ERROR
    "hotpath placement (ledger R-12): no linker map under ${MAP_DIR}, so TCM "
    "placement cannot be verified. Enable CONFIG_OUTPUT_PRINT_MEMORY_USAGE "
    "and the linker map output, or remove this check.")
endif()

file(STRINGS "${_map}" _lines)
set(_section "")
set(_itcm_objects "")
set(_dtcm_objects "")
foreach(_line IN LISTS _lines)
  if(_line MATCHES "^([^ \t][^ \t]*)")
    set(_candidate "${CMAKE_MATCH_1}")
    if(_candidate MATCHES "^\\.itcm")
      set(_section "itcm")
    elseif(_candidate MATCHES "^\\.dtcm")
      set(_section "dtcm")
    elseif(_candidate MATCHES "^[.A-Za-z_]")
      set(_section "")
    endif()
  endif()
  if(_section AND _line MATCHES "\\(([^()]+\\.obj)\\)")
    if(_section STREQUAL "itcm")
      list(APPEND _itcm_objects "${CMAKE_MATCH_1}")
    else()
      list(APPEND _dtcm_objects "${CMAKE_MATCH_1}")
    endif()
  endif()
endforeach()
list(REMOVE_DUPLICATES _itcm_objects)
list(REMOVE_DUPLICATES _dtcm_objects)

set(_missing "")
string(REPLACE "|" ";" _expect_itcm "${EXPECT_ITCM}")
string(REPLACE "|" ";" _expect_dtcm "${EXPECT_DTCM}")
foreach(_object IN LISTS _expect_itcm)
  if(NOT "${_object}" IN_LIST _itcm_objects)
    list(APPEND _missing "ITCM: ${_object}")
  endif()
endforeach()
foreach(_object IN LISTS _expect_dtcm)
  if(NOT "${_object}" IN_LIST _dtcm_objects)
    list(APPEND _missing "DTCM: ${_object}")
  endif()
endforeach()

if(_missing)
  string(REPLACE ";" "\n  " _report "${_missing}")
  message(FATAL_ERROR
    "hotpath placement (ledger R-12): the periodic control path did not reach "
    "tightly-coupled memory. Missing placements:\n  ${_report}\n"
    "Placed in ITCM: ${_itcm_objects}\n"
    "Placed in DTCM: ${_dtcm_objects}\n"
    "A renamed or split generated translation unit is the usual cause; the "
    "name list lives in src/hotpath_placement.cmake.")
endif()

message(STATUS "hotpath placement verified: ITCM ${_itcm_objects}")
message(STATUS "hotpath placement verified: DTCM ${_dtcm_objects}")
