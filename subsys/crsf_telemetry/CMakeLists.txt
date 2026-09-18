if(CONFIG_RDD2_CRSF_TELEMETRY)
  target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_SOURCE_DIR}/src
  )

  target_sources(app PRIVATE
    crsf_telemetry.c
    crsf_telemetry_encode.c
  )
endif()
