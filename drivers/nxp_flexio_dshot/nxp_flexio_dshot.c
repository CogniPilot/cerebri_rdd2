/*
 * Copyright 2024 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/kernel.h>

#define LOG_MODULE_NAME nxp_flexio_dshot
#include <fsl_clock.h>
#include <fsl_flexio.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/misc/nxp_flexio_dshot/nxp_flexio_dshot.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/shell/shell.h>
#include <zephyr/drivers/sensor_clock.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(LOG_MODULE_NAME, CONFIG_LOG_DEFAULT_LEVEL);

#ifdef CONFIG_SOC_MIMX9596_M7
#include <zephyr/drivers/firmware/scmi/clk.h>
#include <zephyr/dt-bindings/clock/imx95_clock.h>
#endif

#include <zephyr/drivers/misc/nxp_flexio/nxp_flexio.h>

#define DT_DRV_COMPAT       cognipilot_flexio_dshot
#define DSHOT_INIT_PRIORITY CONFIG_KERNEL_INIT_PRIORITY_DEVICE

#define DSHOT_THROTTLE_POSITION  5u
#define DSHOT_TELEMETRY_POSITION 4u
#define NIBBLES_SIZE             4u
#define DSHOT_NUMBER_OF_NIBBLES  3u

/*
 * Bidirectional DShot receive-baud training.
 *
 * The eRPM response is sent back by the ESC on the same wire at 5/4 of the
 * output bitrate, but the exact turnaround timing varies from ESC to ESC. To
 * lock onto it reliably the driver sweeps a small offset applied to the receive
 * timer compare value, records which offsets decode cleanly, and settles on the
 * center of the widest good window.
 */
#define BDSHOT_OFFLINE_COUNT    200 /* No responses for this many cycles -> offline */
#define BDSHOT_RETRAIN_COUNT    (2 * BDSHOT_OFFLINE_COUNT)
#define BDSHOT_TCMP_MIN_OFFSET  (-16)
#define BDSHOT_TCMP_MAX_OFFSET  15
#define BDSHOT_TCMP_TO_MASK(x)  ((x) - BDSHOT_TCMP_MIN_OFFSET)
#define BDSHOT_TRAINING_TRIES   25
#define BDSHOT_TRAINING_SUCCESS 24

/*
 * Extended DShot Telemetry (EDT).
 *
 * When enabled, the ESC interleaves telemetry frames with the eRPM responses.
 * The 12-bit payload is eRPM when the 9-bit mantissa MSB is set; otherwise, if
 * EDT is enabled, it is an EDT frame carrying a type nibble and an 8-bit value.
 * The ESC only starts emitting EDT once it receives the enable command. It only
 * accepts commands after it has been idle for a while, so the driver holds off a
 * second after a channel comes online, spaces the retries a second apart and
 * gives up after a few unanswered attempts.
 */
#define DSHOT_CMD_EXTENDED_TELEMETRY_ENABLE 13
#define BDSHOT_EDT_ENABLE_REPEATS           10
#define BDSHOT_EDT_MAX_ATTEMPTS             5
#define BDSHOT_EDT_REQUEST_DELAY_MS         1000U

#define BDSHOT_EDT_MANTISSA_MSB 0x0100 /* Set -> eRPM frame, clear -> EDT frame */
#define BDSHOT_EDT_TYPE_MASK    0x0F00
#define BDSHOT_EDT_TYPE_SHIFT   8
#define BDSHOT_EDT_VALUE_MASK   0x00FF

/* EDT frame type nibbles */
#define DSHOT_EDT_TEMPERATURE 0x02 /* degrees C, 1 C per step */
#define DSHOT_EDT_VOLTAGE     0x04 /* 0.25 V per step */
#define DSHOT_EDT_CURRENT     0x06 /* 1 A per step */

/* Freshness bits for the per-channel EDT sub-values */
#define BDSHOT_EDT_VALID_TEMPERATURE BIT(0)
#define BDSHOT_EDT_VALID_VOLTAGE     BIT(1)
#define BDSHOT_EDT_VALID_CURRENT     BIT(2)

static const uint32_t gcr_decode[32] = {0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x9, 0xA,
					0xB, 0x0, 0xD, 0xE, 0xF, 0x0, 0x0, 0x2, 0x3, 0x0, 0x5,
					0x6, 0x7, 0x0, 0x0, 0x8, 0x1, 0x0, 0x4, 0xC, 0x0};

typedef enum {
	DSHOT_START = 0,
	DSHOT_12BIT_FIFO,
	DSHOT_12BIT_TRANSFERRED,
	DSHOT_TRANSMIT_COMPLETE,
	BDSHOT_RECEIVE,
	BDSHOT_RECEIVE_COMPLETE,
} dshot_state;

struct nxp_flexio_dshot_channel {
	uint8_t dshot_channel_count;
	struct nxp_flexio_dshot_channel_config *dshot_info;
};

struct nxp_flexio_dshot_config {
	const struct device *flexio_dev;
	FLEXIO_Type *flexio_base;
	const struct pinctrl_dev_config *pincfg;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
	const struct nxp_flexio_dshot_channel *channel;
	const struct nxp_flexio_child *child;
	const uint32_t speed;
};

struct nxp_flexio_dshot_data {
	uint32_t flexio_clk;
	uint32_t dshot_tcmp;
	uint32_t dshot_mask;
	uint32_t dshot_timer_mask;
	uint32_t bdshot_recv_mask;
	uint32_t bdshot_parsed_recv_mask;
	uint32_t bdshot_busy_us;
	const struct device *dev;
	struct k_work_delayable hold_work;
	uint8_t hold_attempts;
	bool hold_low;                  /* lines held low, no frames sent */ /* Window a bidirectional channel stays receive-armed */
	uint64_t last_trigger_ns;
};

struct nxp_flexio_dshot_channel_config {
	/** Flexio used pin index */
	uint8_t pin_id;
	bool init;
	uint32_t data_seg1;
	uint32_t irq_data;
	dshot_state state;
	bool bdshot;
	bool edt;
	uint32_t raw_response;
	uint16_t erpm;
	uint32_t crc_error_cnt;
	uint32_t frame_error_cnt;
	uint32_t decoded_cnt;
	uint32_t no_response_cnt;
	uint32_t last_no_response_cnt;

	/* Per-channel bidirectional DShot receive-baud training state */
	uint32_t bdshot_tcmp;            /* Trained receive timer compare value */
	int8_t bdshot_tcmp_offset;       /* Current sweep offset applied to tcmp */
	uint32_t bdshot_training_mask;   /* Bit set per offset that decoded cleanly */
	uint8_t bdshot_training_count;   /* Frames sampled at current offset */
	uint8_t bdshot_training_success; /* Clean decodes at current offset */
	bool bdshot_training_done;       /* Set once a good offset was locked in */
	bool online;                     /* ESC currently responding */
	uint32_t tx_started;             /* Cycle count when the last frame was armed */
	uint16_t consecutive_successes;
	uint16_t consecutive_failures;

