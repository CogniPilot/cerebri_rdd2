/* SPDX-License-Identifier: Apache-2.0 */

#ifndef RDD2_GNSS_MESH_H_
#define RDD2_GNSS_MESH_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * Bring-up visibility for the mesh GNSS backend, in the order the counters
 * answer questions. `topic_found` proves the inbound CSyn topic resolved.
 * `samples` counts distinct producer generations copied onto the internal bus.
 * `stale_polls` counts polls that observed no new generation, and
 * `producer_restarts` counts observed backward steps of the source
 * generation, each of which re-seeds local freshness state rather than
 * stalling.
 */
struct rdd2_gnss_mesh_stats {
  uint32_t samples;           /* distinct generations copied to gnss_fix */
  uint32_t published;         /* successful gnss_fix publications */
  uint32_t publish_failed;
  uint32_t copy_failed;       /* csyn copy returned no consistent sample */
  uint32_t size_mismatch;     /* received payload was not the struct size */
  uint32_t stale_polls;       /* polls with no newly observed generation */
  uint32_t producer_restarts; /* source generation stepped backward, re-seeded */
  uint32_t last_generation;   /* last source generation acted on */
  int32_t last_latitude_deg_e7;
  int32_t last_longitude_deg_e7;
  int32_t last_altitude_msl_mm;
  uint16_t last_hacc_mm;
  int64_t last_sample_ms; /* -1 until the first sample is copied */
  uint8_t last_fix_type;
  uint8_t last_satellites;
  uint8_t stable_samples;
  bool topic_found; /* csyn "gnss" RX topic resolved */
  bool usable;      /* last sample passed the usability gate */
  bool ready;
};

void rdd2_gnss_mesh_stats_get(struct rdd2_gnss_mesh_stats *stats);
bool rdd2_gnss_mesh_ready_get(void);

#endif /* RDD2_GNSS_MESH_H_ */
