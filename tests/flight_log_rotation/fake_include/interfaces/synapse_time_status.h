/* SPDX-License-Identifier: Apache-2.0 */

/* Time surface the flight logger reads for the boot clock and the TimeReference
 * record. The rotation suite runs on a free-running boot clock derived from the
 * simulated kernel uptime. */

#ifndef RDD2_TEST_FAKE_SYNAPSE_TIME_STATUS_H_
#define RDD2_TEST_FAKE_SYNAPSE_TIME_STATUS_H_

#include <stdbool.h>
#include <stdint.h>

#include <synapse/mcap_topics.h>

uint64_t synapse_time_boot_ns(void);
synapse_types_TimeStatus_enum_t synapse_time_status_resolve(bool *ever_synced, int64_t *offset_ns);
uint64_t synapse_time_apply_offset(uint64_t boot_ns, int64_t offset_ns);

#endif /* RDD2_TEST_FAKE_SYNAPSE_TIME_STATUS_H_ */
