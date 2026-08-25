/* SPDX-License-Identifier: Apache-2.0 */

#ifndef RDD2_DIAGNOSTICS_STEP_TIMING_H_
#define RDD2_DIAGNOSTICS_STEP_TIMING_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * On-silicon timing for the periodic control path, measured with the
 * Cortex-M7 DWT cycle counter.
 *
 * WHY THIS EXISTS: every step-cost figure this project has is a model. QEMU
 * counts instructions and does not model external-flash wait states, cache
 * behaviour, or store buffering, and an instruction count times an assumed
 * IPC is not a measurement. The DWT cycle counter is the only thing that
 * observes what the core actually did, and it only means anything on real
 * silicon.
 *
 * UNDER FASTDYN THE NUMBERS ARE NOT MEASUREMENTS. The rehosted image runs
 * with a QEMU-supplied clock, src/platform/fastdyn_board.c pins
 * SystemCoreClock at 24 MHz rather than the 600 MHz the part runs at, and
 * QEMU consumes part of the DWT register block internally. The
 * instrumentation is built to run there without breaking the mission, and
 * reports its clock source so a rehosted figure cannot be mistaken for a
 * bench figure. Do not quote a FastDyn number as a silicon number.
 *
 * The whole facility compiles away when CONFIG_RDD2_STEP_TIMING is not set,
 * which is the default, so flight images are unaffected unless asked for.
 */

#define RDD2_STEP_TIMING_BUCKETS 8

struct rdd2_step_timing_snapshot {
	uint32_t max_cycles;
	uint32_t mean_cycles;
	uint32_t samples;
	uint32_t overruns;
	uint32_t buckets[RDD2_STEP_TIMING_BUCKETS];
	uint32_t core_clock_hz;
	uint32_t budget_cycles;
	bool counter_available;
	bool clock_is_emulated;
};

#if defined(CONFIG_RDD2_STEP_TIMING)

#include <cmsis_core.h>

struct rdd2_step_timing {
	const char *name;
	uint32_t release_rate_hz;
	uint32_t budget_cycles;
	uint32_t max_cycles;
	uint32_t samples;
	uint32_t overruns;
	uint64_t total_cycles;
	uint32_t buckets[RDD2_STEP_TIMING_BUCKETS];
};

/*
 * Enable DEMCR.TRCENA and DWT.CYCCNTENA, then prove the counter actually
 * advances. A part with the DWT locked, or an emulator that swallows the
 * register block, leaves it stuck; that is reported rather than published as
 * a run of zero-cost steps.
 */
bool rdd2_step_timing_init(void);

/* Declare a measurement point. release_rate_hz is the rate the step is called
 * at; its period is the budget the step must fit inside, used only to count
 * overruns and to report headroom. Budget cycles are derived from the core
 * clock at registration, so a rehosted image reports against the clock it
 * actually ran at. */
void rdd2_step_timing_register(struct rdd2_step_timing *timing, const char *name,
			       uint32_t release_rate_hz);

void rdd2_step_timing_snapshot(const struct rdd2_step_timing *timing,
			       struct rdd2_step_timing_snapshot *snapshot);
void rdd2_step_timing_reset(struct rdd2_step_timing *timing);

/* Log every registered point at a low rate. Call from a periodic thread; it
 * rate-limits internally and does nothing between reports, so it costs one
 * comparison on the ticks it does not report. */
void rdd2_step_timing_report(void);

static inline uint32_t rdd2_step_timing_begin(void)
{
	return DWT->CYCCNT;
}

void rdd2_step_timing_accumulate(struct rdd2_step_timing *timing, uint32_t elapsed);

static inline void rdd2_step_timing_end(struct rdd2_step_timing *timing, uint32_t started)
{
	/* Unsigned wrap gives the correct interval across the 32-bit rollover,
	 * which at 600 MHz happens every 7.2 s. */
	rdd2_step_timing_accumulate(timing, DWT->CYCCNT - started);
}

#define RDD2_STEP_TIMING_DEFINE(symbol) static struct rdd2_step_timing symbol

#else /* !CONFIG_RDD2_STEP_TIMING */

struct rdd2_step_timing {
	char unused;
};

static inline bool rdd2_step_timing_init(void)
{
	return false;
}
static inline void rdd2_step_timing_register(struct rdd2_step_timing *timing, const char *name,
					     uint32_t release_rate_hz)
{
	(void)timing;
	(void)name;
	(void)release_rate_hz;
}
static inline void rdd2_step_timing_snapshot(const struct rdd2_step_timing *timing,
					     struct rdd2_step_timing_snapshot *snapshot)
{
	(void)timing;
	*snapshot = (struct rdd2_step_timing_snapshot){0};
}
static inline void rdd2_step_timing_reset(struct rdd2_step_timing *timing)
{
	(void)timing;
}
static inline void rdd2_step_timing_report(void)
{
}
static inline uint32_t rdd2_step_timing_begin(void)
{
	return 0U;
}
static inline void rdd2_step_timing_end(struct rdd2_step_timing *timing, uint32_t started)
{
	(void)timing;
	(void)started;
}

#define RDD2_STEP_TIMING_DEFINE(symbol)                                                            \
	static struct rdd2_step_timing symbol __attribute__((unused))

#endif /* CONFIG_RDD2_STEP_TIMING */

#endif /* RDD2_DIAGNOSTICS_STEP_TIMING_H_ */
