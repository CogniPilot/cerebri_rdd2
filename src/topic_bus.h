#ifndef RDD2_TOPIC_BUS_H_
#define RDD2_TOPIC_BUS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/sys/util.h>

#include <zros/zros_topic.h>

#include "topic_flatbuffer.h"

#ifdef CONFIG_ZENOH_PICO_SHELL_MAX_PAYLOAD
#define RDD2_TOPIC_MOCAP_FRAME_MAX_PAYLOAD CONFIG_ZENOH_PICO_SHELL_MAX_PAYLOAD
#else
#define RDD2_TOPIC_MOCAP_FRAME_MAX_PAYLOAD 512U
#endif

#ifdef CONFIG_ZENOH_PICO_SHELL_MAX_KEYEXPR
#define RDD2_TOPIC_MOCAP_FRAME_MAX_KEYEXPR CONFIG_ZENOH_PICO_SHELL_MAX_KEYEXPR
#else
#define RDD2_TOPIC_MOCAP_FRAME_MAX_KEYEXPR 96U
#endif

#define RDD2_TOPIC_MOCAP_RIGID_BODY_MAX 4U

struct rdd2_topic_mocap_frame {
	uint8_t payload[RDD2_TOPIC_MOCAP_FRAME_MAX_PAYLOAD];
	size_t payload_len;
	char keyexpr[RDD2_TOPIC_MOCAP_FRAME_MAX_KEYEXPR + 1U];
	uint64_t receive_count;
	int64_t receive_stamp_ms;
};

struct rdd2_topic_mocap_rigid_body_pose {
	int32_t id;
	float position_m[3];
	float attitude_xyzw[4];
	float residual;
	bool tracking_valid;
};

struct rdd2_topic_mocap_rigid_bodies {
	uint64_t timestamp_us;
	uint32_t frame_number;
	uint32_t count;
	uint64_t receive_count;
	int64_t receive_stamp_ms;
	struct rdd2_topic_mocap_rigid_body_pose bodies[RDD2_TOPIC_MOCAP_RIGID_BODY_MAX];
};

BUILD_ASSERT(sizeof(rdd2_topic_flight_state_blob_t) == RDD2_TOPIC_FB_FLIGHT_STATE_SIZE);
BUILD_ASSERT(sizeof(rdd2_topic_motor_output_blob_t) == RDD2_TOPIC_FB_MOTOR_OUTPUT_SIZE);

ZROS_TOPIC_DECLARE(rc, synapse_topic_RcChannels16_t);
ZROS_TOPIC_DECLARE(flight_state, rdd2_topic_flight_state_blob_t);
ZROS_TOPIC_DECLARE(motor_output, rdd2_topic_motor_output_blob_t);
ZROS_TOPIC_DECLARE(mocap_frame, struct rdd2_topic_mocap_frame);
ZROS_TOPIC_DECLARE(mocap_rigid_bodies, struct rdd2_topic_mocap_rigid_bodies);

uint32_t rdd2_topic_generation(const struct zros_topic *topic);
bool rdd2_topic_has_sample(const struct zros_topic *topic);
bool rdd2_topic_copy_blob(const struct zros_topic *topic, uint8_t *buf, size_t buf_size,
			  size_t *len);
uint32_t rdd2_topic_flight_state_generation(void);
bool rdd2_topic_flight_state_copy_blob(uint8_t *buf, size_t buf_size, size_t *len);
uint32_t rdd2_topic_motor_output_generation(void);
bool rdd2_topic_motor_output_copy_blob(uint8_t *buf, size_t buf_size, size_t *len);
uint32_t rdd2_topic_mocap_frame_generation(void);
uint32_t rdd2_topic_mocap_rigid_bodies_generation(void);

#endif