	/* Extended DShot Telemetry (EDT) state, only used when this channel's edt is set */
	uint8_t edt_enable_repeats; /* Remaining enable commands to send to the ESC */
	uint8_t edt_attempts;       /* Enable commands sent since the channel came online */
	bool edt_confirmed;         /* An EDT frame arrived after one of our requests */
	uint32_t edt_online_ms;     /* Uptime the channel came online, 0 while offline */
	uint8_t edt_valid;          /* BDSHOT_EDT_VALID_* bits not reported yet */
	uint8_t edt_temperature;    /* degrees C */
	uint8_t edt_voltage;        /* raw, 0.25 V per step */
	uint8_t edt_current;        /* amps */
};

/*

 * Recompute a channel's receive timer compare from the base bdshot timing and
 * its current training offset. The low byte holds the baud divider that the
 * training sweep nudges to align sampling with the ESC's actual turnaround.
 */
static void nxp_flexio_dshot_set_tcmp(const struct device *dev, uint32_t channel)
{
	const struct nxp_flexio_dshot_config *config = dev->config;
	struct nxp_flexio_dshot_data *data = dev->data;
	struct nxp_flexio_dshot_channel_config *dshot_info = &config->channel->dshot_info[channel];
	const int dshot_pwm_freq = config->speed * 1000;

	dshot_info->bdshot_tcmp = 0x2900 | (((data->flexio_clk / (dshot_pwm_freq * 5 / 4) / 2) +
					     dshot_info->bdshot_tcmp_offset) &
					    0xFF);
}

static void nxp_flexio_dshot_output(const struct device *dev, uint32_t channel)
{
	const struct nxp_flexio_dshot_config *config = dev->config;
	struct nxp_flexio_dshot_data *data = dev->data;
	FLEXIO_Type *flexio_base = (FLEXIO_Type *)(config->flexio_base);
	struct nxp_flexio_child *child = (struct nxp_flexio_child *)(config->child);
	struct nxp_flexio_dshot_channel_config *dshot_info = &config->channel->dshot_info[channel];

	flexio_timer_config_t timerConfig;
	flexio_shifter_config_t shifterConfig;

	/* Disable timer, TIMCFG and TIMCMP may only be written while it is disabled */
	flexio_base->TIMCTL[child->res.timer_index[channel]] = 0;

	/* Disable Shifter */
	(void)memset(&shifterConfig, 0, sizeof(shifterConfig));
	FLEXIO_SetShifterConfig(flexio_base, child->res.shifter_index[channel], &shifterConfig);

	/* No start bit, stop bit low */
	shifterConfig.inputSource = kFLEXIO_ShifterInputFromPin;
	shifterConfig.shifterStop = kFLEXIO_ShifterStopBitLow;
	shifterConfig.shifterStart = kFLEXIO_ShifterStartBitDisabledLoadDataOnEnable;

	/* Transmit mode, output to FXIO pin, inverted output for bdshot */
	shifterConfig.timerSelect = child->res.timer_index[channel];
	shifterConfig.timerPolarity = kFLEXIO_ShifterTimerPolarityOnPositive;
	shifterConfig.pinConfig = kFLEXIO_PinConfigOutput;
	shifterConfig.pinSelect = dshot_info->pin_id;
	shifterConfig.pinPolarity = dshot_info->bdshot;
	shifterConfig.shifterMode = kFLEXIO_ShifterModeTransmit;

	FLEXIO_SetShifterConfig(flexio_base, child->res.shifter_index[channel], &shifterConfig);

	(void)memset(&timerConfig, 0, sizeof(timerConfig));

	/* Start transmitting on trigger, disable on compare */
	timerConfig.timerOutput = kFLEXIO_TimerOutputOneNotAffectedByReset;
	timerConfig.timerDecrement = kFLEXIO_TimerDecSrcOnFlexIOClockShiftTimerOutput;
	timerConfig.timerReset = kFLEXIO_TimerResetNever;
	timerConfig.timerDisable = kFLEXIO_TimerDisableOnTimerCompare;
	timerConfig.timerEnable = kFLEXIO_TimerEnableOnTriggerHigh;
	timerConfig.timerStop = kFLEXIO_TimerStopBitDisabled;
	timerConfig.timerStart = kFLEXIO_TimerStartBitDisabled;

	timerConfig.timerCompare = data->dshot_tcmp;

	/* Baud mode, Trigger on shifter write */
	timerConfig.triggerSelect =
		FLEXIO_TIMER_TRIGGER_SEL_SHIFTnSTAT(child->res.shifter_index[channel]);
	timerConfig.triggerPolarity = kFLEXIO_TimerTriggerPolarityActiveLow;
	timerConfig.triggerSource = kFLEXIO_TimerTriggerSourceInternal;
	timerConfig.pinConfig = kFLEXIO_PinConfigOutputDisabled;
	timerConfig.pinSelect = 0;
	timerConfig.pinPolarity = kFLEXIO_PinActiveLow;
	timerConfig.timerMode = kFLEXIO_TimerModeDual8BitBaudBit;

	FLEXIO_SetTimerConfig(flexio_base, child->res.timer_index[channel], &timerConfig);
}

static void nxp_flexio_bdshot_input(const struct device *dev, uint32_t channel)
{
	const struct nxp_flexio_dshot_config *config = dev->config;
	FLEXIO_Type *flexio_base = (FLEXIO_Type *)(config->flexio_base);
	struct nxp_flexio_child *child = (struct nxp_flexio_child *)(config->child);
	struct nxp_flexio_dshot_channel_config *dshot_info = &config->channel->dshot_info[channel];

	flexio_timer_config_t timerConfig;
	flexio_shifter_config_t shifterConfig;

	(void)memset(&timerConfig, 0, sizeof(timerConfig));
	(void)memset(&shifterConfig, 0, sizeof(shifterConfig));

	/* Input data from pin, no start/stop bit*/
	shifterConfig.inputSource = kFLEXIO_ShifterInputFromPin;
	shifterConfig.shifterStop = kFLEXIO_ShifterStopBitDisable;
	shifterConfig.shifterStart = kFLEXIO_ShifterStartBitDisabledLoadDataOnShift;

	/* Shifter receive mode, on FXIO pin input */
	shifterConfig.timerSelect = child->res.timer_index[channel];
	shifterConfig.timerPolarity = kFLEXIO_ShifterTimerPolarityOnPositive;
	shifterConfig.pinConfig = kFLEXIO_PinConfigOutputDisabled;
	shifterConfig.pinSelect = dshot_info->pin_id;
	shifterConfig.pinPolarity = kFLEXIO_PinActiveLow;
	shifterConfig.shifterMode = kFLEXIO_ShifterModeReceive;

	FLEXIO_SetShifterConfig(flexio_base, child->res.shifter_index[channel], &shifterConfig);

	/* Make sure there no shifter flags high from transmission */
	FLEXIO_ClearShifterStatusFlags(flexio_base, 1 << child->res.shifter_index[channel]);

	/* Enable on pin transition, resynchronize through reset on the rising
	 * edge. Output must start low (Zero) so the first shift lands mid-bit
	 * rather than a full baud period after the start edge, otherwise every
	 * sample sits on a bit boundary and only an ESC-faster baud decodes.
	 */
	timerConfig.timerOutput = kFLEXIO_TimerOutputZeroAffectedByReset;
	timerConfig.timerDecrement = kFLEXIO_TimerDecSrcOnFlexIOClockShiftTimerOutput;
	timerConfig.timerReset = kFLEXIO_TimerResetOnTimerPinRisingEdge;
	timerConfig.timerDisable = kFLEXIO_TimerDisableOnTimerCompare;
	timerConfig.timerEnable = kFLEXIO_TimerEnableOnTriggerBothEdge;
	timerConfig.timerStop = kFLEXIO_TimerStopBitEnableOnTimerDisable;
	timerConfig.timerStart = kFLEXIO_TimerStartBitEnabled;

	/* Per-channel receive compare, seeded at init and nudged by training */
	timerConfig.timerCompare = dshot_info->bdshot_tcmp;

	/* Baud mode, Trigger on shifter write */
	timerConfig.triggerSelect = FLEXIO_TIMER_TRIGGER_SEL_PININPUT(dshot_info->pin_id);
	timerConfig.triggerPolarity = kFLEXIO_TimerTriggerPolarityActiveHigh;
	timerConfig.triggerSource = kFLEXIO_TimerTriggerSourceInternal;
	timerConfig.pinConfig = kFLEXIO_PinConfigOutputDisabled;
	timerConfig.pinSelect = dshot_info->pin_id;
	timerConfig.pinPolarity = kFLEXIO_PinActiveLow;
	timerConfig.timerMode = kFLEXIO_TimerModeDual8BitBaudBit;

	FLEXIO_SetTimerConfig(flexio_base, child->res.timer_index[channel], &timerConfig);
}

