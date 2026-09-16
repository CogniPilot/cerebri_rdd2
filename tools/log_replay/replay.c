/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Host replay of the RDD2 navigation estimator eFMU on logged sensor data.
 *
 * Mirrors the firmware estimator process: every IMU sample is composed into
 * the preintegration window, the window is closed every `divisor` samples,
 * the GNSS and optical-flow adapters stage and validate their samples exactly
 * as on the vehicle, and the generated block steps once per release.
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "control_safety.h"
#include "imu_preintegration.h"
#include "interfaces/data.h"
#include "navigation_gps.h"
#include "navigation_optical_flow.h"
#include "navigation_optical_flow_raw.h"
#include "Vehicles_Rdd2_NavigationEstimator.h"

#define IMU_RATE_HZ 800.0f
#define MAX_DENY 16
#define GPS_ORIGIN_INITIALIZATION_TIMEOUT_NS UINT64_C(100000000)
#define TIME_BASE_NS UINT64_C(1000000000)
#define RDD2_OPTICAL_FLOW_COMPENSATION_RATE_NOISE_RAD_S_RTHZ (5.0e-3f)
#define RDD2_NAVIGATION_IMU_HOLD_MAX_RELEASES 2U

struct imu_row { double t; float g[3]; float a[3]; };
struct gps_row { double t, lat, lon, alt, alte, vn, ve, vd, hacc, vacc, sacc; int fix, sats, pos_valid, vel_valid; };
struct flow_row { double t, vx, vy, dist, q; int valid; };
struct flow_raw_row { double t, fx, fy, dax, day, daz, integ, dist, dq, q, flags; };

static const struct rdd2_navigation_optical_flow_config g_flow_config = {
    .max_age_ns = 150ULL * 1000000ULL, .min_distance_m = 0.05f, .max_distance_m = 5.0f,
    .max_tilt_rad = 0.7f, .max_speed_m_s = 10.0f, .best_stddev_m_s = 0.1f,
    .worst_stddev_m_s = 1.0f, .range_variance_m2 = 0.05f * 0.05f, .sensor_id = 0,
    .nominal_integration_time_s = 0.025f, .min_integration_time_s = 0.005f,
    .max_integration_time_s = 0.200f, .min_quality = 100, .require_gptp = true,
};

/*
 * Default tightly coupled optical-flow raw config, matching the Kconfig
 * defaults in subsys/optical_flow_source/Kconfig
 * (RDD2_OPTICAL_FLOW_RAW_*): 150 ms staleness, 0.05 to 5.0 m range gate, 5 to
 * 200 ms integration window, line-of-sight noise floor 300 urad with a 5% to
 * 25% sensitivity fraction, ICM45686 rate-noise density 3800 urad/s/rtHz,
 * range stddev 30 to 150 mm, mount yaw 0 degrees, min quality 100.
 */
static const struct rdd2_navigation_optical_flow_raw_config g_flow_raw_config = {
    .max_age_ns = 150ULL * 1000000ULL, .min_distance_m = 0.05f, .max_distance_m = 5.0f,
    .min_integration_time_s = 0.005f, .max_integration_time_s = 0.200f,
    .los_floor_rad = 300.0e-6f, .los_sens_best_frac = 0.05f, .los_sens_worst_frac = 0.25f,
    .gyro_rate_noise_rad_s_rthz = 3800.0e-6f, .range_best_stddev_m = 0.03f,
    .range_worst_stddev_m = 0.15f, .mount_yaw_deg = 0, .sensor_id = 0,
    .min_quality = 100, .require_gptp = true,
};

static void *xrealloc(void *p, size_t n) { p = realloc(p, n); if (!p) { perror("realloc"); exit(1); } return p; }

