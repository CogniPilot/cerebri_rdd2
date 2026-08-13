/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gnss_m10_topic.h"

#include <zephyr/sys/util.h>

/* Below this the receiver's heading of motion is noise rather than a course. */
#define COURSE_VALID_MIN_MM_S 150

static int64_t days_from_civil(int64_t y, unsigned m, unsigned d);

static uint16_t saturate_u16(uint32_t value) {
  return (uint16_t)MIN(value, 65535U);
}

static bool is_leap_year(uint16_t year) {
  return year % 4U == 0U && (year % 100U != 0U || year % 400U == 0U);
}

static bool utc_timestamp_ns(const struct ubx_nav_pvt *pvt,
                             uint64_t *timestamp_ns) {
  static const uint8_t days_per_month[] = {31U, 28U, 31U, 30U, 31U, 30U,
                                           31U, 31U, 30U, 31U, 30U, 31U};
  uint64_t seconds;
  uint64_t whole_ns;
  uint8_t month_days;
  int64_t days;

  if ((pvt->time.valid & (UBX_NAV_PVT_VALID_DATE | UBX_NAV_PVT_VALID_TIME)) !=
          (UBX_NAV_PVT_VALID_DATE | UBX_NAV_PVT_VALID_TIME) ||
      pvt->time.year < 1970U || pvt->time.month < 1U || pvt->time.month > 12U ||
      pvt->time.hour > 23U || pvt->time.minute > 59U ||
      pvt->time.second > 59U || pvt->time.nano <= -1000000000 ||
      pvt->time.nano >= 1000000000) {
    return false;
  }
  month_days = days_per_month[pvt->time.month - 1U];
  if (pvt->time.month == 2U && is_leap_year(pvt->time.year)) {
    month_days++;
  }
  if (pvt->time.day < 1U || pvt->time.day > month_days) {
    return false;
  }

  days = days_from_civil(pvt->time.year, pvt->time.month, pvt->time.day);
  if (days < 0) {
    return false;
  }
  seconds = (uint64_t)days * 86400ULL + (uint64_t)pvt->time.hour * 3600ULL +
            (uint64_t)pvt->time.minute * 60ULL + (uint64_t)pvt->time.second;
  if (seconds > UINT64_MAX / 1000000000ULL) {
    return false;
  }
  whole_ns = seconds * 1000000000ULL;
  if (pvt->time.nano < 0) {
    uint64_t fraction = (uint64_t)(-(int64_t)pvt->time.nano);

    if (whole_ns < fraction) {
      return false;
    }
    *timestamp_ns = whole_ns - fraction;
  } else {
    uint64_t fraction = (uint64_t)pvt->time.nano;

    if (whole_ns > UINT64_MAX - fraction) {
      return false;
    }
    *timestamp_ns = whole_ns + fraction;
  }
  return true;
}

static synapse_types_GnssFixType_enum_t
fix_type_from(enum rdd2_gnss_m10_fix fix) {
  switch (fix) {
  case RDD2_GNSS_M10_FIX_DEAD_RECKONING:
    return synapse_types_GnssFixType_DeadReckoning;
  case RDD2_GNSS_M10_FIX_2D:
    return synapse_types_GnssFixType_Fix2d;
  case RDD2_GNSS_M10_FIX_3D:
    return synapse_types_GnssFixType_Fix3d;
  case RDD2_GNSS_M10_FIX_DGNSS:
    return synapse_types_GnssFixType_Dgnss;
  case RDD2_GNSS_M10_FIX_RTK_FLOAT:
    return synapse_types_GnssFixType_RtkFloat;
  case RDD2_GNSS_M10_FIX_RTK_FIXED:
    return synapse_types_GnssFixType_RtkFixed;
  case RDD2_GNSS_M10_FIX_TIME_ONLY:
    return synapse_types_GnssFixType_TimeOnly;
  case RDD2_GNSS_M10_FIX_NONE:
  default:
    return synapse_types_GnssFixType_NoFix;
  }
}

/* Days from 1970-01-01, Howard Hinnant's days_from_civil. */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
  int64_t era;
  unsigned yoe;
  unsigned doy;
  unsigned doe;

  y -= m <= 2;
  era = (y >= 0 ? y : y - 399) / 400;
  yoe = (unsigned)(y - era * 400);
  doy = (153U * (m + (m > 2 ? -3U : 9U)) + 2U) / 5U + d - 1U;
  doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;

  return era * 146097 + (int64_t)doe - 719468;
}

