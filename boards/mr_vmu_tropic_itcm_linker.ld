/*
 * ITCM placement for the mr_vmu_tropic (i.MX RT1064).
 *
 * The image runs XIP from QSPI flash, where an instruction fetch that misses
 * the cache costs hundreds of cycles. Everything reached from an interrupt
 * runs from the 256 KB ITCM instead, which is single cycle and never misses,
 * so the DShot600 frame deadlines (13.3 us between shifter words, 26.7 us to
 * the end of frame event) hold even when a lower priority thread has just
 * evicted the cache.
 *
 * This file is included inside the .itcm output section, which the linker
 * places ahead of both the relocation sections and the main text section, so
 * an input section named here wins over the generic *(.text.*) catch-all.
 * It takes whole archives where the hot code is spread across an entire
 * library, and single functions elsewhere. Whole objects, including the
 * read-only data that has to follow their code and the data and bss of the
 * interrupt drivers, are placed by the zephyr_code_relocate() calls in
 * src/CMakeLists.txt instead, so nothing already listed there is repeated
 * here; that is where the FlexIO, DShot, CRSF, LPUART and eDMA drivers come
 * from.
 *
 * Every pattern below matches something in the current configuration. A
 * pattern that matches nothing is not an error, it is simply dead weight.
 */

/*
 * The dispatch table the Cortex-M wrapper indexes, read on every interrupt,
 * one entry per vector. The wrapper itself and the exception return come
 * from the relocated arch objects.
 */
KEEP(*(.gnu.linkonce.sw_isr_table*))
*(.text.z_arm_svc*)

/*
 * Whole libraries, taken by archive rather than by function. The scheduler
 * inlines the swap and pend paths into every blocking call, so the caller,
 * not just z_reschedule(), runs with the scheduler spinlock held; a cache
 * miss on flash inside that locked region delays the FlexIO interrupt by
 * tens of microseconds even though FlexIO is the highest priority source.
 * Taking the kernel, the Cortex-M arch core, the system timer and the
 * drivers whose handlers and callbacks take spinlocks removes the whole
 * class of miss. Read-only data of these archives stays in flash: none of it
 * is on the locked path.
 *
 * reset.S, prep_c.c and scb.c are excluded because they run before
 * arch_data_copy() fills the ITCM: the reset handler branches to
 * z_arm_init_arch_hw_at_boot() in scb.c, and z_prep_c() copies the ITCM
 * before it calls anything else. For the same reason memcpy and memset are
 * left in flash: arch_data_copy() is what calls them. Anything added here
 * that runs before that copy leaves the CPU executing an empty ITCM.
 */
*libkernel.a:*(.text .text.*)
*libarch__arm__core.a:*(.text .text.*)
*libarch__arm__core__mpu.a:*(.text .text.*)
*libarch__arm__core__cortex_m.a:*(EXCLUDE_FILE(*reset.S.obj *prep_c.c.obj *scb.c.obj) .text
                                  EXCLUDE_FILE(*reset.S.obj *prep_c.c.obj *scb.c.obj) .text.*)
*libdrivers__timer.a:*(.text .text.*)
*libdrivers__gpio.a:*(.text .text.*)
*libdrivers__spi.a:*(.text .text.*)
*libdrivers__ethernet.a:*(.text .text.*)
*libdrivers__sensor__tdk__icm45686.a:*(.text .text.*)
*libsubsys__rtio.a:*(.text .text.*)
*libzros.a:*(.text .text.*)
*libgcc.a:*(.text .text.*)
/*
 * The unwind table of the moved libgcc members has to follow them: its
 * entries are 31-bit PC-relative, which cannot span from flash to ITCM.
 */
*libgcc.a:*(.ARM.exidx*)

/*
 * What is left of the clock plumbing outside the timer driver, plus the two
 * interrupt sources whose driver is neither relocated nor taken by archive
 * above: the I2C power monitor and the PTP clock on the Ethernet MAC.
 */
*(.text.sys_clock_*)
*(.text.mcux_lpi2c_isr*)
*(.text.ptp_clock_nxp_enet_isr*)

/*
 * Interrupt handlers and the sensor, actuator and logging paths they run.
 */
*(.text.ring_buf_put)
*(.text.power_work_handler)
*(.text.z_thread_entry)