static int nxp_flexio_dshot_set_clock(const struct nxp_flexio_dshot_config *cfg)
{
	int ret = 0;
#ifdef CONFIG_SOC_MIMXRT1176
	/* Init System Pll2 pfd3 to 432Mhz */
	CLOCK_InitPfd(kCLOCK_PllSys2, kCLOCK_Pfd3, 22);

	clock_root_config_t rootCfg = {0};

	/* Configure FLEXIO1 using SysPLL3Div2 @ 108MHz */
	rootCfg.mux = kCLOCK_FLEXIO1_ClockRoot_MuxSysPll2Pfd3;
	rootCfg.div = 4;
	CLOCK_SetRootClock(kCLOCK_Root_Flexio1, &rootCfg);
#endif

#ifdef CONFIG_SOC_MIMX9596_M7
	uint64_t flexio_clk = 133333333;
	struct scmi_protocol *proto = cfg->clock_dev->data;
	struct scmi_clock_rate_config clk_cfg = {0};
	ret = scmi_clock_parent_set(proto, IMX95_CLK_FLEXIO1, IMX95_CLK_SYSPLL1_PFD1_DIV2);
	if (ret) {
		return ret;
	}

	clk_cfg.flags = SCMI_CLK_RATE_SET_FLAGS_ROUNDS_AUTO;
	clk_cfg.clk_id = IMX95_CLK_FLEXIO1;
	clk_cfg.rate[0] = flexio_clk & 0xffffffff;
	clk_cfg.rate[1] = (flexio_clk >> 32) & 0xffffffff;

	ret = scmi_clock_rate_set(proto, &clk_cfg);
	if (ret) {
		return ret;
	}
#endif

#ifdef CONFIG_SOC_MIMXRT1064
	/* 108Mhz clock for FlexIO using PLL3 PFD2 @ 520 */
	CLOCK_InitUsb1Pfd(kCLOCK_Pfd2, 16U);

	CLOCK_SetMux(kCLOCK_Flexio1Mux, 1); // PPL3 PFD2
	CLOCK_SetDiv(kCLOCK_Flexio1Div, 0);
	CLOCK_SetDiv(kCLOCK_Flexio1PreDiv, 4);
#endif

	return ret;
}

/* Alternates between releasing the lines after a hold and, a few seconds
 * later, checking whether every bidirectional channel came online; a channel
 * still silent gets another hold, up to three in total.
 */
static void nxp_flexio_dshot_hold_work(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct nxp_flexio_dshot_data *data =
		CONTAINER_OF(dwork, struct nxp_flexio_dshot_data, hold_work);
	const struct nxp_flexio_dshot_config *config = data->dev->config;
	FLEXIO_Type *flexio_base = (FLEXIO_Type *)(config->flexio_base);
	struct nxp_flexio_child *child = (struct nxp_flexio_child *)(config->child);
	bool all_online = true;

	for (uint32_t channel = 0; channel < config->channel->dshot_channel_count; channel++) {
		struct nxp_flexio_dshot_channel_config *dshot_info =
			&config->channel->dshot_info[channel];
		uint32_t idx = child->res.shifter_index[channel];

		if (!dshot_info->bdshot) {
			continue;
		}
		if (data->hold_low) {
			flexio_base->SHIFTCTL[idx] |= FLEXIO_SHIFTCTL_PINPOL_MASK;
		} else if (!dshot_info->online) {
			flexio_base->SHIFTCTL[idx] &= ~FLEXIO_SHIFTCTL_PINPOL_MASK;
			all_online = false;
		}
	}

	if (data->hold_low) {
		data->hold_low = false;
		if (data->hold_attempts < 3) {
			k_work_schedule(&data->hold_work, K_SECONDS(5));
		}
	} else if (!all_online) {
		data->hold_attempts++;
		data->hold_low = true;
		k_work_schedule(&data->hold_work, K_SECONDS(4));
	}
}

