/* SPDX-License-Identifier: Apache-2.0 */

/* Topic handle table the flight logger subscribes to. The rotation suite backs
 * each with an opaque handle and paces them from the harness rate table. */

#ifndef RDD2_TEST_FAKE_ZROS_TOPICS_H_
#define RDD2_TEST_FAKE_ZROS_TOPICS_H_

#include <zros/zros_topic.h>

extern struct zros_topic topic_control_imu;
extern struct zros_topic topic_pwm_signal_outputs;
extern struct zros_topic topic_navigation_odometry;
extern struct zros_topic topic_attitude_estimate;
extern struct zros_topic topic_vehicle_health;
extern struct zros_topic topic_control_loop_metrics;
extern struct zros_topic topic_attitude_command;
extern struct zros_topic topic_rate_command;
extern struct zros_topic topic_manual_input;
extern struct zros_topic topic_optical_flow_vel;
extern struct zros_topic topic_optical_flow;
extern struct zros_topic topic_gnss_fix;

#endif /* RDD2_TEST_FAKE_ZROS_TOPICS_H_ */
