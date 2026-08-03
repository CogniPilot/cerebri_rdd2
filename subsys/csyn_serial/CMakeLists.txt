if(CONFIG_RDD2_CSYN_SERIAL)
  target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR})

  target_sources(app PRIVATE
    csyn_serial.c
  )

  target_sources_ifdef(CONFIG_SHELL app PRIVATE
    csyn_serial_shell.c
  )
endif()
