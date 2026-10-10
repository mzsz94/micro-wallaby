/* SPDX-License-Identifier: Apache-2.0 */

#ifndef MICROWALLABY_SENSOR_FAST_H_
#define MICROWALLABY_SENSOR_FAST_H_

#include <microwallaby/io_types.h>

int mw_sensor_fast_init(void);
int mw_sensor_fast_sample(struct mw_input_snapshot *snapshot);
const char *mw_fixture_name(enum mw_fixture_profile profile);

#endif /* MICROWALLABY_SENSOR_FAST_H_ */
