/* SPDX-License-Identifier: Apache-2.0 */

#include "interfaces/drivers.h"
#include "power_source_estimate.h"

#include <math.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(rdd2_power_source, CONFIG_RDD2_POWER_SOURCE_LOG_LEVEL);

#define POWER_PERIOD_MS 100

static const struct device *const power_dev = DEVICE_DT_GET(DT_ALIAS(power0));

/* Voltage and current are published as one word so a reader can never pair a
 * voltage from one sample with a current from the next. */
static atomic_t g_voltage_current;
static atomic_t g_remaining_pct;

/* Series cell count, latched from the first plausible sample. */
static int g_cells;

/* Charge drawn since boot, owned by the work handler and published rounded. */
static float g_consumed_mah_acc;
static atomic_t g_consumed_mah;
static int64_t g_last_sample_ms;

/*
 * ponytail: the cell count is inferred assuming the pack is near full at
 * power-on, so a 6S pack connected already at rest voltage reads as 5S.
 * Upgrade path is a smart BMS reporting its own cell count and state of
 * charge behind rdd2_power_get.
 *
 * No pack capacity is configured anywhere, so the percentage stays the
 * voltage-derived one; the coulomb count below only reports charge drawn.
 */
static int8_t remaining_pct(float volts, float amps)
{
	if (g_cells == 0) {
		if (volts < 3.0f) {
			return 0;
		}
		g_cells = MAX((int)lroundf(volts / 4.2f), 1);
	}

	return rdd2_power_remaining_pct(volts, amps, g_cells,
					CONFIG_RDD2_POWER_SOURCE_ESR_MOHM);
}

static void power_work_handler(struct k_work *work)
{
	struct sensor_value voltage = {0};
	struct sensor_value current = {0};
	float volts;
	float amps;
	int64_t now_ms;
	uint32_t dt_ms;
	long voltage_cv;
	long current_da;

	ARG_UNUSED(work);

	if (sensor_sample_fetch(power_dev) != 0) {
		LOG_WRN_RATELIMIT_RATE(30000, "power monitor sample fetch failed");
		return;
	}

	(void)sensor_channel_get(power_dev, SENSOR_CHAN_VOLTAGE, &voltage);
	(void)sensor_channel_get(power_dev, SENSOR_CHAN_CURRENT, &current);

	volts = sensor_value_to_float(&voltage);
	amps = sensor_value_to_float(&current);
	voltage_cv = lroundf(volts * 100.0f);
	current_da = lroundf(amps * 10.0f);
	voltage_cv = CLAMP(voltage_cv, 0, UINT16_MAX);
	current_da = CLAMP(current_da, INT16_MIN, INT16_MAX);

	atomic_set(&g_voltage_current,
		   (atomic_val_t)(((uint32_t)voltage_cv << 16) | (uint16_t)(int16_t)current_da));
	atomic_set(&g_remaining_pct, remaining_pct(volts, amps));

	/* Integrate over the time really elapsed rather than the nominal
	 * period, so a late work item still counts its charge; a stall longer
	 * than a second counts as a second instead of a jump. */
	now_ms = k_uptime_get();
	dt_ms = g_last_sample_ms == 0 ? POWER_PERIOD_MS
				      : (uint32_t)MIN(now_ms - g_last_sample_ms, 1000);
	g_last_sample_ms = now_ms;
	g_consumed_mah_acc = rdd2_power_accumulate_mah(g_consumed_mah_acc, amps, dt_ms);
	atomic_set(&g_consumed_mah, (atomic_val_t)(uint32_t)g_consumed_mah_acc);
}

static K_WORK_DEFINE(g_power_work, power_work_handler);

static void power_timer_handler(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	k_work_submit(&g_power_work);
}

static K_TIMER_DEFINE(power_timer, power_timer_handler, NULL);

void rdd2_power_get(uint16_t *voltage_cv, int16_t *current_da, int8_t *remaining,
		    uint32_t *consumed_mah)
{
	uint32_t packed = (uint32_t)atomic_get(&g_voltage_current);

	*voltage_cv = (uint16_t)(packed >> 16);
	*current_da = (int16_t)(uint16_t)packed;
	*remaining = (int8_t)atomic_get(&g_remaining_pct);
	if (consumed_mah != NULL) {
		*consumed_mah = (uint32_t)atomic_get(&g_consumed_mah);
	}
}

static int power_source_init(void)
{
	if (!device_is_ready(power_dev)) {
		LOG_WRN("power monitor %s not ready, battery telemetry stays zero",
			power_dev->name);
		return 0;
	}

	k_timer_start(&power_timer, K_MSEC(POWER_PERIOD_MS), K_MSEC(POWER_PERIOD_MS));
	LOG_INF("power source ready");
	return 0;
}

SYS_INIT(power_source_init, APPLICATION, 0);