static int nxp_flexio_dshot_init(const struct device *dev)
{
	const struct nxp_flexio_dshot_config *config = dev->config;
	struct nxp_flexio_dshot_data *data = dev->data;
	uint8_t channel = 0;
	int err;
	struct nxp_flexio_child *child = (struct nxp_flexio_child *)(config->child);

	if (!device_is_ready(config->clock_dev)) {
		return -ENODEV;
	}

	if (nxp_flexio_dshot_set_clock(config)) {
		return -EINVAL;
	}

	if (clock_control_get_rate(config->clock_dev, config->clock_subsys, &data->flexio_clk)) {
		return -EINVAL;
	}

	const int dshot_pwm_freq = config->speed * 1000;

	/* Calculate dshot timings based on dshot_pwm_freq */
	data->dshot_tcmp = 0x2F00 | (((data->flexio_clk / (dshot_pwm_freq * 3) / 2) - 1) & 0xFF);

	/* 16-bit frame, ESC turnaround, 21-bit response at 5/4 of the output rate
	 * and margin for interrupt latency: how long a bidirectional channel may
	 * still be busy after its frame was armed.
	 */
	data->bdshot_busy_us = 16000000U / dshot_pwm_freq + 30U +
			       (21U * 4000000U) / (5U * dshot_pwm_freq) + 50U;

	err = pinctrl_apply_state(config->pincfg, PINCTRL_STATE_DEFAULT);
	if (err) {
		LOG_ERR("Failed to configure pins");
		return err;
	}

	err = nxp_flexio_child_attach(config->flexio_dev, child);
	if (err < 0) {
		LOG_ERR("Failed to attach child");
		return err;
	}

	data->dshot_mask = 0;
	data->dshot_timer_mask = 0;

	for (channel = 0; channel < config->channel->dshot_channel_count; channel++) {
		struct nxp_flexio_dshot_channel_config *dshot_info =
			&config->channel->dshot_info[channel];

		/* Seed the receive-baud sweep so the first responses drive
		 * training. The rest of the training state is zero-initialised.
		 */
		if (dshot_info->bdshot) {
			dshot_info->bdshot_tcmp_offset = BDSHOT_TCMP_MIN_OFFSET;
			nxp_flexio_dshot_set_tcmp(dev, channel);
		}

		nxp_flexio_dshot_output(dev, channel);
		dshot_info->init = true;
		data->dshot_mask |= (1 << child->res.shifter_index[channel]);
		data->dshot_timer_mask |= (1 << child->res.timer_index[channel]);
	}

	/* Hold every line low for the first seconds and send nothing. An AM32
	 * ESC that lost its signal while the line stayed high (a debugger halt
	 * leaves the inverted idle driven) only recovers after it has timed
	 * out, reset and found the line low; on the bench that takes just over
	 * 3 s and does not always succeed on the first hold. A normal power-up
	 * is unaffected and the rest of the system boots meanwhile.
	 */
	FLEXIO_Type *flexio_base = (FLEXIO_Type *)(config->flexio_base);

	for (channel = 0; channel < config->channel->dshot_channel_count; channel++) {
		flexio_base->SHIFTCTL[child->res.shifter_index[channel]] &= ~FLEXIO_SHIFTCTL_PINPOL_MASK;
	}
	data->dev = dev;
	data->hold_low = true;
	data->hold_attempts = 1;
	k_work_init_delayable(&data->hold_work, nxp_flexio_dshot_hold_work);
	k_work_schedule(&data->hold_work, K_SECONDS(4));

	return 0;
}

static void nxp_flexio_dshot_hw_trigger(const struct device *dev)
{
	const struct nxp_flexio_dshot_config *config = dev->config;
	struct nxp_flexio_dshot_data *data = dev->data;

	if (data->hold_low) {
		return;
	}
	FLEXIO_Type *flexio_base = (FLEXIO_Type *)(config->flexio_base);
	struct nxp_flexio_child *child = (struct nxp_flexio_child *)(config->child);
	struct nxp_flexio_dshot_channel_config *dshot_info;
	uint32_t recv_mask = data->bdshot_recv_mask;
	uint32_t tx_mask = 0;
	uint32_t tx_timer_mask = 0;
	uint32_t now = k_cycle_get_32();
	uint64_t cycles = 0U;
	unsigned int key;

	if (sensor_clock_get_cycles(&cycles) == 0) {
		data->last_trigger_ns = sensor_clock_cycles_to_ns(cycles);
	} else {
		data->last_trigger_ns = 0U;
	}

	data->bdshot_recv_mask = 0x0;

	for (uint8_t channel = 0; (channel < config->channel->dshot_channel_count); channel++) {
		uint32_t shifter_flag = 1 << child->res.shifter_index[channel];

		dshot_info = &config->channel->dshot_info[channel];

		if (dshot_info->bdshot) {
			/* A response the ISR has not consumed yet: latch it here
			 * instead of destroying it, reading the buffer clears the
			 * shifter flag. The next fetch decodes it.
			 */
			if (dshot_info->state == BDSHOT_RECEIVE &&
			    (FLEXIO_GetShifterStatusFlags(flexio_base) & shifter_flag)) {
				dshot_info->raw_response =
					flexio_base->SHIFTBUFBIS[child->res.shifter_index[channel]];
				dshot_info->state = BDSHOT_RECEIVE_COMPLETE;
				data->bdshot_recv_mask |= shifter_flag;
			} else if ((recv_mask & shifter_flag) == 0) {
				dshot_info->no_response_cnt++;
			}

			/* The ESC may still be driving the line: leave the receive
			 * armed until the busy window has elapsed.
			 */
			if (dshot_info->state != BDSHOT_RECEIVE_COMPLETE &&
			    dshot_info->tx_started != 0 &&
			    k_cyc_to_us_floor32(now - dshot_info->tx_started) <
				    data->bdshot_busy_us) {
				continue;
			}

			/* Receive is over, put the pin back to output. The
			 * reconfiguration raises the shifter flag (buffer
			 * empty), so mask this one channel first, it is
			 * re-enabled below with the frame.
			 */
			key = irq_lock();
			FLEXIO_DisableShifterStatusInterrupts(flexio_base, shifter_flag);
			irq_unlock(key);

			nxp_flexio_dshot_output(dev, channel);
		}

		if (dshot_info->init && dshot_info->data_seg1 != 0) {
			tx_mask |= shifter_flag;
			tx_timer_mask |= 1 << child->res.timer_index[channel];
		}
	}

	FLEXIO_ClearTimerStatusFlags(flexio_base, tx_timer_mask);

	/* The ISR has to queue the second word within the first 24 sub-bits going
	 * out, so nothing may preempt between the buffer write and the interrupt
	 * enable. SHIFTSIEN and TIMIEN have no set/clear alias either, so the
	 * read-modify-write also has to be atomic against the ISR's own.
	 */
	key = irq_lock();

	for (uint8_t channel = 0; (channel < config->channel->dshot_channel_count); channel++) {
		if ((tx_mask & (1 << child->res.shifter_index[channel])) == 0) {
			continue;
		}

		dshot_info = &config->channel->dshot_info[channel];
		dshot_info->state = DSHOT_START;
		dshot_info->tx_started = now;
		flexio_base->SHIFTBUF[child->res.shifter_index[channel]] = dshot_info->data_seg1;
	}

	FLEXIO_EnableShifterStatusInterrupts(flexio_base, tx_mask);
	FLEXIO_EnableTimerStatusInterrupts(flexio_base, tx_timer_mask);
	irq_unlock(key);
}

static uint64_t nxp_flexio_dshot_hw_last_trigger_ns_get(const struct device *dev)
{
	const struct nxp_flexio_dshot_data *data = dev->data;

	return data->last_trigger_ns;
}

static uint8_t nxp_flexio_dshot_hw_channel_count(const struct device *dev)
{
	const struct nxp_flexio_dshot_config *config = dev->config;

	return config->channel->dshot_channel_count;
}

/* Expand packet from 16 bits 48 to get T0H and T1H timing */
uint64_t nxp_flexio_dshot_expand_data(uint16_t packet)
{
	unsigned int mask;
	unsigned int index = 0;
	uint64_t expanded = 0x0;

	for (mask = 0x8000; mask != 0; mask >>= 1) {
		if (packet & mask) {
			expanded = expanded | ((uint64_t)0x3 << index);

		} else {
			expanded = expanded | ((uint64_t)0x1 << index);
		}

		index = index + 3;
	}

	return expanded;
}

/**
 * bits 	1-11	- throttle value (0-47 are reserved, 48-2047 give 2000
 *steps of throttle resolution) bit 	12		- dshot telemetry
 *enable/disable bits 	13-16	- XOR checksum
 **/
