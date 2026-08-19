if(CONFIG_RDD2_SYNAPSE_WIRE)
  target_sources(app PRIVATE synapse_wire.c)
  target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_SOURCE_DIR}/src
  )
  target_link_libraries(app PRIVATE synapse_fbs::wire)
endif()
