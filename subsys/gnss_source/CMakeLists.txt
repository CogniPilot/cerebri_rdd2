if(CONFIG_RDD2_GNSS_SOURCE_ONBOARD)
  target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR})

  target_sources(app PRIVATE
    gnss_m10_protocol.c
    gnss_m10_topic.c
    gnss_onboard.c
  )

  target_sources_ifdef(CONFIG_SHELL app PRIVATE
    gnss_shell.c
  )
endif()

if(CONFIG_RDD2_GNSS_SOURCE_LOCKSTEP)
  target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR})
  target_sources(app PRIVATE gnss_lockstep.c)
endif()

if(CONFIG_RDD2_GNSS_SOURCE_MESH)
  target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR})
  target_sources(app PRIVATE gnss_mesh.c)
endif()