static void nxp_flexio_dshot_hw_data_set(const struct device *dev, unsigned channel,
					 uint16_t throttle, bool telemetry)
{
	const struct nxp_flexio_dshot_config *config = dev->config;
	struct nxp_flexio_dshot_channel_config *dshot_info = &config->channel->dshot_info[channel];

	if (channel < config->channel->dshot_channel_count && dshot_info->init) {
		uint16_t csum_data;
		uint16_t packet = 0;
		uint16_t checksum = 0;

		/* Once a channel comes online with EDT enabled, spend a few
		 * frames sending the extended-telemetry-enable command (with the
		 * telemetry request bit set) before resuming throttle, so the
		 * ESC starts emitting EDT frames.
		 */
		if (dshot_info->edt_enable_repeats > 0) {
			dshot_info->edt_enable_repeats--;
			throttle = DSHOT_CMD_EXTENDED_TELEMETRY_ENABLE;
			telemetry = true;
		}

		packet |= throttle << DSHOT_THROTTLE_POSITION;
		packet |= ((uint16_t)telemetry & 0x01) << DSHOT_TELEMETRY_POSITION;

		if (dshot_info->bdshot) {
			csum_data = ~packet;

		} else {
			csum_data = packet;
		}

		/* XOR checksum calculation */
		csum_data >>= NIBBLES_SIZE;

		for (unsigned i = 0; i < DSHOT_NUMBER_OF_NIBBLES; i++) {
			checksum ^= (csum_data & 0x0F); // XOR data by nibbles
			csum_data >>= NIBBLES_SIZE;
		}

		packet |= (checksum & 0x0F);

		uint64_t dshot_expanded = nxp_flexio_dshot_expand_data(packet);

		/* The frame is only armed by the trigger, which first latches a
		 * response that is still pending on this channel.
		 */
		dshot_info->data_seg1 = (uint32_t)(dshot_expanded & 0xFFFFFF);
		dshot_info->irq_data = (uint32_t)(dshot_expanded >> 24);
	}
}

/*
 * Decode a raw 20-bit GCR response into the 12-bit DShot payload.
 * Returns true and stores the payload when framing, RLL/GCR and the nibble
 * checksum all pass, false otherwise. Kept side-effect free so it can be used
 * both by the training sweep and the normal eRPM decode.
 */
static bool nxp_flexio_bdshot_decode_gcr(uint32_t value, uint16_t *payload)
{
	uint32_t decode_data;
	uint32_t csum_data;

	/* if lowest significant bit isn't 1 we've got a framing error */
	if ((value & 0x1) == 0) {
		return false;
	}

	/* Decode RLL */
	value = value ^ (value >> 1);

	/* Decode GCR */
	decode_data = gcr_decode[value & 0x1fU];
	decode_data |= gcr_decode[(value >> 5U) & 0x1fU] << 4U;
	decode_data |= gcr_decode[(value >> 10U) & 0x1fU] << 8U;
	decode_data |= gcr_decode[(value >> 15U) & 0x1fU] << 12U;

	/* Calculate checksum */
	csum_data = decode_data;
	csum_data = csum_data ^ (csum_data >> 8U);
	csum_data = csum_data ^ (csum_data >> NIBBLES_SIZE);

	if ((csum_data & 0xFU) != 0xFU) {
		return false;
	}

	*payload = (decode_data >> 4) & 0xFFF;
	return true;
}

/* Convert a decoded 12-bit payload into eRPM. */
static uint16_t nxp_flexio_bdshot_payload_to_erpm(uint16_t payload)
{
	uint8_t exponent;
	uint16_t period;

	if (payload == 0xFFF) {
		return 0;
	}

	exponent = (payload >> 9U) & 0x7U; /* 3 bit: exponent */
	period = payload & 0x1ffU;         /* 9 bit: period base */
	period = period << exponent;       /* Period in usec */

	if (period == 0) {
		return 0;
	}

	return (uint16_t)((1000000U * 60U / 100U + period / 2U) / period);
}

/* An ESC that stops responding goes offline but never forces a re-sweep. */
static void nxp_flexio_bdshot_note_success(struct nxp_flexio_dshot_channel_config *ch)
{
	ch->consecutive_failures = 0;

	if (ch->consecutive_successes < BDSHOT_OFFLINE_COUNT) {
		ch->consecutive_successes++;
	}

	if (ch->consecutive_successes >= BDSHOT_OFFLINE_COUNT) {
		ch->online = true;
	}

	if (!ch->online) {
		return;
	}

	if (ch->edt_online_ms == 0) {
		ch->edt_online_ms = k_uptime_get_32();
	}

	/* The ESC only takes the enable command once it has been idle for a
	 * while, so hold off after it comes online and put a second between the
	 * attempts. Give up after a few, and stop as soon as it answers.
	 */
	if (ch->edt && !ch->edt_confirmed && ch->edt_enable_repeats == 0 &&
	    ch->edt_attempts < BDSHOT_EDT_MAX_ATTEMPTS &&
	    (k_uptime_get_32() - ch->edt_online_ms) >=
		    BDSHOT_EDT_REQUEST_DELAY_MS * (ch->edt_attempts + 1U)) {
		ch->edt_enable_repeats = BDSHOT_EDT_ENABLE_REPEATS;
		ch->edt_attempts++;
	}
}

static void nxp_flexio_bdshot_restart_training(const struct device *dev, uint32_t channel)
{
	struct nxp_flexio_dshot_channel_config *ch =
		&((const struct nxp_flexio_dshot_config *)dev->config)
			 ->channel->dshot_info[channel];

	ch->bdshot_training_done = false;
	ch->bdshot_training_mask = 0;
	ch->bdshot_training_count = 0;
	ch->bdshot_training_success = 0;
	ch->bdshot_tcmp_offset = BDSHOT_TCMP_MIN_OFFSET;
	ch->consecutive_successes = 0;
	ch->consecutive_failures = 0;
	ch->online = false;
	nxp_flexio_dshot_set_tcmp(dev, channel);
}

/*
 * A missing response means the ESC is gone, not that the baud is wrong: it
 * takes the channel offline but never restarts the sweep. Only frames that
 * arrive and fail to decode can, after a second offline period, retrigger it.
 */
static void nxp_flexio_bdshot_note_failure(const struct device *dev, uint32_t channel,
					   bool decoded_wrong)
{
	struct nxp_flexio_dshot_channel_config *ch =
		&((const struct nxp_flexio_dshot_config *)dev->config)
			 ->channel->dshot_info[channel];

	if (!ch->bdshot_training_done) {
		return;
	}

	ch->consecutive_successes = 0;
	uint16_t limit = decoded_wrong ? BDSHOT_RETRAIN_COUNT : BDSHOT_OFFLINE_COUNT;

	if (ch->consecutive_failures < limit) {
		ch->consecutive_failures++;
	}

	if (ch->consecutive_failures >= BDSHOT_OFFLINE_COUNT) {
		/* Offline clears the EDT state: the ESC forgets the setting when
		 * it stops, so the next online period has to request it again.
		 */
		ch->online = false;
		ch->edt_online_ms = 0;
		ch->edt_attempts = 0;
		ch->edt_confirmed = false;
	}

	if (decoded_wrong && ch->consecutive_failures >= BDSHOT_RETRAIN_COUNT) {
		nxp_flexio_bdshot_restart_training(dev, channel);
	}
}

