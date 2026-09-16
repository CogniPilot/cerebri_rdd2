/* SPDX-License-Identifier: Apache-2.0 */

#ifndef RDD2_PROCESSES_SCHEDULING_H_
#define RDD2_PROCESSES_SCHEDULING_H_

#include <stdbool.h>
#include <stdint.h>

#define RDD2_CONTROL_RATE_HZ 800U
#define RDD2_CONTROL_PERIOD_NS 1250000ULL
#define RDD2_CONTROL_DT_S (1.0f / 800.0f)
/* 800 / 100 = 8. PX4 defaults EKF2_PREDICT_US to 10000 us, the same 100 Hz,
 * so this is the mainstream configuration rather than a conservative outlier.
 * It is safe only because the estimator thread preintegrates every sample
 * into the packet it closes on release: on point samples it would discard
 * seven of every eight. */
#define RDD2_NAVIGATION_ESTIMATOR_RATE_HZ 100U
/* Guidance does position AND attitude in one process and takes both from the
 * estimator, so releasing it faster than the estimate would recompute on
 * byte-identical feedback. It consumes each estimate exactly once. */
#define RDD2_GUIDANCE_RATE_HZ 100U
#define RDD2_PLANNING_RATE_HZ 50U

_Static_assert((RDD2_CONTROL_RATE_HZ % RDD2_NAVIGATION_ESTIMATOR_RATE_HZ) == 0U,
               "estimator rate must divide the controller rate");
_Static_assert((RDD2_CONTROL_RATE_HZ % RDD2_GUIDANCE_RATE_HZ) == 0U,
               "guidance rate must divide the controller rate");
_Static_assert((RDD2_CONTROL_RATE_HZ % RDD2_PLANNING_RATE_HZ) == 0U,
               "planning rate must divide the controller rate");

/*
 * Maximum age of a guidance rate command before the rate-control allocator
 * treats it as stale and drops to failsafe. It is derived from the guidance
 * period, not written as a raw millisecond constant, so halving the guidance
 * rate widens the tolerance in lockstep with it instead of silently halving the
 * failsafe margin. Guidance publishes one command per guidance period and the
 * allocator keeps tracking it for a fixed number of those periods; five periods
 * rides out a few missed releases from a briefly preempted guidance thread
 * while still catching a genuinely stalled producer within a small fraction of
 * a second. Under the deterministic FastDyn lockstep the controller advances
 * every substep of a plant frame back to back with no idle time, so the lower
 * priority guidance thread is serviced only once per plant frame; the tolerance
 * is widened for that build so a normally scheduled command is not misread as
 * stalled.
 */
#define RDD2_GUIDANCE_COMMAND_PERIOD_NS                                        \
  (UINT64_C(1000000000) / RDD2_GUIDANCE_RATE_HZ)
#if defined(CONFIG_RDD2_LOCKSTEP)
#define RDD2_GUIDANCE_COMMAND_TIMEOUT_PERIODS UINT64_C(20)
#else
#define RDD2_GUIDANCE_COMMAND_TIMEOUT_PERIODS UINT64_C(5)
#endif
#define RDD2_GUIDANCE_COMMAND_TIMEOUT_NS                                       \
  (RDD2_GUIDANCE_COMMAND_TIMEOUT_PERIODS * RDD2_GUIDANCE_COMMAND_PERIOD_NS)
_Static_assert(RDD2_GUIDANCE_COMMAND_TIMEOUT_NS >=
                   3U * RDD2_GUIDANCE_COMMAND_PERIOD_NS,
               "the guidance command timeout must span at least three guidance "
               "periods so a single missed release is not misread as a stall");

/* Lower Zephyr numbers execute first on coincident releases. */
#define RDD2_RATE_PRIORITY 2
#define RDD2_NAVIGATION_PRIORITY 3
#define RDD2_PLANNING_PRIORITY 5
#define RDD2_GUIDANCE_PRIORITY 6

#define RDD2_FLIGHT_STATE_PUBLISH_RATE_HZ 200U
#define RDD2_FLIGHT_STATE_PUBLISH_DIV                                          \
  (RDD2_CONTROL_RATE_HZ / RDD2_FLIGHT_STATE_PUBLISH_RATE_HZ)
_Static_assert((RDD2_CONTROL_RATE_HZ % RDD2_FLIGHT_STATE_PUBLISH_RATE_HZ) == 0U,
               "flight-state publish rate must divide the controller rate");
_Static_assert(CONFIG_MAIN_THREAD_PRIORITY == RDD2_RATE_PRIORITY,
               "main thread must own the highest-priority eFMU process");

struct rdd2_release_scheduler {
  uint32_t phase;
};

static inline bool rdd2_release_due(struct rdd2_release_scheduler *scheduler,
                                    uint32_t source_rate_hz,
                                    uint32_t target_rate_hz) {
#if defined(RDD2_TEST_EVERY_SAMPLE_IS_RELEASE)
  (void)scheduler;
  (void)source_rate_hz;
  (void)target_rate_hz;
  return true;
#else
  scheduler->phase += target_rate_hz;
  if (scheduler->phase < source_rate_hz) {
    return false;
  }
  scheduler->phase -= source_rate_hz;
  return true;
#endif
}

#endif /* RDD2_PROCESSES_SCHEDULING_H_ */
