# Tightly-coupled-memory placement for the periodic control path.
#
# The i.MX RT1064 executes this image from external QSPI flash behind a
# cache, so a cache miss on the per-tick path costs external-bus wait states
# that the on-chip TCMs do not have. ITCM and DTCM are 256 KB each and were
# almost entirely idle, so the hot translation units are placed there.
#
# LEDGER ITEM R-12: THIS IS A KNOWN RECONSTRUCTION, NOT THE INTENDED DESIGN.
#
# Which functions run on a periodic tick, and what each costs, is a fact the
# compiler holds: rumoca knows the DoStep call graph, and GALEC has no
# data-dependent trip counts, so the per-function cost is static. Naming the
# emitted translation units here rebuilds that knowledge downstream, where a
# rename or a translation-unit split would silently place nothing and leave
# only slower code behind, with no diagnostic.
#
# The intended fix is emitter-side: the GALEC target emits overridable
# macros on its own declarations (RUMOCA_GALEC_HOT on periodic-path
# functions, RUMOCA_GALEC_HOT_DATA on per-tick state and scratch), defined
# empty in the generated header, which this integration then defines as
# __itcm_section and __dtcm_bss_section. When that lands, this file reduces
# to those two definitions and the verification below.
#
# Until then this ships in the file-matching form to unblock on-silicon
# timing for the first flight, with two guards against the silent-miss
# failure mode it invites:
#
#   1. The generated paths are taken from the variables efmi.cmake exports
#      per eFMU, keyed by the generated name THIS repository chooses, rather
#      than by matching emitted filenames. A generated name that no longer
#      exists is a hard configure-time error below.
#   2. verify_hotpath_placement.cmake re-reads the linked image after every
#      build and fails it if the objects named here did not actually land in
#      the TCM output sections. That check is worth keeping permanently: it
#      guards the emitter-side form just as well.

if(NOT (CONFIG_CODE_DATA_RELOCATION AND CONFIG_BOARD_MR_VMU_TROPIC_MIMXRT1064))
  return()
endif()
if(CONFIG_RDD2_COMMS_STUB)
  return()
endif()

# eFMUs whose generated code runs on a periodic tick, hottest first:
# rate allocation at 1600 Hz, the estimator at its own release rate, and
# guidance at 200 Hz. The 50 Hz trajectory planner is deliberately excluded:
# it is the largest generated unit and the coldest, so it keeps ITCM free
# for the units that run every millisecond.
set(_rdd2_hotpath_efmus
  Vehicles_Rdd2_RateControlAllocator
  Vehicles_Rdd2_NavigationEstimator
  Vehicles_Rdd2_GuidanceController
)

set(_rdd2_hotpath_generated_sources)
foreach(_efmu ${_rdd2_hotpath_efmus})
  if(NOT DEFINED RDD2_EFMI_SOURCE_${_efmu})
    message(FATAL_ERROR
      "hotpath placement (ledger R-12): no generated source is registered for "
      "eFMU '${_efmu}'. rdd2_add_efmi_control_model() no longer emits that "
      "generated name, so naming it here would place nothing and silently "
      "leave the periodic path in external flash. Update this list, or "
      "replace it with the emitter-emitted RUMOCA_GALEC_HOT macros."
    )
  endif()
  list(APPEND _rdd2_hotpath_generated_sources ${RDD2_EFMI_SOURCE_${_efmu}})
endforeach()

if(NOT DEFINED RDD2_EFMI_SHARED_KERNEL_SOURCE)
  message(FATAL_ERROR
    "hotpath placement (ledger R-12): the shared GALEC kernel source is not "
    "registered; efmi.cmake no longer exports it."
  )
endif()
list(APPEND _rdd2_hotpath_generated_sources ${RDD2_EFMI_SHARED_KERNEL_SOURCE})

# The generated sources do not exist until codegen runs, so LTO cannot be
# disabled per file the way zephyr_code_relocate() does for real sources.
# CONFIG_LTO is not selected by any RDD2 profile; assert that rather than
# discovering the section-name mangling of upstream issue 69730 at runtime.
if(CONFIG_LTO)
  message(FATAL_ERROR
    "hotpath placement (ledger R-12): CONFIG_LTO renames the sections that "
    "code relocation matches on, so the generated periodic path would not be "
    "placed. Disable CONFIG_LTO or drop the generated units from this list."
  )
endif()

zephyr_code_relocate(
  FILES ${_rdd2_hotpath_generated_sources}
  LOCATION ITCM_TEXT_RODATA
)

# The estimator's GALEC scratch arena and per-tick state are the largest
# per-tick working set in the image and are read and written on every
# release, so they belong in DTCM beside the code that touches them.
zephyr_code_relocate(
  FILES ${RDD2_EFMI_SOURCE_Vehicles_Rdd2_NavigationEstimator}
  LOCATION DTCM_DATA_BSS
)

# Objects that must be observable in the TCM output sections of the linked
# image. Names are the basenames the archive members carry.
set(RDD2_HOTPATH_EXPECT_ITCM
  Vehicles_Rdd2_RateControlAllocator.c.obj
  Vehicles_Rdd2_NavigationEstimator.c.obj
  Vehicles_Rdd2_GuidanceController.c.obj
  rumoca_galec_kernels.c.obj
  navigation_estimator.c.obj
  rate_control_allocator.c.obj
)
set(RDD2_HOTPATH_EXPECT_DTCM
  Vehicles_Rdd2_NavigationEstimator.c.obj
  navigation_estimator.c.obj
  guidance_controller.c.obj
  rate_control_allocator.c.obj
)
string(REPLACE ";" "|" _rdd2_expect_itcm "${RDD2_HOTPATH_EXPECT_ITCM}")
string(REPLACE ";" "|" _rdd2_expect_dtcm "${RDD2_HOTPATH_EXPECT_DTCM}")

# Zephyr consumes its extra_post_build_commands property before the
# application directory is processed, so hang the check off the final link
# target instead. A separate ALL target ordered after zephyr_final runs on
# every build and fails it, which is the point: a silent miss is exactly the
# failure mode R-12 warns about.
if(NOT TARGET zephyr_final)
  message(FATAL_ERROR
    "hotpath placement (ledger R-12): the zephyr_final link target is missing, "
    "so TCM placement cannot be verified after the link."
  )
endif()
add_custom_target(rdd2_verify_hotpath_placement ALL
  COMMAND ${CMAKE_COMMAND}
          -DMAP_DIR=${PROJECT_BINARY_DIR}/zephyr
          -DEXPECT_ITCM=${_rdd2_expect_itcm}
          -DEXPECT_DTCM=${_rdd2_expect_dtcm}
          -P ${CMAKE_CURRENT_LIST_DIR}/verify_hotpath_placement.cmake
  COMMENT "Verifying the periodic control path reached tightly-coupled memory"
  VERBATIM
)
add_dependencies(rdd2_verify_hotpath_placement zephyr_final)
