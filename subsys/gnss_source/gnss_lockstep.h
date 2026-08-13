/* SPDX-License-Identifier: Apache-2.0 */

#ifndef RDD2_GNSS_LOCKSTEP_H_
#define RDD2_GNSS_LOCKSTEP_H_

#include <stdbool.h>
#include <stdint.h>

#include <synapse/sensors_reader.h>

int rdd2_gnss_lockstep_init(void);
bool rdd2_gnss_lockstep_submit(const synapse_topic_GnssFixData_t *fix,
                               uint64_t control_now_ns);
bool rdd2_gnss_lockstep_ready_get(void);

#endif /* RDD2_GNSS_LOCKSTEP_H_ */
