/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "zenoh_pico_shell_backend.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include <zros/zros_node.h>
#include <zros/zros_pub.h>
#include <zros/zros_topic.h>

#include "synapse/synapse_mocap_reader.h"
#include "topic_bus.h"

#define RDD2_MOCAP_FRAME_KEYEXPR "synapse/mocap/frame"
#define RDD2_MOCAP_ZENOH_POLL_MS 10U
#define RDD2_MOCAP_ZROS_STACK_SIZE 2048U
#define RDD2_MOCAP_ZROS_PRIORITY 6

static struct zros_node g_mocap_zros_node;
static struct zros_pub g_mocap_frame_zros_pub;
static struct zros_pub g_mocap_rigid_bodies_zros_pub;
static struct rdd2_topic_mocap_frame g_mocap_zros_msg;
static struct rdd2_topic_mocap_rigid_bodies g_mocap_rigid_bodies_msg;

static bool mocap_sample_valid(const struct zp_zephyr_zenoh_shell_sample_snapshot *sample)
{
	if (sample == NULL || sample->receive_count == 0U) {
		return false;
	}

	if (sample->payload_stored_len != sample->payload_len) {
		return false;
	}

	if (strcmp(sample->keyexpr, RDD2_MOCAP_FRAME_KEYEXPR) != 0) {
		return false;
	}

	return sample->payload_len >= 8U;
}

static void mocap_publish_rigid_bodies(const struct zp_zephyr_zenoh_shell_sample_snapshot *sample)
{
	synapse_topic_MocapFrame_table_t frame;
	synapse_topic_MocapRigidBodySample_vec_t rigid_bodies;
	size_t body_count;
	size_t stored_count;

	frame = synapse_topic_MocapFrame_as_root(sample->payload);
	rigid_bodies = synapse_topic_MocapFrame_rigid_bodies(frame);
	body_count = rigid_bodies == NULL ? 0U :
					  synapse_topic_MocapRigidBodySample_vec_len(rigid_bodies);
	stored_count = MIN(body_count, (size_t)RDD2_TOPIC_MOCAP_RIGID_BODY_MAX);

	memset(&g_mocap_rigid_bodies_msg, 0, sizeof(g_mocap_rigid_bodies_msg));
	g_mocap_rigid_bodies_msg.timestamp_us = synapse_topic_MocapFrame_timestamp_us(frame);
	g_mocap_rigid_bodies_msg.frame_number = synapse_topic_MocapFrame_frame_number(frame);
	g_mocap_rigid_bodies_msg.count = stored_count;
	g_mocap_rigid_bodies_msg.receive_count = sample->receive_count;
	g_mocap_rigid_bodies_msg.receive_stamp_ms = sample->receive_stamp_ms;

	for (size_t i = 0U; i < stored_count; i++) {
		synapse_topic_MocapRigidBodySample_struct_t body =
			synapse_topic_MocapRigidBodySample_vec_at(rigid_bodies, i);
		synapse_topic_Vec3f_struct_t position =
			synapse_topic_MocapRigidBodySample_position(body);
		synapse_topic_Quaternionf_struct_t attitude =
			synapse_topic_MocapRigidBodySample_attitude(body);
		struct rdd2_topic_mocap_rigid_body_pose *pose =
			&g_mocap_rigid_bodies_msg.bodies[i];

		pose->id = synapse_topic_MocapRigidBodySample_id(body);
		pose->position_m[0] = position->x;
		pose->position_m[1] = position->y;
		pose->position_m[2] = position->z;
		pose->attitude_xyzw[0] = attitude->x;
		pose->attitude_xyzw[1] = attitude->y;
		pose->attitude_xyzw[2] = attitude->z;
		pose->attitude_xyzw[3] = attitude->w;
		pose->residual = synapse_topic_MocapRigidBodySample_residual(body);
		pose->tracking_valid = synapse_topic_MocapRigidBodySample_tracking_valid(body);
	}

	(void)zros_pub_update(&g_mocap_rigid_bodies_zros_pub);
}

static void mocap_publish_sample(const struct zp_zephyr_zenoh_shell_sample_snapshot *sample)
{
	size_t keyexpr_len;

	if (!mocap_sample_valid(sample)) {
		return;
	}

	memset(&g_mocap_zros_msg, 0, sizeof(g_mocap_zros_msg));
	memcpy(g_mocap_zros_msg.payload, sample->payload, sample->payload_len);
	g_mocap_zros_msg.payload_len = sample->payload_len;
	keyexpr_len = MIN(strlen(sample->keyexpr), sizeof(g_mocap_zros_msg.keyexpr) - 1U);
	memcpy(g_mocap_zros_msg.keyexpr, sample->keyexpr, keyexpr_len);
	g_mocap_zros_msg.receive_count = sample->receive_count;
	g_mocap_zros_msg.receive_stamp_ms = sample->receive_stamp_ms;

	(void)zros_pub_update(&g_mocap_frame_zros_pub);
	mocap_publish_rigid_bodies(sample);
}

