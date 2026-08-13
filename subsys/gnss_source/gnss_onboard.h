#ifndef RDD2_GNSS_ONBOARD_H_
#define RDD2_GNSS_ONBOARD_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * Bring-up visibility for the onboard receiver, in the order the counters
 * answer questions. `frames` counts checksum-valid UBX frames of any kind and
 * proves the wire and the baud are right. `samples` counts NAV-PVT frames
 * specifically, which the receiver emits with or without a lock, so a zero
 * there alongside a non-zero `frames` means the module is not sending NAV-PVT
 * rather than that it has no satellites.
 */
struct rdd2_gnss_onboard_stats {
  uint32_t frames;       /* valid UBX frames, any class */
  uint32_t other_frames; /* valid, but not NAV-PVT */
  uint32_t samples;      /* NAV-PVT frames decoded */
  uint32_t published;
  uint32_t publish_failed;
  uint32_t checksum_errors;
  uint32_t bad_length;
  uint32_t oversize;
  uint32_t ring_overrun;
  uint32_t config_acks;
  uint32_t config_naks;
  uint32_t config_timeouts;
  uint32_t unexpected_acks;
  uint32_t bad_ack_length;
  uint32_t rate_errors;
  uint32_t last_gap_ms;
  uint32_t max_gap_ms;
  uint32_t measured_rate_millihz;
  uint32_t last_hacc_mm;
  uint32_t last_vacc_mm;
  uint32_t last_sacc_mm_s;
  int64_t last_sample_ms; /* -1 until the first sample arrives */
  uint8_t last_fix_type;
  uint8_t last_satellites;
  uint16_t last_hdop_centi;
  uint8_t config_status;
  uint8_t config_step;
  uint8_t config_attempt;
  uint8_t stable_samples;
  bool fix_accepted;
  bool accuracy_accepted;
  bool configured;
  bool ready;
};

void rdd2_gnss_onboard_stats_get(struct rdd2_gnss_onboard_stats *stats);
bool rdd2_gnss_onboard_ready_get(void);

#endif