static size_t load_imu(const char *path, struct imu_row **out) {
  FILE *f = fopen(path, "r"); if (!f) { perror(path); exit(1); }
  char line[512]; size_t n = 0, cap = 0; struct imu_row *r = NULL;
  if (!fgets(line, sizeof line, f)) exit(1);
  while (fgets(line, sizeof line, f)) {
    if (n == cap) { cap = cap ? cap * 2 : 1024; r = xrealloc(r, cap * sizeof *r); }
    if (sscanf(line, "%lf,%f,%f,%f,%f,%f,%f", &r[n].t, &r[n].g[0], &r[n].g[1], &r[n].g[2], &r[n].a[0], &r[n].a[1], &r[n].a[2]) == 7) n++;
  }
  fclose(f); *out = r; return n;
}
static size_t load_gps(const char *path, struct gps_row **out) {
  FILE *f = fopen(path, "r"); if (!f) { perror(path); exit(1); }
  char line[1024]; size_t n = 0, cap = 0; struct gps_row *r = NULL; double e, nn, u; int vdd;
  if (!fgets(line, sizeof line, f)) exit(1);
  while (fgets(line, sizeof line, f)) {
    if (n == cap) { cap = cap ? cap * 2 : 256; r = xrealloc(r, cap * sizeof *r); }
    struct gps_row *g = &r[n];
    if (sscanf(line, "%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%d,%d,%d,%d,%d,%lf,%lf,%lf", &g->t, &g->lat, &g->lon, &g->alt, &g->alte, &g->vn, &g->ve, &g->vd, &g->hacc, &g->vacc, &g->sacc, &g->fix, &g->sats, &g->pos_valid, &g->vel_valid, &vdd, &e, &nn, &u) == 19) n++;
  }
  fclose(f); *out = r; return n;
}
static size_t load_flow(const char *path, struct flow_row **out) {
  FILE *f = fopen(path, "r"); if (!f) { perror(path); exit(1); }
  char line[512]; size_t n = 0, cap = 0; struct flow_row *r = NULL;
  if (!fgets(line, sizeof line, f)) exit(1);
  while (fgets(line, sizeof line, f)) {
    if (n == cap) { cap = cap ? cap * 2 : 256; r = xrealloc(r, cap * sizeof *r); }
    struct flow_row *w = &r[n];
    if (sscanf(line, "%lf,%lf,%lf,%lf,%lf,%d", &w->t, &w->vx, &w->vy, &w->dist, &w->q, &w->valid) == 6) n++;
  }
  fclose(f); *out = r; return n;
}

static size_t load_flow_raw(const char *path, struct flow_raw_row **out) {
  FILE *f = fopen(path, "r"); if (!f) { perror(path); exit(1); }
  char line[512]; size_t n = 0, cap = 0; struct flow_raw_row *r = NULL;
  if (!fgets(line, sizeof line, f)) exit(1);
  while (fgets(line, sizeof line, f)) {
    if (n == cap) { cap = cap ? cap * 2 : 256; r = xrealloc(r, cap * sizeof *r); }
    struct flow_raw_row *w = &r[n];
    if (sscanf(line, "%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf", &w->t, &w->fx, &w->fy, &w->dax, &w->day, &w->daz, &w->integ, &w->dist, &w->dq, &w->q, &w->flags) == 11) n++;
  }
  fclose(f); *out = r; return n;
}

static uint64_t to_ns(double t) { return TIME_BASE_NS + (uint64_t)llround(t * 1e9); }
static uint16_t sat_u16(double v) { return v >= 65535.0 ? 65535U : (uint16_t)llround(v); }

/*
 * When set, the replay omits the VelocityUpValid flag even for fixes whose
 * host-derived velocity is valid, reproducing the vehicle's GNSS receiver which
 * reports course over ground and ground speed but no vertical velocity.
 */
static bool g_no_vertical_velocity = false;

static void gps_to_fix(const struct gps_row *g, synapse_topic_GnssFixData_t *fix) {
  double course = atan2(g->ve, g->vn) * 180.0 / M_PI; if (course < 0) course += 360.0;
  memset(fix, 0, sizeof *fix);
  fix->timestamp_ns = to_ns(g->t);
  fix->latitude_deg_e7 = (int32_t)llround(g->lat * 1e7);
  fix->longitude_deg_e7 = (int32_t)llround(g->lon * 1e7);
  fix->altitude_msl_mm = (int32_t)llround(g->alt * 1e3);
  fix->altitude_ellipsoid_mm = (int32_t)llround(g->alte * 1e3);
  fix->horizontal_accuracy_mm = sat_u16(g->hacc * 1e3);
  fix->vertical_accuracy_mm = sat_u16(g->vacc * 1e3);
  fix->velocity_accuracy_mm_s = sat_u16(g->sacc * 1e3);
  fix->ground_speed_cm_s = sat_u16(hypot(g->vn, g->ve) * 100.0);
  fix->course_over_ground_cdeg = (uint16_t)(llround(course * 100.0) % 36000);
  fix->velocity_up_cm_s = (int16_t)llround(-g->vd * 100.0);
  if (!g->vel_valid) {
    fix->flags = 0U;
  } else if (g_no_vertical_velocity) {
    fix->flags = synapse_topic_GnssFixFlags_CourseValid;
  } else {
    fix->flags = synapse_topic_GnssFixFlags_CourseValid | synapse_topic_GnssFixFlags_VelocityUpValid;
  }
  fix->fix_type = (uint8_t)g->fix;
  fix->satellites_used = (uint8_t)g->sats;
  fix->time_status = synapse_types_TimeStatus_GptpSynced;
}

