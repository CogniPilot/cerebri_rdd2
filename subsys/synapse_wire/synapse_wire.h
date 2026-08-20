/* SPDX-License-Identifier: Apache-2.0 */

#ifndef RDD2_SYNAPSE_WIRE_H_
#define RDD2_SYNAPSE_WIRE_H_

#include <stdbool.h>
#include <stdint.h>

bool rdd2_synapse_wire_gnss_ready_get(void);

/* Per-stream receiver counters, copied out under the receiver lock. */
struct rdd2_synapse_wire_stream_stats {
	uint32_t received;
	uint32_t accepted;
	uint32_t publish_failed;
	uint32_t socket_errors;
	uint32_t sequence_gaps;
	uint32_t session_changes;
	uint32_t last_sequence;
	uint64_t session_id;
	uint16_t last_header_flags;
	uint8_t last_receiver_time_status;
};

/* Snapshot of both direct-wire sensor streams. */
struct rdd2_synapse_wire_snapshot {
	struct rdd2_synapse_wire_stream_stats optical;
	struct rdd2_synapse_wire_stream_stats gnss;
};

/*
 * Copy the current per-stream receiver counters out under the receiver lock,
 * without disturbing the receiver thread. Safe to poll from another thread.
 */
void rdd2_synapse_wire_stats_snapshot(struct rdd2_synapse_wire_snapshot *out);

#endif /* RDD2_SYNAPSE_WIRE_H_ */
