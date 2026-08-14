/* SPDX-License-Identifier: Apache-2.0 */

#ifndef RDD2_GNSS_SOURCE_H_
#define RDD2_GNSS_SOURCE_H_

#include <stdbool.h>

#if defined(CONFIG_RDD2_GNSS_SOURCE_ONBOARD)
#include "gnss_onboard.h"

static inline bool rdd2_position_source_ready_get(void) {
  return rdd2_gnss_onboard_ready_get();
}
#elif defined(CONFIG_RDD2_GNSS_SOURCE_LOCKSTEP)
bool rdd2_gnss_lockstep_ready_get(void);

static inline bool rdd2_position_source_ready_get(void) {
  return rdd2_gnss_lockstep_ready_get();
}
#elif defined(CONFIG_RDD2_GNSS_SOURCE_MESH)
bool rdd2_gnss_mesh_ready_get(void);

static inline bool rdd2_position_source_ready_get(void) {
  return rdd2_gnss_mesh_ready_get();
}
#else
static inline bool rdd2_position_source_ready_get(void) { return true; }
#endif

#endif /* RDD2_GNSS_SOURCE_H_ */
