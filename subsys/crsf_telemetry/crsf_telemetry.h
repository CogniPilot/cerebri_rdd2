/* SPDX-License-Identifier: Apache-2.0 */

#ifndef RDD2_CRSF_TELEMETRY_H_
#define RDD2_CRSF_TELEMETRY_H_

#include <stdint.h>

/* Scheduled telemetry frames, in the order the counters report them. */
enum crsf_telem_entry {
	CRSF_TELEM_ENTRY_ATTITUDE = 0,
	CRSF_TELEM_ENTRY_STATUS,
	CRSF_TELEM_ENTRY_GPS,
	CRSF_TELEM_ENTRY_FLIGHT_MODE,
	CRSF_TELEM_ENTRY_BATTERY,
	CRSF_TELEM_ENTRY_BATTERY_PASSTHROUGH,
	CRSF_TELEM_ENTRY_PARAMS,
	CRSF_TELEM_ENTRY_COUNT,
};

/*
 * Frames handed to the driver and accepted by it. The driver drops a frame
 * silently when another send is in flight, so these count what this subsystem
 * offered rather than what reached the receiver.
 */
struct rdd2_crsf_telemetry_counters {
	uint32_t frames[CRSF_TELEM_ENTRY_COUNT];
	uint32_t text_frames;
	uint32_t bytes;
};

void rdd2_crsf_telemetry_counters_get(struct rdd2_crsf_telemetry_counters *out);

#endif /* RDD2_CRSF_TELEMETRY_H_ */
