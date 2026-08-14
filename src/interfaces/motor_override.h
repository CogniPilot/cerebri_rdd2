/* SPDX-License-Identifier: Apache-2.0 */

#ifndef RDD2_INTERFACES_MOTOR_OVERRIDE_H_
#define RDD2_INTERFACES_MOTOR_OVERRIDE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static inline bool rdd2_motor_override_copy_f32(bool active, float *output,
                                                 const float *override,
                                                 size_t count) {
  if (active) {
    for (size_t index = 0U; index < count; ++index) {
      output[index] = override[index];
    }
  }
  return active;
}

static inline bool rdd2_motor_override_copy_u16(bool active, uint16_t *output,
                                                 const uint16_t *override,
                                                 size_t count) {
  if (active) {
    for (size_t index = 0U; index < count; ++index) {
      output[index] = override[index];
    }
  }
  return active;
}

#endif /* RDD2_INTERFACES_MOTOR_OVERRIDE_H_ */
