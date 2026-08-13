/* SPDX-License-Identifier: Apache-2.0 */

#ifndef RDD2_PROCESSES_SCHEDULING_H_
#define RDD2_PROCESSES_SCHEDULING_H_

#include <stdbool.h>
#include <stdint.h>

#define RDD2_CONTROL_RATE_HZ 1600U
#define RDD2_CONTROL_PERIOD_NS 625000ULL
#define RDD2_CONTROL_DT_S (1.0f / 1600.0f)
#define RDD2_NAVIGATION_ESTIMATOR_RATE_HZ 1000U
#define RDD2_GUIDANCE_RATE_HZ 200U
#define RDD2_PLANNING_RATE_HZ 50U

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
