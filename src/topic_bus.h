#ifndef RDD2_TOPIC_BUS_H_
#define RDD2_TOPIC_BUS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/sys/util.h>

#include <zros/zros_topic.h>

#include <csyn/csyn_zros.h>

#include "synapse_messages.h"

BUILD_ASSERT(sizeof(rdd2_topic_motor_output_blob_t) == 48U);

ZROS_TOPIC_DECLARE(rc, rdd2_rc_channels_t);

uint32_t rdd2_topic_generation(const struct zros_topic *topic);
bool rdd2_topic_has_sample(const struct zros_topic *topic);
bool rdd2_topic_copy_blob(const struct zros_topic *topic, uint8_t *buf, size_t buf_size,
			  size_t *len);
uint32_t rdd2_topic_flight_state_generation(void);
bool rdd2_topic_flight_state_copy_blob(uint8_t *buf, size_t buf_size, size_t *len);
uint32_t rdd2_topic_motor_output_generation(void);
bool rdd2_topic_motor_output_copy_blob(uint8_t *buf, size_t buf_size, size_t *len);
/* Latest GNSS fix, whatever filled it. Returns false until a fix arrives. */
bool rdd2_topic_gnss_copy(synapse_topic_GnssFixData_t *fix, uint32_t *generation);
/* Used by the onboard receiver bridge; the radio path publishes via the
 * transport instead. */
bool rdd2_topic_gnss_publish(const synapse_topic_GnssFixData_t *fix);
#endif
