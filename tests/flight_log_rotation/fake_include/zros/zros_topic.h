/* SPDX-License-Identifier: Apache-2.0 */

#ifndef RDD2_TEST_FAKE_ZROS_TOPIC_H_
#define RDD2_TEST_FAKE_ZROS_TOPIC_H_

/* Opaque topic handle: the rotation suite identifies each source by the order
 * the logger initialises its subscribers, not by the topic contents. */
struct zros_topic {
	int _id;
};

#endif /* RDD2_TEST_FAKE_ZROS_TOPIC_H_ */
