/* SPDX-License-Identifier: Apache-2.0 */

#ifndef RDD2_TEST_FAKE_ZROS_SUB_H_
#define RDD2_TEST_FAKE_ZROS_SUB_H_

#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#include <zros/zros_node.h>
#include <zros/zros_topic.h>

/* Each subscriber is paced to a per-source rate against the simulated boot
 * clock, reproducing the recorded flight per-channel publish cadence. */
struct zros_sub {
	int source_index;
	uint64_t period_ns;
	uint64_t next_due_ns;
	void *data;
};

int zros_sub_init(struct zros_sub *sub, struct zros_node *node, struct zros_topic *topic,
		  void *data, double rate_limit_hz);
int zros_sub_update(struct zros_sub *sub);
bool zros_sub_update_available(struct zros_sub *sub);
int zros_sub_wait_many(struct zros_sub *const *subs, size_t count, k_timeout_t timeout);

#endif /* RDD2_TEST_FAKE_ZROS_SUB_H_ */
