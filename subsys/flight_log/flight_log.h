/* SPDX-License-Identifier: Apache-2.0 */

#ifndef RDD2_FLIGHT_LOG_H_
#define RDD2_FLIGHT_LOG_H_

#include <stdbool.h>
#include <stdint.h>

/* Runtime state of the logger, copied out for the shell and health path. */
struct rdd2_flight_log_status {
	bool mounted;         /* FAT volume mounted */
	bool active;          /* a session file is open and channels registered */
	bool last_sync_ok;    /* the most recent flush synced cleanly */
	bool want_logging;    /* operator/auto request to keep logging */
	uint32_t session_index;
	uint64_t bytes_written;
	uint32_t dropped_frames;
	uint32_t ring_high_water;
	uint32_t flush_errors;
};

void rdd2_flight_log_status_get(struct rdd2_flight_log_status *out);

/*
 * True while the logger is healthy: a session file is open on a mounted card
 * and the last flush synced without error. Drives the Logging health bit.
 */
bool rdd2_flight_log_healthy(void);

/*
 * True while a session file is open and the writer may touch the card. The
 * fs_mgmt hook reads this to deny mcumgr file access while logging is live, so
 * a network retrieval never races the writer inside non-reentrant FatFs.
 */
bool rdd2_flight_log_session_active(void);

/* Operator controls. Each records a request the writer thread acts on. */
void rdd2_flight_log_request_start(void);
void rdd2_flight_log_request_stop(void);
void rdd2_flight_log_request_rotate(void);

#endif /* RDD2_FLIGHT_LOG_H_ */
