target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}
  ${WORKSPACE_ROOT}/modules/lib/zenoh-pico/src/system/zephyr
)

target_sources(app PRIVATE
  attitude_control.c
  attitude_estimator.c
  control_io.c
  flight_mode.c
  imu_stream.c
  imu_latency_stats.c
  main.c
  motor_output.c
  rc_input.c
  rate_control.c
  topic_bus.c
  synapse_messages.c
)

target_sources_ifdef(CONFIG_SHELL app PRIVATE
  top_shell.c
)

target_sources_ifdef(CONFIG_RDD2_FASTDYN app PRIVATE fastdyn_board.c)

if(CONFIG_RDD2_FASTDYN)
  # Both the external board module and the FastDyn image provide this hook.
  # app/libapp.a is linked first, so its no-hardware implementation wins.
  zephyr_link_libraries(-Wl,--allow-multiple-definition)
endif()

if(CONFIG_CODE_DATA_RELOCATION AND CONFIG_BOARD_MR_VMU_TROPIC_MIMXRT1064)
  zephyr_code_relocate(
    FILES
      main.c
      control_io.c
      imu_stream.c
      rc_input.c
      flight_mode.c
      attitude_estimator.c
      attitude_control.c
      rate_control.c
      motor_output.c
      imu_latency_stats.c
      synapse_messages.c
    LOCATION ITCM_TEXT_RODATA
  )

  zephyr_code_relocate(
    FILES
      imu_stream.c
      rc_input.c
    LOCATION DTCM_DATA_BSS
  )
endif()