/*
 * Sweep the receive-baud offset and settle on the centre of the range that
 * decodes cleanly. Called once per response while training is not yet done.
 */
static void nxp_flexio_bdshot_train(const struct device *dev, uint32_t channel, uint32_t value)
{
	struct nxp_flexio_dshot_channel_config *ch =
		&((const struct nxp_flexio_dshot_config *)dev->config)
			 ->channel->dshot_info[channel];
	uint16_t payload;

	if (nxp_flexio_bdshot_decode_gcr(value, &payload)) {
		/* Count successful responses at this offset */
		ch->bdshot_training_success++;

	} else if ((value & 0x1) == 0) {
		/* Framing error invalidates this offset immediately */
		ch->bdshot_training_count = BDSHOT_TRAINING_TRIES - 1;
	}

	ch->bdshot_training_count++;

	if (ch->bdshot_training_count < BDSHOT_TRAINING_TRIES) {
		return;
	}

	if (ch->bdshot_training_success >= BDSHOT_TRAINING_SUCCESS) {
		ch->bdshot_training_mask |= (1u << BDSHOT_TCMP_TO_MASK(ch->bdshot_tcmp_offset));
	}

	ch->bdshot_training_count = 0;
	ch->bdshot_training_success = 0;
	ch->bdshot_tcmp_offset++;

	if (ch->bdshot_tcmp_offset > BDSHOT_TCMP_MAX_OFFSET) {
		if (ch->bdshot_training_mask == 0) {
			/* No good offsets found, sweep again */
			ch->bdshot_tcmp_offset = BDSHOT_TCMP_MIN_OFFSET;

		} else {
			/* Lock onto the centre of the longest run of clean offsets.
			 * Isolated clean offsets far from that run are aliases where
			 * the sampler lands on a different edge; spanning them would
			 * put the centre in a gap that never decoded cleanly. */
			int best_low = 0;
			int best_len = 0;
			int run_low = 0;
			int run_len = 0;

			for (int bit = 0; bit < 32; bit++) {
				if (ch->bdshot_training_mask & (1u << bit)) {
					if (run_len == 0) {
						run_low = bit;
					}
					run_len++;
					if (run_len > best_len) {
						best_len = run_len;
						best_low = run_low;
					}
				} else {
					run_len = 0;
				}
			}
			ch->bdshot_tcmp_offset =
				(best_low + (best_len - 1) / 2) + BDSHOT_TCMP_MIN_OFFSET;
			ch->bdshot_training_done = true;
			ch->consecutive_failures = 0;
			ch->consecutive_successes = BDSHOT_OFFLINE_COUNT;
			ch->online = true;
		}
	}

	nxp_flexio_dshot_set_tcmp(dev, channel);
}

/*
 * Store an EDT frame's value into the matching per-channel field. Frames that
 * are not eRPM carry a type nibble and an 8-bit value; unknown types (debug,
 * state/event) are ignored. Returns true if the frame was a recognised EDT
 * sub-value.
 */
static bool nxp_flexio_bdshot_store_edt(struct nxp_flexio_dshot_channel_config *ch,
					uint16_t payload)
{
	uint8_t type = (payload & BDSHOT_EDT_TYPE_MASK) >> BDSHOT_EDT_TYPE_SHIFT;
	uint8_t value = payload & BDSHOT_EDT_VALUE_MASK;

	switch (type) {
	case DSHOT_EDT_TEMPERATURE:
		ch->edt_temperature = value;
		ch->edt_valid |= BDSHOT_EDT_VALID_TEMPERATURE;
		return true;
	case DSHOT_EDT_VOLTAGE:
		ch->edt_voltage = value;
		ch->edt_valid |= BDSHOT_EDT_VALID_VOLTAGE;
		return true;
	case DSHOT_EDT_CURRENT:
		ch->edt_current = value;
		ch->edt_valid |= BDSHOT_EDT_VALID_CURRENT;
		return true;
	default:
		return false;
	}
}

/*
 * Decode captured responses for every channel and, while a channel is still
 * training, feed its sweep. Publishes eRPM through SENSOR_CHAN_RPM. Called from
 * the sensor sample_fetch handler.
 */
static int nxp_flexio_bdshot_decode_erpm(const struct device *dev)

{
	const struct nxp_flexio_dshot_config *config = dev->config;
	struct nxp_flexio_dshot_data *data = dev->data;
	struct nxp_flexio_child *child = (struct nxp_flexio_child *)(config->child);
	uint32_t value;
	uint16_t payload;
	uint32_t shifter_flag;

	data->bdshot_parsed_recv_mask = 0;

	// Decode each individual channel
	for (uint8_t channel = 0; (channel < config->channel->dshot_channel_count); channel++) {
		struct nxp_flexio_dshot_channel_config *dshot_info =
			&config->channel->dshot_info[channel];

		shifter_flag = 1 << child->res.shifter_index[channel];

		if ((data->bdshot_recv_mask & shifter_flag) == 0) {
			/* No response captured on this channel this cycle */
			if (dshot_info->bdshot) {
				nxp_flexio_bdshot_note_failure(dev, channel, false);
			}
			continue;
		}

		value = ~dshot_info->raw_response & 0xFFFFF;

		/* While the receive baud has not been trained yet, feed the
		 * sweep instead of publishing eRPM.
		 */
		if (dshot_info->bdshot && !dshot_info->bdshot_training_done) {
			nxp_flexio_bdshot_train(dev, channel, value);
			continue;
		}

		if (!nxp_flexio_bdshot_decode_gcr(value, &payload)) {
			if ((value & 0x1) == 0) {
				dshot_info->frame_error_cnt++;
			} else {
				dshot_info->crc_error_cnt++;
			}

			if (dshot_info->bdshot) {
				nxp_flexio_bdshot_note_failure(dev, channel, true);
			}
			continue;
		}

		/* An EDT frame carries temperature/voltage/current instead of
		 * eRPM (mantissa MSB clear). Store it and, since it is still a
		 * valid decode, count it toward link health but do not touch
		 * the eRPM value or the parsed mask.
		 */
		if (dshot_info->edt && (payload & BDSHOT_EDT_MANTISSA_MSB) == 0) {
			if (nxp_flexio_bdshot_store_edt(dshot_info, payload) &&
			    dshot_info->edt_attempts > 0) {
				/* Only a frame that followed one of our own
				 * requests proves the ESC took the command.
				 */
				dshot_info->edt_confirmed = true;
			}

			nxp_flexio_bdshot_note_success(dshot_info);
			continue;
		}

		dshot_info->erpm = nxp_flexio_bdshot_payload_to_erpm(payload);
		dshot_info->decoded_cnt++;
		data->bdshot_parsed_recv_mask |= shifter_flag;
		dshot_info->last_no_response_cnt = dshot_info->no_response_cnt;

		if (dshot_info->bdshot) {
			nxp_flexio_bdshot_note_success(dshot_info);
		}
	}

	return data->bdshot_parsed_recv_mask != 0;
}

