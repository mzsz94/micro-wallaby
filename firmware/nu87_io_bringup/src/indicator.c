/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

#include <microwallaby/indicator.h>

static const struct gpio_dt_spec led_red = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec led_green = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
static const struct gpio_dt_spec led_blue = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);

static int set_rgb(bool red, bool green, bool blue)
{
	int ret = gpio_pin_set_dt(&led_red, red);

	ret = ret < 0 ? ret : gpio_pin_set_dt(&led_green, green);
	ret = ret < 0 ? ret : gpio_pin_set_dt(&led_blue, blue);
	return ret;
}
int mw_indicator_init(void)
{
	if (!gpio_is_ready_dt(&led_red) || !gpio_is_ready_dt(&led_green) ||
	    !gpio_is_ready_dt(&led_blue)) {
		return -ENODEV;
	}

	int ret = gpio_pin_configure_dt(&led_red, GPIO_OUTPUT_INACTIVE);

	ret = ret < 0 ? ret : gpio_pin_configure_dt(&led_green, GPIO_OUTPUT_INACTIVE);
	ret = ret < 0 ? ret : gpio_pin_configure_dt(&led_blue, GPIO_OUTPUT_INACTIVE);
	return ret;
}

int mw_indicator_self_test(void)
{
	int ret = set_rgb(true, false, false);

	if (ret < 0) {
		return ret;
	}
	k_sleep(K_MSEC(150));
	ret = set_rgb(false, true, false);
	if (ret < 0) {
		return ret;
	}
	k_sleep(K_MSEC(150));
	ret = set_rgb(false, false, true);
	if (ret < 0) {
		return ret;
	}
	k_sleep(K_MSEC(150));
	return set_rgb(false, false, false);
}

int mw_indicator_apply(enum mw_robot_state state)
{
	switch (state) {
	case MW_STATE_FAULT:
		return set_rgb(true, false, false);
	case MW_STATE_MANUAL_EMU:
		return set_rgb(false, false, true);
	case MW_STATE_SAFE_IDLE:
		return set_rgb(false, true, false);
	case MW_STATE_BOOT:
	case MW_STATE_SELF_TEST:
	default:
		return set_rgb(false, false, false);
	}
}