static void mocap_zenoh_to_zros_thread(void *arg0, void *arg1, void *arg2)
{
	uint64_t last_receive_count = 0U;

	ARG_UNUSED(arg0);
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);

	zros_node_init(&g_mocap_zros_node, "rdd2_mocap");
	if (zros_pub_init(&g_mocap_frame_zros_pub, &g_mocap_zros_node, &topic_mocap_frame,
			  &g_mocap_zros_msg) != 0) {
		return;
	}
	if (zros_pub_init(&g_mocap_rigid_bodies_zros_pub, &g_mocap_zros_node,
			  &topic_mocap_rigid_bodies, &g_mocap_rigid_bodies_msg) != 0) {
		return;
	}

	while (true) {
		struct zp_zephyr_zenoh_shell_sample_snapshot sample = {0};

		if (zp_zephyr_zenoh_shell_latest_sample_get(&sample) &&
		    sample.receive_count != last_receive_count) {
			last_receive_count = sample.receive_count;
			mocap_publish_sample(&sample);
		}

		k_sleep(K_MSEC(RDD2_MOCAP_ZENOH_POLL_MS));
	}
}

K_THREAD_DEFINE(g_mocap_zros_tid, RDD2_MOCAP_ZROS_STACK_SIZE, mocap_zenoh_to_zros_thread, NULL,
		NULL, NULL, RDD2_MOCAP_ZROS_PRIORITY, 0, 0);

static void mocap_shell_print_rigid_body(const struct shell *sh,
					 synapse_topic_MocapRigidBodySample_struct_t body)
{
	synapse_topic_Vec3f_struct_t position;
	synapse_topic_Quaternionf_struct_t attitude;

	position = synapse_topic_MocapRigidBodySample_position(body);
	attitude = synapse_topic_MocapRigidBodySample_attitude(body);

	shell_print(sh,
		    "rigid_body[0] id=%ld valid=%d residual=%9.6f "
		    "pos_m=[%9.5f %9.5f %9.5f] quat_xyzw=[%9.6f %9.6f %9.6f %9.6f]",
		    (long)synapse_topic_MocapRigidBodySample_id(body),
		    synapse_topic_MocapRigidBodySample_tracking_valid(body) ? 1 : 0,
		    (double)synapse_topic_MocapRigidBodySample_residual(body),
		    (double)position->x, (double)position->y, (double)position->z,
		    (double)attitude->x, (double)attitude->y, (double)attitude->z,
		    (double)attitude->w);
}

static int cmd_mocap_latest(const struct shell *sh, size_t argc, char **argv)
{
	struct rdd2_topic_mocap_frame sample = {0};
	synapse_topic_MocapFrame_table_t frame;
	synapse_topic_MocapMarkerSample_vec_t labeled_markers;
	synapse_topic_MocapMarkerSample_vec_t unlabeled_markers;
	synapse_topic_MocapRigidBodySample_vec_t rigid_bodies;
	synapse_topic_MocapSegmentSample_vec_t segments;
	size_t labeled_count;
	size_t unlabeled_count;
	size_t rigid_body_count;
	size_t segment_count;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!rdd2_topic_has_sample(&topic_mocap_frame) ||
	    zros_topic_read(&topic_mocap_frame, &sample) != 0) {
		shell_error(sh, "no zros mocap_frame sample received yet");
		return -ENOENT;
	}

	if (sample.payload_len < 8U) {
		shell_error(sh, "latest zros mocap_frame is too small: bytes=%zu",
			    sample.payload_len);
		return -EINVAL;
	}

	frame = synapse_topic_MocapFrame_as_root(sample.payload);
	labeled_markers = synapse_topic_MocapFrame_labeled_markers(frame);
	unlabeled_markers = synapse_topic_MocapFrame_unlabeled_markers(frame);
	rigid_bodies = synapse_topic_MocapFrame_rigid_bodies(frame);
	segments = synapse_topic_MocapFrame_skeleton_segments(frame);
	labeled_count = labeled_markers == NULL ?
				0U :
				synapse_topic_MocapMarkerSample_vec_len(labeled_markers);
	unlabeled_count = unlabeled_markers == NULL ?
				  0U :
				  synapse_topic_MocapMarkerSample_vec_len(unlabeled_markers);
	rigid_body_count = rigid_bodies == NULL ?
				   0U :
				   synapse_topic_MocapRigidBodySample_vec_len(rigid_bodies);
	segment_count = segments == NULL ? 0U : synapse_topic_MocapSegmentSample_vec_len(segments);

	shell_print(sh,
		    "mocap key=%s bytes=%zu rx=%llu timestamp_us=%llu frame=%lu "
		    "labeled=%zu unlabeled=%zu rigid_bodies=%zu segments=%zu",
		    sample.keyexpr, sample.payload_len, (unsigned long long)sample.receive_count,
		    (unsigned long long)synapse_topic_MocapFrame_timestamp_us(frame),
		    (unsigned long)synapse_topic_MocapFrame_frame_number(frame), labeled_count,
		    unlabeled_count, rigid_body_count, segment_count);

	if (rigid_body_count > 0U) {
		mocap_shell_print_rigid_body(
			sh, synapse_topic_MocapRigidBodySample_vec_at(rigid_bodies, 0));
	}

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_mocap,
			      SHELL_CMD_ARG(latest, NULL,
					    "decode latest zros mocap_frame as MocapFrame",
					    cmd_mocap_latest, 1, 0),
			      SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(mocap, &sub_mocap, "motion capture commands", NULL);