static void flow_to_sample(const struct flow_row *w, synapse_topic_OpticalFlowVelocityData_t *s) {
  memset(s, 0, sizeof *s);
  s->timestamp_ns = to_ns(w->t);
  s->velocity_flu_m_s.x = (float)w->vx; s->velocity_flu_m_s.y = (float)w->vy;
  s->distance_m = (float)w->dist;
  s->quality = (uint8_t)llround(w->q * 255.0);
  s->flags = w->valid ? 7U : 0U;
  s->time_status = synapse_types_TimeStatus_GptpSynced;
}

static void flow_raw_to_sample(const struct flow_raw_row *w, synapse_topic_OpticalFlowData_t *s) {
  memset(s, 0, sizeof *s);
  s->timestamp_ns = to_ns(w->t);
  s->timestamp_sample_ns = s->timestamp_ns;
  s->flow_rad.x = (float)w->fx; s->flow_rad.y = (float)w->fy;
  s->delta_angle_flu_rad.x = (float)w->dax; s->delta_angle_flu_rad.y = (float)w->day; s->delta_angle_flu_rad.z = (float)w->daz;
  s->distance_m = (float)w->dist;
  s->integration_timespan_ns = (uint32_t)llround(w->integ * 1e9);
  s->distance_quality = (uint8_t)llround(w->dq);
  s->quality = (uint8_t)llround(w->q);
  s->flags = (uint8_t)llround(w->flags);
  s->time_status = synapse_types_TimeStatus_GptpSynced;
  s->id = 0U;
}

