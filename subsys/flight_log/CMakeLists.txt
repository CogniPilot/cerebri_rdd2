if(CONFIG_RDD2_FLIGHT_LOG)
  target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_SOURCE_DIR}/src
  )

  target_sources(app PRIVATE
    flight_log_fs.c
    mcap_stream.c
    flight_log.c
  )

  target_sources_ifdef(CONFIG_RDD2_FLIGHT_LOG_SD_SHELL app PRIVATE
    sd_shell.c
  )

  target_sources_ifdef(CONFIG_SHELL app PRIVATE
    log_shell.c
  )

  target_sources_ifdef(CONFIG_MCUMGR_GRP_FS_FILE_ACCESS_HOOK app PRIVATE
    fs_mgmt_hook.c
  )
endif()
