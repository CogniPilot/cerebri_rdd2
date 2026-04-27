target_sources_ifdef(CONFIG_RDD2_LOG_SDCARD app PRIVATE
  log_sdcard.c
)

set_source_files_properties(log_sdcard.c PROPERTIES
  COMPILE_OPTIONS -Wno-misleading-indentation
)

if(CONFIG_RDD2_LOG_SDCARD AND CONFIG_FAT_FILESYSTEM_ELM)
  target_link_libraries(app PRIVATE ELMFAT)
endif()
