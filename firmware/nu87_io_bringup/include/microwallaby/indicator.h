/* SPDX-License-Identifier: Apache-2.0 */

#ifndef MICROWALLABY_INDICATOR_H_
#define MICROWALLABY_INDICATOR_H_

#include <microwallaby/io_types.h>

int mw_indicator_init(void);
int mw_indicator_self_test(void);
int mw_indicator_apply(enum mw_robot_state state);

#endif /* MICROWALLABY_INDICATOR_H_ */
