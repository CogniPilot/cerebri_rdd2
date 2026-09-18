/* SPDX-License-Identifier: Apache-2.0 */

#ifndef RDD2_POWER_SOURCE_ESTIMATE_H_
#define RDD2_POWER_SOURCE_ESTIMATE_H_

/*
 * Battery state of charge and consumed charge, kept free of Zephyr so the
 * host self-check in tests/power_source_estimate builds the same code.
 */

#include <math.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Open-circuit voltage of a LiPo cell against state of charge, the table the
 * Yaapu telemetry script and the common RC battery monitors use for their
 * voltage-only fuel gauge (4.20 V full, 3.30 V empty, flat between 3.9 V and
 * 3.7 V). Interpolated linearly between the points, descending.
 */
struct rdd2_power_ocv_point {
	float volts;
	uint8_t pct;
};

static const struct rdd2_power_ocv_point rdd2_power_ocv[] = {
	{4.20f, 100}, {4.10f, 90}, {4.00f, 75}, {3.90f, 58}, {3.80f, 40},
	{3.70f, 20},  {3.60f, 8},  {3.50f, 3},  {3.30f, 0},
};

/** State of charge in percent for an open-circuit cell voltage. */
static inline int rdd2_power_pct_from_cell_volts(float per_cell)
{
	size_t i;

	if (per_cell >= rdd2_power_ocv[0].volts) {
		return 100;
	}
	for (i = 1U; i < sizeof(rdd2_power_ocv) / sizeof(rdd2_power_ocv[0]); ++i) {
		const struct rdd2_power_ocv_point *hi = &rdd2_power_ocv[i - 1U];
		const struct rdd2_power_ocv_point *lo = &rdd2_power_ocv[i];

		if (per_cell >= lo->volts) {
			return (int)lroundf((float)lo->pct + (per_cell - lo->volts) *
								    (float)(hi->pct - lo->pct) /
								    (hi->volts - lo->volts));
		}
	}
	return 0;
}

/*
 * Remaining percent of a pack of cells at volts while it draws amps.
 *
 * ponytail: the load sag is undone with one fixed internal resistance per
 * cell (esr_mohm) instead of a measured pack impedance, so a cold, old or
 * high-C pack is compensated by the wrong few percent and the estimate stays
 * voltage-only, with no notion of the pack's capacity or its history. Upgrade
 * path is a smart BMS reporting its own state of charge behind rdd2_power_get.
 */
static inline int8_t rdd2_power_remaining_pct(float volts, float amps, int cells, int esr_mohm)
{
	float per_cell;
	int pct;

	if (cells < 1) {
		return 0;
	}
	/* Only a discharge sags the pack; a negative reading is monitor offset. */
	if (amps < 0.0f) {
		amps = 0.0f;
	}
	per_cell = volts / (float)cells + amps * (float)esr_mohm * 0.001f;
	pct = rdd2_power_pct_from_cell_volts(per_cell);
	return (int8_t)(pct < 0 ? 0 : (pct > 100 ? 100 : pct));
}

/** Charge drawn over dt_ms added to the mAh count, which never goes negative. */
static inline float rdd2_power_accumulate_mah(float consumed_mah, float amps, uint32_t dt_ms)
{
	float next = consumed_mah + amps * (float)dt_ms / 3600.0f;

	return next > 0.0f ? next : 0.0f;
}

#endif /* RDD2_POWER_SOURCE_ESTIMATE_H_ */
