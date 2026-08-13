/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pure M10 NAV-PVT to flight-topic boundary.
 */

#ifndef RDD2_GNSS_M10_TOPIC_H_
#define RDD2_GNSS_M10_TOPIC_H_

#include "gnss_m10_protocol.h"
#include "interfaces/data.h"

struct rdd2_gnss_m10_publication_state {
  bool invalidation_pending;
};

void rdd2_gnss_m10_topic_invalidate(int64_t now_ms,
                                    synapse_topic_GnssFixData_t *fix);

void rdd2_gnss_m10_topic_build(const struct ubx_nav_pvt *pvt,
                               const struct rdd2_gnss_m10_state *state,
                               int64_t now_ms,
                               synapse_topic_GnssFixData_t *fix);

bool rdd2_gnss_m10_invalidation_due(
    const struct rdd2_gnss_m10_publication_state *publication, bool was_ready,
    const struct rdd2_gnss_m10_state *state);

/* Closes the concurrent readiness gate before the caller attempts a topic
 * update and returns whether the prepared topic sample was usable. */
bool rdd2_gnss_m10_publication_barrier(struct rdd2_gnss_m10_state *state,
                                       const synapse_topic_GnssFixData_t *fix);

void rdd2_gnss_m10_publication_complete(
    struct rdd2_gnss_m10_publication_state *publication,
    struct rdd2_gnss_m10_state *state, bool topic_usable, bool succeeded);

/* A failed topic update always schedules a NoFix retry. This is conservative
 * even when the failed update would have carried a usable fix, because the
 * retained topic value may be an older usable sample. */
void rdd2_gnss_m10_publication_result(
    struct rdd2_gnss_m10_publication_state *publication, bool succeeded);

#endif /* RDD2_GNSS_M10_TOPIC_H_ */
