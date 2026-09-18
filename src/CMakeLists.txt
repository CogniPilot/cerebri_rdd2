target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}
  ${CMAKE_CURRENT_LIST_DIR}/processes
  ${WORKSPACE_ROOT}/modules/lib/zenoh-pico/src/system/zephyr
)

target_sources(app PRIVATE
  interfaces/data.c
  interfaces/zros_topics.c
  main.c
)

if(CONFIG_RDD2_COMMS_STUB)
  target_sources(app PRIVATE
    processes/comms_stub.c
  )
elseif(CONFIG_RDD2_CONTROL_SIMPLE)
  target_sources(app PRIVATE
    interfaces/imu.c
    interfaces/rc.c
    interfaces/motors.c
    processes/navigation_gps.c
    processes/simple/simple_navigation_estimator.c
    processes/simple/simple_guidance_controller.c
    processes/simple/simple_rate_control_allocator.c
    processes/simple/simple_waypoint_trajectory_planner.c
  )
else()
  include(${CMAKE_CURRENT_LIST_DIR}/efmi.cmake)
  target_sources(app PRIVATE
    interfaces/imu.c
    interfaces/rc.c
    interfaces/motors.c
    processes/guidance_controller.c
    processes/imu_preintegration.c
    processes/navigation_estimator.c
    processes/navigation_gps.c
    processes/navigation_optical_flow.c
    processes/rate_control_allocator.c
    processes/waypoint_trajectory_planner.c
  )
endif()

target_sources_ifdef(CONFIG_RDD2_MAGNETOMETER app PRIVATE
  interfaces/magnetometer.c
)

target_sources_ifdef(CONFIG_SHELL app PRIVATE
  interfaces/mission_shell.c
)

if(CONFIG_SHELL AND CONFIG_RDD2_OPTICAL_FLOW_SOURCE_WIRE)
  target_sources(app PRIVATE
    interfaces/optical_flow_shell.c
  )
endif()

target_sources_ifdef(CONFIG_RDD2_OPTICAL_FLOW_SOURCE_WIRE_RAW app PRIVATE
  processes/navigation_optical_flow_raw.c
)

if(CONFIG_SHELL AND CONFIG_RDD2_OPTICAL_FLOW_SOURCE_WIRE_RAW)
  target_sources(app PRIVATE
    interfaces/optical_flow_raw_shell.c
  )
