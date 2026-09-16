/* SPDX-License-Identifier: Apache-2.0 */

#ifndef RDD2_TEST_FAKE_ZROS_NODE_H_
#define RDD2_TEST_FAKE_ZROS_NODE_H_

struct zros_node {
	const char *name;
};

void zros_node_init(struct zros_node *node, const char *name);

#endif /* RDD2_TEST_FAKE_ZROS_NODE_H_ */