static int nxp_flexio_dshot_isr(void *user_data)
{
	const struct device *dev = (const struct device *)user_data;
	const struct nxp_flexio_dshot_config *config = dev->config;
	struct nxp_flexio_dshot_data *data = dev->data;
	FLEXIO_Type *flexio_base = (FLEXIO_Type *)(config->flexio_base);
	struct nxp_flexio_child *child = (struct nxp_flexio_child *)(config->child);

	/* A status flag whose interrupt is masked belongs to a phase that already
	 * completed, so only the enabled ones are events for us.
	 */
	uint32_t flags = FLEXIO_GetShifterStatusFlags(flexio_base) & flexio_base->SHIFTSIEN;
	uint32_t channel;
	uint32_t shifter_flag;
	uint32_t timer_flag;
	struct nxp_flexio_dshot_channel_config *dshot_info;

	for (channel = 0; flags && channel < config->channel->dshot_channel_count; channel++) {
		shifter_flag = 1 << child->res.shifter_index[channel];

		if (flags & shifter_flag) {
			flags &= ~shifter_flag;
			dshot_info = &config->channel->dshot_info[channel];

			/* One event per phase: the second buffer load while
			 * transmitting, the frame while receiving.
			 */
			if (dshot_info->state == DSHOT_START) {
				FLEXIO_DisableShifterStatusInterrupts(flexio_base, shifter_flag);
				dshot_info->state = DSHOT_12BIT_FIFO;
				flexio_base->SHIFTBUF[child->res.shifter_index[channel]] =
					dshot_info->irq_data;
			} else if (dshot_info->state == BDSHOT_RECEIVE) {
				FLEXIO_DisableShifterStatusInterrupts(flexio_base, shifter_flag);
				dshot_info->state = BDSHOT_RECEIVE_COMPLETE;
				dshot_info->raw_response =
					flexio_base->SHIFTBUFBIS[child->res.shifter_index[channel]];

				data->bdshot_recv_mask |= shifter_flag;

				if (data->bdshot_recv_mask == data->dshot_mask) {
					// Received telemetry on all channels
					// Schedule workqueue?
					// up_bdshot_erpm(dev);

					// Trigger RTIO stream?
				}
			}
		}
	}

	flags = FLEXIO_GetTimerStatusFlags(flexio_base) & flexio_base->TIMIEN;

	for (channel = 0; (flags & data->dshot_timer_mask);
	     (channel = (channel + 1) % config->channel->dshot_channel_count)) {
		flags = FLEXIO_GetTimerStatusFlags(flexio_base) & flexio_base->TIMIEN;
		timer_flag = 1 << child->res.timer_index[channel];

		if (flags & timer_flag) {
			FLEXIO_ClearTimerStatusFlags(flexio_base, timer_flag);
			dshot_info = &config->channel->dshot_info[channel];

			if (dshot_info->state == DSHOT_12BIT_FIFO) {
				dshot_info->state = DSHOT_12BIT_TRANSFERRED;

			} else if (!dshot_info->bdshot &&
				   dshot_info->state == DSHOT_12BIT_TRANSFERRED) {
				dshot_info->state = DSHOT_TRANSMIT_COMPLETE;

			} else if (dshot_info->bdshot &&
				   dshot_info->state == DSHOT_12BIT_TRANSFERRED) {
				shifter_flag = 1 << child->res.shifter_index[channel];

				/* The frame is out: only the shifter flag matters
				 * until the next transmit is armed, the receive
				 * timer compares must not re-enter the IRQ.
				 */
				FLEXIO_DisableTimerStatusInterrupts(flexio_base, timer_flag);
				FLEXIO_DisableShifterStatusInterrupts(flexio_base, shifter_flag);
				dshot_info->state = BDSHOT_RECEIVE;

				/* Transmit done, disable timer and reconfigure to receive*/
				flexio_base->TIMCTL[config->child->res.timer_index[channel]] = 0;

				/* Configure shifter and timer to receive data */
				nxp_flexio_bdshot_input(dev, channel);

				/* Enable shifter interrupt for receiving data */
				FLEXIO_EnableShifterStatusInterrupts(flexio_base, shifter_flag);
			}
		}
	}
	return 0;
}

/*
 * Fetch decodes the captured bdshot responses (and drives training). Link
 * health is reported through the sensor read return code: -ENODATA means no
 * bidirectional channel produced a valid eRPM this cycle (all offline or still
 * training), so a caller polling sensor_sample_fetch() sees the read fail while
 * the ESCs are offline and succeed once they come online.
 */
static int nxp_flexio_dshot_sample_fetch(const struct device *dev, enum sensor_channel chan)
{
	if (chan != SENSOR_CHAN_ALL && chan != SENSOR_CHAN_RPM) {
		return -ENOTSUP;
	}

	return nxp_flexio_bdshot_decode_erpm(dev) ? 0 : -ENODATA;
}

static int nxp_flexio_dshot_channel_get(const struct device *dev, enum sensor_channel chan,
					struct sensor_value *val)
{
	const struct nxp_flexio_dshot_config *config = dev->config;
	struct nxp_flexio_dshot_data *data = dev->data;

	switch (chan) {
	case SENSOR_CHAN_RPM:
		for (uint32_t i = 0; i < config->channel->dshot_channel_count; i++) {
			val[i].val1 = (int32_t)config->channel->dshot_info[i].erpm;
			val[i].val2 = 0;
		}

		/* Report link health as a read failure: if no bidirectional
		 * channel decoded a fresh eRPM, the ESCs are offline.
		 */
		if (data->bdshot_parsed_recv_mask == 0) {
			return -ENODATA;
		}
		break;

	/*
	 * Extended DShot Telemetry channels, one sensor_value per output.
	 * Values are reported in real units (degrees C, volts, amps). Each sub-value
	 * is reported once: the read returns -ENODATA unless a channel decoded a
	 * fresh one since the last read, so a corrupt frame that slips through the
	 * 4-bit checksum shows up once instead of sticking.
	 */
	case SENSOR_CHAN_DIE_TEMP: {
		bool any = false;

		for (uint32_t i = 0; i < config->channel->dshot_channel_count; i++) {
			struct nxp_flexio_dshot_channel_config *ch =
				&config->channel->dshot_info[i];

			val[i].val1 = (int32_t)ch->edt_temperature;
			val[i].val2 = 0;
			any |= (ch->edt_valid & BDSHOT_EDT_VALID_TEMPERATURE) != 0;
			ch->edt_valid &= ~BDSHOT_EDT_VALID_TEMPERATURE;
		}

		if (!any) {
			return -ENODATA;
		}
		break;
	}

	case SENSOR_CHAN_VOLTAGE: {
		bool any = false;

		for (uint32_t i = 0; i < config->channel->dshot_channel_count; i++) {
			struct nxp_flexio_dshot_channel_config *ch =
				&config->channel->dshot_info[i];

			/* 0.25 V per step -> volts in val1, remainder as micro-volts */
			val[i].val1 = ch->edt_voltage / 4;
			val[i].val2 = (ch->edt_voltage % 4) * 250000;
			any |= (ch->edt_valid & BDSHOT_EDT_VALID_VOLTAGE) != 0;
			ch->edt_valid &= ~BDSHOT_EDT_VALID_VOLTAGE;
		}

		if (!any) {
			return -ENODATA;
		}
		break;
	}

	case SENSOR_CHAN_CURRENT: {
		bool any = false;

		for (uint32_t i = 0; i < config->channel->dshot_channel_count; i++) {
			struct nxp_flexio_dshot_channel_config *ch =
				&config->channel->dshot_info[i];

			val[i].val1 = (int32_t)ch->edt_current;
			val[i].val2 = 0;
			any |= (ch->edt_valid & BDSHOT_EDT_VALID_CURRENT) != 0;
			ch->edt_valid &= ~BDSHOT_EDT_VALID_CURRENT;
		}

		if (!any) {
			return -ENODATA;
		}
		break;
	}

	default:
		return -EINVAL;
	}

