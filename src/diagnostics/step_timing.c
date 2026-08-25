/* SPDX-License-Identifier: Apache-2.0 */

#include "step_timing.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(rdd2_step_timing, CONFIG_LOG_DEFAULT_LEVEL);

#define RDD2_STEP_TIMING_MAX_POINTS 4
#define RDD2_STEP_TIMING_REPORT_PERIOD_MS 5000

static struct rdd2_step_timing *g_points[RDD2_STEP_TIMING_MAX_POINTS];
static uint8_t g_point_count;
static bool g_counter_available;
static struct k_spinlock g_lock;

static bool g_initialized;

bool rdd2_step_timing_init(void)
{
	uint32_t first;
	uint32_t second;

	if (g_initialized) {
		return g_counter_available;
	}
	g_initialized = true;
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CYCCNT = 0U;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

	/* A locked DWT, a part without the cycle counter, or an emulator that
	 * claims the register block all leave CYCCNT stuck. Prove it moves
	 * before trusting anything it reports. */
	first = DWT->CYCCNT;
	for (volatile int spin = 0; spin < 64; ++spin) {
	}
	second = DWT->CYCCNT;
	g_counter_available = second != first;
	if (!g_counter_available) {
		LOG_WRN("DWT cycle counter is not advancing; step timing disabled");
	}
	return g_counter_available;
}

static void clear_locked(struct rdd2_step_timing *timing)
{
	timing->max_cycles = 0U;
	timing->samples = 0U;
	timing->overruns = 0U;
	timing->total_cycles = 0U;
	for (size_t bucket = 0U; bucket < RDD2_STEP_TIMING_BUCKETS; ++bucket) {
		timing->buckets[bucket] = 0U;
	}
}

void rdd2_step_timing_register(struct rdd2_step_timing *timing, const char *name,
			       uint32_t release_rate_hz)
{
	k_spinlock_key_t key = k_spin_lock(&g_lock);

	timing->name = name;
	timing->release_rate_hz = release_rate_hz;
	timing->budget_cycles = release_rate_hz != 0U ? SystemCoreClock / release_rate_hz : 0U;
	clear_locked(timing);
	if (g_point_count < RDD2_STEP_TIMING_MAX_POINTS) {
		g_points[g_point_count++] = timing;
	}
	k_spin_unlock(&g_lock, key);
}

/*
 * Accumulation is deliberately branch-light: it runs inside the measured
 * loop, so the instrumentation must not dominate what it measures. The
 * histogram is eight buckets of one eighth of the budget each, with the last
 * bucket absorbing everything at or over budget, which is enough to see a
 * distribution shift without a division.
 */
void rdd2_step_timing_accumulate(struct rdd2_step_timing *timing, uint32_t elapsed)
{
	uint32_t bucket;

	if (!g_counter_available) {
		return;
	}
	timing->samples++;
	timing->total_cycles += elapsed;
	if (elapsed > timing->max_cycles) {
		timing->max_cycles = elapsed;
	}
	if (timing->budget_cycles == 0U) {
		return;
	}
	if (elapsed >= timing->budget_cycles) {
		timing->overruns++;
		bucket = RDD2_STEP_TIMING_BUCKETS - 1U;
	} else {
		bucket = (uint32_t)(((uint64_t)elapsed * RDD2_STEP_TIMING_BUCKETS) /
				    timing->budget_cycles);
		if (bucket >= RDD2_STEP_TIMING_BUCKETS) {
			bucket = RDD2_STEP_TIMING_BUCKETS - 1U;
		}
	}
	timing->buckets[bucket]++;
}

void rdd2_step_timing_snapshot(const struct rdd2_step_timing *timing,
			       struct rdd2_step_timing_snapshot *snapshot)
{
	k_spinlock_key_t key = k_spin_lock(&g_lock);

	snapshot->max_cycles = timing->max_cycles;
	snapshot->samples = timing->samples;
	snapshot->overruns = timing->overruns;
	snapshot->mean_cycles =
		timing->samples != 0U ? (uint32_t)(timing->total_cycles / timing->samples) : 0U;
	for (size_t bucket = 0U; bucket < RDD2_STEP_TIMING_BUCKETS; ++bucket) {
		snapshot->buckets[bucket] = timing->buckets[bucket];
	}
	snapshot->core_clock_hz = SystemCoreClock;
	snapshot->budget_cycles = timing->budget_cycles;
	snapshot->counter_available = g_counter_available;
	snapshot->clock_is_emulated = IS_ENABLED(CONFIG_RDD2_FASTDYN);
	k_spin_unlock(&g_lock, key);
}

void rdd2_step_timing_reset(struct rdd2_step_timing *timing)
{
	k_spinlock_key_t key = k_spin_lock(&g_lock);

	clear_locked(timing);
	k_spin_unlock(&g_lock, key);
}

static uint32_t cycles_to_microseconds(uint32_t cycles, uint32_t clock_hz)
{
	if (clock_hz == 0U) {
		return 0U;
	}
	return (uint32_t)(((uint64_t)cycles * 1000000ULL) / clock_hz);
}

void rdd2_step_timing_report(void)
{
	static int64_t next_report_ms;
	int64_t now_ms = k_uptime_get();
	uint8_t count;

	if (!g_counter_available || now_ms < next_report_ms) {
		return;
	}
	next_report_ms = now_ms + RDD2_STEP_TIMING_REPORT_PERIOD_MS;

	count = g_point_count;
	for (uint8_t index = 0U; index < count; ++index) {
		struct rdd2_step_timing_snapshot snapshot;

		rdd2_step_timing_snapshot(g_points[index], &snapshot);
		if (snapshot.samples == 0U) {
			continue;
		}
		/* The clock qualifier is part of the number. A FastDyn figure is
		 * a QEMU artefact and must never be quoted as a bench result. */
		LOG_INF("%s: n=%u max=%u cyc (%u us) mean=%u cyc (%u us) over=%u "
			"budget=%u cyc clk=%u Hz%s",
			g_points[index]->name, snapshot.samples, snapshot.max_cycles,
			cycles_to_microseconds(snapshot.max_cycles, snapshot.core_clock_hz),
			snapshot.mean_cycles,
			cycles_to_microseconds(snapshot.mean_cycles, snapshot.core_clock_hz),
			snapshot.overruns, snapshot.budget_cycles, snapshot.core_clock_hz,
			snapshot.clock_is_emulated ? " EMULATED-NOT-A-MEASUREMENT" : "");
		LOG_INF("%s: histogram %u %u %u %u %u %u %u %u", g_points[index]->name,
			snapshot.buckets[0], snapshot.buckets[1], snapshot.buckets[2],
			snapshot.buckets[3], snapshot.buckets[4], snapshot.buckets[5],
			snapshot.buckets[6], snapshot.buckets[7]);
	}
}