endif()

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
      interfaces/zros_topics.c
    LOCATION ITCM_TEXT_RODATA
  )

  if(CONFIG_RDD2_COMMS_STUB)
    zephyr_code_relocate(
      FILES
        processes/comms_stub.c
      LOCATION ITCM_TEXT_RODATA
    )
  elseif(CONFIG_RDD2_CONTROL_SIMPLE)
    zephyr_code_relocate(
      FILES
        interfaces/imu.c
        interfaces/rc.c
        interfaces/motors.c
        processes/navigation_gps.c
        processes/simple/simple_navigation_estimator.c
        processes/simple/simple_guidance_controller.c
        processes/simple/simple_rate_control_allocator.c
        processes/simple/simple_waypoint_trajectory_planner.c
      LOCATION ITCM_TEXT_RODATA
    )
  else()
    zephyr_code_relocate(
      FILES
        interfaces/imu.c
        interfaces/rc.c
        interfaces/motors.c
        processes/guidance_controller.c
        processes/imu_preintegration.c
        processes/navigation_estimator.c
        processes/navigation_gps.c
        processes/navigation_optical_flow.c
        processes/rate_control_allocator.c
        processes/waypoint_trajectory_planner.c
      LOCATION ITCM_TEXT_RODATA
    )
  endif()

  if(NOT CONFIG_RDD2_COMMS_STUB)
    zephyr_code_relocate(
      FILES
        interfaces/imu.c
        interfaces/rc.c
      LOCATION DTCM_DATA_BSS
    )
  endif()

  # The kernel, the Cortex-M arch core, the system timer and the interrupt
  # driven drivers are placed in the .itcm output section by archive and
  # section pattern, so a scheduler spinlock or an interrupt handler never
  # waits on a QSPI flash cache miss. The second file asserts the placement
  # at link time, including that the pre-copy boot path stays in flash.
  zephyr_linker_sources(ITCM_SECTION
    ${CMAKE_SOURCE_DIR}/boards/mr_vmu_tropic_itcm_linker.ld
  )
  zephyr_linker_sources(SECTIONS
    ${CMAKE_SOURCE_DIR}/boards/mr_vmu_tropic_itcm_assert.ld
  )

  # The interrupt path runs from ITCM: the Cortex-M wrapper and exception
  # return, the FlexIO parent driver and the DShot child (the switch from
  # output to input after each frame has to land within the ESC's response
  # delay), the CRSF UART and its DMA. From QSPI flash their latency is set
  # by cache misses.
  zephyr_code_relocate(
    FILES
      ${ZEPHYR_BASE}/arch/arm/core/cortex_m/isr_wrapper.c
      ${ZEPHYR_BASE}/arch/arm/core/cortex_m/exc_exit.c
      ${ZEPHYR_BASE}/drivers/misc/mcux_flexio/mcux_flexio.c
      ${ZEPHYR_BASE}/drivers/serial/uart_mcux_lpuart.c
      ${ZEPHYR_BASE}/drivers/dma/dma_mcux_edma.c
      ${ZEPHYR_BASE}/drivers/input/input_crsf.c
      ${ZEPHYR_BASE}/../modules/hal/nxp/mcux/mcux-sdk-ng/drivers/flexio/fsl_flexio.c
      ${CMAKE_SOURCE_DIR}/drivers/nxp_flexio_dshot/nxp_flexio_dshot.c
      ${CMAKE_SOURCE_DIR}/drivers/nxp_flexio_dshot/nxp_flexio_dshot_api.c
    LOCATION ITCM_TEXT_RODATA
  )
  # The other interrupt sources and what their handlers call: the IMU SPI
  # stream, the SD card controller, Ethernet, and the kernel's interrupt
  # table, fault entry and context switch.
  zephyr_code_relocate(
    FILES
      ${CMAKE_BINARY_DIR}/zephyr/isr_tables.c
      ${ZEPHYR_BASE}/arch/arm/core/cortex_m/swap_helper.S
      ${ZEPHYR_BASE}/arch/arm/core/cortex_m/irq_manage.c
      ${ZEPHYR_BASE}/arch/arm/core/cortex_m/fault.c
      ${ZEPHYR_BASE}/kernel/sem.c
      ${ZEPHYR_BASE}/drivers/spi/spi_nxp_lpspi/spi_nxp_lpspi_common.c
      ${ZEPHYR_BASE}/drivers/spi/spi_nxp_lpspi/spi_nxp_lpspi_rtio.c
      ${ZEPHYR_BASE}/drivers/sensor/tdk/icm45686/icm45686_stream.c
      ${ZEPHYR_BASE}/drivers/sensor/tdk/icm45686/icm45686_bus.c
      ${ZEPHYR_BASE}/drivers/sdhc/imx_usdhc.c
      ${ZEPHYR_BASE}/drivers/ethernet/eth_nxp_enet.c
      ${ZEPHYR_BASE}/subsys/rtio/rtio_executor.c
    LOCATION ITCM_TEXT_RODATA
  )
  zephyr_code_relocate(
    FILES
      ${CMAKE_SOURCE_DIR}/drivers/nxp_flexio_dshot/nxp_flexio_dshot.c
      ${ZEPHYR_BASE}/drivers/misc/mcux_flexio/mcux_flexio.c
      ${CMAKE_SOURCE_DIR}/src/interfaces/zros_topics.c
    LOCATION DTCM_DATA_BSS
  )

  # The interrupt stack and the main thread's stack (the rate loop) in
  # DTCM: exception entry and the rate loop never wait on a cache line.
  zephyr_code_relocate(
    FILES ${ZEPHYR_BASE}/kernel/init.c
    LOCATION DTCM_NOINIT
  )
endif()
