if(CONFIG_RDD2_ZROS_SERIAL)
  target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR})

  target_sources(app PRIVATE
    zros_serial.c
  )

  target_sources_ifdef(CONFIG_SHELL app PRIVATE
    zros_serial_shell.c
  )
endif()
