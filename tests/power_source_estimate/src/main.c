/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Self-check for the battery state of charge curve and the coulomb count.
 *
 * Builds both as a Zephyr test (native_sim) and standalone:
 *   cc -std=c99 -Wall -Wextra -I subsys/power_source \
 *      tests/power_source_estimate/src/main.c -lm
 */

#include "power_source_estimate.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(cond)                                                                                \
	do {                                                                                       \
		if (!(cond)) {                                                                     \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                     \
			exit(1);                                                                    \
		}                                                                                  \
	} while (0)

int main(void)
{
	float volts;
	float mah;
	int i;
	int prev;

	/* Endpoints and every table point, on a 4S pack, unloaded. */
	CHECK(rdd2_power_remaining_pct(4.30f * 4.0f, 0.0f, 4, 5) == 100);
	CHECK(rdd2_power_remaining_pct(4.20f * 4.0f, 0.0f, 4, 5) == 100);
	CHECK(rdd2_power_remaining_pct(3.30f * 4.0f, 0.0f, 4, 5) == 0);
	CHECK(rdd2_power_remaining_pct(2.00f * 4.0f, 0.0f, 4, 5) == 0);
	CHECK(rdd2_power_remaining_pct(0.0f, 0.0f, 0, 5) == 0);
	for (i = 0; i < (int)(sizeof(rdd2_power_ocv) / sizeof(rdd2_power_ocv[0])); ++i) {
		CHECK(rdd2_power_remaining_pct(rdd2_power_ocv[i].volts * 4.0f, 0.0f, 4, 5) ==
		      (int8_t)rdd2_power_ocv[i].pct);
	}

	/* Monotonic in voltage, and the midpoint of a segment interpolates. */
	prev = -1;
	for (volts = 3.0f; volts <= 4.4f; volts += 0.01f) {
		int pct = rdd2_power_remaining_pct(volts * 4.0f, 0.0f, 4, 5);

		CHECK(pct >= prev);
		CHECK(pct >= 0 && pct <= 100);
		prev = pct;
	}
	CHECK(rdd2_power_remaining_pct(3.85f * 4.0f, 0.0f, 4, 5) == 49); /* 40 + 58 halved */

	/* Sag compensation lifts the estimate under load and only under load,
	 * and a zero resistance disables it. 40 A through 5 mOhm is 0.2 V per
	 * cell, which is 3.80 V reading back as the 4.00 V point. */
	CHECK(rdd2_power_remaining_pct(3.80f * 4.0f, 40.0f, 4, 5) == 75);
	CHECK(rdd2_power_remaining_pct(3.80f * 4.0f, 40.0f, 4, 0) == 40);
	CHECK(rdd2_power_remaining_pct(3.80f * 4.0f, 10.0f, 4, 5) >
	      rdd2_power_remaining_pct(3.80f * 4.0f, 0.0f, 4, 5));
	CHECK(rdd2_power_remaining_pct(3.80f * 4.0f, -10.0f, 4, 5) ==
	      rdd2_power_remaining_pct(3.80f * 4.0f, 0.0f, 4, 5));

	/* 1 A for 3600 s is 1000 mAh, counted in 100 ms steps. */
	mah = 0.0f;
	for (i = 0; i < 36000; ++i) {
		mah = rdd2_power_accumulate_mah(mah, 1.0f, 100U);
	}
	CHECK(mah > 999.0f && mah < 1001.0f);

	/* The count never runs backwards past zero on a negative reading. */
	CHECK(rdd2_power_accumulate_mah(0.0f, -10.0f, 100U) == 0.0f);
	CHECK(rdd2_power_accumulate_mah(1000.0f, 0.0f, 100U) == 1000.0f);

	printf("mah=%.1f pct(3.85V/cell)=%d pct(3.80V/cell,40A)=%d\n", (double)mah,
	       rdd2_power_remaining_pct(3.85f * 4.0f, 0.0f, 4, 5),
	       rdd2_power_remaining_pct(3.80f * 4.0f, 40.0f, 4, 5));
	printf("PASS\n");
	return 0;
}