/* Verbatim wiring from the firmware estimator process. */
static void copy_imu_input_to_efmu(NavigationEstimatorState *efmu, const struct rdd2_imu_packet *packet) {
  efmu->imu_valid = packet->valid; efmu->imu_fresh = packet->valid;
  efmu->imu_timestamp_s = (float)packet->timestamp_ns * 1.0e-9f;
  efmu->imu_integrationTime_s = packet->integration_time_s;
  for (size_t axis = 0U; axis < 3U; ++axis) {
    efmu->imu_angularVelocityBodyFlu_rad_s[axis] = packet->angular_velocity_rad_s[axis];
    efmu->specificForceBodyFlu_m_s2[axis] = packet->specific_force_m_s2[axis];
    efmu->deltaAngleBodyFlu_rad[axis] = packet->delta_angle_rad[axis];
    efmu->deltaVelocityBodyFlu_m_s[axis] = packet->delta_velocity_m_s[axis];
    efmu->deltaPositionBodyFlu_m[axis] = packet->delta_position_m[axis];
    efmu->gyroscopeBiasLinearizationBodyFlu_rad_s[axis] = packet->gyroscope_bias_linearization_rad_s[axis];
    efmu->accelerometerBiasLinearizationBodyFlu_m_s2[axis] = packet->accelerometer_bias_linearization_m_s2[axis];
    for (size_t column = 0U; column < 3U; ++column) {
      efmu->deltaRotationGyroscopeBiasJacobian_s[axis][column] = packet->rotation_gyroscope_bias_jacobian_s[axis][column];
      efmu->deltaVelocityGyroscopeBiasJacobian_m[axis][column] = packet->velocity_gyroscope_bias_jacobian_m[axis][column];
      efmu->deltaVelocityAccelerometerBiasJacobian_s[axis][column] = packet->velocity_accelerometer_bias_jacobian_s[axis][column];
      efmu->deltaPositionGyroscopeBiasJacobian_m_s[axis][column] = packet->position_gyroscope_bias_jacobian_m_s[axis][column];
      efmu->deltaPositionAccelerometerBiasJacobian_s2[axis][column] = packet->position_accelerometer_bias_jacobian_s2[axis][column];
    }
  }
  for (size_t element = 0U; element < 4U; ++element) efmu->deltaQuaternionBodyFlu[element] = packet->delta_quaternion[element];
}
static void copy_gps_input_to_efmu(NavigationEstimatorState *efmu, const struct rdd2_navigation_gps_measurement *m) {
  efmu->gps_valid = m->valid; efmu->gps_fresh = m->fresh; efmu->positionValid = m->position_valid; efmu->velocityValid = m->velocity_valid;
  efmu->gps_timestamp_s = (float)m->timestamp_ns * 1.0e-9f;
  for (size_t axis = 0U; axis < 3U; ++axis) {
    efmu->geodetic_deg_m[axis] = m->geodetic_deg_m[axis];
    efmu->gps_positionWorldEnu_m[axis] = m->position_enu_m[axis];
    efmu->gps_velocityWorldEnu_m_s[axis] = m->velocity_enu_m_s[axis];
    for (size_t column = 0U; column < 3U; ++column) {
      efmu->gps_positionCovarianceWorld_m2[axis][column] = m->position_covariance_enu_m2[axis][column];
      efmu->velocityCovarianceWorld_m2_s2[axis][column] = m->velocity_covariance_enu_m2_s2[axis][column];
    }
  }
}
static void copy_optical_flow_input_to_efmu(NavigationEstimatorState *efmu, const struct rdd2_navigation_optical_flow_measurement *m) {
  float integration_time_s = m->integration_time_s > 0.0f ? m->integration_time_s : 1.0e-9f;
  float ground_distance_m = m->ground_distance_m > 0.0f ? m->ground_distance_m : 1.0e-9f;
  float radians_per_velocity = integration_time_s / ground_distance_m;
  efmu->opticalFlow_valid = m->valid; efmu->opticalFlow_fresh = m->fresh; efmu->opticalFlow_timestamp_s = (float)m->timestamp_ns * 1.0e-9f;
  efmu->integratedLineOfSight_rad[0] = -m->velocity_body_flu_m_s[1] * radians_per_velocity;
  efmu->integratedLineOfSight_rad[1] = m->velocity_body_flu_m_s[0] * radians_per_velocity;
  efmu->integratedLineOfSightCovariance_rad2[0][0] = m->velocity_covariance_body_m2_s2[1][1] * radians_per_velocity * radians_per_velocity;
  efmu->integratedLineOfSightCovariance_rad2[0][1] = -m->velocity_covariance_body_m2_s2[1][0] * radians_per_velocity * radians_per_velocity;
  efmu->integratedLineOfSightCovariance_rad2[1][0] = -m->velocity_covariance_body_m2_s2[0][1] * radians_per_velocity * radians_per_velocity;
  efmu->integratedLineOfSightCovariance_rad2[1][1] = m->velocity_covariance_body_m2_s2[0][0] * radians_per_velocity * radians_per_velocity;
  for (size_t axis = 0U; axis < 3U; ++axis) {
    efmu->integratedGyroscopeBodyFlu_rad[axis] = 0.0f;
    for (size_t column = 0U; column < 3U; ++column)
      efmu->integratedGyroscopeCovariance_rad2[axis][column] = axis == column ? RDD2_OPTICAL_FLOW_COMPENSATION_RATE_NOISE_RAD_S_RTHZ * RDD2_OPTICAL_FLOW_COMPENSATION_RATE_NOISE_RAD_S_RTHZ * integration_time_s : 0.0f;
  }
  efmu->opticalFlow_integrationTime_s = integration_time_s;
  efmu->groundDistance_m = m->ground_distance_m;
  efmu->groundDistanceVariance_m2 = m->ground_distance_variance_m2;
  efmu->quality = m->quality;
}
/* Verbatim wiring from the firmware estimator process
 * (navigation_estimator.c copy_optical_flow_raw_input_to_efmu): the raw
 * measurement is copied into the eFMU inputs field for field, including the
 * full integrated gyro (no zeroing) and its covariance. */
static void copy_optical_flow_raw_input_to_efmu(NavigationEstimatorState *efmu, const struct rdd2_navigation_optical_flow_raw_measurement *m) {
  efmu->opticalFlow_valid = m->valid; efmu->opticalFlow_fresh = m->fresh; efmu->opticalFlow_timestamp_s = (float)m->timestamp_ns * 1.0e-9f;
  for (size_t row = 0U; row < 2U; ++row) {
    efmu->integratedLineOfSight_rad[row] = m->integrated_line_of_sight_rad[row];
    for (size_t column = 0U; column < 2U; ++column)
      efmu->integratedLineOfSightCovariance_rad2[row][column] = m->integrated_line_of_sight_cov_rad2[row][column];
  }
  for (size_t row = 0U; row < 3U; ++row) {
    efmu->integratedGyroscopeBodyFlu_rad[row] = m->integrated_gyro_body_flu_rad[row];
    for (size_t column = 0U; column < 3U; ++column)
      efmu->integratedGyroscopeCovariance_rad2[row][column] = m->integrated_gyro_cov_rad2[row][column];
  }
  efmu->opticalFlow_integrationTime_s = m->integration_time_s;
  efmu->groundDistance_m = m->ground_distance_m;
  efmu->groundDistanceVariance_m2 = m->ground_distance_variance_m2;
  efmu->quality = m->quality;
}
static bool efmu_estimate_is_finite(const NavigationEstimatorState *efmu) {
  const float values[] = {
      efmu->estimate_quaternionWorldBody[0], efmu->estimate_quaternionWorldBody[1], efmu->estimate_quaternionWorldBody[2], efmu->estimate_quaternionWorldBody[3],
      efmu->estimate_positionWorldEnu_m[0], efmu->estimate_positionWorldEnu_m[1], efmu->estimate_positionWorldEnu_m[2],
      efmu->estimate_velocityWorldEnu_m_s[0], efmu->estimate_velocityWorldEnu_m_s[1], efmu->estimate_velocityWorldEnu_m_s[2],
      efmu->estimate_angularVelocityBodyFlu_rad_s[0], efmu->estimate_angularVelocityBodyFlu_rad_s[1], efmu->estimate_angularVelocityBodyFlu_rad_s[2], efmu->estimate_timestamp_s};
  return efmu->estimate_timestamp_s >= 0.0f && rdd2_control_values_are_finite(values, sizeof values / sizeof values[0]);
}

