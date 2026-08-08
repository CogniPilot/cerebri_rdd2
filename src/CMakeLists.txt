target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}
  ${WORKSPACE_ROOT}/modules/lib/zenoh-pico/src/system/zephyr
)

target_sources(app PRIVATE
  interfaces/data.c
  interfaces/imu.c
  interfaces/motors.c
  interfaces/rc.c
  interfaces/zros_topics.c
  main.c
  processes/guidance_controller.c
  processes/navigation_estimator.c
  processes/rate_control_allocator.c
  processes/waypoint_trajectory_planner.c
)

target_sources_ifdef(CONFIG_TOP_SHELL app PRIVATE
  diagnostics/top_shell.c
)

target_sources_ifdef(CONFIG_RDD2_FASTDYN app PRIVATE
  platform/fastdyn_board.c
)

if(CONFIG_RDD2_FASTDYN)
  # Both the external board module and the FastDyn image provide this hook.
  # app/libapp.a is linked first, so its no-hardware implementation wins.
  zephyr_link_libraries(-Wl,--allow-multiple-definition)
endif()

if(CONFIG_CODE_DATA_RELOCATION AND CONFIG_BOARD_MR_VMU_TROPIC_MIMXRT1064)
  zephyr_code_relocate(
    FILES
      main.c
      interfaces/data.c
      interfaces/imu.c
      interfaces/motors.c
      interfaces/rc.c
      interfaces/zros_topics.c
      processes/guidance_controller.c
      processes/navigation_estimator.c
      processes/rate_control_allocator.c
      processes/waypoint_trajectory_planner.c
    LOCATION ITCM_TEXT_RODATA
  )

  zephyr_code_relocate(
    FILES
      interfaces/imu.c
      interfaces/rc.c
    LOCATION DTCM_DATA_BSS
  )
endif()
