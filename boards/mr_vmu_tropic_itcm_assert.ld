/*
 * Link-time check on the ITCM placement in mr_vmu_tropic_itcm_linker.ld. A
 * renamed section or a dropped pattern then fails the build instead of
 * showing up as late interrupts on the bench. The two symbols stand in for
 * the whole interrupt path: the dispatch table the Cortex-M wrapper reads on
 * every interrupt, and the cycle counter the handlers timestamp with.
 *
 * The last check runs the other way: the reset handler branches to
 * z_arm_init_arch_hw_at_boot() before arch_data_copy() has filled the ITCM,
 * so that function has to stay in flash or the CPU executes an empty ITCM.
 *
 * This lives outside the .itcm output section because an ASSERT inside one is
 * evaluated before the linker has placed it.
 */
__itcm_isr_table_check = ASSERT(_sw_isr_table < ORIGIN(ITCM) + LENGTH(ITCM),
	"_sw_isr_table is not in ITCM");
__itcm_cycle_read_check = ASSERT(sys_clock_cycle_get_32 < ORIGIN(ITCM) + LENGTH(ITCM),
	"sys_clock_cycle_get_32 is not in ITCM");
__itcm_pend_check = ASSERT(z_pend_curr < ORIGIN(ITCM) + LENGTH(ITCM),
	"z_pend_curr is not in ITCM");
__itcm_sem_take_check = ASSERT(z_impl_k_sem_take < ORIGIN(ITCM) + LENGTH(ITCM),
	"z_impl_k_sem_take is not in ITCM");
__itcm_boot_path_check = ASSERT(z_arm_init_arch_hw_at_boot >= ORIGIN(ITCM) + LENGTH(ITCM),
	"z_arm_init_arch_hw_at_boot must stay in flash, it runs before the ITCM is loaded");