static void euler_from_quat(const float q[4], double e[3]) {
  double w = q[0], x = q[1], y = q[2], z = q[3];
  e[0] = atan2(2.0 * (w * x + y * z), 1.0 - 2.0 * (x * x + y * y));
  double s = 2.0 * (w * y - z * x); if (s > 1) s = 1; if (s < -1) s = -1;
  e[1] = asin(s);
  e[2] = atan2(2.0 * (w * z + x * y), 1.0 - 2.0 * (y * y + z * z));
}

int main(int argc, char **argv) {
  const char *input = NULL, *output = NULL, *flow_raw_path = NULL; int divisor = 8; bool use_flow = true, use_gps = true; int verbose = 0; double q_gyro = -1, q_accel = -1, q_gbias = -1, q_abias = -1, gate = -1, v_gbias = -1, v_abias = -1, v_att = -1;
  double deny0[MAX_DENY], deny1[MAX_DENY]; int ndeny = 0;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--input") && i + 1 < argc) input = argv[++i];
    else if (!strcmp(argv[i], "--output") && i + 1 < argc) output = argv[++i];
    else if (!strcmp(argv[i], "--divisor") && i + 1 < argc) divisor = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--gps-deny") && i + 1 < argc && ndeny < MAX_DENY) { sscanf(argv[++i], "%lf:%lf", &deny0[ndeny], &deny1[ndeny]); ndeny++; }
    else if ((!strcmp(argv[i], "--flow-raw") || !strcmp(argv[i], "--raw-flow")) && i + 1 < argc) flow_raw_path = argv[++i];
    else if (!strcmp(argv[i], "--no-flow")) use_flow = false;
    else if (!strcmp(argv[i], "--no-gps")) use_gps = false;
    else if (!strcmp(argv[i], "--no-vertical-velocity")) g_no_vertical_velocity = true;
    else if (!strcmp(argv[i], "--verbose")) verbose = 1;
    else if (!strcmp(argv[i], "--gyro-noise") && i + 1 < argc) q_gyro = atof(argv[++i]);
    else if (!strcmp(argv[i], "--accel-noise") && i + 1 < argc) q_accel = atof(argv[++i]);
    else if (!strcmp(argv[i], "--gyro-bias-noise") && i + 1 < argc) q_gbias = atof(argv[++i]);
    else if (!strcmp(argv[i], "--accel-bias-noise") && i + 1 < argc) q_abias = atof(argv[++i]);
    else if (!strcmp(argv[i], "--gate") && i + 1 < argc) gate = atof(argv[++i]);
    else if (!strcmp(argv[i], "--init-gyro-bias-var") && i + 1 < argc) v_gbias = atof(argv[++i]);
    else if (!strcmp(argv[i], "--init-accel-bias-var") && i + 1 < argc) v_abias = atof(argv[++i]);
    else if (!strcmp(argv[i], "--init-att-var") && i + 1 < argc) v_att = atof(argv[++i]);
    else { fprintf(stderr, "usage: %s --input DIR --output FILE [--divisor N] [--gps-deny T0:T1]... [--flow-raw FILE] [--no-flow] [--no-gps]\n", argv[0]); return 2; }
  }
  if (!input || !output) { fprintf(stderr, "missing --input/--output\n"); return 2; }
  char path[1024]; struct imu_row *imu; struct gps_row *gps; struct flow_row *flow;
  struct flow_raw_row *flow_raw = NULL; size_t nflow_raw = 0; bool use_flow_raw = flow_raw_path != NULL;
  snprintf(path, sizeof path, "%s/imu.csv", input); size_t nimu = load_imu(path, &imu);
  snprintf(path, sizeof path, "%s/gps.csv", input); size_t ngps = load_gps(path, &gps);
  snprintf(path, sizeof path, "%s/flow.csv", input); size_t nflow = load_flow(path, &flow);
  if (use_flow_raw) { nflow_raw = load_flow_raw(flow_raw_path, &flow_raw); use_flow = false; }
  fprintf(stderr, "loaded imu %zu gps %zu flow %zu flow_raw %zu\n", nimu, ngps, nflow, nflow_raw);
  FILE *out = fopen(output, "w"); if (!out) { perror(output); return 1; }
  fprintf(out, "t_s,e_m,n_m,u_m,ve_m_s,vn_m_s,vu_m_s,qw,qx,qy,qz,roll_rad,pitch_rad,yaw_rad,bgx_rad_s,bgy_rad_s,bgz_rad_s,bax_m_s2,bay_m_s2,baz_m_s2,pos_valid,att_valid,"
               "initialized,recovery_stage,correction_outcome,correction_source,anchor_source,nis,gps_pos_acc,gps_vel_acc,flow_acc,gps_rej,flow_rej,imu_held,sig_e,sig_n,sig_u,sig_ve,sig_vn,sig_vu,sig_att_x,sig_att_y,sig_att_z,step_status\n");

  static NavigationEstimatorState efmu; static struct rdd2_imu_preintegrator pre; struct rdd2_imu_packet packet;
  struct rdd2_navigation_gps_adapter gps_adapter; struct rdd2_navigation_optical_flow_adapter flow_adapter;
  struct rdd2_navigation_optical_flow_raw_adapter flow_raw_adapter;
  rdd2_navigation_gps_init(&gps_adapter); rdd2_navigation_optical_flow_init(&flow_adapter);
  rdd2_navigation_optical_flow_raw_init(&flow_raw_adapter);
  NavigationEstimator_startup(&efmu);
  for (size_t i = 0U; i < 3U; ++i) { efmu.mocap_positionCovarianceWorld_m2[i][i] = 0.01f; efmu.attitudeCovarianceBody_rad2[i][i] = 0.01f; }
  efmu.samplePeriod = (float)divisor / IMU_RATE_HZ;
  for (size_t i = 0U; i < 3U; ++i) {
    if (q_gyro >= 0) efmu.gyroscope_rad2_s[i][i] = (float)q_gyro;
    if (q_accel >= 0) efmu.accelerometer_m2_s3[i][i] = (float)q_accel;
    if (q_gbias >= 0) efmu.gyroscopeBias_rad2_s3[i][i] = (float)q_gbias;
    if (q_abias >= 0) efmu.accelerometerBias_m2_s5[i][i] = (float)q_abias;
    if (v_gbias >= 0) efmu.initialVariances_gyroscopeBias_rad2_s2[i] = (float)v_gbias;
    if (v_abias >= 0) efmu.initialVariances_accelerometerBias_m2_s4[i] = (float)v_abias;
    if (v_att >= 0) efmu.initialVariances_attitude_rad2[i] = (float)v_att;
  }
  if (gate > 0) efmu.innovationGate = (float)gate;
  fprintf(stderr, "process noise diag: gyro %g accel %g gyroBias %g accelBias %g\n", efmu.gyroscope_rad2_s[0][0], efmu.accelerometer_m2_s3[0][0], efmu.gyroscopeBias_rad2_s3[0][0], efmu.accelerometerBias_m2_s5[0][0]);
  NavigationEstimator_recalibrate(&efmu);
  rdd2_imu_preintegrator_reset(&pre, efmu.initialGyroscopeBiasBodyFlu_rad_s, efmu.initialAccelerometerBiasBodyFlu_m_s2);
  pre.nominal_sample_period_s = 1.0f / IMU_RATE_HZ;
  fprintf(stderr, "efmu: samplePeriod %.5f innovationGate %.3f aidingStaleTimeout %.3f maxAidingDelay %.3f gravity [%g %g %g] initQ [%g %g %g %g] minFlowQuality %g minFlowDist %g\n",
          efmu.samplePeriod, efmu.innovationGate, efmu.aidingStaleTimeout_s, efmu.maximumAidingDelay_s, efmu.gravityWorldEnu_m_s2[0], efmu.gravityWorldEnu_m_s2[1], efmu.gravityWorldEnu_m_s2[2],
          efmu.initialQuaternionWorldBody[0], efmu.initialQuaternionWorldBody[1], efmu.initialQuaternionWorldBody[2], efmu.initialQuaternionWorldBody[3], efmu.minimumOpticalFlowQuality, efmu.minimumOpticalFlowGroundDistance_m);

  synapse_topic_GnssFixData_t fix = {0}; synapse_topic_OpticalFlowVelocityData_t flow_sample = {0}; synapse_topic_VehicleHealthData_t health = {0};
  synapse_topic_OpticalFlowData_t flow_raw_sample = {0};
  size_t gi = 0, fi = 0, ri = 0; bool initialized = false, origin_pending = false; uint64_t origin_started_ns = 0;
  uint16_t hold_count = 0; bool usable_observed = false; unsigned long steps = 0, valid_steps = 0, gps_acc = 0, flow_acc = 0;
  double first_valid_t = -1, first_gps_t = -1;
  for (size_t k = 0; k < nimu; ++k) {
    uint64_t now = to_ns(imu[k].t);
    (void)rdd2_imu_preintegrator_accumulate(&pre, imu[k].g, imu[k].a, now);
    if ((k + 1) % (size_t)divisor != 0) continue;
    /* close packet against the bias the block published on its previous step */
    float gb[3], ab[3]; bool bias_usable = efmu.status_initialized;
    for (size_t a = 0; a < 3; ++a) { gb[a] = efmu.gyroscopeBiasBodyFlu_rad_s[a]; ab[a] = efmu.accelerometerBiasBodyFlu_m_s2[a]; }
    bias_usable = bias_usable && rdd2_control_values_are_finite(gb, 3) && rdd2_control_values_are_finite(ab, 3);
    if (!bias_usable) for (size_t a = 0; a < 3; ++a) { gb[a] = efmu.initialGyroscopeBiasBodyFlu_rad_s[a]; ab[a] = efmu.initialAccelerometerBiasBodyFlu_m_s2[a]; }
    rdd2_imu_preintegrator_close(&pre, &packet, now, gb, ab);
    copy_imu_input_to_efmu(&efmu, &packet);
    efmu.mocap_valid = false; efmu.mocap_fresh = false;
    /* deliver sensor messages whose timestamps have arrived */
    bool gnss_fresh = false, flow_fresh = false, flow_raw_fresh = false;
    while (gi < ngps && to_ns(gps[gi].t) <= now) {
      bool denied = !use_gps; for (int d = 0; d < ndeny; ++d) if (gps[gi].t >= deny0[d] && gps[gi].t < deny1[d]) denied = true;
      if (!denied) { gps_to_fix(&gps[gi], &fix); gnss_fresh = true; }
      gi++;
    }
    while (fi < nflow && to_ns(flow[fi].t) <= now) { if (use_flow) { flow_to_sample(&flow[fi], &flow_sample); flow_fresh = true; } fi++; }
    while (ri < nflow_raw && to_ns(flow_raw[ri].t) <= now) { if (use_flow_raw) { flow_raw_to_sample(&flow_raw[ri], &flow_raw_sample); flow_raw_fresh = true; } ri++; }
    health.timestamp_ns = now; health.flags = 0U;
    struct rdd2_navigation_gps_measurement gm; struct rdd2_navigation_optical_flow_measurement fm;
    struct rdd2_navigation_optical_flow_raw_measurement frm;
    bool origin_captured = rdd2_navigation_gps_step(&gps_adapter, &gm, &fix, gnss_fresh, &health, true, now);
    copy_gps_input_to_efmu(&efmu, &gm);
    if (use_flow_raw) {
      rdd2_navigation_optical_flow_raw_step(&flow_raw_adapter, &frm, &flow_raw_sample, flow_raw_fresh, now, &g_flow_raw_config);
      copy_optical_flow_raw_input_to_efmu(&efmu, &frm);
    } else {
      rdd2_navigation_optical_flow_step(&flow_adapter, &fm, &flow_sample, flow_fresh, now, &g_flow_config);
      copy_optical_flow_input_to_efmu(&efmu, &fm);
    }
    if (origin_captured) { origin_pending = true; origin_started_ns = now; if (verbose) fprintf(stderr, "origin captured at t=%.3f lat %.7f lon %.7f alt %.3f\n", imu[k].t, gps_adapter.origin_latitude_deg_e7 * 1e-7, gps_adapter.origin_longitude_deg_e7 * 1e-7, gps_adapter.origin_altitude_msl_mm * 1e-3); }
    efmu.reset = !initialized || origin_pending;
    NavigationEstimator_dostep(&efmu);
    bool step_ok = rdd2_generated_step_ok(efmu.rumoca_galec_error_signal_status);
    bool finite = efmu_estimate_is_finite(&efmu);
    bool state_usable = step_ok && finite && efmu.estimate_valid && efmu.status_initialized;
    bool estimate_valid;
    if (!state_usable) { hold_count = 0; usable_observed = false; estimate_valid = false; }
    else if (!efmu.status_imuPayloadHeld) { hold_count = 0; estimate_valid = efmu.imu_valid; if (estimate_valid) usable_observed = true; }
    else { if (hold_count < UINT16_MAX) hold_count++; estimate_valid = usable_observed && hold_count > 0 && hold_count <= RDD2_NAVIGATION_IMU_HOLD_MAX_RELEASES; }
    initialized = origin_pending ? estimate_valid : state_usable;
    if (estimate_valid) origin_pending = false;
    if (origin_pending && now - origin_started_ns > GPS_ORIGIN_INITIALIZATION_TIMEOUT_NS) origin_pending = false;
    steps++; if (estimate_valid) { valid_steps++; if (first_valid_t < 0) first_valid_t = imu[k].t; }
    if (efmu.status_gpsPositionCorrectionAccepted) { gps_acc++; if (first_gps_t < 0) first_gps_t = imu[k].t; }
    if (efmu.status_opticalFlowCorrectionAccepted) flow_acc++;
    double eul[3]; euler_from_quat(efmu.estimate_quaternionWorldBody, eul);
    fprintf(out, "%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.6f,%.6f,%.6f,%.6f,%.5f,%.5f,%.5f,%.6f,%.6f,%.6f,%.5f,%.5f,%.5f,%d,%d,%d,%d,%d,%d,%d,%.3f,%d,%d,%d,%d,%d,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.5f,%.5f,%.5f,%u\n",
            imu[k].t, efmu.estimate_positionWorldEnu_m[0], efmu.estimate_positionWorldEnu_m[1], efmu.estimate_positionWorldEnu_m[2],
            efmu.estimate_velocityWorldEnu_m_s[0], efmu.estimate_velocityWorldEnu_m_s[1], efmu.estimate_velocityWorldEnu_m_s[2],
            efmu.estimate_quaternionWorldBody[0], efmu.estimate_quaternionWorldBody[1], efmu.estimate_quaternionWorldBody[2], efmu.estimate_quaternionWorldBody[3],
            eul[0], eul[1], eul[2], efmu.gyroscopeBiasBodyFlu_rad_s[0], efmu.gyroscopeBiasBodyFlu_rad_s[1], efmu.gyroscopeBiasBodyFlu_rad_s[2],
            efmu.accelerometerBiasBodyFlu_m_s2[0], efmu.accelerometerBiasBodyFlu_m_s2[1], efmu.accelerometerBiasBodyFlu_m_s2[2],
            estimate_valid && gps_adapter.origin_valid, estimate_valid,
            efmu.status_initialized, efmu.status_recoveryStage, efmu.status_correctionOutcome, efmu.status_correctionSource, efmu.status_anchorSource, efmu.status_normalizedInnovationSquared,
            efmu.status_gpsPositionCorrectionAccepted, efmu.status_gpsVelocityCorrectionAccepted, efmu.status_opticalFlowCorrectionAccepted,
            efmu.gpsConsecutiveRejections, efmu.opticalFlowConsecutiveRejections, efmu.status_imuPayloadHeld,
            sqrt(efmu.navigationCovarianceLocal[0][0]), sqrt(efmu.navigationCovarianceLocal[1][1]), sqrt(efmu.navigationCovarianceLocal[2][2]), sqrt(efmu.navigationCovarianceLocal[3][3]), sqrt(efmu.navigationCovarianceLocal[4][4]), sqrt(efmu.navigationCovarianceLocal[5][5]), sqrt(efmu.stateCovariance[6][6]), sqrt(efmu.stateCovariance[7][7]), sqrt(efmu.stateCovariance[8][8]), efmu.rumoca_galec_error_signal_status);
  }
  fclose(out);
  fprintf(stderr, "steps %lu valid %lu (%.1f%%) first valid t=%.2f first gps accept t=%.2f gps accepts %lu flow accepts %lu origin lat %.7f lon %.7f alt %.3f\n",
          steps, valid_steps, 100.0 * valid_steps / (double)steps, first_valid_t, first_gps_t, gps_acc, flow_acc,
          gps_adapter.origin_latitude_deg_e7 * 1e-7, gps_adapter.origin_longitude_deg_e7 * 1e-7, gps_adapter.origin_altitude_msl_mm * 1e-3);
  return 0;
}
