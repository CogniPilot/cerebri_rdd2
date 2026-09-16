/* SPDX-License-Identifier: Apache-2.0 */

#ifndef RDD2_SYNAPSE_WIRE_RESTAMP_H_
#define RDD2_SYNAPSE_WIRE_RESTAMP_H_

#include <stdint.h>

#include <synapse/types_reader.h>

/*
 * Re-express a wire payload timestamp in the control IMU clock domain.
 *
 * Wire payload timestamps are produced in the sensor node's clock: the
 * producer's own freerun boot clock before it is gPTP synchronized, and the TAI
 * grandmaster timescale afterward. The estimator adapters compare a sample
 * against imu_timestamp_ns, the flight controller's LocalFreerun IMU boot clock,
 * and that clock stays in the boot domain even after the controller itself
 * disciplines to gPTP. A raw producer timestamp is therefore hundreds of
 * milliseconds ahead while both nodes are freerun and about 1.79e9 s ahead once
 * the producer is on TAI, so every fix and flow sample is rejected as far in the
 * future and none is ever fused. Re-stamp the sample into the boot domain at
 * ingress:
 *
 *  - When the producer and the receiver are both gPTP disciplined they share the
 *    grandmaster timescale, so subtract the receiver's own gPTP offset to
 *    recover the capture instant in the boot domain exactly.
 *  - Otherwise the two clocks share no reference, so approximate the capture
 *    instant with the local receive time minus the configured transport latency.
 *
 * The IMU stream never adopts the gPTP domain on this controller, so the payload
 * timestamp is never kept as-is; if that ever changes, a case that keeps a gPTP
 * payload against a gPTP IMU would be added here. The caller leaves the
 * producer's time_status unchanged, so a downstream require_gptp check reflects
 * whether the SOURCE timing was disciplined rather than the re-stamped
 * timestamp's own domain.
 */
static inline uint64_t
rdd2_synapse_wire_local_boot_timestamp_ns(uint64_t payload_timestamp_ns,
					  synapse_types_TimeStatus_enum_t payload_status,
					  uint64_t receive_monotonic_ns,
					  synapse_types_TimeStatus_enum_t receiver_time_status,
					  int64_t offset_ns,
					  uint64_t transport_latency_ns)
{
	if (receiver_time_status == synapse_types_TimeStatus_GptpSynced &&
	    (payload_status == synapse_types_TimeStatus_GptpSynced ||
	     payload_status == synapse_types_TimeStatus_GptpHoldover)) {
		int64_t boot_ns = (int64_t)payload_timestamp_ns - offset_ns;

		if (boot_ns < 0) {
			boot_ns = 0;
		}
		return (uint64_t)boot_ns;
	}
	return receive_monotonic_ns > transport_latency_ns
		       ? receive_monotonic_ns - transport_latency_ns
		       : receive_monotonic_ns;
}

#endif /* RDD2_SYNAPSE_WIRE_RESTAMP_H_ */