	return 0;
}

static const struct nxp_flexio_dshot_driver_api nxp_flexio_dshot_api_funcs = {
	.sensor =
		{
			.sample_fetch = nxp_flexio_dshot_sample_fetch,
			.channel_get = nxp_flexio_dshot_channel_get,
		},
	.data_set = nxp_flexio_dshot_hw_data_set,
	.trigger = nxp_flexio_dshot_hw_trigger,
	.last_trigger_ns_get = nxp_flexio_dshot_hw_last_trigger_ns_get,
	.channel_count = nxp_flexio_dshot_hw_channel_count,
};

/* EDT requires bidirectional dshot; ignore the property otherwise. */
#define _FLEXIO_DSHOT_GEN_CONFIG(n)                                                                \
	{                                                                                          \
		.pin_id = DT_PROP(n, pin_id),                                                      \
		.bdshot = DT_PROP(n, bidirectional_dshot),                                         \
		.edt = DT_PROP(n, bidirectional_dshot) && DT_PROP(n, extended_telemetry),          \
	},

#define FLEXIO_DSHOT_GEN_CONFIG(n)                                                                 \
	static struct nxp_flexio_dshot_channel_config flexio_dshot_##n##_init[] = {                \
		DT_INST_FOREACH_CHILD_STATUS_OKAY(n, _FLEXIO_DSHOT_GEN_CONFIG)};                   \
	static const struct nxp_flexio_dshot_channel flexio_dshot_##n##_info = {                   \
		.dshot_channel_count = ARRAY_SIZE(flexio_dshot_##n##_init),                        \
		.dshot_info = flexio_dshot_##n##_init,                                             \
	};

#define FLEXIO_DSHOT_FLEXIO_INDEX_INIT(n)                                                          \
	static uint8_t flexio_dshot_##n##_timer_index[ARRAY_SIZE(flexio_dshot_##n##_init)];        \
	static uint8_t flexio_dshot_##n##_shifter_index[ARRAY_SIZE(flexio_dshot_##n##_init)];

#define FLEXIO_DSHOT_CHILD_CONFIG(n)                                                               \
	static const struct nxp_flexio_child mcux_flexio_dshot_child_##n = {                       \
		.isr = nxp_flexio_dshot_isr,                                                       \
		.user_data = (void *)DEVICE_DT_INST_GET(n),                                        \
		.res = {.shifter_index = (uint8_t *)flexio_dshot_##n##_shifter_index,              \
			.shifter_count = ARRAY_SIZE(flexio_dshot_##n##_init),                      \
			.timer_index = (uint8_t *)flexio_dshot_##n##_timer_index,                  \
			.timer_count = ARRAY_SIZE(flexio_dshot_##n##_init)}};

#define FLEXIO_DSHOT_GEN_GET_CONFIG(n) .channel = &flexio_dshot_##n##_info,

#define NXP_FLEXIO_DSHOT_INIT(n)                                                                   \
	PINCTRL_DT_INST_DEFINE(n);                                                                 \
	FLEXIO_DSHOT_GEN_CONFIG(n)                                                                 \
	FLEXIO_DSHOT_FLEXIO_INDEX_INIT(n)                                                          \
	FLEXIO_DSHOT_CHILD_CONFIG(n)                                                               \
	static const struct nxp_flexio_dshot_config nxp_flexio_dshot_config_##n = {                \
		.flexio_dev = DEVICE_DT_GET(DT_INST_PARENT(n)),                                    \
		.flexio_base = (FLEXIO_Type *)DT_REG_ADDR(DT_INST_PARENT(n)),                      \
		.pincfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),                                       \
		.clock_dev = DEVICE_DT_GET(DT_CLOCKS_CTLR(DT_INST_PARENT(n))),                     \
		.clock_subsys = (clock_control_subsys_t)DT_CLOCKS_CELL(DT_INST_PARENT(n), name),   \
		.child = &mcux_flexio_dshot_child_##n,                                             \
		.speed = DT_INST_PROP(n, speed),                                                   \
		FLEXIO_DSHOT_GEN_GET_CONFIG(n)};                                                   \
                                                                                                   \
	static struct nxp_flexio_dshot_data nxp_flexio_dshot_data_##n;                             \
	SENSOR_DEVICE_DT_INST_DEFINE(n, &nxp_flexio_dshot_init, NULL, &nxp_flexio_dshot_data_##n,  \
				     &nxp_flexio_dshot_config_##n, POST_KERNEL,                    \
				     DSHOT_INIT_PRIORITY, &nxp_flexio_dshot_api_funcs);

DT_INST_FOREACH_STATUS_OKAY(NXP_FLEXIO_DSHOT_INIT)

#if defined(CONFIG_SHELL) && DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)
/* Per-channel bidirectional DShot link state. */
static int cmd_dshot_status(const struct shell *sh, size_t argc, char **argv)
{
	const struct device *dev = DEVICE_DT_INST_GET(0);
	const struct nxp_flexio_dshot_config *config = dev->config;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	for (uint8_t i = 0; i < config->channel->dshot_channel_count; i++) {
		const struct nxp_flexio_dshot_channel_config *ch = &config->channel->dshot_info[i];

		shell_print(sh,
			    "ch%u bdshot=%d edt=%d online=%d trained=%d tcmp=%u offset=%d ok=%u "
			    "no_resp=%u crc_err=%u frame_err=%u erpm=%u edt_valid=0x%x",
			    i, ch->bdshot, ch->edt, ch->online, ch->bdshot_training_done,
			    ch->bdshot_tcmp, ch->bdshot_tcmp_offset, ch->decoded_cnt,
			    ch->no_response_cnt, ch->crc_error_cnt, ch->frame_error_cnt, ch->erpm,
			    ch->edt_valid);
	}

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(dshot_cmds,
			       SHELL_CMD(status, NULL, "Bidirectional DShot per-channel state.",
					 cmd_dshot_status),
			       SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(dshot, &dshot_cmds, "DShot driver diagnostics.", NULL);
#endif
