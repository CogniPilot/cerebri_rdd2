/* SPDX-License-Identifier: Apache-2.0 */

#include <fsl_clock.h>

/*
 * The MR-VMU-Tropic board hook configures physical strap/reset GPIOs and waits
 * on the DWT cycle counter twice. FastDyn replaces those peripherals with the
 * lockstep boundary, so the hardware sequencing is unnecessary and can deadlock
 * when QEMU consumes part of the DWT register block internally.
 *
 * Providing the application definition keeps the board archive's hook from
 * being pulled into the FastDyn-only image. Normal hardware builds are
 * unchanged.
 */
void board_early_init_hook(void)
{
}

/*
 * Keep the SDK's clock-rate bookkeeping valid without polling analog PLL
 * status bits. The rehosted CPU is clocked by QEMU; selecting the 24 MHz
 * oscillator path is sufficient for Zephyr drivers which calculate divisors
 * from the IPG/AHB rate (notably ENET MDIO).
 */
void clock_init(void)
{
	CLOCK_SetXtalFreq(24000000U);
	CLOCK_SetRtcXtalFreq(32768U);
	CLOCK_SetMux(kCLOCK_PeriphClk2Mux, 1U);
	CLOCK_SetMux(kCLOCK_PeriphMux, 1U);
	SystemCoreClock = 24000000U;
}

uint32_t CLOCK_GetAhbFreq(void)
{
	return 24000000U;
}

uint32_t CLOCK_GetIpgFreq(void)
{
	return 24000000U;
}

uint32_t CLOCK_GetPerClkFreq(void)
{
	return 24000000U;
}

uint32_t CLOCK_GetFreq(clock_name_t name)
{
	return name == kCLOCK_RtcClk ? 32768U : 24000000U;
}