/*
 * The IMU stream: LPSPI with RTIO submission and the eDMA below it.
 */
*(.text.rtio*)
*(.text.GPIO_PinWrite)
*(.text.EDMA_*)
*(.text.sensor_iodev_submit)

/*
 * Kernel, driver and control code reached from those handlers: the
 * scheduler and context switch, timeouts and spinlocks, the semaphore,
 * mutex, poll and work fast paths, the ZROS topic locks, the rate loop and
 * allocator, and the storage, network and console paths that share the CPU
 * with them.
 */
*(.text.USDHC_TransferNonBlocking)
*(.text.f_write)
*(.text.USDHC_TransferHandleIRQ)
*(.text.fs_write)
*(.text.f_lseek)
*(.text.LPI2C_MasterTransferHandleIRQ)
*(.text.validate)
*(.text.USDHC_SetADMA2Descriptor)
*(.text.card_write_blocks)
*(.text.fatfs_write)
*(.text.USDHC_SetAdmaTableConfig)
*(.text.mcux_lpi2c_transfer)
*(.text.card_write)
*(.text.ring_buf_get_claim)
*(.text.sdmmc_wait_ready)
*(.text.sd_is_card_present)
*(.text.disk_sdmmc_access_status)
*(.text.sync_window)
*(.text.disk_access_get_di)
*(.text.strncmp)
*(.text.LPI2C_MasterTransferNonBlocking)
*(.text.sdmmc_read_status)
*(.text.disk_status)
*(.text.USDHC_ReceiveCommandResponse)
*(.text.USDHC_SetTransferConfig)
*(.text.USDHC_SendCommand)
*(.text.DCACHE_CleanByRange)
*(.text.disk_access_status)
*(.text.net_send_data)
*(.text.disk_sdmmc_access_write)
*(.text.move_window)
*(.text.pkt_alloc_with_buffer)
*(.text.LPI2C_MasterCheckAndClearError)
*(.text.sdmmc_write_blocks)
*(.text.net_pkt_cursor_operate)
*(.text.USDHC_SetInternalDmaConfig)
*(.text.pkt_cursor_update)
*(.text.disk_write)
*(.text.disk_access_write)
*(.text.net_buf_alloc_len)
*(.text.ethernet_send)
*(.text.net_buf_simple_add)
*(.text.net_context_recv)
*(.text.z_impl_net_if_get_by_index)
*(.text.pkt_alloc)
*(.text.pkt_alloc_on_iface)
*(.text.z_impl_zsock_sendto)
*(.text.z_impl_net_addr_pton)
*(.text.net_pkt_set_data)
*(.text.net_udp_finalize)
*(.text.mcux_lpi2c_master_transfer_callback)
*(.text.LPUART_GetStatusFlags)
*(.text.zsock_sendto_ctx)
*(.text.uart_callback)
*(.text.sock_sendto_vmeth)
*(.text.net_pkt_available_payload_buffer)
*(.text.card_read_blocks)
*(.text.net_context_sendto)
*(.text.net_pkt_get_contiguous_len)
*(.text.net_pkt_get_data)
*(.text.log_output_process)
*(.text.write_uart)
*(.text.net_buf_reset)
*(.text.send_check_and_wait)
*(.text.need_calc_checksum)
*(.text.bind_default)
*(.text.fixed_data_alloc)
*(.text.net_buf_unref)
*(.text.net_buf_simple_max_len)
*(.text.pkt_cursor_jump)
*(.text.net_pkt_write)
*(.text.net_udp_create)
*(.text.zvfs_fdtable_call_ioctl)
*(.text.disk_access_read)
*(.text.transport_evt_handler)
*(.text.z_shell_print_cmd)
*(.text.net_pkt_trim_buffer)
*(.text.disk_sdmmc_access_read)
*(.text.ENET_TransmitIRQHandler)
*(.text.DCACHE_CleanInvalidateByRange)
*(.text.create_chain)
*(.text.free_list_remove_bidx)
*(.text.split_chunks)
*(.text.sys_heap_free)
*(.text.sys_heap_alloc)
*(.text.zvfs_get_fd_obj_and_vtable)
*(.text.mpsc_pbuf_free)
*(.text.default_lf_get_timestamp)
*(.text.log_output_msg_process)
*(.text.z_shell_raw_fprintf)
*(.text.shell_thread)
