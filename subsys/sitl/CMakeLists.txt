if(CONFIG_RDD2_SITL)
  target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR})

  target_sources(app PRIVATE
    sitl_flatbuffer.c
    sitl_transport.c
  )

  target_sources_ifdef(CONFIG_RDD2_SITL_TRANSPORT_UDP app PRIVATE
    sitl_udp_coordinator.c
  )

  target_sources_ifdef(CONFIG_RDD2_SITL_TRANSPORT_ZENOH app PRIVATE
    sitl_zenoh_coordinator.c
  )
endif()
