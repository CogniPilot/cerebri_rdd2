/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Controls the flight-log rotation harness shares with the fake filesystem and
 * bus layer. The test body sets the injected card stall and pauses the offered
 * load to drive the idle-ring and manual-rotate cases.
 */

#ifndef RDD2_TEST_FLIGHT_LOG_ROTATION_HARNESS_H_
#define RDD2_TEST_FLIGHT_LOG_ROTATION_HARNESS_H_

#include <stdbool.h>
#include <stdint.h>

/* Milliseconds the fake inline extent reservation (f_expand) blocks the writer.
 * This is the mid-flight rotation burst the deferral change moves off the
 * rotation path. The spare-rename rotation path never incurs it. */
extern volatile uint32_t rdd2_test_expand_stall_ms;

/* When true the capture bus produces no samples, so the ring drains to idle. */
extern volatile bool rdd2_test_load_paused;

/* How many session extents each path has produced: the inline reservation
 * (f_expand) and the rename of a pre-built reservation. Their difference is how
 * the suite tells the two session-open paths apart. */
extern volatile uint32_t rdd2_test_prealloc_count;
extern volatile uint32_t rdd2_test_rename_count;

/* How many standing reservations of the full rotation size the fake card
 * currently carries, out of RDD2_FLIGHT_LOG_RESERVE_SLOTS. */
uint32_t rdd2_test_ready_reservations(void);

#endif /* RDD2_TEST_FLIGHT_LOG_ROTATION_HARNESS_H_ */