void rdd2_gnss_m10_topic_invalidate(int64_t now_ms,
                                    synapse_topic_GnssFixData_t *fix) {
  *fix = (synapse_topic_GnssFixData_t){
      .timestamp_ns = (uint64_t)now_ms * 1000000ULL,
      .fix_type = synapse_types_GnssFixType_NoFix,
      .horizontal_accuracy_mm = RDD2_GNSS_M10_ACCURACY_UNUSABLE,
      .vertical_accuracy_mm = RDD2_GNSS_M10_ACCURACY_UNUSABLE,
      .velocity_accuracy_mm_s = RDD2_GNSS_M10_ACCURACY_UNUSABLE,
      .hdop_centi = RDD2_GNSS_M10_ACCURACY_UNUSABLE,
      .vdop_centi = RDD2_GNSS_M10_ACCURACY_UNUSABLE,
  };
}

void rdd2_gnss_m10_topic_build(const struct ubx_nav_pvt *pvt,
                               const struct rdd2_gnss_m10_state *state,
                               int64_t now_ms,
                               synapse_topic_GnssFixData_t *fix) {
  struct rdd2_gnss_m10_output output;
  int32_t ground_speed_mm_s = pvt->nav.ground_speed;
  uint32_t course_cdeg;

  rdd2_gnss_m10_output_get(state, &output);
  if (!output.usable) {
    rdd2_gnss_m10_topic_invalidate(now_ms, fix);
    return;
  }

  *fix = (synapse_topic_GnssFixData_t){0};
  fix->timestamp_ns = (uint64_t)now_ms * 1000000ULL;
  fix->latitude_deg_e7 = pvt->nav.latitude;
  fix->longitude_deg_e7 = pvt->nav.longitude;
  fix->altitude_msl_mm = pvt->nav.hmsl;
  fix->altitude_ellipsoid_mm = pvt->nav.height;

  /* The estimates NMEA could not provide. */
  fix->horizontal_accuracy_mm = output.horizontal_accuracy_mm;
  fix->vertical_accuracy_mm = output.vertical_accuracy_mm;
  fix->velocity_accuracy_mm_s = output.velocity_accuracy_mm_s;
  fix->hdop_centi = saturate_u16(pvt->nav.pdop);
  fix->vdop_centi = saturate_u16(pvt->nav.pdop);

  fix->ground_speed_cm_s =
      saturate_u16((uint32_t)MAX(ground_speed_mm_s, 0) / 10U);
  fix->velocity_up_cm_s = rdd2_gnss_m10_velocity_up_cm_s(pvt->nav.vel_down);

  /* Heading of motion is 1e-5 deg; the contract wants centidegrees. */
  course_cdeg =
      (uint32_t)(((int64_t)pvt->nav.head_motion / 1000) % 36000 + 36000) %
      36000;
  fix->course_over_ground_cdeg = (uint16_t)course_cdeg;

  fix->fix_type = fix_type_from(output.fix);
  fix->satellites_used = pvt->nav.num_sv;
  fix->satellites_visible = pvt->nav.num_sv;
  fix->flags = synapse_topic_GnssFixFlags_VelocityUpValid;
  if (ground_speed_mm_s >= COURSE_VALID_MIN_MM_S) {
    fix->flags |= synapse_topic_GnssFixFlags_CourseValid;
  }

  /* NAV-PVT retains time after acquisition. Publish it only when date and
   * time are valid and this sample also carries an accepted fix. */
  if (utc_timestamp_ns(pvt, &fix->time_unix_ns)) {
    fix->flags |= synapse_topic_GnssFixFlags_TimeValid;
  }
}

bool rdd2_gnss_m10_invalidation_due(
    const struct rdd2_gnss_m10_publication_state *publication, bool was_ready,
    const struct rdd2_gnss_m10_state *state) {
  return publication->invalidation_pending || (was_ready && !state->ready);
}

bool rdd2_gnss_m10_publication_barrier(struct rdd2_gnss_m10_state *state,
                                       const synapse_topic_GnssFixData_t *fix) {
  bool topic_usable =
      state->ready && fix->fix_type != synapse_types_GnssFixType_NoFix;

  state->ready = false;
  return topic_usable;
}

void rdd2_gnss_m10_publication_complete(
    struct rdd2_gnss_m10_publication_state *publication,
    struct rdd2_gnss_m10_state *state, bool topic_usable, bool succeeded) {
  rdd2_gnss_m10_publication_result(publication, succeeded);
  if (succeeded && topic_usable) {
    state->ready = true;
  } else if (!succeeded) {
    /* Require a new stable run after any uncertain topic handoff. */
    state->ready = false;
    state->stable_samples = 0U;
  }
}

void rdd2_gnss_m10_publication_result(
    struct rdd2_gnss_m10_publication_state *publication, bool succeeded) {
  publication->invalidation_pending = !succeeded;
}
