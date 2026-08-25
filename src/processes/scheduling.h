/* SPDX-License-Identifier: Apache-2.0 */

#ifndef RDD2_PROCESSES_SCHEDULING_H_
#define RDD2_PROCESSES_SCHEDULING_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * Every periodic process is released by dividing the control tick, which is
 * the IMU data-ready interrupt. The divisors are asserted below: a target
 * rate that does not divide the control rate would make rdd2_release_due fire
 * on every tick and silently run that process at the control rate.
 */
#define RDD2_CONTROL_RATE_HZ 800U
#define RDD2_CONTROL_PERIOD_NS 1250000ULL
#define RDD2_CONTROL_DT_S (1.0f / 800.0f)
/* 800 / 100 = 8. PX4 defaults EKF2_PREDICT_US to 10000 us, the same 100 Hz,
 * so this is the mainstream configuration rather than a conservative outlier.
 * It is safe only because the estimator thread preintegrates every sample
 * into the packet it closes here: on point samples it would discard seven of
 * every eight. Against the 0.953 ms worst measured step at 600 MHz and one
 * instruction per cycle the release period leaves 10.5x headroom. */
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
