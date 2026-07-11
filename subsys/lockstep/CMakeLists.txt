if(CONFIG_RDD2_LOCKSTEP)
  target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR})

  target_sources(app PRIVATE
    lockstep_input.c
    lockstep_transport.c
  )

  target_sources_ifdef(CONFIG_RDD2_FASTDYN app PRIVATE
    lockstep_fastdyn.c
  )

  target_sources_ifdef(CONFIG_BOARD_NATIVE_SIM app PRIVATE
    lockstep_direct.c
  )
endif()
