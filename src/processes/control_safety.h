/* SPDX-License-Identifier: Apache-2.0 */

#ifndef RDD2_PROCESSES_CONTROL_SAFETY_H_
#define RDD2_PROCESSES_CONTROL_SAFETY_H_

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RDD2_GUIDANCE_COMMAND_TIMEOUT_US 25000U

static inline bool rdd2_control_timestamp_is_fresh(bool observed, uint64_t timestamp_us,
						   uint64_t control_now_us,
						   uint64_t timeout_us)
{
	return observed && timestamp_us <= control_now_us &&
	       control_now_us - timestamp_us <= timeout_us;
}

static inline bool rdd2_generated_step_ok(uint32_t error_signal_status)
{
	return error_signal_status == 0U;
}

static inline bool rdd2_control_fault_latch(bool latched, bool arm_switch_valid,
					    bool arm_switch, bool current_fault)
{
	if (arm_switch_valid && !arm_switch) {
		return false;
	}
	return latched || current_fault;
}

static inline bool rdd2_guidance_arm_allowed(bool health_armed, bool health_failsafe,
					     bool manual_valid, bool arm_switch)
{
	return health_armed && !health_failsafe && manual_valid && arm_switch;
}

static inline bool rdd2_control_value_is_finite(float value)
{
	return isfinite(value);
}

static inline bool rdd2_control_values_are_finite(const float *values, size_t count)
{
	for (size_t index = 0U; index < count; ++index) {
		if (!rdd2_control_value_is_finite(values[index])) {
			return false;
		}
	}
	return true;
}

#endif /* RDD2_PROCESSES_CONTROL_SAFETY_H_ */
